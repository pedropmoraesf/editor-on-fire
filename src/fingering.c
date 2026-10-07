/*
 * fingering.c
 *
 * Note>Optimize fingering
 *
 * Re-chooses the string/fret of single notes in the active pro guitar/bass
 * track difficulty so that the fretting hand moves as little as possible.
 * The whole sequence is optimized at once with dynamic programming (Viterbi),
 * the usual approach for the "optimal fingering problem" on string
 * instruments (Sayegh 1989; Radisavljevic & Driessen 2004; Tuohy & Potter 2005),
 * instead of greedily looking only at the previous note.
 *
 * Chords, and notes whose technique depends on the exact place where they
 * are played (bends, slides, hammer-ons/pull-offs, taps, harmonics, vibrato,
 * linked notes, string mutes, trills/tremolos, tech notes), are never moved.
 * They act as fixed "anchors" that the surrounding free notes take into
 * account.
 */

#include <allegro.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agup/agup.h"
#include "main.h"
#include "song.h"
#include "undo.h"
#include "dialog.h"
#include "tuning.h"
#include "menu/track.h"
#include "fingering.h"

#ifdef USEMEMWATCH
#include "memwatch.h"
#endif

#define EOF_FINGERING_MAX_STATES 8

/* Techniques that tie a note to the exact string/fret it was written on */
#define EOF_FINGERING_LOCKED_FLAGS (EOF_PRO_GUITAR_NOTE_FLAG_BEND | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP | \
	EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN | EOF_PRO_GUITAR_NOTE_FLAG_UNPITCH_SLIDE | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_REVERSE | \
	EOF_PRO_GUITAR_NOTE_FLAG_HO | EOF_PRO_GUITAR_NOTE_FLAG_PO | EOF_PRO_GUITAR_NOTE_FLAG_TAP | \
	EOF_PRO_GUITAR_NOTE_FLAG_HARMONIC | EOF_PRO_GUITAR_NOTE_FLAG_P_HARMONIC | EOF_PRO_GUITAR_NOTE_FLAG_VIBRATO | \
	EOF_PRO_GUITAR_NOTE_FLAG_LINKNEXT | EOF_PRO_GUITAR_NOTE_FLAG_STRING_MUTE | EOF_NOTE_FLAG_IS_TRILL | EOF_NOTE_FLAG_IS_TREMOLO)

typedef struct
{
	unsigned long note;		//Index of the note in the track's pgnote[] array
	char anchor;			//Nonzero if this note must keep its string/fret
	char anchor_fretted;	//For anchors, nonzero if at least one used string is fretted (not open, not muted)
	double anchor_fret;		//For anchors, the average fret of the fretted strings
	double anchor_string;	//For anchors, the average string index of the used strings
	int orig_string;		//For free notes, the original string index (0 = lowest string)
	int orig_fret;			//For free notes, the original fret
	int prev;				//Index of the item whose states precede this item's states (-1 if none)
	int numstates;
	int cand_string[EOF_FINGERING_MAX_STATES];
	int cand_fret[EOF_FINGERING_MAX_STATES];
	double cost[EOF_FINGERING_MAX_STATES];
	int pred[EOF_FINGERING_MAX_STATES];	//Index of the predecessor state in item[prev] (-1 if none)
	int choice;				//For free notes, the chosen state (-1 if not decided yet)
} eof_fingering_item;

static double eof_fingering_intrinsic_cost(int fret, int avoid_open)
{
	if(fret == 0)
	{	//Open string
		if(avoid_open)
			return 4000.0;	//Very expensive, but still used when it's the only way to play the note
		return -5.0;		//Otherwise a small bonus:  the fretting hand doesn't have to do anything
	}
	if(fret > 12)
		return (double)(fret - 12) * (double)(fret - 12) * 0.5;	//Very high positions are slightly less comfortable

	return 0.0;
}

static double eof_fingering_transition_cost(int has_ref, double ref_fret, double ref_string, double string, double fret, int is_bass)
{
	double dist_fret, dist_string, cost;

	if(!has_ref)
		return fret * 0.5;

	dist_fret = (fret > 0.0) ? fabs(fret - ref_fret) : 0.0;	//An open string doesn't require moving the hand
	dist_string = fabs(string - ref_string);

	cost = dist_fret * 12.0;
	if(dist_fret >= 5.0)
		cost += (dist_fret - 5.0 + 1.0) * (dist_fret - 5.0 + 1.0) * 30.0;
	else
		cost += dist_fret * dist_fret * 3.0;

	if(dist_string == 0.0)
		cost += 2.0;
	else if(dist_string == 1.0)
		cost += 15.0;
	else if(is_bass && (dist_fret <= 5.0))
		cost += dist_string * 120.0;
	else
		cost += dist_string * dist_string * 300.0;

	return cost;
}

static void eof_fingering_state_ref(eof_fingering_item *items, int idx, int state, int *has_ref, double *ref_fret, double *ref_string, int global_has_ref, double global_ref_fret)
{
	//Returns the reference position used for a transition starting at the specified state of the specified item
	if(items[idx].anchor)
	{
		*ref_fret = items[idx].anchor_fret;
		*ref_string = items[idx].anchor_string;
		*has_ref = 1;
		return;
	}
	*ref_string = items[idx].cand_string[state];
	if(items[idx].cand_fret[state] > 0)
	{
		*ref_fret = items[idx].cand_fret[state];
		*has_ref = 1;
	}
	else
	{	//An open string doesn't define a hand position, fall back to the last anchor's fret
		*ref_fret = global_ref_fret;
		*has_ref = global_has_ref;
	}
}

static void eof_fingering_backtrack(eof_fingering_item *items, int last, int state)
{
	//Walks back from the specified state of the specified free item, fixing the choice of every free item in the segment
	int idx = last;

	while((idx >= 0) && !items[idx].anchor && (state >= 0))
	{
		items[idx].choice = state;
		state = items[idx].pred[state];
		idx = items[idx].prev;
	}
}

static unsigned long eof_fingering_optimize_track(EOF_SONG *sp, unsigned long track, unsigned char diff, int avoid_open, int use_selection, unsigned long *examined)
{
	EOF_PRO_GUITAR_TRACK *tp;
	eof_fingering_item *items;
	int base[EOF_TUNING_LENGTH];
	unsigned long ctr, numitems = 0, changed = 0;
	int is_bass, numstrings, numfrets, i, s;
	int prev_idx = -1;			//The item holding the states the next item transitions from
	int last_free = -1;			//The last free item of the open segment (-1 if the segment is empty)
	int global_has_ref = 0;		//Nonzero once a fretted anchor has defined a hand position
	double global_ref_fret = 0.0;

	if(examined)
		*examined = 0;
	if(!sp || (track >= sp->tracks) || !eof_track_is_pro_guitar_track(sp, track))
		return 0;

	tp = sp->pro_guitar_track[sp->track[track]->tracknum];
	numstrings = tp->numstrings;
	numfrets = tp->numfrets;
	if((numstrings < 1) || (numstrings > EOF_TUNING_LENGTH))
		return 0;
	is_bass = eof_track_is_bass_arrangement(tp, track);

	for(s = 0; s < numstrings; s++)
	{	//Build the absolute MIDI pitch of each open string
		int def = eof_lookup_default_string_tuning_absolute(tp, track, s);
		if(def < 0)
			return 0;	//Unsupported track
		base[s] = def + tp->tuning[s];
	}

	items = malloc(sizeof(eof_fingering_item) * (tp->pgnotes + 1));
	if(!items)
		return 0;

	//Build the list of notes in the difficulty, deciding which ones are anchors
	for(ctr = 0; ctr < tp->pgnotes; ctr++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[ctr];
		eof_fingering_item *it;
		unsigned long bitmask;
		int used = 0, string = -1;

		if(np->type != diff)
			continue;

		it = &items[numitems];
		memset(it, 0, sizeof(eof_fingering_item));
		it->note = ctr;
		it->prev = -1;
		it->choice = -1;

		for(s = 0, bitmask = 1; s < numstrings; s++, bitmask <<= 1)
		{
			if(np->note & bitmask)
			{
				used++;
				string = s;
			}
		}

		if((used != 1) || np->ghost || (np->flags & EOF_FINGERING_LOCKED_FLAGS) ||
		   (np->slideend > 0) || (np->unpitchend > 0) || (np->tflags & EOF_NOTE_TFLAG_SLIDE_IN) ||
		   (np->frets[string] & 0x80) || (np->frets[string] > numfrets))
		{
			it->anchor = 1;		//Chords, ghosted/muted notes and notes with position dependent techniques keep their position
		}
		else if(use_selection && !eof_selection.multi[ctr])
		{
			it->anchor = 1;		//Only the selected notes are re-fretted
		}
		else if(eof_pro_guitar_note_has_tech_note(tp, ctr, NULL))
		{
			it->anchor = 1;		//Tech notes are placed on specific strings, moving the note would detach them
		}

		if(!it->anchor)
		{
			it->orig_string = string;
			it->orig_fret = np->frets[string];
		}
		numitems++;
	}
	if(examined)
		*examined = numitems;

	//Lock both ends of slides, unpitched slides, linknext, and legato/HOPO connections unconditionally
	for(ctr = 0; ctr < numitems; ctr++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[items[ctr].note];
		unsigned long flags = np->flags;
		int is_slide = 0;

		if((flags & (EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN |
		             EOF_PRO_GUITAR_NOTE_FLAG_UNPITCH_SLIDE | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_REVERSE |
		             EOF_PRO_GUITAR_NOTE_FLAG_LINKNEXT)) ||
		   (np->slideend > 0) || (np->unpitchend > 0) ||
		   (np->tflags & EOF_NOTE_TFLAG_SLIDE_IN))
		{
			is_slide = 1;
		}

		if(is_slide)
		{
			items[ctr].anchor = 1;	//The slide note itself must be an anchor

			//Lock the immediate next note in sequence
			if(ctr + 1 < numitems)
			{
				items[ctr + 1].anchor = 1;
			}

			//Lock any notes that overlap the slide's sustain and the note right at its landing
			if(np->length > 0)
			{
				unsigned long endpos = np->pos + np->length;
				unsigned long k;
				for(k = ctr + 1; k < numitems; k++)
				{
					if(tp->pgnote[items[k].note]->pos <= endpos)
					{
						items[k].anchor = 1;
					}
					else
					{
						items[k].anchor = 1;	//Arrival note right after sustain
						break;
					}
				}
			}

			//Lock the next note that plays on the same string
			{
				unsigned long k;
				for(k = ctr + 1; k < numitems; k++)
				{
					if(tp->pgnote[items[k].note]->note & np->note)
					{
						items[k].anchor = 1;
						break;
					}
				}
			}

			//Slide-in notes lock the preceding note
			if((np->tflags & EOF_NOTE_TFLAG_SLIDE_IN) && (ctr > 0))
			{
				items[ctr - 1].anchor = 1;
			}
		}

		//Hammer-on and pull-off phrases lock both neighbors
		if(flags & (EOF_PRO_GUITAR_NOTE_FLAG_HO | EOF_PRO_GUITAR_NOTE_FLAG_PO))
		{
			items[ctr].anchor = 1;
			if(ctr > 0)
			{
				items[ctr - 1].anchor = 1;
			}
			if(ctr + 1 < numitems)
			{
				items[ctr + 1].anchor = 1;
			}
			//Also lock previous note that used the same string
			if(ctr > 0)
			{
				long k;
				for(k = (long)ctr - 1; k >= 0; k--)
				{
					if(tp->pgnote[items[k].note]->note & np->note)
					{
						items[k].anchor = 1;
						break;
					}
				}
			}
		}
	}

	//Define the reference position of each anchor
	for(ctr = 0; ctr < numitems; ctr++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[items[ctr].note];
		unsigned long bitmask;
		double fretsum = 0.0, stringsum = 0.0;
		int fretcount = 0, stringcount = 0;

		if(!items[ctr].anchor)
			continue;
		for(s = 0, bitmask = 1; s < numstrings; s++, bitmask <<= 1)
		{
			if(np->note & bitmask)
			{
				stringsum += s;
				stringcount++;
				if(!(np->frets[s] & 0x80) && (np->frets[s] > 0))
				{	//If this string is fretted
					fretsum += np->frets[s];
					fretcount++;
				}
			}
		}
		if(fretcount)
		{
			items[ctr].anchor_fretted = 1;
			items[ctr].anchor_fret = fretsum / fretcount;
			items[ctr].anchor_string = stringcount ? stringsum / stringcount : 0.0;
		}
	}

	//Viterbi pass
	for(i = 0; i < (int)numitems; i++)
	{
		eof_fingering_item *it = &items[i];

		if(it->anchor)
		{
			if(!it->anchor_fretted)
				continue;	//An anchor played only on open strings doesn't define a hand position

			//Close the open segment, choosing the final state that best leads into this anchor
			if((prev_idx >= 0) && !items[prev_idx].anchor && (last_free >= 0))
			{
				int best_state = -1, st;
				double best_cost = 0.0;

				for(st = 0; st < items[prev_idx].numstates; st++)
				{
					int has_ref;
					double ref_fret, ref_string, c;

					eof_fingering_state_ref(items, prev_idx, st, &has_ref, &ref_fret, &ref_string, global_has_ref, global_ref_fret);
					c = items[prev_idx].cost[st] + eof_fingering_transition_cost(has_ref, ref_fret, ref_string, it->anchor_string, it->anchor_fret, is_bass);
					if((best_state < 0) || (c < best_cost))
					{
						best_state = st;
						best_cost = c;
					}
				}
				eof_fingering_backtrack(items, last_free, best_state);
			}
			last_free = -1;
			it->numstates = 1;
			it->cost[0] = 0.0;
			prev_idx = i;
			global_has_ref = 1;
			global_ref_fret = it->anchor_fret;
			continue;
		}

		//Free note: every string/fret combination that produces the same pitch is a possible state
		{
			int pitch = base[it->orig_string] + it->orig_fret;

			it->numstates = 0;
			for(s = 0; s < numstrings; s++)
			{
				int fret = pitch - base[s];
				if((fret >= 0) && (fret <= numfrets) && (it->numstates < EOF_FINGERING_MAX_STATES))
				{
					it->cand_string[it->numstates] = s;
					it->cand_fret[it->numstates] = fret;
					it->numstates++;
				}
			}
			if(!it->numstates)
			{	//Keep the original position if nothing else fits
				it->cand_string[0] = it->orig_string;
				it->cand_fret[0] = it->orig_fret;
				it->numstates = 1;
			}
		}

		it->prev = prev_idx;
		for(s = 0; s < it->numstates; s++)
		{
			double intrinsic = eof_fingering_intrinsic_cost(it->cand_fret[s], avoid_open);

			if(prev_idx < 0)
			{	//First note of the track
				it->cost[s] = eof_fingering_transition_cost(global_has_ref, global_ref_fret, 0.0, it->cand_string[s], it->cand_fret[s], is_bass) + intrinsic;
				it->pred[s] = -1;
			}
			else
			{
				int st, best_pred = -1;
				double best_cost = 0.0;

				for(st = 0; st < items[prev_idx].numstates; st++)
				{
					int has_ref;
					double ref_fret, ref_string, c;

					eof_fingering_state_ref(items, prev_idx, st, &has_ref, &ref_fret, &ref_string, global_has_ref, global_ref_fret);
					c = items[prev_idx].cost[st] + eof_fingering_transition_cost(has_ref, ref_fret, ref_string, it->cand_string[s], it->cand_fret[s], is_bass) + intrinsic;
					if((best_pred < 0) || (c < best_cost))
					{
						best_pred = st;
						best_cost = c;
					}
				}
				it->cost[s] = best_cost;
				it->pred[s] = items[prev_idx].anchor ? -1 : best_pred;
			}
		}
		prev_idx = i;
		last_free = i;
	}

	//Close the last segment
	if((last_free >= 0) && (prev_idx == last_free))
	{
		int best_state = 0, st;
		for(st = 1; st < items[last_free].numstates; st++)
		{
			if(items[last_free].cost[st] < items[last_free].cost[best_state])
				best_state = st;
		}
		eof_fingering_backtrack(items, last_free, best_state);
	}

	//Count the notes that will actually change
	for(ctr = 0; ctr < numitems; ctr++)
	{
		eof_fingering_item *it = &items[ctr];
		if(it->anchor || (it->choice < 0))
			continue;
		if((it->cand_string[it->choice] != it->orig_string) || (it->cand_fret[it->choice] != it->orig_fret))
			changed++;
	}

	//Apply the changes
	if(changed)
	{
		eof_prepare_undo(EOF_UNDO_TYPE_NONE);
		for(ctr = 0; ctr < numitems; ctr++)
		{
			eof_fingering_item *it = &items[ctr];
			EOF_PRO_GUITAR_NOTE *np;
			int newstring, newfret;

			if(it->anchor || (it->choice < 0))
				continue;
			newstring = it->cand_string[it->choice];
			newfret = it->cand_fret[it->choice];
			if((newstring == it->orig_string) && (newfret == it->orig_fret))
				continue;

			np = tp->pgnote[it->note];
			np->frets[it->orig_string] = 0;
			np->finger[it->orig_string] = 0;
			np->note = (unsigned char)(1 << newstring);
			np->frets[newstring] = (unsigned char)newfret;
			np->finger[newstring] = 0;	//The previous fingering no longer applies
		}
	}

	free(items);
	return changed;
}

static char eof_fingering_dialog_scope_text[100] = {0};

DIALOG eof_menu_note_optimize_fingering_dialog[] =
{
	/* (proc)             (x)  (y)  (w)  (h)  (fg) (bg) (key) (flags) (d1) (d2) (dp)                                        (dp2) (dp3) */
	{ eof_window_proc,    0,   48,  380, 146, 2,   23,  0,    0,      0,   0,   "Optimize fingering",                        NULL, NULL },
	{ d_agup_text_proc,   12,  84,  300, 8,   2,   23,  0,    0,      0,   0,   "Re-fret single notes to minimize hand movement.", NULL, NULL },
	{ d_agup_text_proc,   12,  100, 300, 8,   2,   23,  0,    0,      0,   0,   eof_fingering_dialog_scope_text,            NULL, NULL },
	{ d_agup_check_proc,  12,  120, 290, 16,  2,   23,  0,    0,      0,   0,   "Avoid open strings as much as possible",   NULL, NULL },
	{ d_agup_button_proc, 96,  152, 84,  28,  2,   23,  '\r', D_EXIT, 0,   0,   "OK",                                        NULL, NULL },
	{ d_agup_button_proc, 200, 152, 84,  28,  2,   23,  0,    D_EXIT, 0,   0,   "Cancel",                                    NULL, NULL },
	{ NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, NULL }
};

int eof_menu_note_optimize_fingering(void)
{
	EOF_PRO_GUITAR_TRACK *tp;
	char restore_tech_view;
	int use_selection = 0;
	unsigned long ctr, selected = 0, changed, examined;

	if(!eof_song || !eof_song_loaded)
		return 1;
	if(!eof_track_is_pro_guitar_track(eof_song, eof_selected_track))
	{
		allegro_message("Optimize fingering only works on pro guitar/bass tracks.");
		return 1;
	}
	tp = eof_song->pro_guitar_track[eof_song->track[eof_selected_track]->tracknum];

	restore_tech_view = eof_menu_track_get_tech_view_state(eof_song, eof_selected_track);
	if(!restore_tech_view && (eof_selection.track == eof_selected_track))
	{	//The note selection only refers to the normal notes when tech view is off
		for(ctr = 0; ctr < tp->pgnotes; ctr++)
		{
			if(eof_selection.multi[ctr] && (tp->pgnote[ctr]->type == eof_note_type))
				selected++;
		}
	}
	use_selection = (selected > 0);
	if(use_selection)
		(void) snprintf(eof_fingering_dialog_scope_text, sizeof(eof_fingering_dialog_scope_text) - 1, "Scope: %lu selected note%s", selected, (selected == 1) ? "" : "s");
	else
		(void) snprintf(eof_fingering_dialog_scope_text, sizeof(eof_fingering_dialog_scope_text) - 1, "Scope: all notes in the active difficulty");

	eof_cursor_visible = 0;
	eof_render();
	eof_color_dialog(eof_menu_note_optimize_fingering_dialog, gui_fg_color, gui_bg_color);
	eof_conditionally_center_dialog(eof_menu_note_optimize_fingering_dialog);
	if(eof_popup_dialog(eof_menu_note_optimize_fingering_dialog, 0) == 4)
	{	//User clicked OK
		int avoid_open = (eof_menu_note_optimize_fingering_dialog[3].flags & D_SELECTED) ? 1 : 0;

		if(restore_tech_view)
			eof_menu_track_set_tech_view_state(eof_song, eof_selected_track, 0);	//Work on the normal notes
		changed = eof_fingering_optimize_track(eof_song, eof_selected_track, eof_note_type, avoid_open, use_selection, &examined);
		if(restore_tech_view)
			eof_menu_track_set_tech_view_state(eof_song, eof_selected_track, 1);

		if(changed)
		{
			eof_track_fixup_notes(eof_song, eof_selected_track, 1);
		}

		if(!examined)
			allegro_message("There are no notes to optimize in the active difficulty.");
		else if(!changed)
			allegro_message("The fingering is already optimal.  No notes were changed.");
		else
			allegro_message("%lu note%s re-fretted.", changed, (changed == 1) ? " was" : "s were");
	}
	eof_cursor_visible = 1;
	eof_pen_visible = 1;
	eof_show_mouse(NULL);
	eof_render();

	return 1;
}

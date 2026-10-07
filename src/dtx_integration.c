#include <allegro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "song.h"
#include "note.h"
#include "gp_import.h"
#include "undo.h"
#include "menu/file.h"
#include "menu/song.h"
#include "menu/track.h"
#include "dtx_integration.h"

/* The original GP drum importer is still kept intact for compatibility with
 * PART DRUMS/PART REAL_DRUMS_PS.  During the dedicated DTX import path we
 * temporarily hide the parsed notes from that legacy importer.  Its final
 * eof_fixup_notes() call is redirected here, where the parsed note count is
 * restored before normal project cleanup runs. */
static EOF_PRO_GUITAR_TRACK *eof_dtx_suppressed_gp_track = NULL;
static unsigned long eof_dtx_suppressed_gp_notes = 0;
static int eof_dtx_gp_import_pending = 0;
static unsigned char eof_dtx_gp_target_diff = EOF_NOTE_SPECIAL;

/* This is intentionally per-import state rather than a saved preference.
 * Both GP import front-ends set it immediately before importing percussion. */
int eof_gp_detect_crash_as_ride = 0;

typedef struct
{
	unsigned long pos;
	unsigned char as_ride;
} EOF_DTX_GP_CRASH_POINT;

int eof_dtx_integration_channel_from_midi(unsigned midi_note)
{
	switch(midi_note)
	{
		case 31:
		case 35:
		case 36:
			return EOF_DTX_INT_KICK;

		case 37:
		case 38:
		case 39:
		case 40:
		case 62:
			return EOF_DTX_INT_SNARE;

		case 42:
		case 44:
			return EOF_DTX_INT_HH;

		case 46:
			return EOF_DTX_INT_HH_OPEN;

		case 49:
		case 52:
		case 55:
		case 57:
			return EOF_DTX_INT_CRASH;

		case 51:
		case 53:
		case 59:
			return EOF_DTX_INT_RIDE;

		case 48:
		case 50:
			return EOF_DTX_INT_TOM1;

		case 45:
		case 47:
			return EOF_DTX_INT_TOM2;

		case 41:
		case 43:
			return EOF_DTX_INT_TOM3;
	}

	return -1;
}

void eof_dtx_warn_crash_as_ride_detection(void)
{
	allegro_message(
		"Warning: \"Detect Crash Cymbals Used as Ride\" uses rhythmic/context heuristics and may produce false positives.\n\n"
		"A real crash cymbal can be classified as ride. Review the imported drum chart, especially section starts, fills and transitions."
	);
}

static int eof_dtx_gp_note_has_midi(const EOF_PRO_GUITAR_NOTE *np, unsigned midi_note)
{
	unsigned stringnum;
	unsigned long mask;

	if(!np)
		return 0;

	for(stringnum = 0, mask = 1; stringnum < 6; stringnum++, mask <<= 1)
	{
		if((np->note & mask) && ((np->frets[stringnum] & 0x7F) == midi_note))
			return 1;
	}
	return 0;
}

static double eof_dtx_gp_beat_length_at(unsigned long pos)
{
	unsigned long low, high, mid, beat = 0;
	double length = 500.0;

	if(!eof_song || !eof_song->beat || (eof_song->beats < 2))
		return length;

	/* Find the last beat at or before this note. */
	low = 0;
	high = eof_song->beats;
	while(low < high)
	{
		mid = low + ((high - low) / 2);
		if(eof_song->beat[mid]->pos <= pos)
		{
			beat = mid;
			low = mid + 1;
		}
		else
		{
			high = mid;
		}
	}

	if((beat + 1 < eof_song->beats) && (eof_song->beat[beat + 1]->pos > eof_song->beat[beat]->pos))
		length = (double)(eof_song->beat[beat + 1]->pos - eof_song->beat[beat]->pos);
	else if(beat && (eof_song->beat[beat]->pos > eof_song->beat[beat - 1]->pos))
		length = (double)(eof_song->beat[beat]->pos - eof_song->beat[beat - 1]->pos);

	if(length < 1.0)
		length = 500.0;
	return length;
}

static double eof_dtx_gp_gap_beats(unsigned long first, unsigned long second)
{
	double beat1, beat2, average;

	if(second <= first)
		return 0.0;

	beat1 = eof_dtx_gp_beat_length_at(first);
	beat2 = eof_dtx_gp_beat_length_at(second);
	average = (beat1 + beat2) * 0.5;
	if(average < 1.0)
		average = 500.0;
	return (double)(second - first) / average;
}

static int eof_dtx_gp_gap_is_ride_like(double beats)
{
	static const double targets[] = {0.25, 0.3333333333, 0.5, 0.6666666667, 0.75, 1.0};
	unsigned i;

	if((beats < 0.18) || (beats > 1.12))
		return 0;

	for(i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
	{
		double delta = beats - targets[i];
		double tolerance = targets[i] * 0.18 + 0.015;
		if(delta < 0.0)
			delta = -delta;
		if(delta <= tolerance)
			return 1;
	}
	return 0;
}

static int eof_dtx_gp_gaps_are_consistent(double first, double second)
{
	double delta = first - second;
	double largest = (first > second) ? first : second;

	if(delta < 0.0)
		delta = -delta;
	return (delta <= (largest * 0.22 + 0.02));
}

static int eof_dtx_gp_has_explicit_ride_in_range(EOF_PRO_GUITAR_TRACK *source, unsigned long start, unsigned long end)
{
	unsigned long ctr;

	if(!source)
		return 0;
	for(ctr = 0; ctr < source->notes; ctr++)
	{
		EOF_PRO_GUITAR_NOTE *np = source->note[ctr];
		if(!np)
			continue;
		if(np->pos < start)
			continue;
		if(np->pos > end)
			break;
		if(eof_dtx_gp_note_has_midi(np, 51) || eof_dtx_gp_note_has_midi(np, 53) || eof_dtx_gp_note_has_midi(np, 59))
			return 1;
	}
	return 0;
}

static EOF_DTX_GP_CRASH_POINT *eof_dtx_gp_classify_crash_as_ride(EOF_PRO_GUITAR_TRACK *source, unsigned long *point_count)
{
	EOF_DTX_GP_CRASH_POINT *points;
	unsigned long ctr, count = 0, start;

	if(point_count)
		*point_count = 0;
	if(!source || !source->notes)
		return NULL;

	/* At most one classifier point is needed for each GP note timestamp. */
	points = (EOF_DTX_GP_CRASH_POINT *)calloc(source->notes, sizeof(*points));
	if(!points)
	{
		eof_log("GP crash/ride detector: not enough memory; detection skipped", 1);
		return NULL;
	}

	for(ctr = 0; ctr < source->notes; ctr++)
	{
		EOF_PRO_GUITAR_NOTE *np = source->note[ctr];
		if(!np || !eof_dtx_gp_note_has_midi(np, 49))
			continue;
		if(!count || (points[count - 1].pos != np->pos))
			points[count++].pos = np->pos;
	}

	/* Split MIDI 49 attacks into contiguous rhythmic runs.  A run boundary is
	 * any interval that is implausible for a time-keeping cymbal. */
	for(start = 0; start < count; )
	{
		unsigned long end = start;
		unsigned long run_length, idx;

		while((end + 1 < count) &&
		      eof_dtx_gp_gap_is_ride_like(eof_dtx_gp_gap_beats(points[end].pos, points[end + 1].pos)))
			end++;

		run_length = end - start + 1;
		if((run_length >= 4) &&
		   !eof_dtx_gp_has_explicit_ride_in_range(source, points[start].pos, points[end].pos))
		{
			/* Keep both edges as crash.  Section starts/ends and post-fill
			 * accents are exactly where false positives are most damaging.
			 * Only strongly regular interior attacks can become ride. */
			for(idx = start + 1; idx < end; idx++)
			{
				double left_gap = eof_dtx_gp_gap_beats(points[idx - 1].pos, points[idx].pos);
				double right_gap = eof_dtx_gp_gap_beats(points[idx].pos, points[idx + 1].pos);
				double average = (left_gap + right_gap) * 0.5;
				unsigned long minimum_run;
				int supported = 0;

				if(!eof_dtx_gp_gaps_are_consistent(left_gap, right_gap))
					continue;

				/* Dense eighth/sixteenth/triplet time-keeping becomes
				 * convincing quickly.  Quarter-note repetition is much more
				 * ambiguous, so require a substantially longer run. */
				if(average <= 0.56)
					minimum_run = 4;
				else if(average <= 0.80)
					minimum_run = 5;
				else
					minimum_run = 8;
				if(run_length < minimum_run)
					continue;

				if(idx >= start + 2)
				{
					double outer = eof_dtx_gp_gap_beats(points[idx - 2].pos, points[idx - 1].pos);
					if(eof_dtx_gp_gaps_are_consistent(outer, left_gap))
						supported = 1;
				}
				if(!supported && (idx + 2 <= end))
				{
					double outer = eof_dtx_gp_gap_beats(points[idx + 1].pos, points[idx + 2].pos);
					if(eof_dtx_gp_gaps_are_consistent(outer, right_gap))
						supported = 1;
				}
				if(supported)
					points[idx].as_ride = 1;
			}
		}
		start = end + 1;
	}

	if(point_count)
		*point_count = count;
	return points;
}

static int eof_dtx_gp_crash_position_is_ride(const EOF_DTX_GP_CRASH_POINT *points, unsigned long count, unsigned long pos)
{
	unsigned long low = 0, high = count;

	while(low < high)
	{
		unsigned long mid = low + ((high - low) / 2);
		if(points[mid].pos < pos)
			low = mid + 1;
		else
			high = mid;
	}
	return ((low < count) && (points[low].pos == pos) && points[low].as_ride);
}

static int eof_dtx_channel_lane(int channel)
{
	/* Match EOF's familiar drum layout: kick and snare each have their own
	 * row, while each cymbal shares a row with its corresponding tom.  The
	 * DTX color and shape still make the two instruments unambiguous. */
	switch(channel)
	{
		case EOF_DTX_INT_KICK:
			return 0;
		case EOF_DTX_INT_SNARE:
			return 1;
		case EOF_DTX_INT_HH:
		case EOF_DTX_INT_HH_OPEN:
		case EOF_DTX_INT_TOM1:
			return 2;
		case EOF_DTX_INT_RIDE:
		case EOF_DTX_INT_TOM2:
			return 3;
		case EOF_DTX_INT_CRASH:
		case EOF_DTX_INT_TOM3:
			return 4;
	}

	/* Lane six is reserved for GP percussion notes that have no DTXMania
	 * equivalent. */
	return 5;
}

static int eof_dtx_channel_is_cymbal(int channel)
{
	return ((channel == EOF_DTX_INT_HH) ||
		(channel == EOF_DTX_INT_HH_OPEN) ||
		(channel == EOF_DTX_INT_RIDE) ||
		(channel == EOF_DTX_INT_CRASH));
}

static int eof_dtx_channel_color(int channel)
{
	switch(channel)
	{
		case EOF_DTX_INT_KICK:
			return eof_color_white;
		case EOF_DTX_INT_SNARE:
			return makecol(218, 165, 32);       /* gold */
		case EOF_DTX_INT_HH:
		case EOF_DTX_INT_HH_OPEN:
			return eof_color_blue;
		case EOF_DTX_INT_RIDE:
			return eof_color_lighter_blue;
		case EOF_DTX_INT_TOM1:
			return eof_color_green;
		case EOF_DTX_INT_TOM2:
			return eof_color_red;
		case EOF_DTX_INT_TOM3:
			return eof_color_purple;
		case EOF_DTX_INT_CRASH:
			return eof_color_gray;
	}

	/* EOF's sixth-lane/orange convention gives unknown percussion a color
	 * that does not collide with any of the DTX colors above. */
	return eof_color_orange;
}

static int eof_dtx_channel_dot_color(int channel)
{
	switch(channel)
	{
		case EOF_DTX_INT_HH:
		case EOF_DTX_INT_HH_OPEN:
		case EOF_DTX_INT_TOM1:
		case EOF_DTX_INT_TOM2:
		case EOF_DTX_INT_TOM3:
			return eof_color_white;
		default:
			return eof_color_black;
	}
}

static const char *eof_dtx_difficulty_short_name(unsigned diff)
{
	static const char *names[5] = {"BSC", "ADV", "EXT", "MAS", "ULT"};
	return (diff < 5) ? names[diff] : "DTX";
}

static int eof_dtx_difficulty_has_notes(EOF_PRO_GUITAR_TRACK *tp, unsigned char diff)
{
	unsigned long ctr;
	if(!tp)
		return 0;
	for(ctr = 0; ctr < tp->pgnotes; ctr++)
	{
		if(tp->pgnote[ctr] && (tp->pgnote[ctr]->type == diff))
			return 1;
	}
	return 0;
}

static void eof_dtx_erase_difficulty(EOF_PRO_GUITAR_TRACK *tp, unsigned char diff)
{
	unsigned long ctr;
	if(!tp)
		return;

	/* DTX never uses tech notes.  Force the regular note array active before
	 * deleting so the pro-guitar carrier cannot accidentally delete from the
	 * tech-note array. */
	eof_menu_track_set_tech_view_state(eof_song, EOF_TRACK_DRUM_DTX, 0);
	for(ctr = tp->pgnotes; ctr > 0; ctr--)
	{
		if(tp->pgnote[ctr - 1] && (tp->pgnote[ctr - 1]->type == diff))
			eof_pro_guitar_track_delete_note(tp, ctr - 1);
	}
}

static void eof_dtx_warn_unsupported(const unsigned char unsupported[128])
{
	char list[512] = {0};
	char temp[16];
	unsigned i, count = 0;
	size_t used = 0;

	if(!unsupported)
		return;

	for(i = 0; i < 128; i++)
	{
		if(!unsupported[i])
			continue;
		count++;
		if(used + 8 >= sizeof(list))
			continue;
		(void) snprintf(temp, sizeof(temp), "%s%u", used ? ", " : "", i);
		strncat(list, temp, sizeof(list) - strlen(list) - 1);
		used = strlen(list);
	}

	if(count)
	{
		allegro_message(
			"The imported Guitar Pro drum track contains percussion pieces that do not have a DTXMania mapping.\n\n"
			"MIDI note(s): %s\n\n"
			"They were kept in PART_REAL_DRUM_DTX and highlighted in yellow/orange so you can review them.\n"
			"They will not be written as normal DTX drum chips until a mapping is defined.",
			list[0] ? list : "(multiple)"
		);
	}
}

int eof_dtx_import_selected_gp_drum_track(void)
{
	unsigned long selected, ctr, stringnum, tracknum, copied = 0;
	unsigned long crash_point_count = 0, crash_converted = 0;
	unsigned char unsupported[128] = {0};
	unsigned char target_diff = EOF_NOTE_SPECIAL;
	EOF_PRO_GUITAR_TRACK *source, *dest;
	EOF_DTX_GP_CRASH_POINT *crash_points = NULL;

	if(!eof_song || !eof_parsed_gp_file || !eof_parsed_gp_file->track)
		return 0;

	selected = (unsigned long)eof_gp_import_dialog[1].d1;
	if(selected >= eof_parsed_gp_file->numtracks)
		return 0;
	if(!eof_parsed_gp_file->track[selected])
		return 0;

	if((EOF_TRACK_DRUM_DTX >= eof_song->tracks) || !eof_song->track[EOF_TRACK_DRUM_DTX])
	{
		allegro_message("PART_REAL_DRUM_DTX is not available in this project.");
		return 0;
	}
	if(eof_song->track[EOF_TRACK_DRUM_DTX]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT)
	{
		allegro_message("PART_REAL_DRUM_DTX has an unexpected internal format.");
		return 0;
	}

	tracknum = eof_song->track[EOF_TRACK_DRUM_DTX]->tracknum;
	if((tracknum >= eof_song->pro_guitar_tracks) || !eof_song->pro_guitar_track[tracknum])
		return 0;

	source = eof_parsed_gp_file->track[selected];
	dest = eof_song->pro_guitar_track[tracknum];

	/* When the DTX track is active, the selected BSC/ADV/EXT/MAS/ULT tab is the
	 * import destination.  Importing drums from another EOF track defaults to
	 * ULT, preserving the long-standing "highest GP drum chart" behavior. */
	if((eof_selected_track == EOF_TRACK_DRUM_DTX) && (eof_note_type < 5))
		target_diff = eof_note_type;
	eof_dtx_gp_target_diff = target_diff;

	if(eof_dtx_difficulty_has_notes(dest, target_diff))
	{
		char line1[160], line2[192];
		(void) snprintf(line1, sizeof(line1), "PART_REAL_DRUM_DTX %s already contains notes.", eof_dtx_difficulty_short_name(target_diff));
		(void) snprintf(line2, sizeof(line2), "Overwrite only the %s difficulty with this Guitar Pro drum track?", eof_dtx_difficulty_short_name(target_diff));
		if(alert(line1, line2, "The other DTX difficulties will be kept unchanged.", "&Yes", "&No", 'y', 'n') != 1)
			return 0;
	}

	if(!gp_import_undo_made)
	{
		eof_prepare_undo(EOF_UNDO_TYPE_NONE);
		gp_import_undo_made = 1;
	}

	/* Replace only the selected DTX difficulty.  This allows a hand-authored GP
	 * drum chart to replace (for example) ADV while BSC/EXT/MAS/ULT remain as
	 * generated or independently customized charts. */
	eof_dtx_erase_difficulty(dest, target_diff);
	dest->numstrings = 6;
	dest->numfrets = 127;
	dest->capo = 0;
	dest->ignore_tuning = 1;

	eof_pro_guitar_track_sort_notes(source);
	if(eof_gp_detect_crash_as_ride)
		crash_points = eof_dtx_gp_classify_crash_as_ride(source, &crash_point_count);
	for(ctr = 0; ctr < source->notes; ctr++)
	{
		EOF_PRO_GUITAR_NOTE *src = source->note[ctr];
		EOF_PRO_GUITAR_NOTE *np;
		int has_unsupported = 0;

		if(!src)
			continue;

		np = eof_pro_guitar_track_add_note(dest);
		if(!np)
		{
			allegro_message("Error allocating memory while importing PART_REAL_DRUM_DTX.");
			break;
		}
		memcpy(np, src, sizeof(EOF_PRO_GUITAR_NOTE));
		np->type = target_diff;
		np->length = 1;

		/* Guitar-specific techniques are not meaningful for DTX percussion.
		 * Keep the raw note mask/frets and only add static highlighting when an
		 * unsupported percussion MIDI number is encountered. */
		np->flags = 0;
		np->eflags = 0;
		np->ghost = 0;

		for(stringnum = 0; stringnum < 6; stringnum++)
		{
			unsigned long mask = 1UL << stringnum;
			unsigned midi_note;

			if(!(np->note & mask))
				continue;
			midi_note = np->frets[stringnum] & 0x7F;
			if(eof_gp_detect_crash_as_ride && crash_points && (midi_note == 49) &&
			   eof_dtx_gp_crash_position_is_ride(crash_points, crash_point_count, np->pos))
			{
				/* Preserve the fret byte's high status bit, changing only the
				 * seven-bit MIDI percussion number from Crash 1 (49) to Ride
				 * 1 (51). */
				np->frets[stringnum] = (np->frets[stringnum] & 0x80) | 51;
				midi_note = 51;
				crash_converted++;
			}
			if(eof_dtx_integration_channel_from_midi(midi_note) < 0)
			{
				has_unsupported = 1;
				if(midi_note < 128)
					unsupported[midi_note] = 1;
			}
		}
		if(has_unsupported)
			np->flags |= EOF_NOTE_FLAG_HIGHLIGHT;
		copied++;
	}

	if(crash_points)
		free(crash_points);
	if(eof_gp_detect_crash_as_ride)
	{
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"GP crash/ride detector: %lu MIDI 49 position(s) analyzed, %lu hit(s) converted to MIDI 51",
			crash_point_count, crash_converted);
		eof_log(eof_log_string, 1);
	}

	eof_pro_guitar_track_sort_notes(dest);
	eof_note_type = target_diff;
	eof_note_type_i = target_diff;
	(void) eof_detect_difficulties(eof_song, EOF_TRACK_DRUM_DTX);
	eof_changes = 1;
	eof_dtx_gp_import_pending = copied ? 1 : 0;

	if(!copied)
		allegro_message("The selected Guitar Pro drum track did not contain any importable notes.");
	else
		eof_dtx_warn_unsupported(unsupported);

	return copied ? 1 : 0;
}

int eof_dtx_alert(const char *s1, const char *s2, const char *s3,
	const char *b1, const char *b2, int c1, int c2)
{
	/* A GP track already identified as percussion now has one canonical
	 * destination: PART_REAL_DRUM_DTX.  Skip the old confirmation. */
	if(s2 && !strcmp(s2, "Import the selected track as a drum track?"))
		return 1;

	/* The legacy GP post-processing examines the track that was active before
	 * the DTX import.  No tremolo phrases were imported into that track. */
	if(eof_dtx_gp_import_pending && s2 && !strcmp(s2, "Remove the track difficulty limit to show imported tremolo phrases?"))
		return 2;

	return alert(s1, s2, s3, b1, b2, c1, c2);
}

int eof_dtx_alert3(const char *s1, const char *s2, const char *s3,
	const char *b1, const char *b2, const char *b3, int c1, int c2, int c3)
{
	if(s2 && !strcmp(s2, "Import into which drum track(s)?"))
	{
		unsigned long selected;

		(void) eof_dtx_import_selected_gp_drum_track();

		/* The old importer is called immediately after alert3() returns.  Hide
		 * its parsed notes until its final fixup call so it becomes a harmless
		 * no-op instead of duplicating the import into PART DRUMS/PS. */
		if(eof_parsed_gp_file && eof_parsed_gp_file->track)
		{
			selected = (unsigned long)eof_gp_import_dialog[1].d1;
			if((selected < eof_parsed_gp_file->numtracks) && eof_parsed_gp_file->track[selected])
			{
				eof_dtx_suppressed_gp_track = eof_parsed_gp_file->track[selected];
				eof_dtx_suppressed_gp_notes = eof_dtx_suppressed_gp_track->notes;
				eof_dtx_suppressed_gp_track->notes = 0;
			}
		}

		/* Zero means none of the legacy Normal/Phase Shift destinations. */
		return 0;
	}

	return alert3(s1, s2, s3, b1, b2, b3, c1, c2, c3);
}

void eof_dtx_fixup_notes_hook(EOF_SONG *sp)
{
	if(eof_dtx_suppressed_gp_track)
	{
		eof_dtx_suppressed_gp_track->notes = eof_dtx_suppressed_gp_notes;
		eof_dtx_suppressed_gp_track = NULL;
		eof_dtx_suppressed_gp_notes = 0;
	}

	/* The DTX import already created its own undo snapshot.  Reset the legacy
	 * GP-import bookkeeping so its guitar-specific post-processing does not
	 * run against the unrelated track that happened to be active beforehand. */
	if(eof_dtx_gp_import_pending)
		gp_import_undo_made = 0;

	eof_fixup_notes(sp);
}

void eof_dtx_track_find_crazy_notes_hook(EOF_SONG *sp, unsigned long track, int option)
{
	if(eof_dtx_gp_import_pending)
		return;
	eof_track_find_crazy_notes(sp, track, option);
}

void eof_dtx_track_fixup_notes_hook(EOF_SONG *sp, unsigned long track, int sel)
{
	if(eof_dtx_gp_import_pending)
		return;
	eof_track_fixup_notes(sp, track, sel);
}

unsigned long eof_dtx_note_count_colors_hook(EOF_SONG *sp, unsigned long track, unsigned long note)
{
	if(eof_dtx_gp_import_pending)
		return 0;
	return eof_note_count_colors(sp, track, note);
}

void eof_dtx_log_hook(const char *text, int level)
{
	eof_log(text, level);

	/* This is the last GP-specific cleanup log immediately before
	 * eof_gp_import_common() returns.  Switching tracks here avoids making its
	 * cached pro-guitar pointer refer to a different active track mid-cleanup. */
	if(eof_dtx_gp_import_pending && text && strstr(text, "guitar/bass notes exist after cleanup"))
	{
		eof_dtx_gp_import_pending = 0;
		eof_note_type = eof_dtx_gp_target_diff;
		eof_note_type_i = eof_dtx_gp_target_diff;
		(void) eof_menu_track_selected_track_number(EOF_TRACK_DRUM_DTX, 1);
		(void) eof_detect_difficulties(eof_song, EOF_TRACK_DRUM_DTX);
	}
}

static void eof_dtx_draw_triangle(BITMAP *bmp, int x, int y, int radius, int color, int border)
{
#ifdef ALLEGRO_MACOSX
	int points[6];
	points[0] = x;
	points[1] = y - radius;
	points[2] = x + radius;
	points[3] = y + radius;
	points[4] = x - radius;
	points[5] = y + radius;
	polygon(bmp, 3, points, color);
#else
	triangle(bmp, x, y - radius, x + radius, y + radius, x - radius, y + radius, color);
#endif
	if(border >= 0)
	{
		line(bmp, x, y - radius, x + radius, y + radius, border);
		line(bmp, x + radius, y + radius, x - radius, y + radius, border);
		line(bmp, x - radius, y + radius, x, y - radius, border);
	}
}

int eof_dtx_note_draw(unsigned long track, unsigned long notenum, int p, EOF_WINDOW *window)
{
	unsigned long tracknum, stringnum;
	EOF_PRO_GUITAR_TRACK *tp;
	EOF_PRO_GUITAR_NOTE *np;
	long position, leftcoord, rollpos, npos;
	int radius, dotsize;

	if(!eof_song || !window || (track != EOF_TRACK_DRUM_DTX))
		return eof_note_draw(track, notenum, p, window);
	if((track >= eof_song->tracks) || !eof_song->track[track] ||
	   (eof_song->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT))
		return eof_note_draw(track, notenum, p, window);

	tracknum = eof_song->track[track]->tracknum;
	if((tracknum >= eof_song->pro_guitar_tracks) || !eof_song->pro_guitar_track[tracknum])
		return eof_note_draw(track, notenum, p, window);
	tp = eof_song->pro_guitar_track[tracknum];
	if((notenum >= tp->notes) || !tp->note[notenum])
		return 0;
	np = tp->note[notenum];

	if(window == eof_window_info)
	{
		position = eof_music_catalog_pos;
		leftcoord = 140;
	}
	else
	{
		position = eof_music_pos.value;
		leftcoord = 300;
	}
	rollpos = position / eof_zoom;
	if(rollpos < leftcoord)
		npos = 20 + np->pos / eof_zoom;
	else
		npos = 20 - (rollpos - leftcoord) + np->pos / eof_zoom;

	radius = eof_screen_layout.note_size;
	dotsize = eof_screen_layout.note_dot_size;
	if(npos - radius > window->screen->w)
		return 1;
	if(npos + radius < 0)
		return -1;

	eof_set_2D_lane_positions(track);
	for(stringnum = 0; stringnum < 6; stringnum++)
	{
		unsigned long mask = 1UL << stringnum;
		unsigned midi_note;
		int channel, lane, y, color, dotcolor, border, cymbal;

		if(!(np->note & mask))
			continue;

		midi_note = np->frets[stringnum] & 0x7F;
		channel = eof_dtx_integration_channel_from_midi(midi_note);
		lane = eof_dtx_channel_lane(channel);
		color = eof_dtx_channel_color(channel);
		dotcolor = eof_dtx_channel_dot_color(channel);
		cymbal = eof_dtx_channel_is_cymbal(channel);
		y = EOF_EDITOR_RENDER_OFFSET + 15 + ychart[lane];

		/* A tom and cymbal can legally occur at the same timestamp on the
		 * shared EOF-style row.  Nudge them apart slightly so both symbols
		 * remain visible instead of perfectly covering each other. */
		if((lane >= 2) && (lane <= 4))
		{
			if(cymbal)
				y -= radius / 3;
			else
				y += radius / 3;
		}

		if(np->flags & EOF_NOTE_FLAG_HIGHLIGHT)
			border = eof_color_yellow;
		else if(p)
			border = eof_color_white;
		else
			border = eof_color_black;

		if(cymbal)
		{
			eof_dtx_draw_triangle(window->screen, (int)npos, y, radius, color, border);
			circlefill(window->screen, (int)npos, y + dotsize / 2, (dotsize > 1) ? dotsize - 1 : 1, dotcolor);
		}
		else
		{
			circlefill(window->screen, (int)npos, y, radius, color);
			circlefill(window->screen, (int)npos, y, dotsize, dotcolor);
			circle(window->screen, (int)npos, y, radius, border);
		}

		if(channel < 0)
		{
			char number[8];
			(void) snprintf(number, sizeof(number), "%u", midi_note);
			textout_centre_ex(window->screen, eof_mono_font, number, (int)npos, y - text_height(eof_mono_font) / 2, eof_color_black, -1);
		}
	}

	return 0;
}

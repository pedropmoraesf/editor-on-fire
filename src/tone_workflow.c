#include <allegro.h>
#include <limits.h>
#include <string.h>
#include "main.h"
#include "dialog.h"
#include "song.h"
#include "undo.h"
#include "menu/track.h"
#include "tone_analysis.h"
#include "tone_analysis_hq.h"
#include "tone_workflow.h"

#define EOF_TONE_WORKFLOW_MENU_MAX 16

static MENU eof_tone_workflow_menu[EOF_TONE_WORKFLOW_MENU_MAX];
static int eof_tone_workflow_installed = 0;

static unsigned long eof_tone_intro_pos(EOF_PRO_GUITAR_TRACK *tp)
{
	unsigned long pos = 1UL;
	if(tp && tp->pgnotes && tp->note[0])
		pos = tp->note[0]->pos;
	if(!pos)
		pos = 1UL;
	return pos;
}

static unsigned long eof_tone_state_hash(EOF_PRO_GUITAR_TRACK *tp)
{
	unsigned long i, j, hash = 2166136261UL;
	if(!tp)
		return 0UL;
	for(i = 0; i < sizeof(tp->defaulttone) && tp->defaulttone[i]; i++)
	{
		hash ^= (unsigned char)tp->defaulttone[i];
		hash *= 16777619UL;
	}
	hash ^= tp->tonechanges;
	hash *= 16777619UL;
	for(i = 0; i < tp->tonechanges; i++)
	{
		hash ^= tp->tonechange[i].start_pos;
		hash *= 16777619UL;
		hash ^= tp->tonechange[i].end_pos;
		hash *= 16777619UL;
		for(j = 0; j < sizeof(tp->tonechange[i].name) && tp->tonechange[i].name[j]; j++)
		{
			hash ^= (unsigned char)tp->tonechange[i].name[j];
			hash *= 16777619UL;
		}
	}
	return hash;
}

static int eof_tone_track_is_two_layout(EOF_PRO_GUITAR_TRACK *tp)
{
	unsigned long intro;
	if(!tp || tp->tonechanges != 2UL || !tp->defaulttone[0])
		return 0;
	intro = eof_tone_intro_pos(tp);
	if(tp->tonechange[0].start_pos != 0UL || tp->tonechange[0].end_pos != 1UL)
		return 0;
	if(ustricmp(tp->tonechange[0].name, tp->defaulttone))
		return 0;
	if(tp->tonechange[1].start_pos != intro)
		return 0;
	return 1;
}

static void eof_tone_pick_names(EOF_PRO_GUITAR_TRACK *tp, char *base, size_t basesz,
	char *other, size_t othersz)
{
	unsigned long i, j, bestdur = 0;
	if(!tp || !base || !other || !basesz || !othersz)
		return;

	base[0] = other[0] = '\0';
	if(tp->defaulttone[0])
		ustrzcpy(base, (int)basesz, tp->defaulttone);
	else if(tp->tonechanges)
		ustrzcpy(base, (int)basesz, tp->tonechange[0].name);
	else
		ustrzcpy(base, (int)basesz, "Default");

	/* Prefer the non-default tone that was in effect for the greatest total
	 * duration.  This makes the two-tone reduction more stable than simply
	 * keeping whichever transient change the detector happened to emit first. */
	for(i = 0; i < tp->tonechanges; i++)
	{
		unsigned long total = 0;
		if(!tp->tonechange[i].name[0] || !ustricmp(tp->tonechange[i].name, base))
			continue;
		for(j = 0; j < tp->tonechanges; j++)
		{
			unsigned long end;
			if(ustricmp(tp->tonechange[j].name, tp->tonechange[i].name))
				continue;
			end = (j + 1UL < tp->tonechanges) ? tp->tonechange[j + 1UL].start_pos : eof_chart_length;
			if(end > tp->tonechange[j].start_pos)
				total += end - tp->tonechange[j].start_pos;
		}
		if(!other[0] || total > bestdur)
		{
			bestdur = total;
			ustrzcpy(other, (int)othersz, tp->tonechange[i].name);
		}
	}
	if(!other[0])
		ustrzcpy(other, (int)othersz, "Tone 2");
}

int eof_tone_reduce_track_to_two(EOF_SONG *sp, unsigned long track, int make_undo)
{
	EOF_PRO_GUITAR_TRACK *tp;
	unsigned long ptrack, intro;
	char base[EOF_SECTION_NAME_LENGTH + 1] = {0};
	char other[EOF_SECTION_NAME_LENGTH + 1] = {0};

	if(!sp || !track || track >= sp->tracks || !eof_track_is_pro_guitar_track(sp, track))
		return 0;
	ptrack = sp->track[track]->tracknum;
	if(ptrack >= sp->pro_guitar_tracks || !sp->pro_guitar_track[ptrack])
		return 0;
	tp = sp->pro_guitar_track[ptrack];
	if(!tp->pgnotes)
		return 0;

	eof_track_pro_guitar_sort_tone_changes(tp);
	if(eof_tone_track_is_two_layout(tp))
		return 1;
	eof_tone_pick_names(tp, base, sizeof(base), other, sizeof(other));
	intro = eof_tone_intro_pos(tp);

	if(make_undo)
		eof_undo_add(EOF_UNDO_TYPE_NONE);
	while(tp->tonechanges)
		eof_track_pro_guitar_delete_tone_change(tp, tp->tonechanges - 1UL);
	ustrzcpy(tp->defaulttone, sizeof(tp->defaulttone), base);

	/* end_pos is intentionally used as EOF's existing "is default" field for
	 * Rocksmith tone changes (same convention used by tone_analysis.c). */
	if(!eof_track_add_section(sp, track, EOF_RS_TONE_CHANGE, 0, 0UL, 1UL, 0, base))
		return 0;
	if(!eof_track_add_section(sp, track, EOF_RS_TONE_CHANGE, 0, intro, 0UL, 0, other))
		return 0;
	eof_track_pro_guitar_sort_tone_changes(tp);
	eof_project_unsaved = 1;
	return 1;
}

unsigned eof_tone_ensure_two_for_populated_tracks(EOF_SONG *sp, int make_undo)
{
	unsigned long track;
	unsigned populated = 0;
	int undo_made = 0;
	if(!sp)
		return 0;
	for(track = 1; track < sp->tracks; track++)
	{
		unsigned long ptrack;
		EOF_PRO_GUITAR_TRACK *tp;
		if(!eof_track_is_pro_guitar_track(sp, track) || !eof_get_track_size(sp, track))
			continue;
		populated++;
		ptrack = sp->track[track]->tracknum;
		if(ptrack >= sp->pro_guitar_tracks || !sp->pro_guitar_track[ptrack])
			continue;
		tp = sp->pro_guitar_track[ptrack];
		eof_track_pro_guitar_sort_tone_changes(tp);
		if(eof_tone_track_is_two_layout(tp))
			continue;
		if(make_undo && !undo_made)
		{
			eof_undo_add(EOF_UNDO_TYPE_NONE);
			undo_made = 1;
		}
		eof_tone_reduce_track_to_two(sp, track, 0);
	}
	return populated;
}

static int eof_tone_workflow_analyze_two(void)
{
	EOF_PRO_GUITAR_TRACK *tp = NULL;
	unsigned long ptrack = 0, before = 0, after = 0;
	int result;

	if(eof_song && eof_selected_track < eof_song->tracks &&
	   eof_track_is_pro_guitar_track(eof_song, eof_selected_track))
	{
		ptrack = eof_song->track[eof_selected_track]->tracknum;
		if(ptrack < eof_song->pro_guitar_tracks)
			tp = eof_song->pro_guitar_track[ptrack];
	}
	before = eof_tone_state_hash(tp);
	result = eof_tone_analysis_hq_menu();
	after = eof_tone_state_hash(tp);

	/* A cancelled analysis leaves the track untouched.  Only collapse the
	 * detector output when the detector actually changed the tone state. */
	if(tp && before != after && eof_get_track_size(eof_song, eof_selected_track))
		eof_tone_reduce_track_to_two(eof_song, eof_selected_track, 0);
	return result;
}

static int eof_tone_workflow_add_two(void)
{
	if(!eof_song || !eof_song_loaded || !eof_track_is_pro_guitar_track(eof_song, eof_selected_track))
	{
		allegro_message("Select a populated Rocksmith pro guitar or bass track first.");
		return D_O_K;
	}
	if(!eof_get_track_size(eof_song, eof_selected_track))
	{
		allegro_message("This arrangement has no notes.");
		return D_O_K;
	}
	if(eof_tone_reduce_track_to_two(eof_song, eof_selected_track, 1))
		allegro_message("The arrangement now has exactly two tone references:\nDefault plus one change at the first played note.");
	return D_O_K;
}

void eof_tone_workflow_install_menu(void)
{
	unsigned i, count = 0;
	MENU *source = NULL;
	if(eof_tone_workflow_installed)
		return;

	for(i = 0; eof_track_rocksmith_menu[i].text; i++)
	{
		if(eof_track_rocksmith_menu[i].child && strstr(eof_track_rocksmith_menu[i].text, "Tone change"))
		{
			source = eof_track_rocksmith_menu[i].child;
			break;
		}
	}
	if(!source)
		return;

	while(source[count].text && count + 3U < EOF_TONE_WORKFLOW_MENU_MAX)
	{
		eof_tone_workflow_menu[count] = source[count];
		if(strstr(source[count].text, "experimental"))
			eof_tone_workflow_menu[count].proc = eof_tone_workflow_analyze_two;
		count++;
	}
	eof_tone_workflow_menu[count].text = "Add only &two tones";
	eof_tone_workflow_menu[count].proc = eof_tone_workflow_add_two;
	eof_tone_workflow_menu[count].child = NULL;
	eof_tone_workflow_menu[count].flags = 0;
	eof_tone_workflow_menu[count].dp = NULL;
	count++;
	memset(&eof_tone_workflow_menu[count], 0, sizeof(MENU));
	eof_track_rocksmith_menu[i].child = eof_tone_workflow_menu;
	eof_tone_workflow_installed = 1;
}

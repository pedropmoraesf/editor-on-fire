#ifndef EOF_DTX_EXIT_HOOK_H
#define EOF_DTX_EXIT_HOOK_H

/* Force-included only for main.c.  The declarations are loaded before the
 * macros are defined so only main.c call sites are redirected.  DTX keeps the
 * ordinary EOF 3D window/highway and uses EOF's stock 3D note renderer through
 * the bridge in dtx_3d.c. */
#include "window.h"
#include "main.h"
#include "menu/file.h"
#include "menu/song.h"
#include "note.h"
#include "config.h"
#include "dtx_workflow.h"
#include "dtx_3d.h"
#include "gp_advanced.h"
#include "tone_analysis.h"
#include "tone_workflow.h"
#include "psarc_export.h"
#include "audio_normalize.h"

#define eof_menu_file_exit eof_dtx_menu_file_exit

/* During this stabilization phase, force exhaustive logging after eof.cfg is
 * read.  Install custom menus at the same deterministic point, after EOF's
 * static menu tables have been initialized and before the main UI loop. */
static void eof_dtx_main_load_config(char *fn)
{
	eof_dtx_load_config(fn);
	eof_audio_normalize_install();
	eof_gp_advanced_install_menu();
	eof_tone_analysis_install_menu();
	eof_tone_workflow_install_menu();
	eof_psarc_export_install_menu();
}
#define eof_load_config(fn) eof_dtx_main_load_config((fn))

/* The stock eof_sort_notes() has no per-track log and old projects acquire a
 * newly-created, empty DTX carrier during load.  Route main.c's initialization
 * sort through a defensive wrapper that validates every track and skips an
 * empty DTX carrier. */
#define eof_sort_notes(sp) eof_dtx_safe_sort_notes((sp))

/* eof_selected_track is loaded from eof.cfg, not from notes.eof.  A previous
 * DTX editing session can therefore leave track 15 selected.  When an older
 * project is opened, EOF adds PART_REAL_DRUM_DTX during loading; that makes the
 * formerly out-of-range value 15 suddenly valid and eof_init_after_load()
 * enters a brand-new empty pro-guitar-backed carrier.  Do not let main.c's
 * automatic track restoration select that empty carrier.  This wrapper is
 * deliberately limited to main.c, so choosing an empty DTX track manually from
 * Song > Track still works normally. */
static int eof_dtx_main_track_select(unsigned long tracknum, int updatetitle)
{
	if(eof_song && (tracknum == EOF_TRACK_DRUM_DTX) &&
	   (tracknum < eof_song->tracks) && eof_song->track[tracknum] &&
	   (eof_song->track[tracknum]->track_format == EOF_PRO_GUITAR_TRACK_FORMAT))
	{
		unsigned long backing = eof_song->track[tracknum]->tracknum;
		EOF_PRO_GUITAR_TRACK *tp = NULL;

		if((backing < eof_song->pro_guitar_tracks) && eof_song->pro_guitar_track[backing])
			tp = eof_song->pro_guitar_track[backing];

		if(!tp || (!tp->pgnotes && !tp->technotes))
		{
			eof_log("DTX load: refusing automatic selection of empty PART_REAL_DRUM_DTX; using PART GUITAR", 0);
			tracknum = EOF_TRACK_GUITAR;
		}
	}

	return eof_menu_track_selected_track_number(tracknum, updatetitle);
}

#define eof_menu_track_selected_track_number(tracknum, updatetitle) \
	eof_dtx_main_track_select((tracknum), (updatetitle))

#define eof_note_draw_3d(track, notenum, p) \
	eof_dtx_note_draw_3d((track), (notenum), (p))
#define eof_note_tail_draw_3d(track, notenum, p) \
	eof_dtx_note_tail_draw_3d((track), (notenum), (p))

#endif

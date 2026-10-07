#ifndef EOF_DTX_RENDER_HOOK_H
#define EOF_DTX_RENDER_HOOK_H

#include <stdio.h>

/* Force-included only for editor.c.  DTX note drawing remains customized, but
 * track/difficulty state is no longer synchronized from the renderer. */
#include "window.h"
#include "main.h"
#include "note.h"
#include "editor.h"
#include "menu/song.h"
#include "menu/track.h"
#include "dtx_integration.h"
#include "dtx_workflow.h"
#include "dtx_import.h"

#define eof_note_draw(track, notenum, p, window) \
	eof_dtx_note_draw((track), (notenum), (p), (window))

/* PART_REAL_DRUM_DTX intentionally uses EOF_PRO_GUITAR_TRACK as a storage
 * carrier, but it is not a pro-guitar editor track.  This distinction is
 * especially important when the carrier is empty: EOF represents both the
 * regular and tech note arrays with NULL pointers, so stock editor.c sees
 * (tp->note == tp->technote) and incorrectly concludes that Tech View is
 * active.  That sends an empty DTX carrier through pro-guitar-only rendering
 * and editing paths.
 *
 * Keep the underlying storage format unchanged, but make editor.c's semantic
 * pro-guitar checks return false for DTX.  Calls in all other translation
 * units still use EOF's original helper unchanged. */
static int eof_dtx_editor_is_pro_guitar_track(EOF_SONG *sp, unsigned long track)
{
	if(sp && (track == EOF_TRACK_DRUM_DTX))
		return 0;
	return eof_track_is_pro_guitar_track(sp, track);
}

#define eof_track_is_pro_guitar_track(sp, track) \
	eof_dtx_editor_is_pro_guitar_track((sp), (track))

/* Difficulty tabs are handled in editor.c, not menu/song.c.  Route those
 * changes through the same DTX workflow so imported per-difficulty audio and
 * BPM-linked pattern synchronization happen immediately on tab selection. */
#define eof_detect_difficulties(sp, track) \
	eof_dtx_detect_difficulties_hook((sp), (track))

/* editor.c owns both the Home-key path and the |< transport button.  The older
 * DTX wrapper changed only eof_selected_track while seeking.  That left the
 * rest of EOF's active-track state (difficulty, cached track state, labels and
 * fretboard setup) describing DTX while the selected-track number described
 * PART GUITAR.  Use EOF's normal track-selection routine in both directions so
 * the temporary surrogate is internally consistent for the entire seek. */
static int eof_dtx_editor_seek_start(void)
{
	unsigned long saved_track, surrogate = 0, ctr;
	int result;

	if(!eof_song || (eof_selected_track != EOF_TRACK_DRUM_DTX))
		return eof_menu_song_seek_start();

	eof_log("DTX transport: editor seek-to-start entered", 0);
	saved_track = eof_selected_track;

	if((EOF_TRACK_GUITAR < eof_song->tracks) && eof_song->track[EOF_TRACK_GUITAR])
		surrogate = EOF_TRACK_GUITAR;
	else
	{
		for(ctr = 1; ctr < eof_song->tracks; ctr++)
		{
			if((ctr != EOF_TRACK_DRUM_DTX) && eof_song->track[ctr])
			{
				surrogate = ctr;
				break;
			}
		}
	}

	if(!surrogate)
	{
		eof_log("DTX transport: no surrogate track available for editor seek", 0);
		return eof_menu_song_seek_start();
	}

	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"DTX transport: fully selecting surrogate track %lu (\"%s\")",
		surrogate, eof_song->track[surrogate]->name);
	eof_log(eof_log_string, 0);

	(void)eof_menu_track_selected_track_number(surrogate, 0);
	eof_log("DTX transport: surrogate selected; calling stock seek-to-start", 0);
	result = eof_menu_song_seek_start();
	eof_log("DTX transport: stock seek-to-start returned; restoring DTX track", 0);
	(void)eof_menu_track_selected_track_number(saved_track, 0);
	eof_window_title_dirty = 1;
	eof_log("DTX transport: editor seek-to-start completed", 0);

	return result;
}

/* menu/song.h is intentionally included above before this function-like macro
 * is defined, so the original function declaration is parsed normally. */
#define eof_menu_song_seek_start() eof_dtx_editor_seek_start()

#endif

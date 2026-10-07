#ifndef EOF_DTX_GP_IMPORT_HOOK_H
#define EOF_DTX_GP_IMPORT_HOOK_H

/* This header is force-included only for menu/file.c by makefile.common.
 * Include the real declarations before defining the macros, so Allegro and
 * EOF headers remain untouched and only calls in menu/file.c are redirected. */
#include <allegro.h>
#include "main.h"
#include "song.h"
#include "note.h"
#include "dialog.h"
#include "utility.h"
/* menu/file.c includes this header later.  Load it NOW, before the
 * strcasestr_spec() call-site macro below is defined, so its real function
 * declaration cannot be rewritten by the preprocessor.  Its include guard
 * makes menu/file.c's later include a no-op. */
#include "foflc/Lyric_storage.h"
#include "dtx_integration.h"
#include "dtx_xml.h"
#include "dtx_import.h"
#include "gp_advanced.h"
#include "project_autosave.h"

#define alert(s1, s2, s3, b1, b2, c1, c2) \
	eof_dtx_alert((s1), (s2), (s3), (b1), (b2), (c1), (c2))
#define alert3(s1, s2, s3, b1, b2, b3, c1, c2, c3) \
	eof_dtx_alert3((s1), (s2), (s3), (b1), (b2), (b3), (c1), (c2), (c3))
#define eof_fixup_notes(sp) eof_dtx_fixup_notes_hook((sp))
#define eof_track_find_crazy_notes(sp, track, option) \
	eof_dtx_track_find_crazy_notes_hook((sp), (track), (option))
#define eof_track_fixup_notes(sp, track, sel) \
	eof_dtx_track_fixup_notes_hook((sp), (track), (sel))
#define eof_note_count_colors(sp, track, note) \
	eof_dtx_note_count_colors_hook((sp), (track), (note))
#define eof_log(text, level) eof_dtx_log_hook((text), (level))
#define eof_save_song(sp, fn) eof_dtx_save_song_hook((sp), (fn))

/* The stock Songsterr URL parser in menu/file.c searches for "-tab-s" and then
 * starts digit parsing at the returned pointer.  Advance that one token past
 * its prefix so URLs such as ...-tab-s6858126/r8952941?... are interpreted as
 * song=6858126, revision=8952941.  The real strcasestr_spec() declaration was
 * deliberately parsed above before this narrow call-site redirection. */
#define strcasestr_spec(haystack, needle) \
	eof_gp_advanced_songsterr_strcasestr((haystack), (needle))

/* The advanced GP dialog can preselect/download a JSON or SVL file, then call
 * EOF's original Songsterr/Sonic Visualiser menu function.  These wrappers are
 * transparent for every ordinary File-menu operation and only bypass the
 * redundant popup/file-browser while that advanced timing bridge is active. */
#define eof_popup_dialog(dialog, focus) \
	eof_gp_advanced_file_popup_dialog((dialog), (focus))
#define ncd_file_select(type, initial, title, filters) \
	eof_gp_advanced_file_select((type), (initial), (title), (filters))

/* eof_new_chart() performs its original Quick Save too early (before
 * eof_song_loaded is set).  Redirect only file.c's post-create initialization
 * and title refresh so project_autosave.c can perform the real first save and
 * then clear the stale "unsaved" flag that eof_new_chart() sets afterwards. */
#define eof_init_after_load(state) eof_project_autosave_init_after_load((state))
#define eof_fix_window_title() eof_project_autosave_fix_window_title()

#endif

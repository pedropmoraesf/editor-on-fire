#ifndef EOF_DTX_INTEGRATION_H
#define EOF_DTX_INTEGRATION_H

#include <allegro.h>
#include "song.h"
#include "window.h"

/* DTX drum channels used by the exporter and by the dedicated EOF preview. */
enum
{
	EOF_DTX_INT_HH = 0,
	EOF_DTX_INT_HH_OPEN,
	EOF_DTX_INT_RIDE,
	EOF_DTX_INT_KICK,
	EOF_DTX_INT_SNARE,
	EOF_DTX_INT_TOM1,
	EOF_DTX_INT_TOM2,
	EOF_DTX_INT_TOM3,
	EOF_DTX_INT_CRASH,
	EOF_DTX_INT_CHANNELS
};

int eof_dtx_integration_channel_from_midi(unsigned midi_note);

/* Shared option used by both normal and Advanced Guitar Pro drum imports.
 * It is deliberately reset for every import and is never a persistent
 * preference because crash/ride inference is heuristic. */
extern int eof_gp_detect_crash_as_ride;
void eof_dtx_warn_crash_as_ride_detection(void);

int eof_dtx_import_selected_gp_drum_track(void);

/* Hooks used only while compiling menu/file.c.  They allow the existing GP
 * selection dialog to route percussion tracks directly to PART_REAL_DRUM_DTX
 * without changing the legacy GP3/4/5 drum importer. */
int eof_dtx_alert(const char *s1, const char *s2, const char *s3,
	const char *b1, const char *b2, int c1, int c2);
int eof_dtx_alert3(const char *s1, const char *s2, const char *s3,
	const char *b1, const char *b2, const char *b3, int c1, int c2, int c3);
void eof_dtx_fixup_notes_hook(EOF_SONG *sp);
void eof_dtx_track_find_crazy_notes_hook(EOF_SONG *sp, unsigned long track, int option);
void eof_dtx_track_fixup_notes_hook(EOF_SONG *sp, unsigned long track, int sel);
unsigned long eof_dtx_note_count_colors_hook(EOF_SONG *sp, unsigned long track, unsigned long note);
void eof_dtx_log_hook(const char *text, int level);

/* Hook used only while compiling editor.c. */
int eof_dtx_note_draw(unsigned long track, unsigned long notenum, int p, EOF_WINDOW *window);

#endif

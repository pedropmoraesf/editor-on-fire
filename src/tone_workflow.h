#ifndef EOF_TONE_WORKFLOW_H
#define EOF_TONE_WORKFLOW_H

#include "song.h"

/* Installs the compact two-tone commands immediately below the existing
 * experimental tone-analysis entry. */
void eof_tone_workflow_install_menu(void);

/* Reduces one Rocksmith pro-guitar/bass arrangement to the mandatory base tone
 * plus one change near the first played note.  When make_undo is nonzero, one
 * undo state is created before the change. */
int eof_tone_reduce_track_to_two(EOF_SONG *sp, unsigned long track, int make_undo);

/* Ensures that every populated Rocksmith pro-guitar/bass arrangement has the
 * two-tone layout.  Empty arrangements are ignored.  Returns the number of
 * populated Rocksmith arrangements found (including those already correct). */
unsigned eof_tone_ensure_two_for_populated_tracks(EOF_SONG *sp, int make_undo);

#endif

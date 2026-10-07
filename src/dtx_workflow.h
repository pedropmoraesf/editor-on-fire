#ifndef EOF_DTX_WORKFLOW_H
#define EOF_DTX_WORKFLOW_H

#include "song.h"

void eof_dtx_sync_editor_state(void);
unsigned char eof_dtx_detect_difficulties_hook(EOF_SONG *sp, unsigned long track);
int eof_dtx_menu_file_exit(void);

/* Diagnostic/safety wrappers used only by force-included integration headers.
 * They leave EOF's original implementations untouched for every non-DTX path. */
void eof_dtx_load_config(char *fn);
void eof_dtx_safe_sort_notes(EOF_SONG *sp);
int eof_dtx_menu_song_seek_start(void);

#endif

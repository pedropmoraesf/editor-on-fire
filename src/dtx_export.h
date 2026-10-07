#ifndef EOF_DTX_EXPORT_H
#define EOF_DTX_EXPORT_H

#include "song.h"

int eof_menu_file_export_dtxmania(void);
int eof_dtx_track_has_notes(EOF_SONG *sp);
int eof_dtx_last_export_was_successful(void);
void eof_dtx_migrate_import_to_ultimate(EOF_SONG *sp);

#endif

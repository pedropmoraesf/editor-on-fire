#ifndef EOF_DTX_XML_H
#define EOF_DTX_XML_H

#include "song.h"

int eof_dtx_export_track_xml(EOF_SONG *sp, const char *project_filename);
int eof_dtx_save_song_hook(EOF_SONG *sp, const char *filename);

#endif

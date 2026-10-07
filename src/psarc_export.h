#ifndef EOF_PSARC_EXPORT_H
#define EOF_PSARC_EXPORT_H

/* EOF uses this core track-selection routine from main.c, but the legacy
 * headers do not expose its prototype to independent translation units.  The
 * PSARC analyzer temporarily switches arrangements, so declare the existing
 * function here with the same signature used by EOF's DTX main hook. */
int eof_menu_track_selected_track_number(unsigned long tracknum, int updatetitle);

void eof_psarc_export_install_menu(void);
int eof_menu_file_export_psarc(void);

#ifndef OTHER_PATH_SEPARATOR
#ifdef ALLEGRO_WINDOWS
#define OTHER_PATH_SEPARATOR '\\'
#else
#define OTHER_PATH_SEPARATOR '/'
#endif
#endif

#endif

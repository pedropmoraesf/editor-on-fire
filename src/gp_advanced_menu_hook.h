#ifndef EOF_GP_ADVANCED_MENU_HOOK_H
#define EOF_GP_ADVANCED_MENU_HOOK_H

/* Force-included only for dialog.c.  Load the stock declarations first, then
 * redirect dialog.c's calls inside eof_prepare_menus() through the wrappers.
 * The File wrapper mirrors the current flags into the displayed File menu that
 * inserts New from GP Audio below New.  The Song wrapper keeps EOF's stock menu
 * preparation and only replaces the user-facing names of populated Rocksmith
 * bass arrangements according to the Bass/Alt Bass rule. */
#include "menu/file.h"
#include "menu/song.h"
#include "gp_advanced.h"
#include "bass_display.h"

#define eof_prepare_file_menu() eof_gp_advanced_prepare_file_menu()
#define eof_prepare_song_menu() eof_bass_display_prepare_song_menu()

#endif

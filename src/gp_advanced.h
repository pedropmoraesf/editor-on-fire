#ifndef EOF_GP_ADVANCED_H
#define EOF_GP_ADVANCED_H

#include <allegro.h>
#include "modules/wfsel.h"

void eof_gp_advanced_install_menu(void);
void eof_gp_advanced_prepare_file_menu(void);
int eof_menu_file_gp_advanced_import(void);
int eof_gp_advanced_import_path(const char *selected_path, int suppress_timing);
	/* Imports a preselected GP/GPA file through the same Advanced mapping UI.
	 * suppress_timing hides/disables Songsterr/SVL controls, for workflows that
	 * already obtain their tempo map from the Guitar Pro source itself. */

/* menu/file.c is force-included with a small bridge so the advanced dialog can
 * feed a preselected JSON/SVL path through EOF's EXISTING Songsterr and Sonic
 * Visualiser import functions.  Outside an active advanced import these two
 * wrappers are transparent pass-throughs. */
int eof_gp_advanced_file_popup_dialog(DIALOG *dialog, int focus);
char *eof_gp_advanced_file_select(int type, char *initial, const char *title, NCDFS_FILTER_LIST *filters);

/* The stock Songsterr downloader in menu/file.c historically leaves its search
 * pointer on the '-' in "-tab-s", then immediately expects a digit.  file.c is
 * built with a narrow hook that advances only this token to its numeric ID,
 * leaving every other strcasestr_spec() call unchanged. */
char *eof_gp_advanced_songsterr_strcasestr(const char *haystack, const char *needle);

#endif

#ifndef EOF_AUDIO_NORMALIZE_FILE_HOOK_H
#define EOF_AUDIO_NORMALIZE_FILE_HOOK_H

/* Force-included after dtx_gp_import_hook.h for menu/file.c.  Keep the existing
 * advanced-GP popup bridge as the fallback and only intercept OGG Settings plus
 * the project-audio load that follows new-project conversion/copying. */
#include "main.h"
#include "dialog.h"
#include "menu/file.h"
#include "gp_advanced.h"
#include "audio_normalize.h"

#ifdef eof_popup_dialog
#undef eof_popup_dialog
#endif
static int eof_audio_normalize_file_popup_dialog(DIALOG *dialog, int focus)
{
	if(dialog == eof_ogg_settings_dialog)
		return eof_audio_normalize_ogg_settings_popup(dialog, focus);
	return eof_gp_advanced_file_popup_dialog(dialog, focus);
}
#define eof_popup_dialog(dialog, focus) eof_audio_normalize_file_popup_dialog((dialog), (focus))

#define eof_load_ogg(filename, function) eof_audio_normalize_intercept_load_ogg((filename), (function))

#endif

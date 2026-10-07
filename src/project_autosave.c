#include <allegro.h>
#include <string.h>

#include "main.h"
#include "undo.h"
#include "menu/file.h"
#include "project_autosave.h"

static int eof_project_autosave_pending_clean = 0;

void eof_project_autosave_init_after_load(char initaftersavestate)
{
	eof_init_after_load(initaftersavestate);

	/* eof_new_chart() historically calls Quick Save before eof_song_loaded is
	 * set, so eof_menu_file_save_logic() immediately returns and notes.eof is
	 * never written.  By the time this first init-after-load call happens the
	 * new project is fully constructed, the audio is loaded and eof_filename is
	 * already the intended .../notes.eof path.  Detect only that narrow case:
	 * a loaded project named notes.eof whose target file does not yet exist. */
	if(!initaftersavestate && eof_song_loaded && eof_song && eof_filename[0] &&
	   eof_loaded_song_name[0] && !ustricmp(eof_loaded_song_name, "notes.eof") &&
	   !exists(eof_filename))
	{
		eof_log("New project autosave: notes.eof does not exist; saving now", 1);
		if((eof_menu_file_quick_save() == 0) && exists(eof_filename))
		{
			/* eof_new_chart() sets eof_changes/eof_project_unsaved back to 1
			 * immediately after this function returns.  Defer one cleanup until
			 * its following eof_fix_window_title() call. */
			eof_project_autosave_pending_clean = 1;
			eof_log("New project autosave: notes.eof created successfully", 1);
		}
		else
		{
			eof_log("New project autosave: automatic notes.eof save failed", 1);
		}
	}
}

void eof_project_autosave_fix_window_title(void)
{
	if(eof_project_autosave_pending_clean)
	{
		eof_changes = 0;
		eof_project_unsaved = 0;
		eof_project_autosave_pending_clean = 0;
	}
	eof_fix_window_title();
}

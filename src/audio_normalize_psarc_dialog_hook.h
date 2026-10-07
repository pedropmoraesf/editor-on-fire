#ifndef EOF_AUDIO_NORMALIZE_PSARC_DIALOG_HOOK_H
#define EOF_AUDIO_NORMALIZE_PSARC_DIALOG_HOOK_H

/* Force-included only for psarc_dialog.c.  The PSARC dialog is assembled into
 * a local DIALOG array immediately before eof_popup_dialog().  Extend that
 * assembled dialog while preserving every existing result index. */
#include <allegro.h>
#include <string.h>
#include "agup/agup.h"
#include "dialog.h"
#include "dialog/proc.h"
#include "main.h"
#include "audio_normalize.h"

static int eof_audio_normalize_psarc_popup(DIALOG *dialog, int focus)
{
	DIALOG ext[38];
	int result, i;
	int xoff, yoff;
	double target;

	if(!dialog || !dialog[0].dp || strcmp((const char *)dialog[0].dp, "Export Rocksmith 2014 PSARC"))
		return eof_popup_dialog(dialog, focus);

	for(;;)
	{
		memset(ext, 0, sizeof(ext));
		/* psarc_dialog.c uses entries 0..33 and entry 34 as its sentinel. */
		for(i = 0; i <= 33; i++)
			ext[i] = dialog[i];
		xoff = dialog[0].x;
		yoff = dialog[0].y - 8;

		/* Make one compact row for normalization without growing the 458px
		 * window beyond EOF's minimum 640x480 display. */
		ext[32].y += 18;
		ext[33].y += 18;
		ext[23].y += 12;
		ext[24].y += 12;

		ext[34].proc = d_agup_check_proc;
		ext[34].x = xoff + 18; ext[34].y = yoff + 382;
		ext[34].w = 150; ext[34].h = 18;
		ext[34].flags = eof_audio_normalize_psarc_checked ? D_SELECTED : 0;
		ext[34].d1 = 1; ext[34].dp = (void *)"Normalize audio";

		ext[35].proc = d_agup_text_proc;
		ext[35].x = xoff + 176; ext[35].y = yoff + 384;
		ext[35].w = 80; ext[35].h = 16;
		ext[35].dp = (void *)"Target LUFS:";

		ext[36].proc = eof_edit_proc;
		ext[36].x = xoff + 260; ext[36].y = yoff + 379;
		ext[36].w = 72; ext[36].h = 20;
		ext[36].d1 = 16; ext[36].dp = eof_audio_normalize_target_text;
		ext[37].proc = NULL;

		eof_color_dialog(ext, gui_fg_color, gui_bg_color);
		result = eof_popup_dialog(ext, focus);

		/* Copy mutable state back because psarc_dialog.c synchronizes its source
		 * dialog from the original local array after this function returns. */
		for(i = 0; i <= 33; i++)
		{
			dialog[i].d1 = ext[i].d1;
			dialog[i].d2 = ext[i].d2;
			dialog[i].flags = ext[i].flags;
		}
		eof_audio_normalize_psarc_checked = (ext[34].flags & D_SELECTED) ? 1 : 0;

		if(result == 23 && eof_audio_normalize_psarc_checked)
		{
			if(!eof_audio_normalize_parse_target(eof_audio_normalize_target_text, &target))
			{
				allegro_message("Enter a loudness target between -70 and -5 LUFS.");
				continue;
			}
			if(!eof_audio_normalize_resolve_ffmpeg(1))
			{
				allegro_message("PSARC audio normalization requires ffmpeg.exe.\nUncheck Normalize audio or select ffmpeg.exe.");
				continue;
			}
		}

		set_config_string("audio_normalize", "target_lufs", eof_audio_normalize_target_text);
		set_config_int("audio_normalize", "psarc", eof_audio_normalize_psarc_checked ? 1 : 0);
		flush_config_file();
		return result;
	}
}

#define eof_popup_dialog(dialog, focus) eof_audio_normalize_psarc_popup((dialog), (focus))

#endif

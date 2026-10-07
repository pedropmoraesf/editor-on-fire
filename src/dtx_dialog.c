#include <allegro.h>
#include <string.h>

#include "agup/agup.h"
#include "dialog.h"
#include "dialog/proc.h"
#include "main.h"
#include "dtx_dialog.h"
#include "dtx_import.h"

#define EOF_DTX_COMPACT_MEDIA (-1000)
#define EOF_DTX_COMPACT_BACK  (-1001)

static int eof_dtx_dialog_session_active = 0;
static int eof_dtx_render_imported_ogg = 0;

int eof_dtx_dialog_render_imported_ogg(void)
{
	return (eof_dtx_import_session_active() && eof_dtx_render_imported_ogg) ? 1 : 0;
}

static void eof_dtx_place_dialog_item(DIALOG *dst, const DIALOG *src, int x, int y, int w, int h)
{
	if(!dst || !src)
		return;
	*dst = *src;
	dst->x = x;
	dst->y = y;
	dst->w = w;
	dst->h = h;
}

static int eof_dtx_dialog_width(void)
{
	int width = 590;
	if((SCREEN_W > 40) && (width > SCREEN_W - 16))
		width = SCREEN_W - 16;
	if(width < 360)
		width = 360;
	return width;
}

static void eof_dtx_sync_check_flag(DIALOG *dst, const DIALOG *src)
{
	if(!dst || !src)
		return;
	dst->flags &= ~D_SELECTED;
	dst->flags |= src->flags & D_SELECTED;
}

static int eof_dtx_compact_main_dialog(DIALOG *source)
{
	DIALOG dlg[19];
	int width, result;

	if(!source)
		return 31;
	width = eof_dtx_dialog_width();
	memset(dlg, 0, sizeof(dlg));

	eof_dtx_place_dialog_item(&dlg[0], &source[0], 0, 20, width, 355);
	dlg[0].dp = "DTXMania Export Settings";

	eof_dtx_place_dialog_item(&dlg[1], &source[1], 14, 46, width - 28, 12);
	eof_dtx_place_dialog_item(&dlg[2], &source[2], 14, 61, width - 28, 20);
	eof_dtx_place_dialog_item(&dlg[3], &source[3], 14, 89, width - 28, 12);
	eof_dtx_place_dialog_item(&dlg[4], &source[4], 14, 104, width - 28, 20);
	eof_dtx_place_dialog_item(&dlg[5], &source[5], 14, 132, width - 28, 12);
	eof_dtx_place_dialog_item(&dlg[6], &source[6], 14, 147, width - 28, 20);

	eof_dtx_place_dialog_item(&dlg[7], &source[7], 14, 177, 182, 12);
	eof_dtx_place_dialog_item(&dlg[8], &source[8], 202, 173, 72, 20);
	eof_dtx_place_dialog_item(&dlg[9], &source[9], 14, 199, width - 28, 12);
	dlg[9].dp = "250 ms is intended to compensate Bluetooth audio latency when using electronic drums.";

	eof_dtx_place_dialog_item(&dlg[10], &source[10], 14, 222, width - 28, 16);
	eof_dtx_place_dialog_item(&dlg[11], &source[11], 34, 247, 174, 12);
	eof_dtx_place_dialog_item(&dlg[12], &source[12], 210, 243, 72, 20);
	eof_dtx_place_dialog_item(&dlg[13], &source[13], 14, 272, width - 28, 16);

	/* Reuse one of the exporter's existing buttons for a compact media page. */
	eof_dtx_place_dialog_item(&dlg[14], &source[16], 14, 303, 150, 24);
	dlg[14].dp = "Media files...";

	/* Only a chart that entered EOF through File > Import > DTX has enough
	 * provenance to replace its source BGM with a FluidSynth render.  Keep this
	 * opt-in every time the dialog opens; ordinary EOF-authored charts cannot
	 * accidentally synthesize/replace their exported song OGG. */
	dlg[15].proc = d_agup_check_proc;
	dlg[15].x = 176;
	dlg[15].y = 306;
	dlg[15].w = width - 190;
	dlg[15].h = 18;
	dlg[15].flags = eof_dtx_render_imported_ogg ? D_SELECTED : 0;
	if(!eof_dtx_import_session_active())
		dlg[15].flags |= D_DISABLED;
	dlg[15].dp = (void *)"Render imported DTX pattern as OGG (FluidSynth)";

	eof_dtx_place_dialog_item(&dlg[16], &source[30], width - 290, 326, 130, 28);
	eof_dtx_place_dialog_item(&dlg[17], &source[31], width - 145, 326, 130, 28);
	dlg[18].proc = NULL;

	eof_color_dialog(dlg, gui_fg_color, gui_bg_color);
	eof_conditionally_center_dialog(dlg);
	result = eof_popup_dialog(dlg, 2);

	eof_dtx_sync_check_flag(&source[10], &dlg[10]);
	eof_dtx_sync_check_flag(&source[13], &dlg[13]);
	eof_dtx_render_imported_ogg = (eof_dtx_import_session_active() && (dlg[15].flags & D_SELECTED)) ? 1 : 0;

	if(result == 14)
		return EOF_DTX_COMPACT_MEDIA;
	if(result == 16)
		return 30;
	if(result == 17)
		return 31;
	return 31;
}

static int eof_dtx_compact_media_dialog(DIALOG *source)
{
	DIALOG dlg[19];
	int width, path_width, button_x, result;

	if(!source)
		return EOF_DTX_COMPACT_BACK;
	width = eof_dtx_dialog_width();
	path_width = width - 252;
	if(path_width < 120)
		path_width = 120;
	button_x = width - 112;
	memset(dlg, 0, sizeof(dlg));

	eof_dtx_place_dialog_item(&dlg[0], &source[0], 0, 20, width, 285);
	dlg[0].dp = "DTXMania Media Files";

	eof_dtx_place_dialog_item(&dlg[1], &source[14], 14, 52, 105, 12);
	eof_dtx_place_dialog_item(&dlg[2], &source[15], 125, 52, path_width, 12);
	eof_dtx_place_dialog_item(&dlg[3], &source[16], button_x, 44, 98, 24);

	eof_dtx_place_dialog_item(&dlg[4], &source[17], 14, 82, 105, 12);
	eof_dtx_place_dialog_item(&dlg[5], &source[18], 125, 82, path_width, 12);
	eof_dtx_place_dialog_item(&dlg[6], &source[19], button_x, 74, 98, 24);

	eof_dtx_place_dialog_item(&dlg[7], &source[20], 14, 112, 105, 12);
	eof_dtx_place_dialog_item(&dlg[8], &source[21], 125, 112, path_width, 12);
	eof_dtx_place_dialog_item(&dlg[9], &source[22], button_x, 104, 98, 24);

	eof_dtx_place_dialog_item(&dlg[10], &source[23], 14, 142, 105, 12);
	eof_dtx_place_dialog_item(&dlg[11], &source[24], 125, 142, path_width, 12);
	eof_dtx_place_dialog_item(&dlg[12], &source[25], button_x, 134, 98, 24);

	eof_dtx_place_dialog_item(&dlg[13], &source[26], 14, 172, 105, 12);
	eof_dtx_place_dialog_item(&dlg[14], &source[27], 125, 172, path_width, 12);
	eof_dtx_place_dialog_item(&dlg[15], &source[28], button_x, 164, 98, 24);

	eof_dtx_place_dialog_item(&dlg[16], &source[29], 14, 207, width - 28, 24);
	dlg[16].dp = "Selected media files are copied into the DTXMania song folder during export.";
	eof_dtx_place_dialog_item(&dlg[17], &source[31], width - 126, 244, 112, 28);
	dlg[17].dp = "Back";
	dlg[18].proc = NULL;

	eof_color_dialog(dlg, gui_fg_color, gui_bg_color);
	eof_conditionally_center_dialog(dlg);
	result = eof_popup_dialog(dlg, 17);

	switch(result)
	{
		case 3: return 16;
		case 6: return 19;
		case 9: return 22;
		case 12: return 25;
		case 15: return 28;
		default: break;
	}
	return EOF_DTX_COMPACT_BACK;
}

int eof_dtx_popup_dialog(DIALOG *dialog, int focus)
{
	int result;
	(void)focus;

	/* This hook is force-included only in dtx_export_v2.c, but keep a defensive
	 * title check in case that source later adds another popup. */
	if(!dialog || !dialog[0].dp || strcmp((const char *)dialog[0].dp, "DTXMania Export Settings"))
		return eof_popup_dialog(dialog, focus);

	if(!eof_dtx_dialog_session_active)
	{
		/* Uppercase only the title/artist values copied automatically from the
		 * EOF project.  Once editing starts, the normal edit procedures preserve
		 * exactly the casing entered by the user, including across media browsing. */
		if(dialog[2].dp)
			ustrupr((char *)dialog[2].dp);
		if(dialog[4].dp)
			ustrupr((char *)dialog[4].dp);
		eof_dtx_dialog_session_active = 1;
		eof_dtx_render_imported_ogg = 0;
	}

	for(;;)
	{
		result = eof_dtx_compact_main_dialog(dialog);
		if(result != EOF_DTX_COMPACT_MEDIA)
		{
			if(result == 30 || result == 31)
				eof_dtx_dialog_session_active = 0;
			return result;
		}

		result = eof_dtx_compact_media_dialog(dialog);
		if(result == EOF_DTX_COMPACT_BACK)
			continue;

		/* Return the original export-dialog button index.  dtx_export_v2.c will
		 * run its existing native file picker and then call us again. */
		return result;
	}
}

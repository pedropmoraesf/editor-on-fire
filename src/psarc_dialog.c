#include <allegro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agup/agup.h"
#include "dialog.h"
#include "main.h"
#include "utility.h"
#include "psarc_dialog.h"

#define EOF_PSARC_ART_LABEL      25
#define EOF_PSARC_ART_EDIT       26
#define EOF_PSARC_ART_CHOOSE     27
#define EOF_PSARC_ART_PREVIEW    28
#define EOF_PSARC_WWISE_LABEL    29
#define EOF_PSARC_WWISE_EDIT     30
#define EOF_PSARC_WWISE_CHOOSE   31
#define EOF_PSARC_PLATFORM_PC    32
#define EOF_PSARC_PLATFORM_MAC   33
#define EOF_PSARC_DIALOG_END     34
#define EOF_PSARC_DIALOG_COUNT   35

static char eof_psarc_album_art[1024] = {0};
static char eof_psarc_wwise_bin[1024] = {0};
static BITMAP *eof_psarc_album_preview = NULL;
static int eof_psarc_dialog_session_active = 0;
static int eof_psarc_platform_pc = 1;
static int eof_psarc_platform_mac = 0;

static void eof_psarc_dialog_place(DIALOG *dst, const DIALOG *src, int x, int y, int w, int h)
{
	if(!dst || !src)
		return;
	*dst = *src;
	dst->x = x;
	dst->y = y;
	dst->w = w;
	dst->h = h;
}

static void eof_psarc_destroy_album_preview(void)
{
	if(eof_psarc_album_preview)
	{
		destroy_bitmap(eof_psarc_album_preview);
		eof_psarc_album_preview = NULL;
	}
}

static void eof_psarc_refresh_album_preview(void)
{
	BITMAP *source;

	eof_psarc_destroy_album_preview();
	if(!eof_psarc_album_art[0] || !exists(eof_psarc_album_art))
		return;

	/* Allegro 4 only previews formats registered with load_bitmap().  RSToolkit
	 * accepts more formats through System.Drawing, so preview failure is not an
	 * export failure: the preview canvas simply remains hidden. */
	source = load_bitmap(eof_psarc_album_art, NULL);
	if(!source)
		return;

	eof_psarc_album_preview = create_bitmap(70, 70);
	if(eof_psarc_album_preview)
	{
		clear_to_color(eof_psarc_album_preview, makecol(0, 0, 0));
		stretch_blit(source, eof_psarc_album_preview, 0, 0, source->w, source->h, 0, 0, 70, 70);
	}
	destroy_bitmap(source);
}

static int eof_psarc_find_default_album(char *out, size_t outsz)
{
	char exe[1024] = {0}, dir[1024] = {0}, candidate[1024] = {0};
	const char *relative[] = {
		"tools/psarc/default_album.png",
		"../tools/psarc/default_album.png"
	};
	unsigned i;

	if(!out || !outsz)
		return 0;
	out[0] = '\0';

	get_executable_name(exe, sizeof(exe));
	ustrzcpy(dir, sizeof(dir), exe);
	*get_filename(dir) = '\0';
	for(i = 0; i < sizeof(relative) / sizeof(relative[0]); i++)
	{
		(void)snprintf(candidate, sizeof(candidate) - 1, "%s%s", dir, relative[i]);
		fix_filename_slashes(candidate);
		if(exists(candidate))
		{
			ustrzcpy(out, (int)outsz, candidate);
			return 1;
		}
	}

	for(i = 0; i < sizeof(relative) / sizeof(relative[0]); i++)
	{
		ustrzcpy(candidate, sizeof(candidate), relative[i]);
		fix_filename_slashes(candidate);
		if(exists(candidate))
		{
			ustrzcpy(out, (int)outsz, candidate);
			return 1;
		}
	}
	return 0;
}

static int eof_psarc_copy_album_to_staging(void)
{
	char staging[1024] = {0}, dest[1024] = {0}, old[1024] = {0};
	const char *ext;
	const char *known[] = {"png", "jpg", "jpeg", "bmp", "tga", "dds"};
	unsigned i;

	if(!eof_psarc_album_art[0] || !exists(eof_psarc_album_art))
		return 0;

	replace_filename(staging, eof_loaded_ogg_name, "eof_psarc_tmp", sizeof(staging));
	if(!eof_folder_exists(staging) && eof_mkdir(staging))
		return 0;

	for(i = 0; i < sizeof(known) / sizeof(known[0]); i++)
	{
		(void)snprintf(old, sizeof(old) - 1, "%s%calbum_art.%s", staging, OTHER_PATH_SEPARATOR, known[i]);
		if(exists(old))
			delete_file(old);
	}

	ext = get_extension(eof_psarc_album_art);
	if(!ext || !ext[0])
		ext = "png";
	(void)snprintf(dest, sizeof(dest) - 1, "%s%calbum_art.%s", staging, OTHER_PATH_SEPARATOR, ext);
	return eof_copy_file(eof_psarc_album_art, dest);
}

static void eof_psarc_sync_dialog_state(DIALOG *source, DIALOG *dlg)
{
	int i;
	if(!source || !dlg)
		return;
	source[12].d1 = dlg[12].d1;
	source[16].d1 = dlg[16].d1;
	for(i = 18; i <= 22; i++)
	{
		if(i == 20 || i == 21)
			continue;
		source[i].flags &= ~D_SELECTED;
		source[i].flags |= dlg[i].flags & D_SELECTED;
	}
	eof_psarc_platform_pc = (dlg[EOF_PSARC_PLATFORM_PC].flags & D_SELECTED) ? 1 : 0;
	eof_psarc_platform_mac = (dlg[EOF_PSARC_PLATFORM_MAC].flags & D_SELECTED) ? 1 : 0;
}

static void eof_psarc_load_saved_wwise(void)
{
	const char *saved_bin = get_config_string("psarc", "wwise_bin", "");
	const char *saved_cli = get_config_string("psarc", "wwise_cli", "");
	eof_psarc_wwise_bin[0] = '\0';

	if(saved_bin && saved_bin[0] && eof_folder_exists(saved_bin))
	{
		ustrzcpy(eof_psarc_wwise_bin, sizeof(eof_psarc_wwise_bin), saved_bin);
		return;
	}

	/* Migrate the older setting that stored the executable itself. */
	if(saved_cli && saved_cli[0] && exists(saved_cli))
	{
		ustrzcpy(eof_psarc_wwise_bin, sizeof(eof_psarc_wwise_bin), saved_cli);
		*get_filename(eof_psarc_wwise_bin) = '\0';
	}
}

static int eof_psarc_wwise_cli_path(char *cli, size_t clisz)
{
	if(cli && clisz) cli[0] = '\0';
	if(!eof_psarc_wwise_bin[0] || !eof_folder_exists(eof_psarc_wwise_bin))
		return 0;
	if(cli && clisz)
		replace_filename(cli, eof_psarc_wwise_bin, "WwiseCLI.exe", (int)clisz);
	return 1;
}

static int eof_psarc_validate_wwise(void)
{
	char cli[1024] = {0}, akcopy[1024] = {0};
	if(!eof_psarc_wwise_cli_path(cli, sizeof(cli)) || !exists(cli))
		return 0;

	/* Match the working Python pipeline: WwiseCLI builds the RSToolkit template
	 * and AkCopyStreamedFiles consumes the generated SoundbanksInfo.xml. */
	(void)snprintf(akcopy, sizeof(akcopy) - 1, "%s%ctools%cAkCopyStreamedFiles.exe",
		eof_psarc_wwise_bin, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR);
	fix_filename_slashes(akcopy);
	return exists(akcopy) ? 1 : 0;
}

static void eof_psarc_store_wwise(void)
{
	char cli[1024] = {0};
	if(!eof_psarc_wwise_cli_path(cli, sizeof(cli)))
		return;
	set_config_string("psarc", "wwise_bin", eof_psarc_wwise_bin);
	/* The export specification stores the full WwiseCLI path; the helper derives
	 * bin\\tools\\AkCopyStreamedFiles.exe from the same selected directory. */
	set_config_string("psarc", "wwise_cli", cli);
	flush_config_file();
}

static void eof_psarc_trim_folder_separator(char *path)
{
	size_t len;
	if(!path)
		return;
	len = strlen(path);
	while(len > 3 && (path[len - 1] == '\\' || path[len - 1] == '/'))
		path[--len] = '\0';
}

static int eof_psarc_validate_rstoolkit_root(const char *root, char *missing, size_t missingsz)
{
	const char *required[] = {
		"RocksmithToolkitGUI.exe",
		"packer.exe",
		"RocksmithToolkitLib.dll",
		"ICSharpCode.SharpZipLib.dll",
		"MiscUtil.dll",
		"Newtonsoft.Json.dll",
		"X360.dll",
		"zlib.net.dll",
		"ddc\\ddc.exe",
		"Template\\Template.wproj"
	};
	char candidate[1024] = {0};
	unsigned i;

	if(missing && missingsz)
		missing[0] = '\0';
	if(!root || !root[0] || !eof_folder_exists(root))
	{
		if(missing && missingsz)
			ustrzcpy(missing, (int)missingsz, "RSToolkit folder");
		return 0;
	}

	for(i = 0; i < sizeof(required) / sizeof(required[0]); i++)
	{
		(void)snprintf(candidate, sizeof(candidate) - 1, "%s%c%s", root, OTHER_PATH_SEPARATOR, required[i]);
		fix_filename_slashes(candidate);
		if(!exists(candidate))
		{
			if(missing && missingsz)
				ustrzcpy(missing, (int)missingsz, required[i]);
			return 0;
		}
	}
	return 1;
}

static int eof_psarc_ensure_rstoolkit_root(void)
{
	const char *saved = get_config_string("psarc", "rstoolkit_root", "");
	char root[1024] = {0}, selected[1024] = {0}, missing[256] = {0};
	const char *filename;

	if(saved && saved[0])
	{
		ustrzcpy(root, sizeof(root), saved);
		eof_psarc_trim_folder_separator(root);
		if(eof_psarc_validate_rstoolkit_root(root, missing, sizeof(missing)))
		{
			/* Normalize legacy values that ended in a backslash. */
			if(strcmp(saved, root))
			{
				set_config_string("psarc", "rstoolkit_root", root);
				flush_config_file();
			}
			return 1;
		}
	}

	/* Allegro 4 has no portable directory picker.  Selecting the RSToolkit GUI
	 * executable identifies exactly the directory requested by the workflow;
	 * every other RSToolkit path is derived from its parent folder. */
	if(root[0])
		replace_filename(selected, root, "RocksmithToolkitGUI.exe", sizeof(selected));
	if(!file_select_ex("Select RocksmithToolkitGUI.exe from the installed RSToolkit folder",
		selected, "exe", sizeof(selected), 560, 420) || !exists(selected))
		return 0;

	filename = get_filename(selected);
	if(!filename || ustricmp(filename, "RocksmithToolkitGUI.exe"))
	{
		allegro_message(
			"Select RocksmithToolkitGUI.exe from the installed RSToolkit folder.\n\n"
			"EOF will obtain packer.exe, ddc\\ddc.exe and Template\\... from that same folder."
		);
		return 0;
	}

	ustrzcpy(root, sizeof(root), selected);
	*get_filename(root) = '\0';
	eof_psarc_trim_folder_separator(root);
	if(!eof_psarc_validate_rstoolkit_root(root, missing, sizeof(missing)))
	{
		char message[768] = {0};
		(void)snprintf(message, sizeof(message) - 1,
			"This is not a complete RSToolkit installation.\n\nMissing: %s\n\n"
			"Select RocksmithToolkitGUI.exe from the folder that also contains packer.exe.",
			missing[0] ? missing : "required RSToolkit file");
		allegro_message("%s", message);
		return 0;
	}

	set_config_string("psarc", "rstoolkit_root", root);
	flush_config_file();
	return 1;
}

static void eof_psarc_store_platform_environment(void)
{
#ifdef ALLEGRO_WINDOWS
	_putenv(eof_psarc_platform_pc ? "EOF_PSARC_PC=1" : "EOF_PSARC_PC=0");
	_putenv(eof_psarc_platform_mac ? "EOF_PSARC_MAC=1" : "EOF_PSARC_MAC=0");
#endif
}

int eof_psarc_popup_dialog(DIALOG *source, int focus)
{
	DIALOG dlg[EOF_PSARC_DIALOG_COUNT];
	int result;
	char selected[1024] = {0};
	(void)focus;

	if(!source || !source[0].dp || strcmp((const char *)source[0].dp, "Export Rocksmith 2014 PSARC"))
		return eof_popup_dialog(source, focus);

	if(!eof_psarc_dialog_session_active)
	{
		eof_psarc_dialog_session_active = 1;
		/* Keep the metadata casing from EOF/user input.  Rocksmith tone display
		 * names are normalized later as "Song - Effect"; forcing the song and
		 * artist to all-caps here would leak into manifests and the game UI. */
		if(source[6].dp)
			ustrzcpy((char *)source[6].dp, 256, "Live");
		if(!eof_psarc_find_default_album(eof_psarc_album_art, sizeof(eof_psarc_album_art)))
			eof_psarc_album_art[0] = '\0';
		eof_psarc_load_saved_wwise();
		eof_psarc_platform_pc = 1;
		eof_psarc_platform_mac = 0;
	}

	for(;;)
	{
		memset(dlg, 0, sizeof(dlg));

		/* Metadata occupies two compact rows. */
		eof_psarc_dialog_place(&dlg[0], &source[0], 0, 8, 610, 458);
		dlg[0].dp = "Export Rocksmith 2014 PSARC";
		eof_psarc_dialog_place(&dlg[1], &source[1], 18, 30, 80, 16);
		eof_psarc_dialog_place(&dlg[2], &source[2], 102, 26, 200, 20);
		eof_psarc_dialog_place(&dlg[3], &source[3], 318, 30, 50, 16);
		eof_psarc_dialog_place(&dlg[4], &source[4], 370, 26, 220, 20);
		eof_psarc_dialog_place(&dlg[5], &source[5], 18, 58, 80, 16);
		eof_psarc_dialog_place(&dlg[6], &source[6], 102, 54, 200, 20);
		eof_psarc_dialog_place(&dlg[7], &source[7], 318, 58, 45, 16);
		eof_psarc_dialog_place(&dlg[8], &source[8], 370, 54, 72, 20);
		eof_psarc_dialog_place(&dlg[9], &source[9], 460, 58, 58, 16);
		eof_psarc_dialog_place(&dlg[10], &source[10], 520, 54, 70, 20);

		/* Album artwork and Wwise each get their own complete row. */
		dlg[EOF_PSARC_ART_LABEL].proc = d_agup_text_proc;
		dlg[EOF_PSARC_ART_LABEL].x = 18;
		dlg[EOF_PSARC_ART_LABEL].y = 86;
		dlg[EOF_PSARC_ART_LABEL].w = 78;
		dlg[EOF_PSARC_ART_LABEL].h = 16;
		dlg[EOF_PSARC_ART_LABEL].dp = (void *)"Album art:";

		dlg[EOF_PSARC_ART_EDIT].proc = d_agup_edit_proc;
		dlg[EOF_PSARC_ART_EDIT].x = 102;
		dlg[EOF_PSARC_ART_EDIT].y = 82;
		dlg[EOF_PSARC_ART_EDIT].w = 350;
		dlg[EOF_PSARC_ART_EDIT].h = 20;
		dlg[EOF_PSARC_ART_EDIT].d1 = 1023;
		dlg[EOF_PSARC_ART_EDIT].dp = eof_psarc_album_art;

		dlg[EOF_PSARC_ART_CHOOSE].proc = d_agup_button_proc;
		dlg[EOF_PSARC_ART_CHOOSE].x = 466;
		dlg[EOF_PSARC_ART_CHOOSE].y = 80;
		dlg[EOF_PSARC_ART_CHOOSE].w = 124;
		dlg[EOF_PSARC_ART_CHOOSE].h = 24;
		dlg[EOF_PSARC_ART_CHOOSE].flags = D_EXIT;
		dlg[EOF_PSARC_ART_CHOOSE].dp = (void *)"Choose image...";

		dlg[EOF_PSARC_WWISE_LABEL].proc = d_agup_text_proc;
		dlg[EOF_PSARC_WWISE_LABEL].x = 18;
		dlg[EOF_PSARC_WWISE_LABEL].y = 114;
		dlg[EOF_PSARC_WWISE_LABEL].w = 78;
		dlg[EOF_PSARC_WWISE_LABEL].h = 16;
		dlg[EOF_PSARC_WWISE_LABEL].dp = (void *)"Wwise bin:";

		dlg[EOF_PSARC_WWISE_EDIT].proc = d_agup_edit_proc;
		dlg[EOF_PSARC_WWISE_EDIT].x = 102;
		dlg[EOF_PSARC_WWISE_EDIT].y = 110;
		dlg[EOF_PSARC_WWISE_EDIT].w = 350;
		dlg[EOF_PSARC_WWISE_EDIT].h = 20;
		dlg[EOF_PSARC_WWISE_EDIT].d1 = 1023;
		dlg[EOF_PSARC_WWISE_EDIT].dp = eof_psarc_wwise_bin;

		dlg[EOF_PSARC_WWISE_CHOOSE].proc = d_agup_button_proc;
		dlg[EOF_PSARC_WWISE_CHOOSE].x = 466;
		dlg[EOF_PSARC_WWISE_CHOOSE].y = 108;
		dlg[EOF_PSARC_WWISE_CHOOSE].w = 124;
		dlg[EOF_PSARC_WWISE_CHOOSE].h = 24;
		dlg[EOF_PSARC_WWISE_CHOOSE].flags = D_EXIT;
		dlg[EOF_PSARC_WWISE_CHOOSE].dp = (void *)"Choose bin...";

		/* Arrangements and effect controls.  The preview has its own column and
		 * never overlaps the buttons/list. */
		eof_psarc_dialog_place(&dlg[11], &source[11], 18, 144, 282, 16);
		eof_psarc_dialog_place(&dlg[12], &source[12], 18, 162, 292, 154);
		eof_psarc_dialog_place(&dlg[13], &source[13], 326, 162, 182, 24);
		eof_psarc_dialog_place(&dlg[14], &source[14], 326, 190, 182, 16);
		eof_psarc_dialog_place(&dlg[15], &source[15], 326, 216, 182, 28);
		eof_psarc_dialog_place(&dlg[16], &source[16], 326, 216, 182, 78);
		eof_psarc_dialog_place(&dlg[17], &source[17], 326, 298, 182, 24);

		eof_psarc_refresh_album_preview();
		dlg[EOF_PSARC_ART_PREVIEW].proc = d_bitmap_proc;
		dlg[EOF_PSARC_ART_PREVIEW].x = 520;
		dlg[EOF_PSARC_ART_PREVIEW].y = 162;
		dlg[EOF_PSARC_ART_PREVIEW].w = 70;
		dlg[EOF_PSARC_ART_PREVIEW].h = 70;
		dlg[EOF_PSARC_ART_PREVIEW].dp = eof_psarc_album_preview;
		if(!eof_psarc_album_preview)
			dlg[EOF_PSARC_ART_PREVIEW].flags = D_HIDDEN;

		/* Bottom options: DD, intro mode, then PC/Mac package targets. */
		eof_psarc_dialog_place(&dlg[18], &source[18], 18, 330, 290, 18);
		eof_psarc_dialog_place(&dlg[19], &source[19], 18, 356, 154, 18);
		eof_psarc_dialog_place(&dlg[20], &source[20], 184, 358, 68, 16);
		eof_psarc_dialog_place(&dlg[21], &source[21], 254, 353, 78, 20);
		eof_psarc_dialog_place(&dlg[22], &source[22], 355, 356, 232, 18);

		dlg[EOF_PSARC_PLATFORM_PC].proc = d_agup_check_proc;
		dlg[EOF_PSARC_PLATFORM_PC].x = 18;
		dlg[EOF_PSARC_PLATFORM_PC].y = 386;
		dlg[EOF_PSARC_PLATFORM_PC].w = 138;
		dlg[EOF_PSARC_PLATFORM_PC].h = 18;
		dlg[EOF_PSARC_PLATFORM_PC].flags = eof_psarc_platform_pc ? D_SELECTED : 0;
		dlg[EOF_PSARC_PLATFORM_PC].dp = (void *)"Generate PC PSARC";

		dlg[EOF_PSARC_PLATFORM_MAC].proc = d_agup_check_proc;
		dlg[EOF_PSARC_PLATFORM_MAC].x = 176;
		dlg[EOF_PSARC_PLATFORM_MAC].y = 386;
		dlg[EOF_PSARC_PLATFORM_MAC].w = 148;
		dlg[EOF_PSARC_PLATFORM_MAC].h = 18;
		dlg[EOF_PSARC_PLATFORM_MAC].flags = eof_psarc_platform_mac ? D_SELECTED : 0;
		dlg[EOF_PSARC_PLATFORM_MAC].dp = (void *)"Generate Mac PSARC";

		eof_psarc_dialog_place(&dlg[23], &source[23], 380, 416, 100, 28);
		eof_psarc_dialog_place(&dlg[24], &source[24], 488, 416, 100, 28);

		dlg[EOF_PSARC_DIALOG_END].proc = NULL;
		eof_color_dialog(dlg, gui_fg_color, gui_bg_color);
		eof_conditionally_center_dialog(dlg);
		result = eof_popup_dialog(dlg, 2);
		eof_psarc_sync_dialog_state(source, dlg);

		if(result == EOF_PSARC_ART_CHOOSE)
		{
			ustrzcpy(selected, sizeof(selected), eof_psarc_album_art);
			if(file_select_ex("Select Rocksmith album artwork", selected,
				"png;jpg;jpeg;bmp;tga;dds", sizeof(selected), 560, 420) && exists(selected))
			{
				ustrzcpy(eof_psarc_album_art, sizeof(eof_psarc_album_art), selected);
			}
			continue;
		}

		if(result == EOF_PSARC_WWISE_CHOOSE)
		{
			/* Allegro 4 has no portable directory chooser.  Let the user click
			 * WwiseCLI.exe inside the desired bin directory, then store/display
			 * only its parent folder. */
			selected[0] = '\0';
			if(eof_psarc_wwise_bin[0])
				replace_filename(selected, eof_psarc_wwise_bin, "WwiseCLI.exe", sizeof(selected));
			if(file_select_ex("Select WwiseCLI.exe inside the Wwise bin folder", selected,
				"exe", sizeof(selected), 560, 420) && exists(selected))
			{
				const char *filename = get_filename(selected);
				if(!filename || ustricmp(filename, "WwiseCLI.exe"))
				{
					allegro_message("Select WwiseCLI.exe from the Wwise bin folder.");
				}
				else
				{
					ustrzcpy(eof_psarc_wwise_bin, sizeof(eof_psarc_wwise_bin), selected);
					*get_filename(eof_psarc_wwise_bin) = '\0';
					if(!eof_psarc_validate_wwise())
					{
						allegro_message(
							"That Wwise bin folder is incomplete.\n\n"
							"EOF needs WwiseCLI.exe and tools\\AkCopyStreamedFiles.exe, exactly like the PSARC script."
						);
						eof_psarc_wwise_bin[0] = '\0';
					}
					else
					{
						eof_psarc_store_wwise();
					}
				}
			}
			continue;
		}

		if(result == 23)
		{
			if(!eof_psarc_platform_pc && !eof_psarc_platform_mac)
			{
				allegro_message("Select at least one PSARC target: PC or Mac.");
				continue;
			}
			if(!eof_psarc_validate_wwise())
			{
				allegro_message(
					"Wwise bin path is not set or is incomplete.\n\n"
					"Use Choose bin... and select WwiseCLI.exe from the Wwise bin folder.\n"
					"That folder must also contain tools\\AkCopyStreamedFiles.exe."
				);
				continue;
			}
			eof_psarc_store_wwise();

			/* RSToolkit is a pre-installed dependency.  Ask for it once, using its
			 * real executable as the anchor, then derive every toolkit executable
			 * and Template path from that one directory. */
			if(!eof_psarc_ensure_rstoolkit_root())
				continue;

			if(!eof_psarc_album_art[0] || !exists(eof_psarc_album_art))
			{
				allegro_message(
					"Album artwork is missing.\n\n"
					"Restore tools\\psarc\\default_album.png or choose another image before generating the PSARC."
				);
				continue;
			}
			if(!eof_psarc_copy_album_to_staging())
			{
				allegro_message("Could not copy the selected album artwork into the PSARC staging folder.");
				continue;
			}

			/* The helper is a child process of EOF, so these two variables are a
			 * simple way to pass the UI selection without changing the legacy JSON
			 * specification format.  The RSToolkit packer reads info.Pc/info.Mac. */
			eof_psarc_store_platform_environment();
		}

		if(result == 23 || result == 24 || result < 0)
		{
			eof_psarc_dialog_session_active = 0;
			eof_psarc_destroy_album_preview();
		}
		return result;
	}
}

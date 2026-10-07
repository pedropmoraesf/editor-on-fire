#include <allegro.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agup/agup.h"
#include "dialog.h"
#include "dialog/proc.h"
#include "main.h"
#include "menu/file.h"
#include "player.h"
#include "rs.h"
#include "spectrogram.h"
#include "utility.h"
#include "waveform.h"
#include "audio_normalize.h"

#define EOF_AUDIO_NORMALIZE_DEFAULT_LUFS (-14.0)
#define EOF_AUDIO_NORMALIZE_MIN_LUFS (-70.0)
#define EOF_AUDIO_NORMALIZE_MAX_LUFS (-5.0)

char eof_audio_normalize_target_text[32] = "-14.0";
int eof_audio_normalize_new_project_checked = 1;
int eof_audio_normalize_psarc_checked = 1;
int eof_audio_normalize_new_dialog_context = 0;
int eof_audio_normalize_skip_next_ogg_dialog = 0;
int eof_audio_normalize_pending_new_project = 0;

static char eof_audio_normalize_installed = 0;

static void eof_audio_normalize_make_work_path(const char *path, const char *suffix, char *out, size_t outsz)
{
	if(!out || !outsz)
		return;
	out[0] = '\0';
	if(!path || !suffix)
		return;
	(void)snprintf(out, outsz - 1U, "%s%s", path, suffix);
}

int eof_audio_normalize_parse_target(const char *text, double *value)
{
	char temp[64] = {0};
	char *end = NULL;
	double parsed;
	size_t i;

	if(!text || !text[0])
		return 0;
	ustrzcpy(temp, sizeof(temp), text);
	for(i = 0; temp[i]; i++)
	{
		if(temp[i] == ',')
			temp[i] = '.';
	}
	parsed = strtod(temp, &end);
	if(end == temp || (end && *end) || !isfinite(parsed))
		return 0;
	if(parsed < EOF_AUDIO_NORMALIZE_MIN_LUFS || parsed > EOF_AUDIO_NORMALIZE_MAX_LUFS)
		return 0;
	if(value)
		*value = parsed;
	return 1;
}

double eof_audio_normalize_target_value(void)
{
	double value = EOF_AUDIO_NORMALIZE_DEFAULT_LUFS;
	if(!eof_audio_normalize_parse_target(eof_audio_normalize_target_text, &value))
		value = EOF_AUDIO_NORMALIZE_DEFAULT_LUFS;
	return value;
}

int eof_audio_normalize_resolve_ffmpeg(int allow_prompt)
{
	char selected[1024] = {0};

	if(eof_ffmpeg_executable_path[0] && exists(eof_ffmpeg_executable_path))
		return 1;
	if(!allow_prompt)
		return 0;

	if(!file_select_ex("Locate ffmpeg.exe for audio normalization", selected, "exe", sizeof(selected), 560, 420))
		return 0;
	if(!exists(selected))
		return 0;
	ustrzcpy(eof_ffmpeg_executable_path, sizeof(eof_ffmpeg_executable_path), selected);
	set_config_string("paths", "eof_ffmpeg_executable_path", eof_ffmpeg_executable_path);
	flush_config_file();
	return 1;
}

static int eof_audio_normalize_run(const char *source, const char *dest, double target, int wav, int bitrate_kbps)
{
	char command[8192] = {0};

	if(!source || !dest || !exists(source) || !eof_audio_normalize_resolve_ffmpeg(0))
		return 0;
	if(wav)
	{
		(void)snprintf(command, sizeof(command) - 1U,
			"\"%s\" -y -hide_banner -loglevel error -i \"%s\" -af loudnorm=I=%.2f:TP=-1.0:LRA=11 -c:a pcm_s16le \"%s\"",
			eof_ffmpeg_executable_path, source, target, dest);
	}
	else
	{
		if(bitrate_kbps < 32 || bitrate_kbps > 512)
			bitrate_kbps = 192;
		(void)snprintf(command, sizeof(command) - 1U,
			"\"%s\" -y -hide_banner -loglevel error -i \"%s\" -af loudnorm=I=%.2f:TP=-1.0:LRA=11 -c:a libvorbis -b:a %dk \"%s\"",
			eof_ffmpeg_executable_path, source, target, bitrate_kbps, dest);
	}
	{
		int exitcode;
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1U,
			"Audio normalize: target %.2f LUFS, source %.700s", target, source);
		eof_log(eof_log_string, 1);
		exitcode = eof_system(command);
		if(exitcode != 0 || !exists(dest))
		{
			(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1U,
				"Audio normalize: ffmpeg loudnorm failed (exit=%d, output=%s).",
				exitcode, exists(dest) ? "present" : "missing");
			eof_log(eof_log_string, 1);
			return 0;
		}
	}
	return 1;
}

static int eof_audio_normalize_replace_file(const char *path, const char *normalized)
{
	char backup[1200] = {0};
	int had_original;

	if(!path || !normalized || !exists(normalized))
		return 0;
	eof_audio_normalize_make_work_path(path, ".eofnorm.bak", backup, sizeof(backup));
	if(exists(backup))
		delete_file(backup);
	had_original = exists(path) ? 1 : 0;
	if(had_original && !eof_copy_file(path, backup))
		return 0;
	if(had_original)
		delete_file(path);
	if(!eof_copy_file(normalized, path) || !exists(path))
	{
		if(exists(path))
			delete_file(path);
		if(had_original && exists(backup))
			(void)eof_copy_file(backup, path);
		if(exists(backup))
			delete_file(backup);
		return 0;
	}
	if(exists(backup))
		delete_file(backup);
	return 1;
}

static int eof_audio_normalize_is_psarc_staged_wav(const char *path)
{
	/* Compatibility safety net for older/stale PSARC hook objects: the staged
	 * Wwise WAV is already valid PCM before normalization.  If loudnorm fails,
	 * callers must keep this file instead of deleting it and aborting export. */
	if(!path)
		return 0;
	return (strstr(path, "eof_psarc_tmp") && strstr(path, "psarc_audio.wav")) ? 1 : 0;
}

int eof_audio_normalize_wav_in_place(const char *path, double target)
{
	char temp[1200] = {0};
	int result;
	if(!path || !exists(path))
		return 0;
	eof_audio_normalize_make_work_path(path, ".eofnorm.tmp.wav", temp, sizeof(temp));
	if(exists(temp))
		delete_file(temp);
	if(!eof_audio_normalize_run(path, temp, target, 1, 0))
	{
		if(exists(temp))
			delete_file(temp);
		if(eof_audio_normalize_is_psarc_staged_wav(path) && exists(path))
		{
			eof_log("PSARC audio normalize guard V4: loudnorm failed; staged WAV is intact and will be used unnormalized.", 1);
			return 1;
		}
		return 0;
	}
	result = eof_audio_normalize_replace_file(path, temp);
	if(exists(temp))
		delete_file(temp);
	if(!result && eof_audio_normalize_is_psarc_staged_wav(path) && exists(path))
	{
		eof_log("PSARC audio normalize guard V4: normalized replacement failed; original staged WAV is intact and will be used.", 1);
		return 1;
	}
	return result;
}

int eof_audio_normalize_ogg_path_in_place(const char *path, double target, int bitrate_kbps)
{
	char tempwav[1200] = {0};
	char tempogg[1200] = {0};
	char command[4096] = {0};
	const char *quality = "4.0";
	int result, encode_result;

	if(!path || !exists(path))
		return 0;

	/*
	 * Do not ask FFmpeg to encode Vorbis here.  Some FFmpeg builds linked by EOF
	 * can decode the project OGG and provide loudnorm/PCM, but do not contain
	 * libvorbis.  That made new-project normalization fail after the MP3->OGG
	 * conversion had already succeeded.
	 *
	 * Normalize to an ordinary PCM16 WAV first, then use EOF's established
	 * wavtoogg path (the same path used elsewhere by EOF and the DTX/GP audio
	 * workflows) to create the replacement OGG.  This keeps loudnorm in FFmpeg
	 * while removing FFmpeg's optional Vorbis encoder from the critical path.
	 */
	eof_audio_normalize_make_work_path(path, ".eofnorm.tmp.wav", tempwav, sizeof(tempwav));
	eof_audio_normalize_make_work_path(path, ".eofnorm.tmp.ogg", tempogg, sizeof(tempogg));
	if(exists(tempwav))
		delete_file(tempwav);
	if(exists(tempogg))
		delete_file(tempogg);

	if(!eof_audio_normalize_run(path, tempwav, target, 1, 0))
	{
		if(exists(tempwav))
			delete_file(tempwav);
		return 0;
	}

	if((eof_ogg_setting >= 0) && (eof_ogg_setting < 7) && eof_ogg_quality[eof_ogg_setting])
		quality = eof_ogg_quality[eof_ogg_setting];

#ifdef ALLEGRO_WINDOWS
	(void)snprintf(command, sizeof(command) - 1U,
		"wavtoogg \"%s\" %s \"%s\"", tempwav, quality, tempogg);
#else
	(void)snprintf(command, sizeof(command) - 1U,
		"oggenc --quiet -q %s --resample 44100 -s 0 \"%s\" -o \"%s\"",
		quality, tempwav, tempogg);
#endif
	(void)bitrate_kbps; /* Kept in the API for callers; EOF quality drives wavtoogg. */
	eof_log("Audio normalize: loudnorm WAV rendered; converting through EOF's WAV-to-OGG path.", 1);
	encode_result = eof_system(command);
	if(encode_result != 0 || !exists(tempogg) || (file_size_ex(tempogg) <= 0))
	{
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1U,
			"Audio normalize: WAV-to-OGG helper failed (exit=%d); trying FFmpeg OGG fallback.",
			encode_result);
		eof_log(eof_log_string, 1);
		if(exists(tempogg))
			delete_file(tempogg);

		/* eof_ffmpeg_convert_file() lets FFmpeg choose an encoder from the .ogg
		 * extension instead of forcing optional libvorbis.  This covers builds
		 * where wavtoogg/oggenc2 is absent while FFmpeg's native Vorbis encoder is
		 * present. */
		encode_result = eof_ffmpeg_convert_file(tempwav, tempogg);
		if(encode_result != 0 || !exists(tempogg) || (file_size_ex(tempogg) <= 0))
		{
			(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1U,
				"Audio normalize: both OGG encoder paths failed (fallback=%d, output=%s).",
				encode_result, exists(tempogg) ? "present" : "missing");
			eof_log(eof_log_string, 1);
			if(exists(tempwav))
				delete_file(tempwav);
			if(exists(tempogg))
				delete_file(tempogg);
			return 0;
		}
		eof_log("Audio normalize: FFmpeg OGG fallback succeeded.", 1);
	}

	result = eof_audio_normalize_replace_file(path, tempogg);
	if(exists(tempwav))
		delete_file(tempwav);
	if(exists(tempogg))
		delete_file(tempogg);
	if(result)
		eof_log("Audio normalize: normalized OGG installed successfully.", 1);
	return result;
}

static void eof_audio_normalize_store_settings(void)
{
	set_config_string("audio_normalize", "target_lufs", eof_audio_normalize_target_text);
	set_config_int("audio_normalize", "new_project", eof_audio_normalize_new_project_checked ? 1 : 0);
	set_config_int("audio_normalize", "psarc", eof_audio_normalize_psarc_checked ? 1 : 0);
	flush_config_file();
}

int eof_audio_normalize_ogg_settings_popup(DIALOG *dialog, int focus)
{
	DIALOG ext[10];
	int result;
	double target;
	(void)focus;

	if(!dialog)
		return -1;
	if(eof_audio_normalize_skip_next_ogg_dialog)
	{
		eof_audio_normalize_skip_next_ogg_dialog = 0;
		return 4; /* Original OGG Settings OK button index. */
	}
	if(!eof_audio_normalize_new_dialog_context)
		return eof_popup_dialog(dialog, focus);

	for(;;)
	{
		memset(ext, 0, sizeof(ext));
		memcpy(ext, dialog, 6U * sizeof(DIALOG));

		/* Preserve the original indices (3=list, 4=OK, 5=Cancel), but rebuild
		 * the geometry from a clean local origin.  The stock dialog can carry
		 * D_USER and moved absolute coordinates from a previous drag; mixing those
		 * with newly appended absolute LUFS controls made the latter render outside
		 * the window. */
		ext[0].x = 0; ext[0].y = 0; ext[0].w = 220; ext[0].h = 296;
		ext[0].flags &= ~D_USER;
		ext[1].x = 66; ext[1].y = 10;
		ext[2].x = 66; ext[2].y = 38;
		ext[3].x = 44; ext[3].y = 54; ext[3].w = 132; ext[3].h = 126;
		ext[4].x = 34; ext[4].y = 256;
		ext[5].x = 118; ext[5].y = 256;

		ext[6].proc = d_agup_check_proc;
		ext[6].x = 24; ext[6].y = 192; ext[6].w = 172; ext[6].h = 18;
		ext[6].fg = 2; ext[6].bg = 23;
		ext[6].flags = eof_audio_normalize_new_project_checked ? D_SELECTED : 0;
		ext[6].d1 = 1; ext[6].dp = (void *)"Normalize audio";

		ext[7].proc = d_agup_text_proc;
		ext[7].x = 26; ext[7].y = 222; ext[7].w = 92; ext[7].h = 12;
		ext[7].fg = 2; ext[7].bg = 23; ext[7].dp = (void *)"Target LUFS:";

		ext[8].proc = eof_edit_proc;
		ext[8].x = 116; ext[8].y = 217; ext[8].w = 72; ext[8].h = 20;
		ext[8].fg = 2; ext[8].bg = 23; ext[8].d1 = 16; ext[8].dp = eof_audio_normalize_target_text;
		ext[9].proc = NULL;

		eof_color_dialog(ext, gui_fg_color, gui_bg_color);
		eof_conditionally_center_dialog(ext);
		result = eof_popup_dialog(ext, 0);
		dialog[3].d1 = ext[3].d1;
		eof_audio_normalize_new_project_checked = (ext[6].flags & D_SELECTED) ? 1 : 0;
		if(result != 4)
			return result;
		if(eof_audio_normalize_new_project_checked)
		{
			if(!eof_audio_normalize_parse_target(eof_audio_normalize_target_text, &target))
			{
				allegro_message("Enter a loudness target between -70 and -5 LUFS.");
				continue;
			}
			if(!eof_audio_normalize_resolve_ffmpeg(1))
			{
				allegro_message("Audio normalization requires ffmpeg.exe.\nUncheck Normalize audio or select ffmpeg.exe.");
				continue;
			}
		}
		eof_audio_normalize_store_settings();
		return result;
	}
}

int eof_audio_normalize_intercept_load_ogg(char *filename, char function)
{
	if(eof_audio_normalize_pending_new_project && filename && exists(filename))
	{
		double target = eof_audio_normalize_target_value();
		int bitrate = eof_ogg_list_bitrate(eof_ogg_setting);
		eof_audio_normalize_pending_new_project = 0;
		if(!eof_audio_normalize_ogg_path_in_place(filename, target, bitrate))
		{
			eof_log("New project audio normalization failed; loading the unnormalized project audio.", 1);
			allegro_message("Audio normalization failed.\nThe project will use the unnormalized audio.\nSee eof_log.txt for details.");
		}
	}
	return eof_load_ogg(filename, function);
}

int eof_audio_normalize_new_wizard(void)
{
	char *returnedfn = NULL;
	int result;

	eof_log("eof_audio_normalize_new_wizard() entered", 1);
	if(eof_menu_prompt_save_changes() == 3)
		return 1;
	eof_cursor_visible = 0;
	eof_pen_visible = 0;
	eof_render();
	if(exists(eof_ffmpeg_executable_path))
		returnedfn = ncd_file_select(0, eof_last_ogg_path, "Select Music File", eof_filter_ffmpeg_files);
	else
		returnedfn = ncd_file_select(0, eof_last_ogg_path, "Select Music File", eof_filter_music_files);
	eof_clear_input();
	if(!returnedfn)
	{
		eof_cursor_visible = 1;
		eof_pen_visible = 1;
		eof_show_mouse(NULL);
		return 1;
	}

	/* Show OGG quality plus loudness controls immediately after the source audio
	 * is selected.  MP3/WAV conversion later calls eof_ogg_settings() again; the
	 * file.c hook suppresses that second popup while preserving the chosen quality. */
	eof_audio_normalize_new_dialog_context = 1;
	if(!eof_ogg_settings())
	{
		eof_audio_normalize_new_dialog_context = 0;
		eof_cursor_visible = 1;
		eof_pen_visible = 1;
		eof_show_mouse(NULL);
		return 1;
	}
	eof_audio_normalize_new_dialog_context = 0;
	eof_audio_normalize_skip_next_ogg_dialog = 1;
	eof_audio_normalize_pending_new_project = eof_audio_normalize_new_project_checked ? 1 : 0;
	result = eof_new_chart(returnedfn);
	eof_audio_normalize_skip_next_ogg_dialog = 0;
	eof_audio_normalize_pending_new_project = 0;
	return result;
}

static int eof_audio_normalize_target_dialog(void)
{
	DIALOG dlg[] =
	{
		{ eof_window_proc, 0, 0, 270, 128, 2, 23, 0, 0, 0, 0, (void *)"Normalize audio", NULL, NULL },
		{ d_agup_text_proc, 16, 38, 116, 14, 2, 23, 0, 0, 0, 0, (void *)"Target loudness (LUFS):", NULL, NULL },
		{ eof_edit_proc, 166, 34, 76, 20, 2, 23, 0, 0, 16, 0, eof_audio_normalize_target_text, NULL, NULL },
		{ d_agup_text_proc, 16, 62, 230, 14, 2, 23, 0, 0, 0, 0, (void *)"Typical streaming target: -14.0 LUFS", NULL, NULL },
		{ d_agup_button_proc, 50, 88, 72, 26, 2, 23, KEY_ENTER, D_EXIT, 0, 0, (void *)"Normalize", NULL, NULL },
		{ d_agup_button_proc, 148, 88, 72, 26, 2, 23, KEY_ESC, D_EXIT, 0, 0, (void *)"Cancel", NULL, NULL },
		{ NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, NULL }
	};
	double target;
	int result;

	for(;;)
	{
		eof_color_dialog(dlg, gui_fg_color, gui_bg_color);
		eof_conditionally_center_dialog(dlg);
		result = eof_popup_dialog(dlg, 2);
		if(result != 4)
			return 0;
		if(!eof_audio_normalize_parse_target(eof_audio_normalize_target_text, &target))
		{
			allegro_message("Enter a loudness target between -70 and -5 LUFS.");
			continue;
		}
		if(!eof_audio_normalize_resolve_ffmpeg(1))
		{
			allegro_message("Audio normalization requires ffmpeg.exe.");
			continue;
		}
		eof_audio_normalize_store_settings();
		return 1;
	}
}

int eof_menu_song_normalize_audio(void)
{
	char path[1024] = {0};
	unsigned long oldpos;
	int bitrate;
	double target;

	if(!eof_song || !eof_song_loaded || eof_silence_loaded || !eof_loaded_ogg_name[0] || !exists(eof_loaded_ogg_name))
	{
		allegro_message("No project audio is available to normalize.");
		return D_O_K;
	}
	if(!eof_audio_normalize_target_dialog())
		return D_O_K;

	ustrzcpy(path, sizeof(path), eof_loaded_ogg_name);
	target = eof_audio_normalize_target_value();
	bitrate = eof_ogg_list_bitrate(eof_ogg_setting);
	oldpos = eof_music_pos.value;
	if(!eof_music_paused)
		eof_music_play(0);

	eof_cursor_visible = 0;
	eof_pen_visible = 0;
	eof_render();
	if(!eof_audio_normalize_ogg_path_in_place(path, target, bitrate))
	{
		eof_cursor_visible = 1;
		eof_pen_visible = 1;
		allegro_message("Audio normalization failed.\nThe project audio was not replaced.\nSee eof_log.txt for details.");
		return D_O_K;
	}

	/* Reload the newly encoded OGG so playback, waveform/spectrogram and all
	 * later exporters observe the normalized project audio. */
	eof_destroy_waveform(eof_waveform);
	eof_waveform = NULL;
	eof_destroy_spectrogram(eof_spectrogram);
	eof_spectrogram = NULL;
	(void)eof_destroy_ogg();
	if(!eof_load_ogg(path, 0))
	{
		eof_cursor_visible = 1;
		eof_pen_visible = 1;
		allegro_message("The normalized OGG was written, but EOF could not reload it.\nReload the project audio before continuing.");
		return D_O_K;
	}
	eof_delete_rocksmith_wav();
	if(oldpos > eof_music_length)
		oldpos = eof_music_length;
	eof_music_pos.value = oldpos;
	eof_music_seek(EOF_SEEK_POS);
	eof_cursor_visible = 1;
	eof_pen_visible = 1;
	eof_render();
	allegro_message("Project audio normalized to %.2f LUFS.", target);
	return D_O_K;
}

void eof_audio_normalize_install(void)
{
	const char *saved;
	double value;

	if(eof_audio_normalize_installed)
		return;
	saved = get_config_string("audio_normalize", "target_lufs", "-14.0");
	if(saved && eof_audio_normalize_parse_target(saved, &value))
		ustrzcpy(eof_audio_normalize_target_text, sizeof(eof_audio_normalize_target_text), saved);
	else
		ustrzcpy(eof_audio_normalize_target_text, sizeof(eof_audio_normalize_target_text), "-14.0");
	eof_audio_normalize_new_project_checked = get_config_int("audio_normalize", "new_project", 1) ? 1 : 0;
	eof_audio_normalize_psarc_checked = get_config_int("audio_normalize", "psarc", 1) ? 1 : 0;

	/* Replace only File > New.  The original eof_menu_file_new_wizard() remains
	 * intact and all project creation logic after source selection is still EOF's. */
	eof_file_menu[0].proc = eof_audio_normalize_new_wizard;
	eof_audio_normalize_installed = 1;
}

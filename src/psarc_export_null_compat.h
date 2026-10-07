#ifndef EOF_PSARC_EXPORT_NULL_COMPAT_H
#define EOF_PSARC_EXPORT_NULL_COMPAT_H

/*
 * This header is force-included only for psarc_export.c.  Load the translation
 * unit's normal declarations first so the macros below affect call sites but
 * cannot rewrite any library prototypes.
 */
#include <allegro.h>
#include <alogg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agup/agup.h"
#include "main.h"
#include "dialog.h"
#include "utility.h"
#include "beat.h"
#include "rs.h"
#include "menu/file.h"
#include "menu/track.h"
#include "tone_analysis.h"
#include "tone_workflow.h"
#include "psarc_export.h"
#include "psarc_dialog.h"

#define EOF_PSARC_INTERNAL_AUDIO       "EOF_INTERNAL_AUDIO"
#define EOF_PSARC_INTERNAL_FLUIDSYNTH  "EOF_INTERNAL_FLUIDSYNTH"
#define EOF_PSARC_INTERNAL_SOUNDFONT   "EOF_INTERNAL_SOUNDFONT"

static int eof_psarc_internal_click_bpm = 120;

/*
 * PSARC audio preparation is self-contained.  The legacy psarc_export.c body
 * still asks for ffmpeg/FluidSynth paths, but for this translation unit these
 * three config lookups are intentionally mapped to internal backends.  This
 * keeps the exporter compatible with the existing dialog/call structure while
 * removing external FFmpeg, FluidSynth and SoundFont requirements from users.
 */
static const char *eof_psarc_get_config_string_compat(const char *section, const char *key, const char *def)
{
	if(section && key && !ustricmp(section, "paths") && !ustricmp(key, "eof_ffmpeg_executable_path"))
		return EOF_PSARC_INTERNAL_AUDIO;
	if(section && key && !ustricmp(section, "psarc"))
	{
		if(!ustricmp(key, "fluidsynth"))
			return EOF_PSARC_INTERNAL_FLUIDSYNTH;
		if(!ustricmp(key, "soundfont"))
			return EOF_PSARC_INTERNAL_SOUNDFONT;
	}
	return get_config_string(section, key, def);
}

static void eof_psarc_parent_directory(char *path)
{
	size_t len;
	char *name;
	if(!path || !path[0])
		return;
	len = strlen(path);
	while(len && (path[len - 1] == '/' || path[len - 1] == '\\'))
		path[--len] = '\0';
	name = get_filename(path);
	if(name && name > path)
		*name = '\0';
}

/*
 * Use the exact installed layout used by the supplied Python generator.  EOF
 * asks for one executable from the main RSToolkit folder and resolves all
 * RSToolkit-side tools/assets from that parent directory:
 *
 *   RocksmithToolkitLib.dll
 *   packer.exe
 *   ddc\ddc.exe
 *   Template\Template.wproj
 *
 * Wwise itself remains a separate installation, exactly as in the Python
 * script, and its bin directory is selected in the PSARC dialog.
 */
static int eof_psarc_is_toolkit_support_root(const char *root)
{
	char candidate[1024] = {0};

	if(!root || !root[0] || !eof_folder_exists(root))
		return 0;

	(void)snprintf(candidate, sizeof(candidate) - 1, "%s%cRocksmithToolkitLib.dll", root, OTHER_PATH_SEPARATOR);
	fix_filename_slashes(candidate);
	if(!exists(candidate))
		return 0;

	(void)snprintf(candidate, sizeof(candidate) - 1, "%s%cpacker.exe", root, OTHER_PATH_SEPARATOR);
	fix_filename_slashes(candidate);
	if(!exists(candidate))
		return 0;

	(void)snprintf(candidate, sizeof(candidate) - 1, "%s%cddc%cddc.exe", root,
		OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR);
	fix_filename_slashes(candidate);
	if(!exists(candidate))
		return 0;

	(void)snprintf(candidate, sizeof(candidate) - 1, "%s%cTemplate%cTemplate.wproj", root,
		OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR);
	fix_filename_slashes(candidate);
	return exists(candidate) ? 1 : 0;
}

static int eof_psarc_toolkit_dll_from_root(const char *root, char *out, size_t outsz)
{
	char dll[1024] = {0};
	if(!root || !out || !outsz || !eof_psarc_is_toolkit_support_root(root))
		return 0;
	(void)snprintf(dll, sizeof(dll) - 1, "%s%cRocksmithToolkitLib.dll", root, OTHER_PATH_SEPARATOR);
	fix_filename_slashes(dll);
	if(!exists(dll))
		return 0;
	ustrzcpy(out, (int)outsz, dll);
	return 1;
}

/*
 * psarc_export.c still calls its legacy "select RocksmithToolkitLib.dll" path.
 * Intercept that selector only.  The user selects the RSToolkit GUI executable
 * (or another EXE in the same top-level folder), EOF derives the directory and
 * returns the DLL marker expected by the legacy caller.  The caller stores only
 * rstoolkit_root in eof.cfg, so this prompt is normally seen once.
 */
static int eof_psarc_file_select_ex_compat(const char *message, char *path, const char *ext,
	int size, int w, int h)
{
	if(message && !ustricmp(message, "Locate RocksmithToolkitLib.dll (RSToolkit)"))
	{
		char selected[1024] = {0}, root[1024] = {0};
		if(!file_select_ex("Select an executable from the main RSToolkit folder", selected,
			"exe", sizeof(selected), w, h))
			return 0;
		if(!exists(selected))
			return 0;

		ustrzcpy(root, sizeof(root), selected);
		eof_psarc_parent_directory(root);
		if(!eof_psarc_toolkit_dll_from_root(root, path, (size_t)size))
		{
			eof_log("PSARC: selected executable is not inside the required RSToolkit installation layout.", 1);
			allegro_message("That executable is not in the required RSToolkit folder.\n\n"
				"Select an EXE from the main RocksmithToolkit folder.\n"
				"The same folder must contain RocksmithToolkitLib.dll and packer.exe,\n"
				"and it must contain ddc\\ddc.exe plus Template\\Template.wproj.");
			return 0;
		}

		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"PSARC RSToolkit root selected from executable: %.900s", root);
		eof_log(eof_log_string, 1);
		return 1;
	}
	return file_select_ex(message, path, ext, size, w, h);
}

static int eof_psarc_is_internal_path(const char *path)
{
	if(!path)
		return 0;
	return (!strcmp(path, EOF_PSARC_INTERNAL_AUDIO) ||
		!strcmp(path, EOF_PSARC_INTERNAL_FLUIDSYNTH) ||
		!strcmp(path, EOF_PSARC_INTERNAL_SOUNDFONT)) ? 1 : 0;
}

/* RocksmithToolkitLib.dll is a valid legacy marker only when it belongs to a
 * complete RSToolkit installation.  Virtual internal-audio paths are reported
 * as present so the legacy prompting code never asks for FFmpeg/FluidSynth. */
static int eof_psarc_exists_compat(const char *path)
{
	char root[1024] = {0};
	const char *filename;
	if(eof_psarc_is_internal_path(path))
		return 1;
	if(!path || !exists(path))
		return 0;
	filename = get_filename(path);
	if(!filename || ustricmp(filename, "RocksmithToolkitLib.dll"))
		return 1;
	ustrzcpy(root, sizeof(root), path);
	eof_psarc_parent_directory(root);
	return eof_psarc_is_toolkit_support_root(root);
}

static void eof_psarc_put_le16_compat(FILE *fp, unsigned long value)
{
	fputc((int)(value & 0xFFUL), fp);
	fputc((int)((value >> 8) & 0xFFUL), fp);
}

static void eof_psarc_put_le32_compat(FILE *fp, unsigned long value)
{
	fputc((int)(value & 0xFFUL), fp);
	fputc((int)((value >> 8) & 0xFFUL), fp);
	fputc((int)((value >> 16) & 0xFFUL), fp);
	fputc((int)((value >> 24) & 0xFFUL), fp);
}

static int eof_psarc_write_s16_stereo_compat(FILE *fp, int left, int right)
{
	unsigned short l = (unsigned short)((short)left);
	unsigned short r = (unsigned short)((short)right);
	if(fputc((int)(l & 0xFFU), fp) == EOF) return 0;
	if(fputc((int)((l >> 8) & 0xFFU), fp) == EOF) return 0;
	if(fputc((int)(r & 0xFFU), fp) == EOF) return 0;
	if(fputc((int)((r >> 8) & 0xFFU), fp) == EOF) return 0;
	return 1;
}

static int eof_psarc_write_prefix_compat(FILE *fp, unsigned long frames, unsigned long rate,
	int sticks, int bpm, unsigned long *data_bytes)
{
	unsigned long i, beat_frames = 0, click_frames = 0, wave_half = 1;
	if(!fp || !data_bytes)
		return 0;
	if(sticks)
	{
		if(bpm < 30 || bpm > 300)
			bpm = 120;
		beat_frames = (rate * 60UL) / (unsigned long)bpm;
		if(!beat_frames) beat_frames = 1;
		click_frames = rate / 40UL; /* 25 ms transient */
		if(!click_frames) click_frames = 1;
		wave_half = rate / 6000UL;  /* roughly 3 kHz square-wave click */
		if(!wave_half) wave_half = 1;
	}

	for(i = 0; i < frames; i++)
	{
		int sample = 0;
		if(sticks)
		{
			unsigned long pos = i % beat_frames;
			if(pos < click_frames)
			{
				long amp = (26000L * (long)(click_frames - pos)) / (long)click_frames;
				sample = ((pos / wave_half) & 1UL) ? (int)-amp : (int)amp;
			}
		}
		if(!eof_psarc_write_s16_stereo_compat(fp, sample, sample))
			return 0;
		*data_bytes += 4UL;
	}
	return 1;
}

/* Decode EOF's project OGG directly through the alogg/Vorbis library that EOF
 * already links.  Wwise receives ordinary 16-bit stereo PCM WAV.  This avoids
 * a second external transcoder and also avoids Windows command-line quoting
 * problems that were causing the previous FFmpeg preparation failure. */
static int eof_psarc_internal_ogg_to_wav_compat(const char *source, const char *dest,
	unsigned long prefix_ms, int sticks, int bpm)
{
	FILE *in = NULL, *out = NULL;
	ALOGG_OGG *ogg = NULL;
	unsigned char buffer[65536];
	unsigned long rate, prefix_frames, data_bytes = 0;
	int stereo, failed = 0;
	long got;

	if(!source || !dest)
		return 0;
	in = fopen(source, "rb");
	if(!in)
	{
		eof_log("PSARC internal WAV: could not open project OGG.", 1);
		return 0;
	}
	ogg = alogg_create_ogg_from_file(in);
	if(!ogg)
	{
		fclose(in);
		eof_log("PSARC internal WAV: alogg could not decode the project audio.", 1);
		return 0;
	}

	rate = (unsigned long)alogg_get_wave_freq_ogg(ogg);
	if(rate < 8000UL || rate > 192000UL)
		rate = 44100UL;
	stereo = alogg_get_wave_is_stereo_ogg(ogg) ? 1 : 0;
	out = fopen(dest, "wb");
	if(!out)
	{
		alogg_destroy_ogg(ogg); /* ov_clear() closes the FILE opened above */
		eof_log("PSARC internal WAV: could not create the staged WAV file.", 1);
		return 0;
	}

	fwrite("RIFF", 1, 4, out);
	eof_psarc_put_le32_compat(out, 36UL);
	fwrite("WAVE", 1, 4, out);
	fwrite("fmt ", 1, 4, out);
	eof_psarc_put_le32_compat(out, 16UL);
	eof_psarc_put_le16_compat(out, 1UL); /* PCM */
	eof_psarc_put_le16_compat(out, 2UL); /* stereo */
	eof_psarc_put_le32_compat(out, rate);
	eof_psarc_put_le32_compat(out, rate * 4UL);
	eof_psarc_put_le16_compat(out, 4UL);
	eof_psarc_put_le16_compat(out, 16UL);
	fwrite("data", 1, 4, out);
	eof_psarc_put_le32_compat(out, 0UL);

	prefix_frames = (unsigned long)(((double)prefix_ms * (double)rate) / 1000.0 + 0.5);
	if(prefix_frames && !eof_psarc_write_prefix_compat(out, prefix_frames, rate, sticks, bpm, &data_bytes))
		failed = 1;

	while(!failed)
	{
		unsigned long i, usable;
		got = alogg_partial_read(ogg, (char *)buffer, (int)sizeof(buffer));
		if(got == 0)
			break;
		if(got < 0)
		{
			eof_log("PSARC internal WAV: Vorbis decode failed while reading project audio.", 1);
			failed = 1;
			break;
		}
		usable = (unsigned long)got & ~1UL;
		if(stereo)
		{
			usable &= ~3UL;
			for(i = 0; i + 3UL < usable; i += 4UL)
			{
				unsigned short lu = (unsigned short)(buffer[i] | ((unsigned short)buffer[i + 1UL] << 8));
				unsigned short ru = (unsigned short)(buffer[i + 2UL] | ((unsigned short)buffer[i + 3UL] << 8));
				int left = (int)lu - 32768;
				int right = (int)ru - 32768;
				if(!eof_psarc_write_s16_stereo_compat(out, left, right))
				{
					failed = 1;
					break;
				}
				data_bytes += 4UL;
			}
		}
		else
		{
			for(i = 0; i + 1UL < usable; i += 2UL)
			{
				unsigned short u = (unsigned short)(buffer[i] | ((unsigned short)buffer[i + 1UL] << 8));
				int sample = (int)u - 32768;
				if(!eof_psarc_write_s16_stereo_compat(out, sample, sample))
				{
					failed = 1;
					break;
				}
				data_bytes += 4UL;
			}
		}
	}

	if(!failed && ferror(out))
		failed = 1;
	if(!failed)
	{
		if(fseek(out, 4L, SEEK_SET) || ferror(out))
			failed = 1;
		else
			eof_psarc_put_le32_compat(out, 36UL + data_bytes);
		if(!failed && (fseek(out, 40L, SEEK_SET) || ferror(out)))
			failed = 1;
		else if(!failed)
			eof_psarc_put_le32_compat(out, data_bytes);
	}

	fclose(out);
	alogg_destroy_ogg(ogg); /* closes input FILE */
	if(failed)
	{
		remove(dest);
		eof_log("PSARC internal WAV: failed while writing PCM WAV data.", 1);
		return 0;
	}

	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"PSARC internal WAV prepared: %.800s (%lu Hz, %lu prefix ms, %s)",
		dest, rate, prefix_ms, sticks ? "stick count-in" : "silence");
	eof_log(eof_log_string, 1);
	return 1;
}

static int eof_psarc_get_quoted_arg_compat(const char *command, int wanted, char *out, size_t outsz)
{
	const char *p, *begin, *end;
	int index = 0;
	if(!command || !out || !outsz || wanted < 0)
		return 0;
	out[0] = '\0';
	p = command;
	while((begin = strchr(p, '"')) != NULL)
	{
		begin++;
		end = strchr(begin, '"');
		if(!end)
			return 0;
		if(index == wanted)
		{
			size_t len = (size_t)(end - begin);
			if(len >= outsz) len = outsz - 1U;
			memcpy(out, begin, len);
			out[len] = '\0';
			return 1;
		}
		index++;
		p = end + 1;
	}
	return 0;
}

static int eof_psarc_get_last_quoted_arg_compat(const char *command, char *out, size_t outsz)
{
	const char *p, *begin, *end;
	int found = 0;
	if(!command || !out || !outsz)
		return 0;
	out[0] = '\0';
	p = command;
	while((begin = strchr(p, '"')) != NULL)
	{
		begin++;
		end = strchr(begin, '"');
		if(!end)
			break;
		{
			size_t len = (size_t)(end - begin);
			if(len >= outsz) len = outsz - 1U;
			memcpy(out, begin, len);
			out[len] = '\0';
			found = 1;
		}
		p = end + 1;
	}
	return found;
}

static int eof_psarc_get_input_arg_compat(const char *command, int wanted, char *out, size_t outsz)
{
	const char *p, *begin, *end;
	int index = 0;
	if(!command || !out || !outsz || wanted < 0)
		return 0;
	out[0] = '\0';
	p = command;
	while((p = strstr(p, "-i \"")) != NULL)
	{
		begin = p + 4;
		end = strchr(begin, '"');
		if(!end)
			return 0;
		if(index == wanted)
		{
			size_t len = (size_t)(end - begin);
			if(len >= outsz) len = outsz - 1U;
			memcpy(out, begin, len);
			out[len] = '\0';
			return 1;
		}
		index++;
		p = end + 1;
	}
	return 0;
}

static int eof_psarc_read_click_bpm_compat(const char *midi)
{
	FILE *fp;
	int a = -1, b = -1, c = -1, d = -1;
	if(!midi || !(fp = fopen(midi, "rb")))
		return 120;
	while((d = fgetc(fp)) != EOF)
	{
		a = b;
		b = c;
		c = d;
		if(a == 0xFF && b == 0x51 && c == 0x03)
		{
			int t1 = fgetc(fp), t2 = fgetc(fp), t3 = fgetc(fp);
			unsigned long mpqn;
			fclose(fp);
			if(t1 == EOF || t2 == EOF || t3 == EOF)
				return 120;
			mpqn = ((unsigned long)t1 << 16) | ((unsigned long)t2 << 8) | (unsigned long)t3;
			if(mpqn)
			{
				int bpm = (int)(60000000UL / mpqn);
				if(bpm >= 30 && bpm <= 300)
					return bpm;
			}
			return 120;
		}
	}
	fclose(fp);
	return 120;
}

static int eof_psarc_internal_fluidsynth_command_compat(const char *command)
{
	char midi[1024] = {0}, placeholder[1024] = {0};
	FILE *fp;
	if(!eof_psarc_get_quoted_arg_compat(command, 2, midi, sizeof(midi)) ||
	   !eof_psarc_get_quoted_arg_compat(command, 3, placeholder, sizeof(placeholder)))
		return -1;
	eof_psarc_internal_click_bpm = eof_psarc_read_click_bpm_compat(midi);
	fp = fopen(placeholder, "wb");
	if(!fp)
		return -1;
	fwrite("EOF internal count-in", 1, 21, fp);
	fclose(fp);
	return 0;
}

static int eof_psarc_internal_audio_command_compat(const char *command)
{
	char source[1024] = {0}, output[1024] = {0};
	const char *duration;
	unsigned long prefix_ms = 0;
	int sticks = 0, source_index = 0;
	double seconds = 0.0;

	if(!command)
		return -1;
	sticks = strstr(command, "atrim=duration=") ? 1 : 0;
	if(sticks)
	{
		source_index = 1; /* input 0 is the legacy click placeholder */
		duration = strstr(command, "atrim=duration=");
		if(duration)
			seconds = strtod(duration + strlen("atrim=duration="), NULL);
	}
	else
	{
		duration = strstr(command, " -t ");
		if(duration)
			seconds = strtod(duration + 4, NULL);
	}
	if(seconds > 0.0)
		prefix_ms = (unsigned long)(seconds * 1000.0 + 0.5);

	if(!eof_psarc_get_input_arg_compat(command, source_index, source, sizeof(source)) ||
	   !eof_psarc_get_last_quoted_arg_compat(command, output, sizeof(output)))
	{
		eof_log("PSARC internal WAV: could not parse the legacy audio preparation command.", 1);
		return -1;
	}
	return eof_psarc_internal_ogg_to_wav_compat(source, output, prefix_ms, sticks,
		sticks ? eof_psarc_internal_click_bpm : 120) ? 0 : -1;
}

/* psarc_export.c still issues its old system() commands.  Intercept only the
 * two virtual audio tools above; PowerShell, the RSToolkit helper and all other
 * commands continue through the normal C runtime system(). */
static int eof_psarc_system_compat(const char *command)
{
	int result;
	if(!command)
		return -1;
	if(strstr(command, "\"" EOF_PSARC_INTERNAL_AUDIO "\"") == command)
		return eof_psarc_internal_audio_command_compat(command);
	if(strstr(command, "\"" EOF_PSARC_INTERNAL_FLUIDSYNTH "\"") == command)
		return eof_psarc_internal_fluidsynth_command_compat(command);

	result = system(command);
	if(result != 0)
	{
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"PSARC external command failed with exit code %d: %.900s", result, command);
		eof_log(eof_log_string, 1);
	}
	return result;
}

/* MinGW defines NULL as ((void *)0).  The legacy DIALOG sentinel in
 * psarc_export.c has integer d1/d2 fields immediately before its pointer fields,
 * so use C's integer null-pointer constant for this one translation unit. */
#ifdef NULL
#undef NULL
#endif
#define NULL 0

#define get_config_string(section, key, def) eof_psarc_get_config_string_compat((section), (key), (def))
#define file_select_ex(message, path, ext, size, w, h) eof_psarc_file_select_ex_compat((message), (path), (ext), (size), (w), (h))
#define exists(path) eof_psarc_exists_compat((path))
#define system(command) eof_psarc_system_compat((command))
#define eof_popup_dialog(dialog, focus) eof_psarc_popup_dialog((dialog), (focus))

#endif

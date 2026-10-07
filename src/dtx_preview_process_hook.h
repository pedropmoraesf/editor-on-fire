#ifndef EOF_DTX_PREVIEW_PROCESS_HOOK_H
#define EOF_DTX_PREVIEW_PROCESS_HOOK_H

#include <allegro.h>
#include <stdio.h>
#include <string.h>

/* Load EOF's declaration before redefining eof_system() for dtx_preview.c. */
#include "utility.h"
#include "main.h"

static int eof_dtx_preview_copy_token(const char *start, char *out, size_t outsize, const char **after)
{
	const char *end;
	size_t len;

	if(!start || !out || (outsize < 2))
		return 0;

	if(*start == '"')
	{
		end = strchr(start + 1, '"');
		if(!end)
			return 0;
		start++;
	}
	else
	{
		end = strchr(start, ' ');
		if(!end)
			end = start + strlen(start);
	}

	len = (size_t)(end - start);
	if(len >= outsize)
		return 0;
	memcpy(out, start, len);
	out[len] = '\0';
	if(after)
		*after = (*end == '"') ? end + 1 : end;
	return 1;
}

static int eof_dtx_preview_copy_quoted_after(const char *command, const char *marker, char *out, size_t outsize)
{
	const char *start, *end;
	size_t len;

	if(!command || !marker || !out || (outsize < 2))
		return 0;
	start = strstr(command, marker);
	if(!start)
		return 0;
	start += strlen(marker);
	end = strchr(start, '"');
	if(!end)
		return 0;
	len = (size_t)(end - start);
	if(len >= outsize)
		return 0;
	memcpy(out, start, len);
	out[len] = '\0';
	return 1;
}

/* On Windows, use the same WAV -> OGG path that EOF already uses when a new
 * project is created from a WAV file.  eof_audio_to_ogg() ultimately runs:
 *
 *     wavtoogg "input.wav" <quality> "output.ogg"
 *
 * and bin/wavtoogg.bat invokes the bundled oggenc2.exe.  That path is already
 * proven to work in the user's installation, whereas launching oggenc2.exe
 * directly from the DTX preview code returns exit code 1.  Quality 4 is the
 * Vorbis quality setting closest to the Python export's ~128 kbps target.
 */
static int eof_dtx_preview_system_process_hook(const char *command)
{
#ifdef ALLEGRO_WINDOWS
	char exe[2048] = {0}, wav[4096] = {0}, dest[4096] = {0}, batch_command[8192] = {0};
	const char *name;
	int result;

	if(command && eof_dtx_preview_copy_token(command, exe, sizeof(exe), NULL))
	{
		name = get_filename(exe);
		if(name && (!ustricmp(name, "oggenc2") || !ustricmp(name, "oggenc2.exe")) &&
		   strstr(command, " -b 128 ") &&
		   eof_dtx_preview_copy_quoted_after(command, " -s 0 \"", wav, sizeof(wav)) &&
		   eof_dtx_preview_copy_quoted_after(command, " -o \"", dest, sizeof(dest)))
		{
			remove(dest);
			(void)snprintf(batch_command, sizeof(batch_command) - 1,
				"wavtoogg \"%s\" 4 \"%s\"", wav, dest);
			eof_log("DTX preview: converting faded WAV through EOF's wavtoogg path", 1);
			result = eof_system(batch_command);
			(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
				"DTX preview: EOF wavtoogg conversion returned %d, output=%s",
				result, exists(dest) ? "present" : "missing");
			eof_log(eof_log_string, 1);
			return result;
		}
	}
#endif

	return eof_system(command);
}

#define eof_system(command) eof_dtx_preview_system_process_hook((command))

#endif

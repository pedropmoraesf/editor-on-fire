#include "gp_synth_process_hook.h"

#ifdef _WIN32
/* Allegro 4 and the Win32 GDI both define BITMAP.  Always let Allegro load
 * first, then use its winalleg compatibility header for Win32 APIs instead of
 * including windows.h directly. */
#include "main.h"
#include <winalleg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define GP_SYNTH_RAW_RATE 44100UL
#define GP_SYNTH_RAW_CHANNELS 2U
#define GP_SYNTH_RAW_BITS 16U

static void gp_synth_raw_put_le16(unsigned char *p, unsigned value)
{
	p[0] = (unsigned char)(value & 0xFFU);
	p[1] = (unsigned char)((value >> 8) & 0xFFU);
}

static void gp_synth_raw_put_le32(unsigned char *p, unsigned long value)
{
	p[0] = (unsigned char)(value & 0xFFUL);
	p[1] = (unsigned char)((value >> 8) & 0xFFUL);
	p[2] = (unsigned char)((value >> 16) & 0xFFUL);
	p[3] = (unsigned char)((value >> 24) & 0xFFUL);
}

static int gp_synth_raw_to_wav(const wchar_t *rawpath, const wchar_t *wavpath)
{
	FILE *in = NULL, *out = NULL;
	unsigned char header[44] = {0};
	unsigned char buffer[65536];
	__int64 data_size64;
	unsigned long data_size;
	size_t got;
	int ok = 0;

	if(!rawpath || !wavpath)
		return 0;

	in = _wfopen(rawpath, L"rb");
	if(!in)
		goto cleanup;
	if(_fseeki64(in, 0, SEEK_END))
		goto cleanup;
	data_size64 = _ftelli64(in);
	if((data_size64 <= 0) || (data_size64 > 0xFFFFFF00LL))
		goto cleanup;
	if(_fseeki64(in, 0, SEEK_SET))
		goto cleanup;
	data_size = (unsigned long)data_size64;

	out = _wfopen(wavpath, L"wb");
	if(!out)
		goto cleanup;

	memcpy(header, "RIFF", 4);
	gp_synth_raw_put_le32(header + 4, 36UL + data_size);
	memcpy(header + 8, "WAVEfmt ", 8);
	gp_synth_raw_put_le32(header + 16, 16UL);
	gp_synth_raw_put_le16(header + 20, 1U);
	gp_synth_raw_put_le16(header + 22, GP_SYNTH_RAW_CHANNELS);
	gp_synth_raw_put_le32(header + 24, GP_SYNTH_RAW_RATE);
	gp_synth_raw_put_le32(header + 28, GP_SYNTH_RAW_RATE * GP_SYNTH_RAW_CHANNELS * (GP_SYNTH_RAW_BITS / 8U));
	gp_synth_raw_put_le16(header + 32, GP_SYNTH_RAW_CHANNELS * (GP_SYNTH_RAW_BITS / 8U));
	gp_synth_raw_put_le16(header + 34, GP_SYNTH_RAW_BITS);
	memcpy(header + 36, "data", 4);
	gp_synth_raw_put_le32(header + 40, data_size);

	if(fwrite(header, 1, sizeof(header), out) != sizeof(header))
		goto cleanup;
	while((got = fread(buffer, 1, sizeof(buffer), in)) > 0)
	{
		if(fwrite(buffer, 1, got, out) != got)
			goto cleanup;
	}
	if(ferror(in))
		goto cleanup;
	ok = 1;

cleanup:
	if(out)
		fclose(out);
	if(in)
		fclose(in);
	if(!ok && wavpath)
		_wremove(wavpath);
	return ok;
}

static int gp_synth_quote_arg(wchar_t *dst, size_t dst_count, const wchar_t *src)
{
	size_t used = 0;
	const wchar_t *p;

	if(!dst || !dst_count || !src || (dst_count < 3))
		return 0;
	dst[used++] = L'"';
	for(p = src; *p; p++)
	{
		if(*p == L'"')
		{
			if(used + 2 >= dst_count)
				return 0;
			dst[used++] = L'\\';
		}
		if(used + 1 >= dst_count)
			return 0;
		dst[used++] = *p;
	}
	if(used + 2 > dst_count)
		return 0;
	dst[used++] = L'"';
	dst[used] = L'\0';
	return 1;
}

intptr_t gp_synth_raw_spawnv(int mode, const wchar_t *path, const wchar_t *const argv[])
{
	const wchar_t *wavpath, *soundfont, *midipath;
	wchar_t rawpath[2304] = {0};
	wchar_t qexe[2304] = {0}, qraw[2304] = {0}, qsf[2304] = {0}, qmidi[2304] = {0};
	wchar_t command[10000] = {0};
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD exit_code = 1;
	BOOL started;

	(void)mode;
	if(!path || !argv || !argv[9] || !argv[10] || !argv[11])
		return (intptr_t)-1;
	wavpath = argv[9];
	soundfont = argv[10];
	midipath = argv[11];

	if(swprintf(rawpath, sizeof(rawpath) / sizeof(rawpath[0]), L"%ls.raw", wavpath) < 0)
		return (intptr_t)-1;
	_wremove(rawpath);
	_wremove(wavpath);

	if(!gp_synth_quote_arg(qexe, sizeof(qexe) / sizeof(qexe[0]), path) ||
	   !gp_synth_quote_arg(qraw, sizeof(qraw) / sizeof(qraw[0]), rawpath) ||
	   !gp_synth_quote_arg(qsf, sizeof(qsf) / sizeof(qsf[0]), soundfont) ||
	   !gp_synth_quote_arg(qmidi, sizeof(qmidi) / sizeof(qmidi[0]), midipath))
	{
		eof_log("GP Audio: could not build FluidSynth command line", 1);
		return (intptr_t)-1;
	}

	if(swprintf(command, sizeof(command) / sizeof(command[0]),
		L"%ls -ni -g 0.8 -r 44100 -T raw -O s16 -E cpu -F %ls %ls %ls",
		qexe, qraw, qsf, qmidi) < 0)
	{
		eof_log("GP Audio: FluidSynth command line was too long", 1);
		return (intptr_t)-1;
	}

	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);

	started = CreateProcessW(path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
		NULL, NULL, &si, &pi);
	if(!started)
	{
		DWORD error_code = GetLastError();
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"GP Audio: CreateProcessW could not start FluidSynth (Windows error %lu)",
			(unsigned long)error_code);
		eof_log(eof_log_string, 1);
		return (intptr_t)-1;
	}

	WaitForSingleObject(pi.hProcess, INFINITE);
	if(!GetExitCodeProcess(pi.hProcess, &exit_code))
		exit_code = 1;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"GP Audio: FluidSynth process exit code %lu", (unsigned long)exit_code);
	eof_log(eof_log_string, 1);
	if(exit_code != 0)
		return (intptr_t)exit_code;

	if(!gp_synth_raw_to_wav(rawpath, wavpath))
	{
		eof_log("GP Audio: FluidSynth RAW output could not be wrapped as WAV", 1);
		return (intptr_t)1;
	}

	_wremove(rawpath);
	eof_log("GP Audio: FluidSynth RAW rendered and wrapped as WAV successfully", 1);
	return (intptr_t)0;
}

#else
int gp_synth_process_hook_unused = 0;
#endif

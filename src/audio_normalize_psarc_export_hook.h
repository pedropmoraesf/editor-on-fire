#ifndef EOF_AUDIO_NORMALIZE_PSARC_EXPORT_HOOK_H
#define EOF_AUDIO_NORMALIZE_PSARC_EXPORT_HOOK_H

/* This header is force-included after psarc_export_null_compat.h and
 * psarc_helper_launch_compat.h.  Let the existing internal alogg backend create
 * the staged PSARC WAV first, then apply the user's loudness target to that WAV
 * only.  The project's guitar.ogg is never modified by PSARC export. */
#include "audio_normalize.h"

#ifdef system
#undef system
#endif

static int eof_audio_normalize_psarc_system_compat(const char *command)
{
	int result;
	int internal_audio = 0;
	char output[1024] = {0};

	if(command && strstr(command, "\"" EOF_PSARC_INTERNAL_AUDIO "\"") == command)
		internal_audio = 1;
	result = eof_psarc_helper_launch_system_compat(command);
	if(result != 0 || !internal_audio || !eof_audio_normalize_psarc_checked)
		return result;

	if(!eof_psarc_get_last_quoted_arg_compat(command, output, sizeof(output)) || !exists(output))
	{
		/* Audio preparation itself already succeeded.  Normalization is optional
		 * for PSARC export, so do not turn a usable staged WAV into a fatal error
		 * merely because the compatibility hook could not locate it. */
		eof_log("PSARC normalize: could not identify staged WAV for normalization; continuing without normalization.", 1);
		return result;
	}
	if(!eof_audio_normalize_wav_in_place(output, eof_audio_normalize_target_value()))
	{
		/* eof_audio_normalize_wav_in_place() writes to a temporary file and only
		 * replaces output on success, therefore the already prepared WAV is still
		 * valid here.  Wwise can consume it directly. */
		eof_log("PSARC normalize: loudness normalization failed; preserving staged WAV and continuing export.", 1);
		return result;
	}
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"PSARC staged WAV normalized to %.2f LUFS: %.800s",
		eof_audio_normalize_target_value(), output);
	eof_log(eof_log_string, 1);
	return 0;
}

#define system(command) eof_audio_normalize_psarc_system_compat((command))

/* Last PSARC compatibility layer: deterministic count-in from the bundled
 * TuxGuitar stick WAV, with no FluidSynth synthesis step. */
#include "psarc_stick_sample_hook.h"

#endif

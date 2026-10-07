#ifndef EOF_PSARC_DIRECT_SYSTEM_HOOK_H
#define EOF_PSARC_DIRECT_SYSTEM_HOOK_H

/*
 * Final PSARC process-dispatch layer.
 *
 * Older compatibility headers intercepted system() in order to synthesize a
 * MIDI count-in and to normalize the staged WAV.  The current PSARC exporter
 * prepares the sampled drumstick count-in directly in eof_psarc_build_audio(),
 * so those command interceptors must not run afterwards.  Keeping this header
 * last in the psarc_export.c force-include list makes all remaining external
 * commands (PowerShell helper build, FFmpeg for the silence-only path, etc.) go
 * straight to the process launcher.
 */

#ifdef system
#undef system
#endif

static int eof_psarc_direct_system_compat(const char *command)
{
	static int announced = 0;
	if(!announced)
	{
		eof_log("PSARC exporter V6: direct process dispatch active; legacy loudnorm/count-in system hooks disabled.", 1);
		announced = 1;
	}
	return eof_psarc_helper_launch_system_compat(command);
}

#define system(command) eof_psarc_direct_system_compat((command))

#endif

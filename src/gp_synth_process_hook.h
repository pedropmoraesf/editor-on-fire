#ifndef EOF_GP_SYNTH_PROCESS_HOOK_H
#define EOF_GP_SYNTH_PROCESS_HOOK_H

/* gp_synth.c used FluidSynth's -T wav fast-render output.  WAV output depends
 * on FluidSynth having been built with libsndfile, which is optional.  Preload
 * process.h before redirecting _wspawnv so its declaration isn't macro-expanded,
 * then let the compatibility worker request raw PCM (always supported) and wrap
 * it in a WAV header itself. */
#ifdef _WIN32
#include <process.h>
#include <stdint.h>
#include <wchar.h>

intptr_t gp_synth_raw_spawnv(int mode, const wchar_t *path, const wchar_t *const argv[]);

#define _wspawnv(mode, path, argv) \
	gp_synth_raw_spawnv((mode), (path), (argv))
#endif

#endif

#ifndef EOF_DTX_MIX_HOOK_H
#define EOF_DTX_MIX_HOOK_H

/* Force-included only for mix.c.  Load the original song declarations before
 * defining the macro so the real eof_track_is_pro_guitar_track() declaration
 * remains intact and the wrapper can delegate to it for every non-DTX track. */
#include "song.h"
#include "dtx_mix.h"

#define eof_track_is_pro_guitar_track(sp, track) \
	eof_dtx_mix_is_pro_guitar_track((sp), (track))

#endif

#ifndef EOF_DTX_MIX_H
#define EOF_DTX_MIX_H

#include "song.h"

int eof_dtx_mix_is_pro_guitar_track(EOF_SONG *sp, unsigned long track);
	/* Used only by mix.c.  PART_REAL_DRUM_DTX uses the pro-guitar structure as
	 * storage, but must not enter guitar MIDI-tone/tuning cue logic. */

#endif

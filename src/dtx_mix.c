#include <allegro.h>

#include "song.h"
#include "dtx_mix.h"

int eof_dtx_mix_is_pro_guitar_track(EOF_SONG *sp, unsigned long track)
{
	/* PART_REAL_DRUM_DTX is structurally backed by EOF_PRO_GUITAR_TRACK, but
	 * frets[] contains General MIDI percussion numbers.  mix.c's pro-guitar
	 * path converts frets through string tunings and queues guitar MIDI tones;
	 * that path is invalid for DTX and is also rebuilt/searched during seeks.
	 * Treat DTX as non-pro-guitar only inside mix.c while preserving the carrier
	 * format everywhere else in EOF. */
	if(sp && (track == EOF_TRACK_DRUM_DTX))
		return 0;

	return eof_track_is_pro_guitar_track(sp, track);
}

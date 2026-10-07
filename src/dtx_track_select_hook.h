#ifndef EOF_DTX_TRACK_SELECT_HOOK_H
#define EOF_DTX_TRACK_SELECT_HOOK_H

/* Force-included only for menu/song.c.  Load the original declaration first,
 * then route difficulty detection through the DTX workflow helper.  The track
 * selector calls eof_detect_difficulties() immediately after assigning
 * eof_selected_track, giving us a safe, event-driven place to activate ULT and
 * swap BSC/ADV/EXT/MAS/ULT labels without touching render-time state. */
#include "song.h"
#include "dtx_workflow.h"

#define eof_detect_difficulties(sp, track) \
	eof_dtx_detect_difficulties_hook((sp), (track))

#endif

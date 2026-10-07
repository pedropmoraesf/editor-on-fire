#include <allegro.h>
#include <string.h>

#include "main.h"
#include "note.h"
#include "dtx_integration.h"
#include "dtx_3d.h"

static EOF_PRO_GUITAR_NOTE *eof_dtx_3d_note(unsigned long track, unsigned long notenum)
{
	unsigned long tracknum;
	EOF_PRO_GUITAR_TRACK *tp;

	if(!eof_song || (track != EOF_TRACK_DRUM_DTX) || (track >= eof_song->tracks) || !eof_song->track[track])
		return NULL;
	if(eof_song->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT)
		return NULL;

	tracknum = eof_song->track[track]->tracknum;
	if((tracknum >= eof_song->pro_guitar_tracks) || !eof_song->pro_guitar_track[tracknum])
		return NULL;

	tp = eof_song->pro_guitar_track[tracknum];
	if((notenum >= tp->notes) || !tp->note[notenum])
		return NULL;

	return tp->note[notenum];
}

static unsigned eof_dtx_3d_lane_from_channel(int channel)
{
	/* Use the same familiar left-to-right layout as EOF's ordinary five-lane
	 * instrument view.  Cymbals and their corresponding toms share a lane here;
	 * the important part is that the normal EOF 3D renderer owns the actual
	 * appearance instead of a separate DTX-specific 3D implementation. */
	switch(channel)
	{
		case EOF_DTX_INT_KICK:
			return 0;
		case EOF_DTX_INT_SNARE:
			return 1;
		case EOF_DTX_INT_HH:
		case EOF_DTX_INT_HH_OPEN:
		case EOF_DTX_INT_TOM1:
			return 2;
		case EOF_DTX_INT_RIDE:
		case EOF_DTX_INT_TOM2:
			return 3;
		case EOF_DTX_INT_CRASH:
		case EOF_DTX_INT_TOM3:
			return 4;
		default:
			return 5;
	}
}

static void eof_dtx_prepare_stock_3d_note(EOF_PRO_GUITAR_NOTE *np)
{
	unsigned long source_mask, source_string;
	unsigned char mapped_mask = 0;
	unsigned char mapped_frets[8] = {0};

	if(!np)
		return;

	/* PART_REAL_DRUM_DTX uses frets[] as storage for General MIDI percussion
	 * numbers.  The stock pro-guitar 3D renderer must never receive those raw
	 * values because they are not guitar frets.  Convert only the temporary
	 * render copy into ordinary, safe EOF lanes/frets. */
	source_mask = np->note;
	for(source_string = 0; source_string < 6; source_string++)
	{
		unsigned long bit = 1UL << source_string;
		unsigned midi_note;
		int channel;
		unsigned lane;

		if(!(source_mask & bit))
			continue;

		midi_note = np->frets[source_string] & 0x7F;
		channel = eof_dtx_integration_channel_from_midi(midi_note);
		lane = eof_dtx_3d_lane_from_channel(channel);
		if(lane > 5)
			lane = 5;
		mapped_mask |= (unsigned char)(1U << lane);
		mapped_frets[lane] = 0;
	}

	/* No DTX percussion information is discarded: this function is only used
	 * on a stack-backed temporary rendering state and the original note is
	 * restored immediately after eof_note_draw_3d() returns. */
	np->note = mapped_mask;
	np->ghost = 0;
	np->flags = 0;
	np->eflags = 0;
	np->legacymask = 0;
	np->length = 1;
	memcpy(np->frets, mapped_frets, sizeof(np->frets));
	memset(np->finger, 0, sizeof(np->finger));
}

int eof_dtx_note_tail_draw_3d(unsigned long track, unsigned long notenum, int p)
{
	if(track != EOF_TRACK_DRUM_DTX)
		return eof_note_tail_draw_3d(track, notenum, p);

	/* DTX drum chips are instantaneous hits.  Suppressing their sustain tail is
	 * both visually correct and prevents the pro-guitar tail renderer from
	 * interpreting the carrier's MIDI percussion values as fret data. */
	(void)notenum;
	(void)p;
	return 0;
}

int eof_dtx_note_draw_3d(unsigned long track, unsigned long notenum, int p)
{
	EOF_PRO_GUITAR_NOTE *np;
	EOF_PRO_GUITAR_NOTE backup;
	int result;

	if(track != EOF_TRACK_DRUM_DTX)
		return eof_note_draw_3d(track, notenum, p);

	np = eof_dtx_3d_note(track, notenum);
	if(!np)
		return 0;

	/* Render PART_REAL_DRUM_DTX through EOF's original 3D note renderer so it
	 * has exactly the same highway/gem style as the other instrument tracks.
	 * Only the temporary note data is sanitized; the real DTX MIDI values are
	 * restored before returning. */
	backup = *np;
	eof_dtx_prepare_stock_3d_note(np);
	result = eof_note_draw_3d(track, notenum, p);
	*np = backup;

	return result;
}

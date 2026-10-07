#ifndef EOF_BASS_DISPLAY_H
#define EOF_BASS_DISPLAY_H

/* Returns the user-facing Rocksmith bass arrangement name for a populated
 * PART_REAL_BASS/PART_REAL_BASS_22 track.  NULL means keep EOF's stock track
 * name.  If both bass tracks are populated, PART_REAL_BASS is Alt Bass and
 * PART_REAL_BASS_22 is Bass; if only one is populated, that one is Bass. */
const char *eof_bass_display_arrangement_name(unsigned long track);

/* Runs EOF's stock Song menu preparation and then applies the dynamic bass
 * display names above without altering the project's internal track names. */
void eof_bass_display_prepare_song_menu(void);

#endif

#include <allegro.h>

#include "main.h"
#include "song.h"
#include "note.h"
#include "menu/song.h"
#include "bass_display.h"

const char *eof_bass_display_arrangement_name(unsigned long track)
{
	int bass_populated = 0, bass22_populated = 0;

	if(!eof_song || !eof_song_loaded)
		return NULL;

	if(EOF_TRACK_PRO_BASS < eof_song->tracks)
		bass_populated = eof_get_track_size(eof_song, EOF_TRACK_PRO_BASS) ? 1 : 0;
	if(EOF_TRACK_PRO_BASS_22 < eof_song->tracks)
		bass22_populated = eof_get_track_size(eof_song, EOF_TRACK_PRO_BASS_22) ? 1 : 0;

	if(track == EOF_TRACK_PRO_BASS)
	{
		if(!bass_populated)
			return NULL;
		return bass22_populated ? "Alt Bass" : "Bass";
	}
	if(track == EOF_TRACK_PRO_BASS_22)
	{
		if(!bass22_populated)
			return NULL;
		return "Bass";
	}

	return NULL;
}

void eof_bass_display_prepare_song_menu(void)
{
	const char *name;
	char *text;

	/* Let EOF build the menu normally first so the leading population/highlight
	 * indicator and every non-bass track keep their stock behavior. */
	eof_prepare_song_menu();

	if(!eof_song || !eof_song_loaded)
		return;

	name = eof_bass_display_arrangement_name(EOF_TRACK_PRO_BASS);
	if(name && (EOF_TRACK_PRO_BASS > 0) && (EOF_TRACK_PRO_BASS - 1 < EOF_TRACKS_MAX))
	{
		text = eof_track_selected_menu[EOF_TRACK_PRO_BASS - 1].text;
		if(text)
			(void)ustrzcpy(&text[1], EOF_TRACK_NAME_SIZE, name);
	}

	name = eof_bass_display_arrangement_name(EOF_TRACK_PRO_BASS_22);
	if(name && (EOF_TRACK_PRO_BASS_22 > 0) && (EOF_TRACK_PRO_BASS_22 - 1 < EOF_TRACKS_MAX))
	{
		text = eof_track_selected_menu[EOF_TRACK_PRO_BASS_22 - 1].text;
		if(text)
			(void)ustrzcpy(&text[1], EOF_TRACK_NAME_SIZE, name);
	}
}

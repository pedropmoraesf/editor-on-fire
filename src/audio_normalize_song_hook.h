#ifndef EOF_AUDIO_NORMALIZE_SONG_HOOK_H
#define EOF_AUDIO_NORMALIZE_SONG_HOOK_H

/* menu/song.c rebuilds a filtered copy of eof_song_menu whenever the menu is
 * prepared.  Insert Normalize audio into that copy, immediately before Leading
 * Silence, without changing the legacy static menu table. */
#include <string.h>
#include "dialog.h"
#include "audio_normalize.h"

extern MENU eof_song_menu[];

static int eof_audio_normalize_create_song_menu(MENU *input, MENU *output, unsigned menu_size)
{
	MENU temp[EOF_SCRATCH_MENU_SIZE];
	unsigned src = 0, dst = 0;
	int inserted = 0;

	if(input != eof_song_menu)
		return eof_create_filtered_menu(input, output, menu_size);
	memset(temp, 0, sizeof(temp));
	while(input[src].text && dst + 2U < EOF_SCRATCH_MENU_SIZE)
	{
		if(!inserted && input[src].text && strstr(input[src].text, "Leading Silence"))
		{
			temp[dst].text = "Normalize &audio";
			temp[dst].proc = eof_menu_song_normalize_audio;
			temp[dst].child = NULL;
			temp[dst].flags = 0;
			temp[dst].dp = NULL;
			dst++;
			inserted = 1;
		}
		temp[dst++] = input[src++];
	}
	if(!inserted && dst + 1U < EOF_SCRATCH_MENU_SIZE)
	{
		temp[dst].text = "Normalize &audio";
		temp[dst].proc = eof_menu_song_normalize_audio;
		dst++;
	}
	memset(&temp[dst], 0, sizeof(MENU));
	return eof_create_filtered_menu(temp, output, menu_size);
}

#define eof_create_filtered_menu(input, output, menu_size) \
	eof_audio_normalize_create_song_menu((input), (output), (menu_size))

#endif

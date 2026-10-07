/*
 * gp_synth.c
 *
 * File > New from GP Audio
 *
 * Creates a normal EOF project, imports selected Guitar Pro arrangements with
 * the existing Advanced importer and renders those imported arrangements to
 * chart audio through FluidSynth.  No second GP parser or mapping window is
 * maintained here.
 */

#include <allegro.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ALLEGRO_WINDOWS
#include <process.h>
#include <wchar.h>
#endif

#include "main.h"
#include "song.h"
#include "beat.h"
#include "midi.h"
#include "gp_import.h"
#include "utility.h"
#include "modules/wfsel.h"
#include "menu/file.h"
#include "gp_advanced.h"
#include "gp_synth.h"

#define GP_SYNTH_RATE 44100UL
#define GP_SYNTH_CONFIG_SECTION "gp_synth"
#define GP_SYNTH_CONFIG_EXE "fluidsynth_path"
#define GP_SYNTH_CONFIG_SF "soundfont_path"

static NCDFS_FILTER_LIST *gp_synth_soundfont_filter = NULL;

static void gp_synth_put_le16(unsigned char *p, unsigned value)
{
	p[0] = (unsigned char)(value & 0xFFU);
	p[1] = (unsigned char)((value >> 8) & 0xFFU);
}

static void gp_synth_put_le32(unsigned char *p, unsigned long value)
{
	p[0] = (unsigned char)(value & 0xFFUL);
	p[1] = (unsigned char)((value >> 8) & 0xFFUL);
	p[2] = (unsigned char)((value >> 16) & 0xFFUL);
	p[3] = (unsigned char)((value >> 24) & 0xFFUL);
}

static int gp_synth_write_seed_wav(const char *path)
{
	PACKFILE *fp;
	unsigned char header[44] = {0};
	unsigned char zero[4096] = {0};
	unsigned long data_bytes = GP_SYNTH_RATE * 2UL * 2UL; /* one second, stereo, 16 bit */
	unsigned long remaining = data_bytes;

	if(!path)
		return 0;
	fp = pack_fopen(path, "wb");
	if(!fp)
		return 0;
	memcpy(header, "RIFF", 4);
	gp_synth_put_le32(header + 4, 36UL + data_bytes);
	memcpy(header + 8, "WAVEfmt ", 8);
	gp_synth_put_le32(header + 16, 16UL);
	gp_synth_put_le16(header + 20, 1U);
	gp_synth_put_le16(header + 22, 2U);
	gp_synth_put_le32(header + 24, GP_SYNTH_RATE);
	gp_synth_put_le32(header + 28, GP_SYNTH_RATE * 4UL);
	gp_synth_put_le16(header + 32, 4U);
	gp_synth_put_le16(header + 34, 16U);
	memcpy(header + 36, "data", 4);
	gp_synth_put_le32(header + 40, data_bytes);
	if(pack_fwrite(header, sizeof(header), fp) != (long)sizeof(header))
	{
		pack_fclose(fp);
		delete_file(path);
		return 0;
	}
	while(remaining)
	{
		unsigned long chunk = remaining > sizeof(zero) ? sizeof(zero) : remaining;
		if(pack_fwrite(zero, (long)chunk, fp) != (long)chunk)
		{
			pack_fclose(fp);
			delete_file(path);
			return 0;
		}
		remaining -= chunk;
	}
	pack_fclose(fp);
	return 1;
}

static int gp_synth_gp_version(const char *text)
{
	if(!strcmp(text, "FICHIER GUITARE PRO v3.00")) return 300;
	if(!strcmp(text, "FICHIER GUITARE PRO v4.00")) return 400;
	if(!strcmp(text, "FICHIER GUITARE PRO v4.06")) return 406;
	if(!strcmp(text, "FICHIER GUITAR PRO L4.06")) return 406;
	if(!strcmp(text, "FICHIER GUITAR PRO v5.00")) return 500;
	if(!strcmp(text, "FICHIER GUITAR PRO v5.10")) return 510;
	return 0;
}

/* The legacy GP loader already reads this value but historically uses the
 * project's existing beat map for note placement.  Read just the fixed header
 * far enough to obtain the initial tempo, then seed the new project's beat map
 * BEFORE calling eof_load_gp().  Time signatures themselves remain imported by
 * the stock GP importer. */
static int gp_synth_read_legacy_tempo(const char *path, unsigned long *tempo)
{
	PACKFILE *fp;
	char buffer[1025];
	unsigned word = 0;
	unsigned long dword = 0, count, i;
	int version;

	if(!path || !tempo)
		return 0;
	fp = pack_fopen(path, "rb");
	if(!fp)
		return 0;
	buffer[0] = '\0';
	if(!eof_read_gp_string(fp, &word, buffer, 0) || (word > 30U))
		goto fail;
	(void)pack_fseek(fp, 30L - (long)word);
	version = gp_synth_gp_version(buffer);
	if(!version)
		goto fail;

	/* title, subtitle, artist, album */
	for(i = 0; i < 4; i++) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail;
	if(version >= 500) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail; /* words */
	/* composer/music, copyright, transcriber, instructions */
	for(i = 0; i < 4; i++) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail;
	pack_ReadDWORDLE(fp, &count);
	if(count > 1000UL) goto fail;
	for(i = 0; i < count; i++) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail;
	if(version < 500)
		(void)pack_getc(fp); /* shuffle feel */

	if(version >= 400)
	{
		pack_ReadDWORDLE(fp, NULL); /* lyrics track */
		for(i = 0; i < 5; i++)
		{
			pack_ReadDWORDLE(fp, NULL); /* start bar */
			pack_ReadDWORDLE(fp, &dword);
			if(dword > 16UL * 1024UL * 1024UL) goto fail;
			if(dword && pack_fseek(fp, (long)dword)) goto fail;
		}
	}

	if(version > 500)
	{
		pack_ReadDWORDLE(fp, NULL); /* master volume */
		if(pack_fseek(fp, 4)) goto fail;
		for(i = 0; i < 11; i++) (void)pack_getc(fp); /* 10 EQ bands + gain */
	}

	if(version >= 500)
	{
		for(i = 0; i < 7; i++) pack_ReadDWORDLE(fp, NULL); /* page setup */
		pack_ReadWORDLE(fp, NULL);
		for(i = 0; i < 10; i++) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail;
		if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail; /* tempo text */
	}
	pack_ReadDWORDLE(fp, &dword);
	pack_fclose(fp);
	if((dword < 20UL) || (dword > 500UL))
		return 0;
	*tempo = dword;
	return 1;

fail:
	pack_fclose(fp);
	return 0;
}

static void gp_synth_apply_initial_tempo(unsigned long bpm)
{
	unsigned long i, ppqn;
	if(!eof_song || !eof_song->beats || (bpm < 20UL) || (bpm > 500UL))
		return;
	ppqn = (60000000UL + bpm / 2UL) / bpm;
	for(i = 0; i < eof_song->beats; i++)
		eof_song->beat[i]->ppqn = ppqn;
	eof_song->beat[0]->flags |= EOF_BEAT_FLAG_ANCHOR;
	eof_calculate_beats(eof_song);
	eof_chart_length = eof_song->beat[eof_song->beats - 1]->pos;
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"GP Audio: initialized project tempo from Guitar Pro at %lu BPM", bpm);
	eof_log(eof_log_string, 1);
}

static NCDFS_FILTER_LIST *gp_synth_get_soundfont_filter(void)
{
	if(!gp_synth_soundfont_filter)
	{
		gp_synth_soundfont_filter = ncdfs_filter_list_create();
		if(gp_synth_soundfont_filter)
			(void)ncdfs_filter_list_add(gp_synth_soundfont_filter, "sf2;sf3", "SoundFont (*.sf2, *.sf3)", 1);
	}
	return gp_synth_soundfont_filter;
}

static int gp_synth_find_nearby_soundfont(const char *fluid, char *soundfont, size_t size)
{
	char folder[1024] = {0}, candidate[1024] = {0};
	static const char *names[] = {"FluidR3_GM.sf2", "default.sf2", "GeneralUser_GS.sf2", NULL};
	int i;
	if(!fluid || !soundfont || !size)
		return 0;
	replace_filename(folder, fluid, "", sizeof(folder));
	for(i = 0; names[i]; i++)
	{
		replace_filename(candidate, folder, names[i], sizeof(candidate));
		if(exists(candidate))
		{
			ustrzcpy(soundfont, (int)size, candidate);
			return 1;
		}
	}
	return 0;
}

static int gp_synth_get_tools(char *fluid, size_t fluidsize, char *soundfont, size_t sfsize)
{
	const char *saved;
	char *selected;

	saved = get_config_string(GP_SYNTH_CONFIG_SECTION, GP_SYNTH_CONFIG_EXE, "");
	ustrzcpy(fluid, (int)fluidsize, saved ? saved : "");
	saved = get_config_string(GP_SYNTH_CONFIG_SECTION, GP_SYNTH_CONFIG_SF, "");
	ustrzcpy(soundfont, (int)sfsize, saved ? saved : "");

	if(!fluid[0] || !exists(fluid))
	{
		selected = ncd_file_select(0, eof_last_eof_path, "Select FluidSynth executable", eof_filter_exe_files);
		eof_clear_input();
		if(!selected)
			return 0;
		ustrzcpy(fluid, (int)fluidsize, selected);
		soundfont[0] = '\0';
	}
	if((!soundfont[0] || !exists(soundfont)) && !gp_synth_find_nearby_soundfont(fluid, soundfont, sfsize))
	{
		selected = ncd_file_select(0, eof_last_eof_path, "Select General MIDI SoundFont", gp_synth_get_soundfont_filter());
		eof_clear_input();
		if(!selected)
			return 0;
		ustrzcpy(soundfont, (int)sfsize, selected);
	}

	set_config_string(GP_SYNTH_CONFIG_SECTION, GP_SYNTH_CONFIG_EXE, fluid);
	set_config_string(GP_SYNTH_CONFIG_SECTION, GP_SYNTH_CONFIG_SF, soundfont);
	flush_config_file();
	return 1;
}

static int gp_synth_dtx_piece(unsigned midi, unsigned char *mask, unsigned long *flags)
{
	if(!mask || !flags)
		return 0;
	*mask = 0;
	*flags = 0;
	switch(midi)
	{
		case 31: case 35: case 36: *mask = 1; return 1;
		case 37: case 38: case 39: case 40: case 62: *mask = 2; return 1;
		case 42: case 44: *mask = 4; *flags = EOF_DRUM_NOTE_FLAG_Y_CYMBAL; return 1;
		case 46: *mask = 4; *flags = EOF_DRUM_NOTE_FLAG_Y_CYMBAL | EOF_DRUM_NOTE_FLAG_Y_HI_HAT_OPEN; return 1;
		case 48: case 50: *mask = 4; return 1;
		case 51: case 53: case 59: *mask = 8; *flags = EOF_DRUM_NOTE_FLAG_B_CYMBAL; return 1;
		case 45: case 47: *mask = 8; return 1;
		case 49: case 52: case 55: case 57: *mask = 16; *flags = EOF_DRUM_NOTE_FLAG_G_CYMBAL; return 1;
		case 41: case 43: *mask = 16; return 1;
	}
	return 0;
}

/* eof_export_music_midi() already writes EOF's tempo map and every imported
 * pro-guitar/bass arrangement.  DTX is intentionally stored in a pro-guitar
 * carrier, so hide that carrier during synth export and expose its highest
 * populated difficulty temporarily through EOF's normal drum track.  This
 * lets the stock music-MIDI exporter create proper GM percussion on channel 10
 * without changing the saved project. */
static unsigned long gp_synth_add_temp_drums(EOF_LEGACY_TRACK **drum_track_out,
	unsigned long *old_count, EOF_PRO_GUITAR_TRACK **dtx_out,
	unsigned long *old_dtx_notes, unsigned long *old_dtx_pgnotes)
{
	EOF_LEGACY_TRACK *drums = NULL;
	EOF_PRO_GUITAR_TRACK *dtx = NULL;
	unsigned long backing, i, added = 0;
	int diff, chosen = -1;

	if(drum_track_out) *drum_track_out = NULL;
	if(old_count) *old_count = 0;
	if(dtx_out) *dtx_out = NULL;
	if(old_dtx_notes) *old_dtx_notes = 0;
	if(old_dtx_pgnotes) *old_dtx_pgnotes = 0;
	if(!eof_song)
		return 0;

	if((EOF_TRACK_DRUM < eof_song->tracks) && eof_song->track[EOF_TRACK_DRUM])
	{
		backing = eof_song->track[EOF_TRACK_DRUM]->tracknum;
		if(backing < eof_song->legacy_tracks)
			drums = eof_song->legacy_track[backing];
	}
	if((EOF_TRACK_DRUM_DTX < eof_song->tracks) && eof_song->track[EOF_TRACK_DRUM_DTX])
	{
		backing = eof_song->track[EOF_TRACK_DRUM_DTX]->tracknum;
		if(backing < eof_song->pro_guitar_tracks)
			dtx = eof_song->pro_guitar_track[backing];
	}
	if(!dtx)
		return 0;

	if(dtx_out) *dtx_out = dtx;
	if(old_dtx_notes) *old_dtx_notes = dtx->notes;
	if(old_dtx_pgnotes) *old_dtx_pgnotes = dtx->pgnotes;
	if(!drums || drums->notes)
	{
		/* Avoid exporting DTX's GM note numbers as guitar frets even when we
		 * cannot borrow the normal drum track. */
		dtx->notes = dtx->pgnotes = 0;
		return 0;
	}
	if(drum_track_out) *drum_track_out = drums;
	if(old_count) *old_count = drums->notes;

	for(diff = EOF_NOTE_SPECIAL; diff >= EOF_NOTE_SUPAEASY; diff--)
	{
		for(i = 0; i < dtx->pgnotes; i++)
		{
			if(dtx->pgnote[i] && dtx->pgnote[i]->type == (unsigned char)diff)
			{
				chosen = diff;
				break;
			}
		}
		if(chosen >= 0) break;
	}
	if(chosen >= 0)
	{
		for(i = 0; i < dtx->pgnotes; i++)
		{
			EOF_PRO_GUITAR_NOTE *src = dtx->pgnote[i];
			unsigned s;
			if(!src || src->type != (unsigned char)chosen)
				continue;
			for(s = 0; s < 6; s++)
			{
				unsigned char mask;
				unsigned long flags;
				EOF_NOTE *np;
				if(!(src->note & (1U << s)))
					continue;
				if(!gp_synth_dtx_piece(src->frets[s] & 0x7F, &mask, &flags))
					continue;
				np = eof_legacy_track_add_note(drums);
				if(!np)
					continue;
				np->note = mask;
				np->type = EOF_NOTE_AMAZING;
				np->pos = src->pos;
				np->length = 1;
				np->flags = flags;
				added++;
			}
		}
	}
	eof_legacy_track_sort_notes(drums);
	dtx->notes = dtx->pgnotes = 0;
	return added;
}

static void gp_synth_restore_temp_drums(EOF_LEGACY_TRACK *drums, unsigned long old_count,
	EOF_PRO_GUITAR_TRACK *dtx, unsigned long old_dtx_notes, unsigned long old_dtx_pgnotes)
{
	if(drums)
	{
		while(drums->notes > old_count)
			eof_legacy_track_delete_note(drums, drums->notes - 1);
	}
	if(dtx)
	{
		dtx->notes = old_dtx_notes;
		dtx->pgnotes = old_dtx_pgnotes;
	}
}

static int gp_synth_export_midi(const char *path)
{
	EOF_LEGACY_TRACK *drums = NULL;
	EOF_PRO_GUITAR_TRACK *dtx = NULL;
	unsigned long old_count = 0, old_dtx_notes = 0, old_dtx_pgnotes = 0;
	int result;

	(void)gp_synth_add_temp_drums(&drums, &old_count, &dtx, &old_dtx_notes, &old_dtx_pgnotes);
	result = eof_export_music_midi(eof_song, (char *)path, 0);
	gp_synth_restore_temp_drums(drums, old_count, dtx, old_dtx_notes, old_dtx_pgnotes);
	return result;
}

static int gp_synth_run_fluidsynth(const char *fluid, const char *soundfont, const char *midi, const char *wav)
{
#ifdef ALLEGRO_WINDOWS
	wchar_t wfluid[2048] = {0}, wsf[2048] = {0}, wmidi[2048] = {0}, wwav[2048] = {0};
	const wchar_t *argv[14];
	intptr_t result;

	(void)uconvert(fluid, U_UTF8, (char *)wfluid, U_UNICODE, sizeof(wfluid));
	(void)uconvert(soundfont, U_UTF8, (char *)wsf, U_UNICODE, sizeof(wsf));
	(void)uconvert(midi, U_UTF8, (char *)wmidi, U_UNICODE, sizeof(wmidi));
	(void)uconvert(wav, U_UTF8, (char *)wwav, U_UNICODE, sizeof(wwav));
	argv[0] = wfluid;
	argv[1] = L"-ni";
	argv[2] = L"-g";
	argv[3] = L"0.8";
	argv[4] = L"-r";
	argv[5] = L"44100";
	argv[6] = L"-T";
	argv[7] = L"wav";
	argv[8] = L"-F";
	argv[9] = wwav;
	argv[10] = wsf;
	argv[11] = wmidi;
	argv[12] = NULL;
	result = _wspawnv(_P_WAIT, wfluid, argv);
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"GP Audio: FluidSynth returned %ld, WAV=%s", (long)result, exists(wav) ? "present" : "missing");
	eof_log(eof_log_string, 1);
	return ((result == 0) && exists(wav) && (file_size_ex(wav) > 44)) ? 1 : 0;
#else
	char command[8192];
	int result;
	snprintf(command, sizeof(command), "\"%s\" -ni -g 0.8 -r 44100 -T wav -F \"%s\" \"%s\" \"%s\"",
		fluid, wav, soundfont, midi);
	result = eof_system(command);
	return ((result == 0) && exists(wav) && (file_size_ex(wav) > 44)) ? 1 : 0;
#endif
}

static int gp_synth_wav_to_project_ogg(const char *wav)
{
	char ogg[1024], original[1024], command[4096];
	int result;
	append_filename(ogg, eof_song_path, "guitar.ogg", sizeof(ogg));
	append_filename(original, eof_song_path, "original.wav", sizeof(original));
	(void)eof_copy_file(wav, original);
	(void)eof_destroy_ogg();
	delete_file(ogg);
#ifdef ALLEGRO_WINDOWS
	snprintf(command, sizeof(command), "wavtoogg \"%s\" 4 \"%s\"", wav, ogg);
#else
	snprintf(command, sizeof(command), "oggenc --quiet -q 4 --resample 44100 -s 0 \"%s\" -o \"%s\"", wav, ogg);
#endif
	result = eof_system(command);
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"GP Audio: EOF WAV-to-OGG conversion returned %d, OGG=%s", result, exists(ogg) ? "present" : "missing");
	eof_log(eof_log_string, 1);
	if(result || !exists(ogg) || (file_size_ex(ogg) <= 0))
		return 0;
	if(!eof_load_ogg_quick(ogg))
		return 0;
	return 1;
}

int eof_menu_file_new_from_gp_audio(void)
{
	char *selected;
	char gp_path[1024] = {0}, fluid[1024] = {0}, soundfont[1024] = {0};
	char executable[1024] = {0}, seed[1024] = {0};
	char midi[1024] = {0}, wav[1024] = {0};
	unsigned long bpm = 0;
	int imported, old_use_ts;

	eof_log("eof_menu_file_new_from_gp_audio() entered", 1);
	selected = ncd_file_select(0,
		(eof_imports_recall_last_path && eof_folder_exists(eof_last_gp_path)) ? eof_last_gp_path : eof_last_eof_path,
		"New project from Guitar Pro synthesized audio", eof_filter_gp_files);
	eof_clear_input();
	if(!selected)
		return D_O_K;
	ustrzcpy(gp_path, sizeof(gp_path), selected);
	(void)replace_filename(eof_last_gp_path, gp_path, "", sizeof(eof_last_gp_path));

	if(!gp_synth_get_tools(fluid, sizeof(fluid), soundfont, sizeof(soundfont)))
		return D_O_K;

	/* A short silent WAV lets us reuse EOF's complete New Project workflow
	 * (destination, metadata, guitar.ogg creation, notes.eof autosave).  It is
	 * replaced by FluidSynth audio before this command finishes. */
	get_executable_name(executable, sizeof(executable));
	replace_filename(seed, executable, "gp_synth_seed.wav", sizeof(seed));
	delete_file(seed);
	if(!gp_synth_write_seed_wav(seed))
	{
		allegro_message("Could not create the temporary audio used to initialize the new EOF project.");
		return D_O_K;
	}
	if(eof_new_chart(seed))
	{
		delete_file(seed);
		return D_O_K;
	}
	delete_file(seed);

	if(gp_synth_read_legacy_tempo(gp_path, &bpm))
		gp_synth_apply_initial_tempo(bpm);
	else
		eof_log("GP Audio: no legacy initial BPM was extracted; keeping the importer/current tempo map", 1);

	/* The synthesized-audio workflow intentionally has no Songsterr/SVL box.
	 * Force GP time-signature import for this one operation, then restore the
	 * user's preference. */
	old_use_ts = eof_use_ts;
	eof_use_ts = 1;
	imported = eof_gp_advanced_import_path(gp_path, 1);
	eof_use_ts = old_use_ts;
	if(!imported)
	{
		allegro_message("No Guitar Pro tracks were imported, so synthesized audio was not generated.");
		return D_O_K;
	}

	append_filename(midi, eof_song_path, "gp_synth.tmp.mid", sizeof(midi));
	append_filename(wav, eof_song_path, "gp_synth.tmp.wav", sizeof(wav));
	delete_file(midi);
	delete_file(wav);
	if(!gp_synth_export_midi(midi) || !exists(midi))
	{
		allegro_message("Could not create the temporary MIDI for FluidSynth. Check eof_log.txt.");
		return D_O_K;
	}
	if(!gp_synth_run_fluidsynth(fluid, soundfont, midi, wav))
	{
		allegro_message("FluidSynth could not render the Guitar Pro arrangements. The temporary MIDI was kept for diagnosis.");
		return D_O_K;
	}
	if(!gp_synth_wav_to_project_ogg(wav))
	{
		allegro_message("FluidSynth rendered the WAV, but EOF could not convert it to guitar.ogg. The temporary WAV was kept for diagnosis.");
		return D_O_K;
	}

	delete_file(midi);
	delete_file(wav);
	eof_truncate_chart(eof_song);
	eof_beat_stats_cached = 0;
	eof_changes = 1;
	(void)eof_menu_file_quick_save();
	eof_fix_window_title();
	eof_render();
	allegro_message("Guitar Pro project created with FluidSynth audio.");
	return D_O_K;
}

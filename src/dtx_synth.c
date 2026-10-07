#include <allegro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ALLEGRO_WINDOWS
#include <process.h>
#include <wchar.h>
#endif

#include "main.h"
#include "beat.h"
#include "menu/file.h"
#include "song.h"
#include "utility.h"
#include "modules/wfsel.h"
#include "gp_synth_process_hook.h"
#include "dtx_synth.h"

#define EOF_DTX_SYNTH_PPQ 480UL

typedef struct
{
	unsigned long tick;
	unsigned char kind; /* 0 tempo, 1 note-off, 2 note-on */
	unsigned char note;
	unsigned long tempo;
} EOF_DTX_SYNTH_EVENT;

static NCDFS_FILTER_LIST *eof_dtx_synth_sf_filter = NULL;

static int eof_dtx_synth_event_cmp(const void *a, const void *b)
{
	const EOF_DTX_SYNTH_EVENT *x = (const EOF_DTX_SYNTH_EVENT *)a;
	const EOF_DTX_SYNTH_EVENT *y = (const EOF_DTX_SYNTH_EVENT *)b;
	if(x->tick < y->tick) return -1;
	if(x->tick > y->tick) return 1;
	if(x->kind < y->kind) return -1;
	if(x->kind > y->kind) return 1;
	if(x->note < y->note) return -1;
	if(x->note > y->note) return 1;
	return 0;
}

static unsigned long eof_dtx_synth_tick_for_ms(unsigned long pos)
{
	unsigned long beat;
	double first_len, offset_ticks, frac;
	if(!eof_song || eof_song->beats < 2) return 0;
	first_len = eof_song->beat[1]->fpos - eof_song->beat[0]->fpos;
	if(first_len <= 0.0) first_len = 500.0;
	offset_ticks = (eof_song->beat[0]->fpos / first_len) * (double)EOF_DTX_SYNTH_PPQ;
	if((double)pos < eof_song->beat[0]->fpos)
	{
		double tick = ((double)pos / first_len) * (double)EOF_DTX_SYNTH_PPQ;
		return tick > 0.0 ? (unsigned long)(tick + 0.5) : 0;
	}
	beat = eof_get_beat(eof_song, pos);
	if(beat >= eof_song->beats - 1) beat = eof_song->beats - 2;
	if(eof_song->beat[beat + 1]->fpos <= eof_song->beat[beat]->fpos) frac = 0.0;
	else frac = ((double)pos - eof_song->beat[beat]->fpos) /
		(eof_song->beat[beat + 1]->fpos - eof_song->beat[beat]->fpos);
	if(frac < 0.0) frac = 0.0;
	if(frac > 1.0) frac = 1.0;
	return (unsigned long)(offset_ticks + ((double)beat + frac) * (double)EOF_DTX_SYNTH_PPQ + 0.5);
}

static int eof_dtx_synth_append_event(EOF_DTX_SYNTH_EVENT **events, unsigned long *count, unsigned long *cap,
	unsigned long tick, unsigned char kind, unsigned char note, unsigned long tempo)
{
	EOF_DTX_SYNTH_EVENT *resized;
	if(!events || !count || !cap) return 0;
	if(*count >= *cap)
	{
		unsigned long next = *cap ? *cap * 2UL : 1024UL;
		resized = (EOF_DTX_SYNTH_EVENT *)realloc(*events, next * sizeof(**events));
		if(!resized) return 0;
		*events = resized;
		*cap = next;
	}
	(*events)[*count].tick = tick;
	(*events)[*count].kind = kind;
	(*events)[*count].note = note;
	(*events)[*count].tempo = tempo;
	(*count)++;
	return 1;
}

static int eof_dtx_synth_buf_byte(unsigned char **buf, unsigned long *used, unsigned long *cap, unsigned char value)
{
	unsigned char *resized;
	if(!buf || !used || !cap) return 0;
	if(*used >= *cap)
	{
		unsigned long next = *cap ? *cap * 2UL : 65536UL;
		resized = (unsigned char *)realloc(*buf, next);
		if(!resized) return 0;
		*buf = resized;
		*cap = next;
	}
	(*buf)[(*used)++] = value;
	return 1;
}

static int eof_dtx_synth_buf_vlq(unsigned char **buf, unsigned long *used, unsigned long *cap, unsigned long value)
{
	unsigned char tmp[5];
	int n = 0, i;
	tmp[n++] = (unsigned char)(value & 0x7FUL);
	while((value >>= 7) != 0) tmp[n++] = (unsigned char)((value & 0x7FUL) | 0x80U);
	for(i = n - 1; i >= 0; i--) if(!eof_dtx_synth_buf_byte(buf, used, cap, tmp[i])) return 0;
	return 1;
}

static void eof_dtx_synth_be32(FILE *fp, unsigned long value)
{
	fputc((int)((value >> 24) & 0xFF), fp);
	fputc((int)((value >> 16) & 0xFF), fp);
	fputc((int)((value >> 8) & 0xFF), fp);
	fputc((int)(value & 0xFF), fp);
}

static int eof_dtx_synth_write_midi(const char *path, unsigned char difficulty)
{
	EOF_PRO_GUITAR_TRACK *tp;
	unsigned long backing, i, count = 0, cap = 0, last_tick = 0, used = 0, bcap = 0;
	EOF_DTX_SYNTH_EVENT *events = NULL;
	unsigned char *data = NULL;
	FILE *fp = NULL;
	int ok = 0;

	if(!path || !eof_song || EOF_TRACK_DRUM_DTX >= eof_song->tracks || !eof_song->track[EOF_TRACK_DRUM_DTX]) return 0;
	backing = eof_song->track[EOF_TRACK_DRUM_DTX]->tracknum;
	if(backing >= eof_song->pro_guitar_tracks || !eof_song->pro_guitar_track[backing]) return 0;
	tp = eof_song->pro_guitar_track[backing];

	/* Tempo at MIDI tick zero must describe the pre-roll before EOF's first beat. */
	{
		unsigned long tempo = eof_song->beats ? eof_song->beat[0]->ppqn : 500000UL;
		if(!tempo) tempo = 500000UL;
		if(!eof_dtx_synth_append_event(&events, &count, &cap, 0, 0, 0, tempo)) goto cleanup;
	}
	for(i = 0; i < eof_song->beats; i++)
	{
		unsigned long tick = eof_dtx_synth_tick_for_ms(eof_song->beat[i]->pos);
		unsigned long tempo = eof_song->beat[i]->ppqn ? eof_song->beat[i]->ppqn : 500000UL;
		if(i == 0 || tempo != eof_song->beat[i - 1]->ppqn)
			if(!eof_dtx_synth_append_event(&events, &count, &cap, tick, 0, 0, tempo)) goto cleanup;
	}
	for(i = 0; i < tp->pgnotes; i++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[i];
		unsigned s;
		unsigned long tick;
		if(!np || np->type != difficulty) continue;
		tick = eof_dtx_synth_tick_for_ms(np->pos);
		for(s = 0; s < 6; s++)
		{
			unsigned char midi;
			if(!(np->note & (1U << s))) continue;
			midi = np->frets[s] & 0x7F;
			if(midi > 127) continue;
			if(!eof_dtx_synth_append_event(&events, &count, &cap, tick, 2, midi, 0)) goto cleanup;
			if(!eof_dtx_synth_append_event(&events, &count, &cap, tick + 60UL, 1, midi, 0)) goto cleanup;
		}
	}
	if(count < 2) goto cleanup;
	qsort(events, count, sizeof(*events), eof_dtx_synth_event_cmp);

	for(i = 0; i < count; i++)
	{
		unsigned long delta = events[i].tick >= last_tick ? events[i].tick - last_tick : 0;
		if(!eof_dtx_synth_buf_vlq(&data, &used, &bcap, delta)) goto cleanup;
		if(events[i].kind == 0)
		{
			unsigned long tempo = events[i].tempo;
			if(!eof_dtx_synth_buf_byte(&data, &used, &bcap, 0xFF) ||
			   !eof_dtx_synth_buf_byte(&data, &used, &bcap, 0x51) ||
			   !eof_dtx_synth_buf_byte(&data, &used, &bcap, 3) ||
			   !eof_dtx_synth_buf_byte(&data, &used, &bcap, (unsigned char)((tempo >> 16) & 0xFF)) ||
			   !eof_dtx_synth_buf_byte(&data, &used, &bcap, (unsigned char)((tempo >> 8) & 0xFF)) ||
			   !eof_dtx_synth_buf_byte(&data, &used, &bcap, (unsigned char)(tempo & 0xFF))) goto cleanup;
		}
		else
		{
			if(!eof_dtx_synth_buf_byte(&data, &used, &bcap, events[i].kind == 2 ? 0x99 : 0x89) ||
			   !eof_dtx_synth_buf_byte(&data, &used, &bcap, events[i].note) ||
			   !eof_dtx_synth_buf_byte(&data, &used, &bcap, events[i].kind == 2 ? 112 : 0)) goto cleanup;
		}
		last_tick = events[i].tick;
	}
	if(!eof_dtx_synth_buf_vlq(&data, &used, &bcap, EOF_DTX_SYNTH_PPQ) ||
	   !eof_dtx_synth_buf_byte(&data, &used, &bcap, 0xFF) ||
	   !eof_dtx_synth_buf_byte(&data, &used, &bcap, 0x2F) ||
	   !eof_dtx_synth_buf_byte(&data, &used, &bcap, 0)) goto cleanup;

	fp = fopen(path, "wb");
	if(!fp) goto cleanup;
	fwrite("MThd", 1, 4, fp); eof_dtx_synth_be32(fp, 6); fputc(0, fp); fputc(0, fp); fputc(0, fp); fputc(1, fp); fputc(1, fp); fputc(0xE0, fp);
	fwrite("MTrk", 1, 4, fp); eof_dtx_synth_be32(fp, used);
	if(fwrite(data, 1, used, fp) != used) goto cleanup;
	ok = 1;

cleanup:
	if(fp) fclose(fp);
	if(!ok && path) remove(path);
	free(events);
	free(data);
	return ok;
}

static NCDFS_FILTER_LIST *eof_dtx_synth_get_sf_filter(void)
{
	if(!eof_dtx_synth_sf_filter)
	{
		eof_dtx_synth_sf_filter = ncdfs_filter_list_create();
		if(eof_dtx_synth_sf_filter) (void)ncdfs_filter_list_add(eof_dtx_synth_sf_filter, "sf2;sf3", "SoundFont (*.sf2, *.sf3)", 1);
	}
	return eof_dtx_synth_sf_filter;
}

static int eof_dtx_synth_get_tools(char *fluid, size_t fluidsz, char *sf, size_t sfsz)
{
	const char *saved;
	char *selected;
	saved = get_config_string("gp_synth", "fluidsynth_path", "");
	ustrzcpy(fluid, (int)fluidsz, saved ? saved : "");
	saved = get_config_string("gp_synth", "soundfont_path", "");
	ustrzcpy(sf, (int)sfsz, saved ? saved : "");
	if(!fluid[0] || !exists(fluid))
	{
		selected = ncd_file_select(0, eof_last_eof_path, "Select FluidSynth executable for DTX audio", eof_filter_exe_files);
		eof_clear_input();
		if(!selected) return 0;
		ustrzcpy(fluid, (int)fluidsz, selected);
	}
	if(!sf[0] || !exists(sf))
	{
		selected = ncd_file_select(0, eof_last_eof_path, "Select General MIDI SoundFont for DTX audio", eof_dtx_synth_get_sf_filter());
		eof_clear_input();
		if(!selected) return 0;
		ustrzcpy(sf, (int)sfsz, selected);
	}
	set_config_string("gp_synth", "fluidsynth_path", fluid);
	set_config_string("gp_synth", "soundfont_path", sf);
	flush_config_file();
	return 1;
}

static int eof_dtx_synth_run(const char *fluid, const char *sf, const char *midi, const char *wav)
{
#ifdef ALLEGRO_WINDOWS
	wchar_t wfluid[2048] = {0}, wsf[2048] = {0}, wmidi[2048] = {0}, wwav[2048] = {0};
	const wchar_t *argv[13] = {0};
	intptr_t result;
	(void)uconvert(fluid, U_UTF8, (char *)wfluid, U_UNICODE, sizeof(wfluid));
	(void)uconvert(sf, U_UTF8, (char *)wsf, U_UNICODE, sizeof(wsf));
	(void)uconvert(midi, U_UTF8, (char *)wmidi, U_UNICODE, sizeof(wmidi));
	(void)uconvert(wav, U_UTF8, (char *)wwav, U_UNICODE, sizeof(wwav));
	argv[9] = wwav; argv[10] = wsf; argv[11] = wmidi;
	result = gp_synth_raw_spawnv(_P_WAIT, wfluid, argv);
	return (result == 0 && exists(wav) && file_size_ex(wav) > 44) ? 1 : 0;
#else
	char command[8192];
	int result;
	snprintf(command, sizeof(command), "\"%s\" -ni -g 0.8 -r 44100 -T wav -F \"%s\" \"%s\" \"%s\"", fluid, wav, sf, midi);
	result = eof_system(command);
	return (result == 0 && exists(wav) && file_size_ex(wav) > 44) ? 1 : 0;
#endif
}

int eof_dtx_synth_render_ogg(const char *dest, unsigned char difficulty)
{
	char fluid[1024] = {0}, sf[1024] = {0}, midi[2048] = {0}, wav[2048] = {0}, command[8192] = {0};
	int result;
	if(!dest || !dest[0] || !eof_song) return 0;
	snprintf(midi, sizeof(midi), "%s.dtx.mid", dest);
	snprintf(wav, sizeof(wav), "%s.dtx.wav", dest);
	remove(midi); remove(wav); remove(dest);
	if(!eof_dtx_synth_get_tools(fluid, sizeof(fluid), sf, sizeof(sf))) return 0;
	if(!eof_dtx_synth_write_midi(midi, difficulty))
	{
		eof_log("DTX synth: could not create temporary percussion MIDI.", 1);
		return 0;
	}
	if(!eof_dtx_synth_run(fluid, sf, midi, wav))
	{
		eof_log("DTX synth: FluidSynth could not render the percussion MIDI.", 1);
		return 0;
	}
#ifdef ALLEGRO_WINDOWS
	snprintf(command, sizeof(command), "wavtoogg \"%s\" 4 \"%s\"", wav, dest);
#else
	snprintf(command, sizeof(command), "oggenc --quiet -q 4 --resample 44100 -s 0 \"%s\" -o \"%s\"", wav, dest);
#endif
	result = eof_system(command);
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"DTX synth: FluidSynth WAV -> OGG returned %d, output=%s, difficulty=%u",
		result, exists(dest) ? "present" : "missing", (unsigned)difficulty);
	eof_log(eof_log_string, 1);
	if(result || !exists(dest) || file_size_ex(dest) <= 0)
		return 0;
	remove(midi);
	remove(wav);
	return 1;
}

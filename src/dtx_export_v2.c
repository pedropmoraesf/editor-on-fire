#include <allegro.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agup/agup.h"
#include "beat.h"
#include "dialog.h"
#include "dialog/proc.h"
#include "editor.h"
#include "main.h"
#include "modules/wfsel.h"
#include "song.h"
#include "utility.h"
#include "menu/file.h"
#include "dtx_export.h"
#include "dtx_dialog.h"
#include "dtx_import.h"
#include "dtx_synth.h"

#define EOF_DTXV2_TICKS 384
#define EOF_DTXV2_CHANNELS 12
#define EOF_DTXV2_DIFFS 5
#define EOF_DTXV2_TARGET_LUFS (-14.0)

/* DTXMania/DTXCreator drum lanes confirmed in DTXmaniaNX:
 * 11 HH, 12 SD, 13 BD, 14 HT, 15 LT, 16 CY, 17 FT,
 * 18 HHO, 19 RD, 1A LC, 1B LP and 1C LB. */
enum
{
	DTXV2_HH = 0,
	DTXV2_SD,
	DTXV2_BD,
	DTXV2_HT,
	DTXV2_LT,
	DTXV2_CY,
	DTXV2_FT,
	DTXV2_HHO,
	DTXV2_RD,
	DTXV2_LC,
	DTXV2_LP,
	DTXV2_LB
};

static const char *eof_dtxv2_code[EOF_DTXV2_CHANNELS] =
{
	"11", "12", "13", "14", "15", "16", "17", "18", "19", "1A", "1B", "1C"
};

static const unsigned char eof_dtxv2_canonical_midi[EOF_DTXV2_CHANNELS] =
{
	42, 38, 36, 48, 45, 57, 43, 46, 51, 49, 44, 35
};

static const char *eof_dtxv2_suffix[EOF_DTXV2_DIFFS] = {"_bsc", "_adv", "_ext", "_mas", "_ult"};
static const char *eof_dtxv2_label[EOF_DTXV2_DIFFS] = {"BASIC", "ADVANCED", "EXTREME", "MASTER", "ULTIMATE"};
static const int eof_dtxv2_dlevel[EOF_DTXV2_DIFFS] = {15, 35, 55, 75, 95};

typedef struct
{
	double start_ms;
	double end_ms;
	unsigned beats;
} EOF_DTXV2_MEASURE_TIME;

typedef struct
{
	unsigned beats;
	double multiplier;
	unsigned char hit[EOF_DTXV2_CHANNELS][EOF_DTXV2_TICKS];
} EOF_DTXV2_MEASURE;

typedef struct
{
	EOF_DTXV2_MEASURE *measure;
	unsigned long count; /* Includes DTX padding measure zero. */
} EOF_DTXV2_GRADE;

typedef struct
{
	char title[256];
	char artist[256];
	char comment[256];
	char jacket[1024];
	char premovie[1024];
	char gameplay_video[1024];
	char stage_image[1024];
	char result_image[1024];
	long latency_ms;
	double target_lufs;
	int normalize;
	int multilevel;
} EOF_DTXV2_CONFIG;

static char dtx_title[256] = {0};
static char dtx_artist[256] = {0};
static char dtx_comment[256] = {0};
static char dtx_latency[32] = "250";
static char dtx_lufs[32] = "-14.0";
static char dtx_jacket[1024] = {0};
static char dtx_premovie[1024] = {0};
static char dtx_gameplay_video[1024] = {0};
static char dtx_stage_image[1024] = {0};
static char dtx_result_image[1024] = {0};
static int dtx_last_export_success = 0;

/* Asset browse buttons deliberately exit and reopen the same dialog.  This is
 * reliable with Allegro 4 and avoids nesting the native file dialog in a GUI
 * callback. */
static DIALOG eof_dtxv2_dialog[] =
{
	/* proc                 x   y    w    h  fg bg key flags       d1  d2  dp */
	{ eof_window_proc,       0,  20,  610, 590, 2, 23, 0, 0,         0,  0,  "DTXMania Export Settings", NULL, NULL },
	{ d_agup_text_proc,     14,  52,  180,  12, 2, 23, 0, 0,         0,  0,  "Song title (required):", NULL, NULL },
	{ eof_edit_proc,        14,  68,  580,  20, 2, 23, 0, 0,       255,  0,  dtx_title, NULL, NULL },
	{ d_agup_text_proc,     14,  96,  180,  12, 2, 23, 0, 0,         0,  0,  "Artist / author (required):", NULL, NULL },
	{ eof_edit_proc,        14, 112,  580,  20, 2, 23, 0, 0,       255,  0,  dtx_artist, NULL, NULL },
	{ d_agup_text_proc,     14, 140,  120,  12, 2, 23, 0, 0,         0,  0,  "Comment:", NULL, NULL },
	{ eof_edit_proc,        14, 156,  580,  20, 2, 23, 0, 0,       255,  0,  dtx_comment, NULL, NULL },
	{ d_agup_text_proc,     14, 184,  185,  12, 2, 23, 0, 0,         0,  0,  "Latency compensation (ms):", NULL, NULL },
	{ eof_edit_proc,       202, 180,   72,  20, 2, 23, 0, 0,        16,  0,  dtx_latency, NULL, NULL },
	{ d_agup_text_proc,    282, 184,  310,  12, 2, 23, 0, 0,         0,  0,  "For Bluetooth audio latency with electronic drums.", NULL, NULL },
	{ d_agup_check_proc,    14, 212,  320,  16, 2, 23, 0, D_SELECTED,1,  0,  "Normalize audio to streaming loudness", NULL, NULL },
	{ d_agup_text_proc,     34, 238,  220,  12, 2, 23, 0, 0,         0,  0,  "Loudness target (LUFS):", NULL, NULL },
	{ eof_edit_proc,       256, 234,   72,  20, 2, 23, 0, 0,        16,  0,  dtx_lufs, NULL, NULL },
	{ d_agup_check_proc,    14, 264,  350,  16, 2, 23, 0, D_SELECTED,1,  0,  "Generate / maintain five DTX difficulty levels", NULL, NULL },

	{ d_agup_text_proc,     14, 296,  115,  12, 2, 23, 0, 0,         0,  0,  "Jacket / cover:", NULL, NULL },
	{ d_agup_text_proc,    132, 296,  340,  12, 2, 23, 0, 0,         0,  0,  dtx_jacket, NULL, NULL },
	{ d_agup_button_proc,  485, 288,  108,  24, 2, 23, 0, D_EXIT,    0,  0,  "Choose...", NULL, NULL },
	{ d_agup_text_proc,     14, 326,  115,  12, 2, 23, 0, 0,         0,  0,  "Preview movie:", NULL, NULL },
	{ d_agup_text_proc,    132, 326,  340,  12, 2, 23, 0, 0,         0,  0,  dtx_premovie, NULL, NULL },
	{ d_agup_button_proc,  485, 318,  108,  24, 2, 23, 0, D_EXIT,    0,  0,  "Choose...", NULL, NULL },
	{ d_agup_text_proc,     14, 356,  115,  12, 2, 23, 0, 0,         0,  0,  "Gameplay video:", NULL, NULL },
	{ d_agup_text_proc,    132, 356,  340,  12, 2, 23, 0, 0,         0,  0,  dtx_gameplay_video, NULL, NULL },
	{ d_agup_button_proc,  485, 348,  108,  24, 2, 23, 0, D_EXIT,    0,  0,  "Choose...", NULL, NULL },
	{ d_agup_text_proc,     14, 386,  115,  12, 2, 23, 0, 0,         0,  0,  "Stage image:", NULL, NULL },
	{ d_agup_text_proc,    132, 386,  340,  12, 2, 23, 0, 0,         0,  0,  dtx_stage_image, NULL, NULL },
	{ d_agup_button_proc,  485, 378,  108,  24, 2, 23, 0, D_EXIT,    0,  0,  "Choose...", NULL, NULL },
	{ d_agup_text_proc,     14, 416,  115,  12, 2, 23, 0, 0,         0,  0,  "Result image:", NULL, NULL },
	{ d_agup_text_proc,    132, 416,  340,  12, 2, 23, 0, 0,         0,  0,  dtx_result_image, NULL, NULL },
	{ d_agup_button_proc,  485, 408,  108,  24, 2, 23, 0, D_EXIT,    0,  0,  "Choose...", NULL, NULL },
	{ d_agup_text_proc,     14, 452,  575,  26, 2, 23, 0, 0,         0,  0,  "Optional assets are copied into the song folder and referenced by PREIMAGE, PREMOVIE, AVI, STAGEFILE and RESULTIMAGE.", NULL, NULL },
	{ d_agup_button_proc,  150, 548,  135,  28, 2, 23, '\r', D_EXIT,0,  0,  "Export", NULL, NULL },
	{ d_agup_button_proc,  325, 548,  135,  28, 2, 23, 0, D_EXIT,    0,  0,  "Cancel", NULL, NULL },
	{ NULL,                  0,   0,    0,   0, 0,  0, 0, 0,         0,  0,  NULL, NULL, NULL }
};

static void dtxv2_join(char *dst, size_t size, const char *folder, const char *name)
{
	if(!dst || !size) return;
	dst[0] = '\0';
	if(folder) ustrzcpy(dst, (int)size, folder);
	if(dst[0]) put_backslash(dst);
	if(name) ustrzcat(dst, (int)size, name);
}

static void dtxv2_sanitize(const char *src, char *dst, size_t size)
{
	size_t i = 0, o = 0;
	if(!dst || !size) return;
	if(!src) src = "";
	while(src[i] && o + 1 < size)
	{
		unsigned char c = (unsigned char)src[i++];
		if(c < 32 || strchr("<>:\"/\\|?*", c)) c = '_';
		if(c == ' ' && (!o || dst[o - 1] == ' ')) continue;
		dst[o++] = (char)c;
	}
	while(o && (dst[o - 1] == ' ' || dst[o - 1] == '.')) o--;
	dst[o] = '\0';
	if(!dst[0]) ustrzcpy(dst, (int)size, "DTX_song");
}

static void dtxv2_basename(const char *artist, const char *title, char *dst, size_t size)
{
	char temp[600] = {0};
	snprintf(temp, sizeof(temp) - 1, "%s%s%s", artist ? artist : "", (artist && *artist && title && *title) ? " - " : "", title ? title : "");
	dtxv2_sanitize(temp, dst, size);
}

static int dtxv2_midi_channel(unsigned midi)
{
	switch(midi)
	{
		case 42: return DTXV2_HH;
		case 44: return DTXV2_LP;
		case 46: return DTXV2_HHO;
		case 37: case 38: case 39: case 40: case 62: return DTXV2_SD;
		case 31: case 36: return DTXV2_BD;
		case 35: return DTXV2_LB;
		case 48: case 50: return DTXV2_HT;
		case 45: case 47: return DTXV2_LT;
		case 41: case 43: return DTXV2_FT;
		case 49: return DTXV2_LC;
		case 57: return DTXV2_CY;
		case 52: case 55: return DTXV2_CY;
		case 51: case 53: case 59: return DTXV2_RD;
	}
	return -1;
}

static int dtxv2_difficulty_populated(EOF_SONG *sp, unsigned diff)
{
	unsigned long i, tracknum;
	EOF_PRO_GUITAR_TRACK *tp;
	if(!sp || sp->tracks <= EOF_TRACK_DRUM_DTX || !sp->track[EOF_TRACK_DRUM_DTX]) return 0;
	tracknum = sp->track[EOF_TRACK_DRUM_DTX]->tracknum;
	if(tracknum >= sp->pro_guitar_tracks || !sp->pro_guitar_track[tracknum]) return 0;
	tp = sp->pro_guitar_track[tracknum];
	for(i = 0; i < tp->pgnotes; i++) if(tp->pgnote[i] && tp->pgnote[i]->type == diff) return 1;
	return 0;
}

void eof_dtx_migrate_import_to_ultimate(EOF_SONG *sp)
{
	unsigned long i, tracknum;
	EOF_PRO_GUITAR_TRACK *tp;
	int has3 = 0, has4 = 0;
	if(!sp || sp->tracks <= EOF_TRACK_DRUM_DTX || !sp->track[EOF_TRACK_DRUM_DTX]) return;
	tracknum = sp->track[EOF_TRACK_DRUM_DTX]->tracknum;
	if(tracknum >= sp->pro_guitar_tracks || !sp->pro_guitar_track[tracknum]) return;
	tp = sp->pro_guitar_track[tracknum];
	for(i = 0; i < tp->pgnotes; i++)
	{
		if(!tp->pgnote[i]) continue;
		if(tp->pgnote[i]->type == 3) has3 = 1;
		if(tp->pgnote[i]->type == 4) has4 = 1;
	}
	/* Existing builds imported the GP drum source as EOF Expert/type 3.  Only
	 * migrate when ULT is empty, so a user-authored MAS is never overwritten. */
	if(has3 && !has4 && !dtxv2_difficulty_populated(sp, 0) && !dtxv2_difficulty_populated(sp, 1) && !dtxv2_difficulty_populated(sp, 2))
	{
		for(i = 0; i < tp->pgnotes; i++) if(tp->pgnote[i] && tp->pgnote[i]->type == 3) tp->pgnote[i]->type = 4;
		eof_pro_guitar_track_sort_notes(tp);
		eof_changes = 1;
	}
}

int eof_dtx_track_has_notes(EOF_SONG *sp)
{
	unsigned long tracknum;
	if(!sp || sp->tracks <= EOF_TRACK_DRUM_DTX || !sp->track[EOF_TRACK_DRUM_DTX]) return 0;
	tracknum = sp->track[EOF_TRACK_DRUM_DTX]->tracknum;
	return (tracknum < sp->pro_guitar_tracks && sp->pro_guitar_track[tracknum] && sp->pro_guitar_track[tracknum]->pgnotes) ? 1 : 0;
}

static EOF_DTXV2_MEASURE_TIME *dtxv2_measure_times(EOF_SONG *sp, unsigned long *count)
{
	EOF_DTXV2_MEASURE_TIME *mt;
	unsigned long i, n = 0, index = 0;
	if(count) *count = 0;
	if(!sp || !count || sp->beats < 2) return NULL;
	eof_process_beat_statistics(sp, EOF_TRACK_DRUM_DTX);
	for(i = 0; i < sp->beats; i++) if(sp->beat[i]->beat_within_measure == 0) n++;
	if(!n) n = (sp->beats + 3) / 4;
	mt = calloc(n, sizeof(*mt));
	if(!mt) return NULL;
	for(i = 0; i < sp->beats && index < n; i++)
	{
		if(sp->beat[i]->beat_within_measure != 0) continue;
		mt[index].start_ms = sp->beat[i]->fpos;
		mt[index].beats = (sp->beat[i]->num_beats_in_measure > 0) ? (unsigned)sp->beat[i]->num_beats_in_measure : 4;
		index++;
	}
	if(!index)
	{
		for(i = 0; i < n; i++)
		{
			unsigned long b = i * 4;
			if(b >= sp->beats) break;
			mt[i].start_ms = sp->beat[b]->fpos;
			mt[i].beats = 4;
		}
		index = n;
	}
	n = index;
	for(i = 0; i < n; i++)
	{
		if(i + 1 < n) mt[i].end_ms = mt[i + 1].start_ms;
		else
		{
			double beatlen = (sp->beats > 1) ? (sp->beat[sp->beats - 1]->fpos - sp->beat[sp->beats - 2]->fpos) : 500.0;
			if(beatlen <= 0.0) beatlen = 500.0;
			mt[i].end_ms = mt[i].start_ms + beatlen * mt[i].beats;
		}
	}
	*count = n;
	return mt;
}

static double dtxv2_average_bpm(EOF_SONG *sp)
{
	double sum = 0.0;
	unsigned long i, intervals = 0;
	if(!sp || sp->beats < 2) return 120.0;
	for(i = 0; i + 1 < sp->beats; i++)
	{
		double d = sp->beat[i + 1]->fpos - sp->beat[i]->fpos;
		if(d > 0.0) { sum += d; intervals++; }
	}
	if(!intervals || sum <= 0.0) return 120.0;
	return 60000.0 / (sum / intervals);
}

static EOF_DTXV2_GRADE *dtxv2_grade_new(const EOF_DTXV2_MEASURE_TIME *mt, unsigned long measures, double bpm, double padding)
{
	EOF_DTXV2_GRADE *g;
	unsigned long i;
	double base = 240000.0 / ((bpm > 0.0) ? bpm : 120.0);
	g = calloc(1, sizeof(*g));
	if(!g) return NULL;
	g->count = measures + 1;
	g->measure = calloc(g->count, sizeof(*g->measure));
	if(!g->measure) { free(g); return NULL; }
	g->measure[0].beats = 4;
	g->measure[0].multiplier = padding / base;
	if(g->measure[0].multiplier < 0.001) g->measure[0].multiplier = 0.001;
	for(i = 0; i < measures; i++)
	{
		g->measure[i + 1].beats = mt[i].beats ? mt[i].beats : 4;
		g->measure[i + 1].multiplier = (mt[i].end_ms - mt[i].start_ms) / base;
		if(g->measure[i + 1].multiplier < 0.001) g->measure[i + 1].multiplier = 0.001;
	}
	return g;
}

static void dtxv2_grade_free(EOF_DTXV2_GRADE *g)
{
	if(!g) return;
	free(g->measure);
	free(g);
}

static EOF_DTXV2_GRADE *dtxv2_grade_from_track(EOF_SONG *sp, unsigned diff, const EOF_DTXV2_MEASURE_TIME *mt, unsigned long measures, double bpm, double padding)
{
	EOF_DTXV2_GRADE *g;
	EOF_PRO_GUITAR_TRACK *tp;
	unsigned long tracknum, i, m = 0, s;
	g = dtxv2_grade_new(mt, measures, bpm, padding);
	if(!g) return NULL;
	tracknum = sp->track[EOF_TRACK_DRUM_DTX]->tracknum;
	tp = sp->pro_guitar_track[tracknum];
	for(i = 0; i < tp->pgnotes; i++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[i];
		double duration, ratio;
		int tick;
		if(!np || np->type != diff) continue;
		while(m + 1 < measures && np->pos >= mt[m].end_ms) m++;
		if(m >= measures || np->pos + 0.5 < mt[m].start_ms || np->pos >= mt[m].end_ms) continue;
		duration = mt[m].end_ms - mt[m].start_ms;
		if(duration <= 0.0) continue;
		ratio = (np->pos - mt[m].start_ms) / duration;
		tick = (int)floor(ratio * EOF_DTXV2_TICKS + 0.5);
		if(tick < 0) tick = 0;
		if(tick >= EOF_DTXV2_TICKS) tick = EOF_DTXV2_TICKS - 1;
		for(s = 0; s < 6; s++)
		{
			unsigned mask = 1U << s;
			int ch;
			if(!(np->note & mask) || np->frets[s] == 0xFF) continue;
			ch = dtxv2_midi_channel(np->frets[s] & 0x7F);
			if(ch >= 0) g->measure[m + 1].hit[ch][tick] = 1;
		}
	}
	return g;
}

static int dtxv2_group(int ch)
{
	if(ch == DTXV2_HH || ch == DTXV2_HHO || ch == DTXV2_RD || ch == DTXV2_LP) return 0;
	if(ch == DTXV2_BD || ch == DTXV2_LB) return 1;
	if(ch == DTXV2_SD) return 2;
	if(ch == DTXV2_HT || ch == DTXV2_LT || ch == DTXV2_FT) return 3;
	return 4; /* CY + LC */
}

static int dtxv2_min_gap(unsigned level, int group, unsigned beats)
{
	int beat = beats ? (EOF_DTXV2_TICKS / (int)beats) : 96;
	int eighth = beat / 2, sixteenth = beat / 4;
	if(level == 3) { int v[5] = {sixteenth, 0, 0, sixteenth, 0}; return v[group]; }
	if(level == 2) { int v[5] = {eighth, sixteenth, sixteenth, eighth, sixteenth}; return v[group]; }
	if(level == 1) { int v[5] = {eighth, sixteenth, sixteenth, -1, beat}; return v[group]; }
	{ int v[5] = {beat, eighth, eighth, -1, 9999}; return v[group]; }
}

static int dtxv2_strength(int tick, int beat)
{
	if(beat <= 0) beat = 96;
	if(tick % beat == 0) return 4;
	if(tick % (beat / 2 ? beat / 2 : 1) == 0) return 3;
	if(tick % (beat / 4 ? beat / 4 : 1) == 0) return 2;
	return 1;
}

static EOF_DTXV2_GRADE *dtxv2_downsample(const EOF_DTXV2_GRADE *src, unsigned level)
{
	EOF_DTXV2_GRADE *dst;
	unsigned long m;
	if(!src || !src->measure) return NULL;
	dst = calloc(1, sizeof(*dst));
	if(!dst) return NULL;
	dst->count = src->count;
	dst->measure = calloc(dst->count, sizeof(*dst->measure));
	if(!dst->measure) { free(dst); return NULL; }
	memcpy(dst->measure, src->measure, dst->count * sizeof(*dst->measure));
	for(m = 1; m < dst->count; m++)
	{
		int group;
		for(group = 0; group < 5; group++)
		{
			int gap = dtxv2_min_gap(level, group, dst->measure[m].beats), beat = dst->measure[m].beats ? EOF_DTXV2_TICKS / (int)dst->measure[m].beats : 96;
			unsigned char out[EOF_DTXV2_CHANNELS][EOF_DTXV2_TICKS] = {{0}};
			int kept[EOF_DTXV2_TICKS], keptn = 0, strength, t, ch;
			if(gap < 0)
			{
				for(ch = 0; ch < EOF_DTXV2_CHANNELS; ch++) if(dtxv2_group(ch) == group) memset(dst->measure[m].hit[ch], 0, EOF_DTXV2_TICKS);
				continue;
			}
			if(gap == 0) continue;
			for(strength = 4; strength >= 1; strength--)
			{
				for(t = 0; t < EOF_DTXV2_TICKS; t++)
				{
					int chosen = -1, k, ok = 1;
					if(dtxv2_strength(t, beat) != strength) continue;
					for(ch = 0; ch < EOF_DTXV2_CHANNELS; ch++) if(dtxv2_group(ch) == group && dst->measure[m].hit[ch][t]) { chosen = ch; break; }
					if(chosen < 0) continue;
					for(k = 0; k < keptn; k++) if(abs(t - kept[k]) < gap) { ok = 0; break; }
					if(ok)
					{
						kept[keptn++] = t;
						for(ch = 0; ch < EOF_DTXV2_CHANNELS; ch++) if(dtxv2_group(ch) == group && dst->measure[m].hit[ch][t]) out[ch][t] = 1;
					}
				}
			}
			for(ch = 0; ch < EOF_DTXV2_CHANNELS; ch++) if(dtxv2_group(ch) == group) memcpy(dst->measure[m].hit[ch], out[ch], EOF_DTXV2_TICKS);
		}
	}
	return dst;
}

static int dtxv2_materialize(EOF_SONG *sp, unsigned diff, const EOF_DTXV2_GRADE *g, const EOF_DTXV2_MEASURE_TIME *mt, unsigned long measures)
{
	EOF_PRO_GUITAR_TRACK *tp;
	unsigned long tracknum, m;
	if(!sp || !g || diff >= 4 || dtxv2_difficulty_populated(sp, diff)) return 1;
	tracknum = sp->track[EOF_TRACK_DRUM_DTX]->tracknum;
	tp = sp->pro_guitar_track[tracknum];
	tp->note = tp->pgnote;
	for(m = 0; m < measures; m++)
	{
		int t, ch;
		double duration = mt[m].end_ms - mt[m].start_ms;
		if(duration <= 0.0) continue;
		for(t = 0; t < EOF_DTXV2_TICKS; t++)
		{
			for(ch = 0; ch < EOF_DTXV2_CHANNELS; ch++)
			{
				EOF_PRO_GUITAR_NOTE *np;
				if(!g->measure[m + 1].hit[ch][t]) continue;
				np = eof_pro_guitar_track_add_note(tp);
				if(!np) return 0;
				np->type = (unsigned char)diff;
				np->pos = (unsigned long)floor(mt[m].start_ms + duration * ((double)t / EOF_DTXV2_TICKS) + 0.5);
				np->length = 1;
				np->note = 1;
				np->frets[0] = eof_dtxv2_canonical_midi[ch];
			}
		}
	}
	eof_pro_guitar_track_sort_notes(tp);
	eof_changes = 1;
	return 1;
}

static int dtxv2_generate_missing_difficulties(EOF_SONG *sp, const EOF_DTXV2_MEASURE_TIME *mt, unsigned long measures, double bpm, double padding)
{
	EOF_DTXV2_GRADE *ult, *generated[4] = {NULL, NULL, NULL, NULL};
	int d, ok = 1;
	eof_dtx_migrate_import_to_ultimate(sp);
	if(!dtxv2_difficulty_populated(sp, 4)) return 0;
	ult = dtxv2_grade_from_track(sp, 4, mt, measures, bpm, padding);
	if(!ult) return 0;
	for(d = 0; d < 4; d++)
	{
		if(dtxv2_difficulty_populated(sp, (unsigned)d)) continue; /* User-authored/generated content wins. */
		generated[d] = dtxv2_downsample(ult, (unsigned)d);
		if(!generated[d] || !dtxv2_materialize(sp, (unsigned)d, generated[d], mt, measures)) ok = 0;
	}
	for(d = 0; d < 4; d++) dtxv2_grade_free(generated[d]);
	dtxv2_grade_free(ult);
	(void)eof_detect_difficulties(sp, EOF_TRACK_DRUM_DTX);
	return ok;
}

static int dtxv2_write_text(PACKFILE *fp, const char *text)
{
	return (fp && text && pack_fputs(text, fp) >= 0) ? 1 : 0;
}

static int dtxv2_copy_asset(const char *source, const char *folder, char *filename, size_t filename_size)
{
	char safe[512], dest[2048], stem[512], ext[64] = {0};
	const char *name, *dot;
	if(filename && filename_size) filename[0] = '\0';
	if(!source || !*source || !exists(source)) return 0;
	name = get_filename(source);
	dot = strrchr(name, '.');
	if(dot && strlen(dot) < sizeof(ext)) ustrzcpy(ext, sizeof(ext), dot);
	if(dot)
	{
		size_t len = (size_t)(dot - name);
		if(len >= sizeof(stem)) len = sizeof(stem) - 1;
		memcpy(stem, name, len); stem[len] = '\0';
	}
	else ustrzcpy(stem, sizeof(stem), name);
	dtxv2_sanitize(stem, safe, sizeof(safe));
	ustrzcat(safe, sizeof(safe), ext);
	dtxv2_join(dest, sizeof(dest), folder, safe);
	if(strcmp(source, dest) && !eof_copy_file(source, dest)) return 0;
	if(filename && filename_size) ustrzcpy(filename, (int)filename_size, safe);
	return 1;
}

static int dtxv2_write_dtx(const char *path, const EOF_DTXV2_GRADE *g, double bpm, const char *audio, const EOF_DTXV2_CONFIG *cfg,
	const char *jacket, const char *premovie, const char *video, const char *stage, const char *result, unsigned diff)
{
	PACKFILE *fp;
	unsigned long m;
	int ch, t;
	char line[2048];
	if(!path || !g || !cfg) return 0;
	fp = pack_fopen(path, "w");
	if(!fp) return 0;
	snprintf(line, sizeof(line), "; Generated by Editor On Fire DTXMania exporter\n#TITLE: %s\n#ARTIST: %s\n#COMMENT: %s\n#BPM: %.4f\n#DLEVEL: %d\n#DIFFICULTY: %u\n#PREVIEW: preview.ogg\n",
		cfg->title, cfg->artist, cfg->comment[0] ? cfg->comment : "Exported with Editor On Fire", bpm, eof_dtxv2_dlevel[diff], diff + 1);
	dtxv2_write_text(fp, line);
	if(jacket && *jacket) { snprintf(line, sizeof(line), "#PREIMAGE: %s\n", jacket); dtxv2_write_text(fp, line); }
	if(premovie && *premovie) { snprintf(line, sizeof(line), "#PREMOVIE: %s\n", premovie); dtxv2_write_text(fp, line); }
	if(stage && *stage) { snprintf(line, sizeof(line), "#STAGEFILE: %s\n", stage); dtxv2_write_text(fp, line); }
	if(result && *result) { snprintf(line, sizeof(line), "#RESULTIMAGE: %s\n", result); dtxv2_write_text(fp, line); }
	if(video && *video) { snprintf(line, sizeof(line), "#AVI01: %s\n", video); dtxv2_write_text(fp, line); }
	snprintf(line, sizeof(line), "#WAV01: %s\n#BGMWAV: 01\n#00001: 01\n", audio); dtxv2_write_text(fp, line);
	if(video && *video) dtxv2_write_text(fp, "#00054: 01\n");
	dtxv2_write_text(fp,
		"#WAV11: hihat.wav\n#WAV12: snare.wav\n#WAV13: kick.wav\n#WAV14: tom1.wav\n#WAV15: tom2.wav\n#WAV16: crash.wav\n#WAV17: tom3.wav\n#WAV18: hihat_open.wav\n#WAV19: ride.wav\n#WAV1A: crash.wav\n#WAV1B: hihat.wav\n#WAV1C: kick.wav\n\n");
	for(m = 0; m < g->count; m++)
	{
		if(fabs(g->measure[m].multiplier - 1.0) > 0.000001 || m <= 1)
		{
			snprintf(line, sizeof(line), "#%03lu02: %.6f\n", m, g->measure[m].multiplier);
			dtxv2_write_text(fp, line);
		}
		for(ch = 0; ch < EOF_DTXV2_CHANNELS; ch++)
		{
			int any = 0;
			for(t = 0; t < EOF_DTXV2_TICKS; t++) if(g->measure[m].hit[ch][t]) { any = 1; break; }
			if(!any) continue;
			snprintf(line, sizeof(line), "#%03lu%s: ", m, eof_dtxv2_code[ch]); dtxv2_write_text(fp, line);
			for(t = 0; t < EOF_DTXV2_TICKS; t++) dtxv2_write_text(fp, g->measure[m].hit[ch][t] ? eof_dtxv2_code[ch] : "00");
			dtxv2_write_text(fp, "\n");
		}
	}
	pack_fclose(fp);
	return 1;
}

static int dtxv2_resolve_ffmpeg(char *cmd, size_t size, int allow_prompt)
{
	int answer;
	if(!cmd || !size) return 0;
	cmd[0] = '\0';
	if(eof_ffmpeg_executable_path[0] && exists(eof_ffmpeg_executable_path))
	{
		ustrzcpy(cmd, (int)size, eof_ffmpeg_executable_path);
		return 1;
	}
	if(exists("ffmpeg.exe")) { ustrzcpy(cmd, (int)size, "ffmpeg.exe"); return 1; }
	if(eof_system("ffmpeg -version > NUL 2>&1") == 0) { ustrzcpy(cmd, (int)size, "ffmpeg"); return 1; }
	if(!allow_prompt) return 0;
	answer = alert3("FFmpeg was not found automatically.",
		"It is only needed for loudness normalization and a 25-second preview.",
		"Without it, EOF will still export the chart and will use the full song as preview audio.",
		"&Locate FFmpeg", "&Continue", "Cancel", 'l', 'c', 0);
	if(answer == 3) return -1;
	if(answer == 1)
	{
		(void)eof_menu_file_link_ffmpeg();
		if(eof_ffmpeg_executable_path[0] && exists(eof_ffmpeg_executable_path))
		{
			ustrzcpy(cmd, (int)size, eof_ffmpeg_executable_path);
			return 1;
		}
	}
	return 0;
}

static int dtxv2_ffmpeg_audio(const char *ffmpeg, const char *src, const char *dest, double lufs, int normalize)
{
	char command[8192];
	if(!ffmpeg || !*ffmpeg || !src || !dest) return 0;
	if(normalize)
		snprintf(command, sizeof(command), "\"%s\" -y -i \"%s\" -af loudnorm=I=%.2f:TP=-1.0:LRA=11 -c:a libvorbis -q:a 5 \"%s\"", ffmpeg, src, lufs, dest);
	else
		snprintf(command, sizeof(command), "\"%s\" -y -i \"%s\" -c:a libvorbis -q:a 5 \"%s\"", ffmpeg, src, dest);
	return (eof_system(command) == 0 && exists(dest)) ? 1 : 0;
}

static int dtxv2_preview(const char *ffmpeg, const char *audio, const char *dest)
{
	char command[8192];
	double start = eof_music_length > 40000 ? (double)eof_music_length * 0.30 / 1000.0 : 0.0;
	if(ffmpeg && *ffmpeg)
	{
		snprintf(command, sizeof(command), "\"%s\" -y -ss %.3f -i \"%s\" -t 25 -af afade=t=in:st=0:d=2,afade=t=out:st=23:d=2 -c:a libvorbis -q:a 4 \"%s\"", ffmpeg, start, audio, dest);
		if(eof_system(command) == 0 && exists(dest)) return 1;
	}
	/* A full-length OGG is a valid PREVIEW target and is preferable to leaving
	 * the chart without preview audio when FFmpeg is not installed. */
	return eof_copy_file(audio, dest);
}

static void dtxv2_browse_asset(char *buffer, size_t size, const char *title)
{
	char *selected;
	eof_render();
	selected = ncd_file_select(0, eof_song_path[0] ? eof_song_path : NULL, title, NULL);
	eof_clear_input();
	if(selected && selected[0]) ustrzcpy(buffer, (int)size, selected);
}

static int dtxv2_prompt(EOF_DTXV2_CONFIG *cfg)
{
	int result;
	char *end;
	if(!cfg || !eof_song || !eof_song->tags) return 0;
	ustrzcpy(dtx_title, sizeof(dtx_title), eof_song->tags->title);
	ustrzcpy(dtx_artist, sizeof(dtx_artist), eof_song->tags->artist);
	if(!dtx_comment[0]) ustrzcpy(dtx_comment, sizeof(dtx_comment), "Exported with Editor On Fire");
	ustrzcpy(dtx_latency, sizeof(dtx_latency), "250");
	ustrzcpy(dtx_lufs, sizeof(dtx_lufs), "-14.0");
	eof_dtxv2_dialog[10].flags |= D_SELECTED;
	eof_dtxv2_dialog[13].flags |= D_SELECTED;
	for(;;)
	{
		eof_cursor_visible = 0; eof_pen_visible = 0; eof_render();
		eof_color_dialog(eof_dtxv2_dialog, gui_fg_color, gui_bg_color);
		eof_conditionally_center_dialog(eof_dtxv2_dialog);
		result = eof_popup_dialog(eof_dtxv2_dialog, 2);
		eof_show_mouse(NULL); eof_cursor_visible = 1; eof_pen_visible = 1;
		if(result == 16) { dtxv2_browse_asset(dtx_jacket, sizeof(dtx_jacket), "Select DTX jacket / cover image"); continue; }
		if(result == 19) { dtxv2_browse_asset(dtx_premovie, sizeof(dtx_premovie), "Select DTX preview movie"); continue; }
		if(result == 22) { dtxv2_browse_asset(dtx_gameplay_video, sizeof(dtx_gameplay_video), "Select DTX gameplay video"); continue; }
		if(result == 25) { dtxv2_browse_asset(dtx_stage_image, sizeof(dtx_stage_image), "Select DTX stage image"); continue; }
		if(result == 28) { dtxv2_browse_asset(dtx_result_image, sizeof(dtx_result_image), "Select DTX result image"); continue; }
		if(result == 31) return 0;
		if(result != 30) return 0;
		if(!eof_check_string(dtx_title) || !eof_check_string(dtx_artist))
		{
			allegro_message("Song title and artist / author are required for DTXMania export.");
			continue;
		}
		break;
	}
	memset(cfg, 0, sizeof(*cfg));
	ustrzcpy(cfg->title, sizeof(cfg->title), dtx_title);
	ustrzcpy(cfg->artist, sizeof(cfg->artist), dtx_artist);
	ustrzcpy(cfg->comment, sizeof(cfg->comment), dtx_comment);
	ustrzcpy(cfg->jacket, sizeof(cfg->jacket), dtx_jacket);
	ustrzcpy(cfg->premovie, sizeof(cfg->premovie), dtx_premovie);
	ustrzcpy(cfg->gameplay_video, sizeof(cfg->gameplay_video), dtx_gameplay_video);
	ustrzcpy(cfg->stage_image, sizeof(cfg->stage_image), dtx_stage_image);
	ustrzcpy(cfg->result_image, sizeof(cfg->result_image), dtx_result_image);
	cfg->latency_ms = strtol(dtx_latency, &end, 10); if(end == dtx_latency || *end) cfg->latency_ms = 250;
	{
		char temp[32]; size_t i;
		ustrzcpy(temp, sizeof(temp), dtx_lufs); for(i = 0; temp[i]; i++) if(temp[i] == ',') temp[i] = '.';
		cfg->target_lufs = strtod(temp, &end); if(end == temp || *end || !isfinite(cfg->target_lufs)) cfg->target_lufs = EOF_DTXV2_TARGET_LUFS;
	}
	cfg->normalize = (eof_dtxv2_dialog[10].flags & D_SELECTED) ? 1 : 0;
	cfg->multilevel = (eof_dtxv2_dialog[13].flags & D_SELECTED) ? 1 : 0;
	return 1;
}

int eof_dtx_last_export_was_successful(void)
{
	return dtx_last_export_success;
}

int eof_menu_file_export_dtxmania(void)
{
	EOF_DTXV2_CONFIG cfg;
	EOF_DTXV2_MEASURE_TIME *mt = NULL;
	EOF_DTXV2_GRADE *grades[EOF_DTXV2_DIFFS] = {NULL, NULL, NULL, NULL, NULL};
	unsigned long measures = 0;
	char base[600], parent[1024], folder[2048], audio_name[700], audio_path[2048], preview_path[2048];
	char jacket[512] = {0}, premovie[512] = {0}, video[512] = {0}, stage[512] = {0}, result[512] = {0};
	char ffmpeg[1024] = {0}, *selected_folder;
	const char *source_audio;
	double bpm, padding;
	int ffstatus, d, highest = -1, success = 0, synth_imported_audio = 0, synth_diff = -1;
	dtx_last_export_success = 0;
	if(!eof_song || !eof_song_loaded || !eof_dtx_track_has_notes(eof_song))
	{
		allegro_message("PART_REAL_DRUM_DTX contains no notes to export.");
		return 1;
	}
	eof_dtx_migrate_import_to_ultimate(eof_song);
	if(!dtxv2_prompt(&cfg)) return 1;
	mt = dtxv2_measure_times(eof_song, &measures);
	if(!mt || !measures) { allegro_message("Unable to determine DTX measure boundaries from the EOF beat map."); goto cleanup; }
	bpm = dtxv2_average_bpm(eof_song);
	padding = mt[0].start_ms + cfg.latency_ms; if(padding < 0.0) padding = 0.0;
	if(cfg.multilevel && !dtxv2_generate_missing_difficulties(eof_song, mt, measures, bpm, padding))
	{
		allegro_message("Unable to generate the missing DTX difficulty levels.");
		goto cleanup;
	}
	for(d = 0; d < EOF_DTXV2_DIFFS; d++) if(dtxv2_difficulty_populated(eof_song, (unsigned)d)) highest = d;
	if(highest < 0) goto cleanup;

	dtxv2_basename(cfg.artist, cfg.title, base, sizeof(base));
	eof_render();
	selected_folder = ncd_folder_select("Select where to create the DTXMania song folder. Cancel to use the notes.eof folder.");
	if(selected_folder && selected_folder[0]) ustrzcpy(parent, sizeof(parent), selected_folder);
	else if(eof_song_path[0]) ustrzcpy(parent, sizeof(parent), eof_song_path);
	else ustrzcpy(parent, sizeof(parent), ".");
	dtxv2_join(folder, sizeof(folder), parent, base);
	if(!eof_folder_exists(folder) && eof_mkdir(folder)) { allegro_message("Unable to create the DTXMania output folder:\n%s", folder); goto cleanup; }

	synth_imported_audio = eof_dtx_dialog_render_imported_ogg();
	source_audio = eof_loaded_ogg_name[0] ? eof_loaded_ogg_name : NULL;
	if(!synth_imported_audio && (!source_audio || !exists(source_audio))) { allegro_message("The chart audio file could not be found."); goto cleanup; }
	snprintf(audio_name, sizeof(audio_name), "%s.ogg", base);
	dtxv2_join(audio_path, sizeof(audio_path), folder, audio_name);
	dtxv2_join(preview_path, sizeof(preview_path), folder, "preview.ogg");
	ffstatus = dtxv2_resolve_ffmpeg(ffmpeg, sizeof(ffmpeg), synth_imported_audio ? 0 : 1);
	if(ffstatus < 0) goto cleanup;
	if(synth_imported_audio)
	{
		synth_diff = (eof_note_type < EOF_DTXV2_DIFFS && dtxv2_difficulty_populated(eof_song, eof_note_type)) ? (int)eof_note_type : highest;
		if(synth_diff < 0 || !eof_dtx_synth_render_ogg(audio_path, (unsigned char)synth_diff))
		{
			allegro_message("FluidSynth could not render the imported DTX pattern as the new OGG.\n\nThe original chart audio was not overwritten. See eof_log.txt.");
			goto cleanup;
		}
		eof_log("DTX export: using FluidSynth-rendered percussion OGG from imported DTX pattern.", 1);
	}
	else if(ffstatus > 0)
	{
		if(!dtxv2_ffmpeg_audio(ffmpeg, source_audio, audio_path, cfg.target_lufs, cfg.normalize))
		{
			if(!eof_copy_file(source_audio, audio_path)) { allegro_message("Audio export failed."); goto cleanup; }
		}
	}
	else if(!eof_copy_file(source_audio, audio_path)) { allegro_message("Audio export failed."); goto cleanup; }
	if(!dtxv2_preview(ffstatus > 0 ? ffmpeg : NULL, audio_path, preview_path))
		allegro_message("The DTX chart was exported, but preview audio could not be created.");

	(void)dtxv2_copy_asset(cfg.jacket, folder, jacket, sizeof(jacket));
	(void)dtxv2_copy_asset(cfg.premovie, folder, premovie, sizeof(premovie));
	(void)dtxv2_copy_asset(cfg.gameplay_video, folder, video, sizeof(video));
	(void)dtxv2_copy_asset(cfg.stage_image, folder, stage, sizeof(stage));
	(void)dtxv2_copy_asset(cfg.result_image, folder, result, sizeof(result));

	for(d = 0; d < EOF_DTXV2_DIFFS; d++)
	{
		if(!dtxv2_difficulty_populated(eof_song, (unsigned)d)) continue;
		grades[d] = dtxv2_grade_from_track(eof_song, (unsigned)d, mt, measures, bpm, padding);
		if(!grades[d]) goto cleanup;
	}
	if(cfg.multilevel)
	{
		PACKFILE *setfp;
		char setpath[2048], dtxname[800], dtxpath[2048], line[1200];
		dtxv2_join(setpath, sizeof(setpath), folder, "set.def");
		setfp = pack_fopen(setpath, "w");
		if(!setfp) goto cleanup;
		snprintf(line, sizeof(line), "#TITLE: %s\n#ARTIST: %s\n", cfg.title, cfg.artist); dtxv2_write_text(setfp, line);
		for(d = 0; d < EOF_DTXV2_DIFFS; d++)
		{
			if(!grades[d]) continue;
			snprintf(dtxname, sizeof(dtxname), "%s%s.dtx", base, eof_dtxv2_suffix[d]);
			snprintf(line, sizeof(line), "#L%dFILE: %s\n#L%dLABEL: %s\n", d + 1, dtxname, d + 1, eof_dtxv2_label[d]); dtxv2_write_text(setfp, line);
			dtxv2_join(dtxpath, sizeof(dtxpath), folder, dtxname);
			if(!dtxv2_write_dtx(dtxpath, grades[d], bpm, audio_name, &cfg, jacket, premovie, video, stage, result, (unsigned)d)) { pack_fclose(setfp); goto cleanup; }
		}
		pack_fclose(setfp);
	}
	else
	{
		char dtxname[800], dtxpath[2048];
		snprintf(dtxname, sizeof(dtxname), "%s.dtx", base); dtxv2_join(dtxpath, sizeof(dtxpath), folder, dtxname);
		if(!dtxv2_write_dtx(dtxpath, grades[highest], bpm, audio_name, &cfg, jacket, premovie, video, stage, result, (unsigned)highest)) goto cleanup;
	}
	dtx_last_export_success = success = 1;
	allegro_message("DTXMania export completed successfully.\n\n%s", folder);

cleanup:
	for(d = 0; d < EOF_DTXV2_DIFFS; d++) dtxv2_grade_free(grades[d]);
	free(mt);
	if(!success && !dtx_last_export_success) eof_log("DTXMania export did not complete", 1);
	return 1;
}

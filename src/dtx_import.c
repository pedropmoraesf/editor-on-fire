#include <allegro.h>
#include <alogg.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "agup/agup.h"
#include "beat.h"
#include "dialog.h"
#include "dialog/proc.h"
#include "editor.h"
#include "main.h"
#include "mix.h"
#include "modules/wfsel.h"
#include "song.h"
#include "utility.h"
#include "undo.h"
#include "menu/file.h"
#include "menu/song.h"
#include "menu/track.h"
#include "dtx_import.h"
#include "dtx_workflow.h"

#define EOF_DTX_IMPORT_DIFFS 5
#define EOF_DTX_IMPORT_MAX_LINE 32768
#define EOF_DTX_IMPORT_MAX_WAVS 1296
#define EOF_DTX_IMPORT_DOUBLECLICK_MS 450

typedef struct
{
	unsigned long measure;
	unsigned long slot;
	unsigned long slots;
	unsigned char midi;
} EOF_DTX_IMPORT_HIT;

typedef struct
{
	unsigned long measure;
	unsigned long slot;
	unsigned long slots;
	double bpm;
} EOF_DTX_IMPORT_TEMPO;

typedef struct
{
	char path[1024];
	char audio[1024];
	char title[256];
	char artist[256];
	char *wav[EOF_DTX_IMPORT_MAX_WAVS];
	double bpm;
	double *ratio;
	unsigned long ratio_count;
	EOF_DTX_IMPORT_HIT *hit;
	unsigned long hits;
	unsigned long hit_cap;
	EOF_DTX_IMPORT_TEMPO *tempo;
	unsigned long tempos;
	unsigned long tempo_cap;
	unsigned long max_measure;
	int difficulty;
	int dlevel;
	int has_complex_tempo;
	unsigned short bgm;
	unsigned long pattern_hash;
} EOF_DTX_IMPORT_CHART;

typedef struct
{
	int active;
	EOF_SONG *song;
	int present[EOF_DTX_IMPORT_DIFFS];
	char audio[EOF_DTX_IMPORT_DIFFS][1024];
	double bpm[EOF_DTX_IMPORT_DIFFS];
	unsigned long pattern_hash[EOF_DTX_IMPORT_DIFFS];
	int current_diff;
	int bpm_linked;
	int tempo_switchable;
	int switching;
} EOF_DTX_IMPORT_SESSION;

static EOF_DTX_IMPORT_SESSION eof_dtx_import_session = {0};
static NCDFS_FILTER_LIST *eof_dtx_import_filter = NULL;
static clock_t eof_dtx_grid_last_click = 0;
static int eof_dtx_grid_last_beat = -1;
static char eof_dtx_grid_start_text[32] = {0};

static DIALOG eof_dtx_grid_start_dialog[] =
{
	{ eof_window_proc,       0,  80, 300, 126, 2, 23, 0, 0,      0, 0, "Set grid start", NULL, NULL },
	{ d_agup_text_proc,     16, 108, 268,  16, 2, 23, 0, 0,      0, 0, "Start (seconds):", NULL, NULL },
	{ eof_edit_proc,        16, 128, 268,  20, 2, 23, 0, 0,     31, 0, eof_dtx_grid_start_text, NULL, NULL },
	{ d_agup_button_proc,   58, 162,  82,  28, 2, 23, '\r', D_EXIT, 0, 0, "OK", NULL, NULL },
	{ d_agup_button_proc,  160, 162,  82,  28, 2, 23, 0, D_EXIT, 0, 0, "Cancel", NULL, NULL },
	{ NULL,                  0,   0,   0,   0, 0,  0, 0, 0,      0, 0, NULL, NULL, NULL }
};

static char *eof_dtx_import_trim(char *text)
{
	char *end;
	if(!text) return text;
	while(*text && isspace((unsigned char)*text)) text++;
	end = text + strlen(text);
	while(end > text && isspace((unsigned char)end[-1])) *--end = '\0';
	if((text[0] == '"') && (end > text + 1) && (end[-1] == '"'))
	{
		text++;
		end[-1] = '\0';
	}
	return text;
}

static int eof_dtx_import_hex2(const char *text)
{
	int a, b;
	if(!text || !isxdigit((unsigned char)text[0]) || !isxdigit((unsigned char)text[1])) return -1;
	a = isdigit((unsigned char)text[0]) ? text[0] - '0' : toupper((unsigned char)text[0]) - 'A' + 10;
	b = isdigit((unsigned char)text[1]) ? text[1] - '0' : toupper((unsigned char)text[1]) - 'A' + 10;
	return (a << 4) | b;
}

static int eof_dtx_import_base36_2(const char *text)
{
	int a, b;
	if(!text) return -1;
	a = isdigit((unsigned char)text[0]) ? text[0] - '0' :
		(isalpha((unsigned char)text[0]) ? toupper((unsigned char)text[0]) - 'A' + 10 : -1);
	b = isdigit((unsigned char)text[1]) ? text[1] - '0' :
		(isalpha((unsigned char)text[1]) ? toupper((unsigned char)text[1]) - 'A' + 10 : -1);
	if(a < 0 || a >= 36 || b < 0 || b >= 36) return -1;
	return a * 36 + b;
}

static int eof_dtx_import_store_wav(EOF_DTX_IMPORT_CHART *chart, int code, const char *value)
{
	char *copy;
	size_t len;
	if(!chart || code < 0 || code >= EOF_DTX_IMPORT_MAX_WAVS || !value) return 0;
	len = strlen(value);
	copy = (char *)malloc(len + 1U);
	if(!copy) return 0;
	memcpy(copy, value, len + 1U);
	free(chart->wav[code]);
	chart->wav[code] = copy;
	return 1;
}

static int eof_dtx_import_lane_midi(int channel)
{
	switch(channel)
	{
		case 0x11: return 42;
		case 0x12: return 38;
		case 0x13: return 36;
		case 0x14: return 48;
		case 0x15: return 45;
		case 0x16: return 57;
		case 0x17: return 43;
		case 0x18: return 46;
		case 0x19: return 51;
		case 0x1A: return 49;
		case 0x1B: return 44;
		case 0x1C: return 35;
	}
	return -1;
}

static unsigned long eof_dtx_import_hash_mix(unsigned long hash, unsigned long value)
{
	hash ^= value;
	hash *= 16777619UL;
	return hash;
}

static int eof_dtx_import_ensure_ratio(EOF_DTX_IMPORT_CHART *chart, unsigned long measure)
{
	double *resized;
	unsigned long old, count;
	if(!chart) return 0;
	if(measure < chart->ratio_count) return 1;
	old = chart->ratio_count;
	count = measure + 32UL;
	resized = (double *)realloc(chart->ratio, count * sizeof(double));
	if(!resized) return 0;
	chart->ratio = resized;
	while(old < count) chart->ratio[old++] = 1.0;
	chart->ratio_count = count;
	return 1;
}

static int eof_dtx_import_add_hit(EOF_DTX_IMPORT_CHART *chart, unsigned long measure, unsigned long slot, unsigned long slots, int midi)
{
	EOF_DTX_IMPORT_HIT *resized;
	if(!chart || !slots || midi < 0 || midi > 127) return 0;
	if(chart->hits >= chart->hit_cap)
	{
		unsigned long cap = chart->hit_cap ? chart->hit_cap * 2UL : 512UL;
		resized = (EOF_DTX_IMPORT_HIT *)realloc(chart->hit, cap * sizeof(*resized));
		if(!resized) return 0;
		chart->hit = resized;
		chart->hit_cap = cap;
	}
	chart->hit[chart->hits].measure = measure;
	chart->hit[chart->hits].slot = slot;
	chart->hit[chart->hits].slots = slots;
	chart->hit[chart->hits].midi = (unsigned char)midi;
	chart->hits++;
	if(measure > chart->max_measure) chart->max_measure = measure;
	return 1;
}

static int eof_dtx_import_add_tempo(EOF_DTX_IMPORT_CHART *chart, unsigned long measure, unsigned long slot, unsigned long slots, double bpm)
{
	EOF_DTX_IMPORT_TEMPO *resized;
	if(!chart || !slots || bpm <= 0.0) return 0;
	if(chart->tempos >= chart->tempo_cap)
	{
		unsigned long cap = chart->tempo_cap ? chart->tempo_cap * 2UL : 64UL;
		resized = (EOF_DTX_IMPORT_TEMPO *)realloc(chart->tempo, cap * sizeof(*resized));
		if(!resized) return 0;
		chart->tempo = resized;
		chart->tempo_cap = cap;
	}
	chart->tempo[chart->tempos].measure = measure;
	chart->tempo[chart->tempos].slot = slot;
	chart->tempo[chart->tempos].slots = slots;
	chart->tempo[chart->tempos].bpm = bpm;
	chart->tempos++;
	chart->has_complex_tempo = 1;
	if(measure > chart->max_measure) chart->max_measure = measure;
	return 1;
}

static int eof_dtx_import_hit_cmp(const void *a, const void *b)
{
	const EOF_DTX_IMPORT_HIT *x = (const EOF_DTX_IMPORT_HIT *)a;
	const EOF_DTX_IMPORT_HIT *y = (const EOF_DTX_IMPORT_HIT *)b;
	double xf, yf;
	if(x->measure < y->measure) return -1;
	if(x->measure > y->measure) return 1;
	xf = (double)x->slot / (double)x->slots;
	yf = (double)y->slot / (double)y->slots;
	if(xf < yf) return -1;
	if(xf > yf) return 1;
	if(x->midi < y->midi) return -1;
	if(x->midi > y->midi) return 1;
	return 0;
}

static int eof_dtx_import_tempo_cmp(const void *a, const void *b)
{
	const EOF_DTX_IMPORT_TEMPO *x = (const EOF_DTX_IMPORT_TEMPO *)a;
	const EOF_DTX_IMPORT_TEMPO *y = (const EOF_DTX_IMPORT_TEMPO *)b;
	double xf, yf;
	if(x->measure < y->measure) return -1;
	if(x->measure > y->measure) return 1;
	xf = (double)x->slot / (double)x->slots;
	yf = (double)y->slot / (double)y->slots;
	return (xf < yf) ? -1 : (xf > yf);
}

static int eof_dtx_import_suffix_diff(const char *path)
{
	char stem[1024];
	char *dot;
	size_t len;
	if(!path) return -1;
	ustrzcpy(stem, sizeof(stem), get_filename(path));
	dot = strrchr(stem, '.');
	if(dot) *dot = '\0';
	len = strlen(stem);
	if(len >= 4)
	{
		const char *s = stem + len - 4;
		if(!ustricmp(s, "_bsc")) return 0;
		if(!ustricmp(s, "_adv")) return 1;
		if(!ustricmp(s, "_ext")) return 2;
		if(!ustricmp(s, "_mas")) return 3;
		if(!ustricmp(s, "_ult")) return 4;
	}
	return -1;
}

static int eof_dtx_import_dlevel_diff(int dlevel)
{
	if(dlevel <= 0) return -1;
	if(dlevel <= 20) return 0;
	if(dlevel <= 40) return 1;
	if(dlevel <= 60) return 2;
	if(dlevel <= 80) return 3;
	return 4;
}

static int eof_dtx_import_parse(const char *path, EOF_DTX_IMPORT_CHART *chart)
{
	FILE *fp;
	char line[EOF_DTX_IMPORT_MAX_LINE];
	double bpm_defs[EOF_DTX_IMPORT_MAX_WAVS] = {0.0};
	unsigned char bpm_defined[EOF_DTX_IMPORT_MAX_WAVS] = {0};
	typedef struct { unsigned long m, s, n; unsigned short code; } REF;
	REF *refs = NULL;
	unsigned long refcount = 0, refcap = 0;
	int explicit_diff = -1;
	if(!path || !chart) return 0;
	memset(chart, 0, sizeof(*chart));
	chart->bpm = 120.0;
	chart->difficulty = -1;
	chart->bgm = 1;
	ustrzcpy(chart->path, sizeof(chart->path), path);
	fp = fopen(path, "rb");
	if(!fp) return 0;
	while(fgets(line, sizeof(line), fp))
	{
		char *p = line, *colon, *key, *value;
		size_t klen;
		unsigned long measure;
		int channel;
		if((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
		p = eof_dtx_import_trim(p);
		if(*p != '#') continue;
		p++;
		colon = strchr(p, ':');
		if(!colon) continue;
		*colon = '\0';
		key = eof_dtx_import_trim(p);
		value = eof_dtx_import_trim(colon + 1);
		klen = strlen(key);

		if(!ustricmp(key, "TITLE")) { ustrzcpy(chart->title, sizeof(chart->title), value); continue; }
		if(!ustricmp(key, "ARTIST")) { ustrzcpy(chart->artist, sizeof(chart->artist), value); continue; }
		if(!ustricmp(key, "BPM"))
		{
			double v = atof(value); if(v > 0.0) chart->bpm = v;
			continue;
		}
		if(!ustricmp(key, "DIFFICULTY"))
		{
			int v = atoi(value); if(v >= 1 && v <= 5) explicit_diff = v - 1;
			continue;
		}
		if(!ustricmp(key, "DLEVEL")) { chart->dlevel = atoi(value); continue; }
		if(!ustricmp(key, "BGMWAV"))
		{
			int v = eof_dtx_import_base36_2(value); if(v >= 0) chart->bgm = (unsigned short)v;
			continue;
		}
		if(klen == 5 && !ustrnicmp(key, "WAV", 3))
		{
			int code = eof_dtx_import_base36_2(key + 3);
			if(code >= 0 && !eof_dtx_import_store_wav(chart, code, value)) { fclose(fp); free(refs); return 0; }
			continue;
		}
		if(klen == 5 && !ustrnicmp(key, "BPM", 3))
		{
			int code = eof_dtx_import_base36_2(key + 3);
			double v = atof(value);
			if(code >= 0 && v > 0.0) { bpm_defs[code] = v; bpm_defined[code] = 1; }
			continue;
		}
		if(klen != 5 || !isdigit((unsigned char)key[0]) || !isdigit((unsigned char)key[1]) || !isdigit((unsigned char)key[2]))
			continue;
		measure = (unsigned long)((key[0] - '0') * 100 + (key[1] - '0') * 10 + (key[2] - '0'));
		channel = eof_dtx_import_hex2(key + 3);
		if(channel < 0) continue;
		if(measure > chart->max_measure) chart->max_measure = measure;
		if(!eof_dtx_import_ensure_ratio(chart, measure)) { fclose(fp); free(refs); return 0; }
		if(channel == 0x02)
		{
			double ratio = atof(value);
			if(ratio > 0.000001) chart->ratio[measure] = ratio;
			continue;
		}
		{
			size_t len = strlen(value), tokens, i;
			while(len && isspace((unsigned char)value[len - 1])) value[--len] = '\0';
			tokens = len / 2;
			if(!tokens) continue;
			if(channel == 0x01)
			{
				for(i = 0; i < tokens; i++)
				if(value[i * 2] != '0' || value[i * 2 + 1] != '0')
				{ int v = eof_dtx_import_base36_2(value + i * 2); if(v >= 0) chart->bgm = (unsigned short)v; break; }
				continue;
			}
			if(channel == 0x08)
			{
				for(i = 0; i < tokens; i++)
				if(value[i * 2] != '0' || value[i * 2 + 1] != '0')
				{
					int code = eof_dtx_import_base36_2(value + i * 2);
					REF *rr;
					if(code < 0) continue;
					if(refcount >= refcap)
					{
						unsigned long cap = refcap ? refcap * 2UL : 32UL;
						rr = (REF *)realloc(refs, cap * sizeof(*rr));
						if(!rr) { fclose(fp); free(refs); return 0; }
						refs = rr; refcap = cap;
					}
					refs[refcount].m = measure; refs[refcount].s = (unsigned long)i; refs[refcount].n = (unsigned long)tokens; refs[refcount].code = (unsigned short)code; refcount++;
				}
				continue;
			}
			if(channel == 0x03)
			{
				for(i = 0; i < tokens; i++)
					if(value[i * 2] != '0' || value[i * 2 + 1] != '0')
					{
						int direct = eof_dtx_import_hex2(value + i * 2);
						if(direct > 0 && !eof_dtx_import_add_tempo(chart, measure, (unsigned long)i, (unsigned long)tokens, (double)direct))
						{ fclose(fp); free(refs); return 0; }
					}
				continue;
			}
			{
				int midi = eof_dtx_import_lane_midi(channel);
				if(midi >= 0)
				{
					for(i = 0; i < tokens; i++)
						if(value[i * 2] != '0' || value[i * 2 + 1] != '0')
							if(!eof_dtx_import_add_hit(chart, measure, (unsigned long)i, (unsigned long)tokens, midi))
							{ fclose(fp); free(refs); return 0; }
				}
			}
		}
	}
	fclose(fp);

	{
		unsigned long i;
		for(i = 0; i < refcount; i++)
			if(bpm_defined[refs[i].code])
				if(!eof_dtx_import_add_tempo(chart, refs[i].m, refs[i].s, refs[i].n, bpm_defs[refs[i].code]))
				{ free(refs); return 0; }
	}
	free(refs);
	if(chart->hits) qsort(chart->hit, chart->hits, sizeof(*chart->hit), eof_dtx_import_hit_cmp);
	if(chart->tempos) qsort(chart->tempo, chart->tempos, sizeof(*chart->tempo), eof_dtx_import_tempo_cmp);

	chart->difficulty = explicit_diff;
	if(chart->difficulty < 0) chart->difficulty = eof_dtx_import_suffix_diff(path);
	if(chart->difficulty < 0) chart->difficulty = eof_dtx_import_dlevel_diff(chart->dlevel);
	if(chart->difficulty < 0) chart->difficulty = 4;

	{
		char folder[1024] = {0};
		const char *audio = chart->wav[chart->bgm] ? chart->wav[chart->bgm] : chart->wav[1];
		if(audio && audio[0])
		{
			replace_filename(folder, path, "", sizeof(folder));
			append_filename(chart->audio, folder, audio, sizeof(chart->audio));
			fix_filename_slashes(chart->audio);
		}
	}
	{
		unsigned long h = 2166136261UL, i;
		for(i = 0; i <= chart->max_measure; i++)
		{
			double ratio = (i < chart->ratio_count) ? chart->ratio[i] : 1.0;
			unsigned long q = (unsigned long)(ratio * 1000000.0 + 0.5);
			h = eof_dtx_import_hash_mix(h, i);
			h = eof_dtx_import_hash_mix(h, q);
		}
		for(i = 0; i < chart->hits; i++)
		{
			h = eof_dtx_import_hash_mix(h, chart->hit[i].measure);
			h = eof_dtx_import_hash_mix(h, chart->hit[i].slot);
			h = eof_dtx_import_hash_mix(h, chart->hit[i].slots);
			h = eof_dtx_import_hash_mix(h, chart->hit[i].midi);
		}
		chart->pattern_hash = h;
	}
	return 1;
}

static void eof_dtx_import_chart_free(EOF_DTX_IMPORT_CHART *chart)
{
	unsigned i;
	if(!chart) return;
	for(i = 0; i < EOF_DTX_IMPORT_MAX_WAVS; i++) free(chart->wav[i]);
	free(chart->ratio);
	free(chart->hit);
	free(chart->tempo);
	memset(chart, 0, sizeof(*chart));
}

static double eof_dtx_import_measure_units(const EOF_DTX_IMPORT_CHART *chart, unsigned long measure)
{
	double ratio = 1.0;
	if(chart && measure < chart->ratio_count && chart->ratio[measure] > 0.000001) ratio = chart->ratio[measure];
	return ratio * 4.0;
}

static double eof_dtx_import_measure_start_units(const EOF_DTX_IMPORT_CHART *chart, unsigned long measure)
{
	double units = 0.0;
	unsigned long i;
	if(!chart) return 0.0;
	for(i = 0; i < measure; i++) units += eof_dtx_import_measure_units(chart, i);
	return units;
}

static double eof_dtx_import_tempo_unit(const EOF_DTX_IMPORT_CHART *chart, const EOF_DTX_IMPORT_TEMPO *tempo)
{
	double base, span;
	if(!chart || !tempo) return 0.0;
	base = eof_dtx_import_measure_start_units(chart, tempo->measure);
	span = eof_dtx_import_measure_units(chart, tempo->measure);
	return base + span * ((double)tempo->slot / (double)tempo->slots);
}

static double eof_dtx_import_time_at_unit(const EOF_DTX_IMPORT_CHART *chart, double target)
{
	double at = 0.0, time_ms = 0.0, bpm;
	unsigned long i;
	if(!chart || target <= 0.0) return 0.0;
	bpm = chart->bpm > 0.0 ? chart->bpm : 120.0;
	for(i = 0; i < chart->tempos; i++)
	{
		double unit = eof_dtx_import_tempo_unit(chart, &chart->tempo[i]);
		if(unit <= at + 0.0000001) { bpm = chart->tempo[i].bpm; continue; }
		if(unit >= target) break;
		time_ms += (unit - at) * 60000.0 / bpm;
		at = unit;
		bpm = chart->tempo[i].bpm;
	}
	if(target > at) time_ms += (target - at) * 60000.0 / bpm;
	return time_ms;
}

static double eof_dtx_import_hit_time(const EOF_DTX_IMPORT_CHART *timing, const EOF_DTX_IMPORT_HIT *hit)
{
	double start, span, unit;
	if(!timing || !hit) return 0.0;
	start = eof_dtx_import_measure_start_units(timing, hit->measure);
	span = eof_dtx_import_measure_units(timing, hit->measure);
	unit = start + span * ((double)hit->slot / (double)hit->slots);
	return eof_dtx_import_time_at_unit(timing, unit);
}

static unsigned eof_dtx_import_measure_beats(const EOF_DTX_IMPORT_CHART *chart, unsigned long measure)
{
	double q = eof_dtx_import_measure_units(chart, measure);
	long rounded = (long)floor(q + 0.5);
	if(rounded >= 1 && rounded <= 32 && fabs(q - (double)rounded) < 0.001) return (unsigned)rounded;
	return 4;
}

static void eof_dtx_import_apply_ts_flag(EOF_BEAT_MARKER *beat, unsigned num)
{
	unsigned long mask = EOF_BEAT_FLAG_START_2_4 | EOF_BEAT_FLAG_START_3_4 | EOF_BEAT_FLAG_START_4_4 |
		EOF_BEAT_FLAG_START_5_4 | EOF_BEAT_FLAG_START_6_4 | EOF_BEAT_FLAG_CUSTOM_TS;
	if(!beat) return;
	beat->flags &= ~mask;
	switch(num)
	{
		case 2: beat->flags |= EOF_BEAT_FLAG_START_2_4; break;
		case 3: beat->flags |= EOF_BEAT_FLAG_START_3_4; break;
		case 4: beat->flags |= EOF_BEAT_FLAG_START_4_4; break;
		case 5: beat->flags |= EOF_BEAT_FLAG_START_5_4; break;
		case 6: beat->flags |= EOF_BEAT_FLAG_START_6_4; break;
		default:
			beat->flags |= EOF_BEAT_FLAG_CUSTOM_TS;
			beat->flags |= ((unsigned long)(num - 1U) & 0xFFUL) << 24;
			beat->flags |= ((unsigned long)(4U - 1U) & 0xFFUL) << 16;
			break;
	}
}

static int eof_dtx_import_build_beatmap(const EOF_DTX_IMPORT_CHART *chart)
{
	unsigned long measure, beats = 1, index = 0;
	double first_unit = 0.0, previous_len = 0.0;
	int skip_measure_zero = 0;
	if(!eof_song || !chart) return 0;

	if(chart->max_measure > 0 && eof_dtx_import_measure_units(chart, 0) < 3.0)
	{
		unsigned long i;
		int hit0 = 0;
		for(i = 0; i < chart->hits; i++) if(chart->hit[i].measure == 0) { hit0 = 1; break; }
		if(!hit0) skip_measure_zero = 1;
	}
	if(skip_measure_zero) first_unit = eof_dtx_import_measure_units(chart, 0);
	for(measure = (unsigned long)skip_measure_zero; measure <= chart->max_measure; measure++)
		beats += eof_dtx_import_measure_beats(chart, measure);
	if(beats < 2) beats = 2;
	if(!eof_song_resize_beats(eof_song, beats)) return 0;

	for(measure = (unsigned long)skip_measure_zero; measure <= chart->max_measure && index + 1 < beats; measure++)
	{
		unsigned count = eof_dtx_import_measure_beats(chart, measure), b;
		double mstart = eof_dtx_import_measure_start_units(chart, measure);
		double mspan = eof_dtx_import_measure_units(chart, measure);
		for(b = 0; b < count && index + 1 < beats; b++)
		{
			double unit = mstart + mspan * ((double)b / (double)count);
			double ms = eof_dtx_import_time_at_unit(chart, unit);
			eof_song->beat[index]->fpos = ms;
			eof_song->beat[index]->pos = (unsigned long)floor(ms + 0.5);
			eof_song->beat[index]->flags &= ~(EOF_BEAT_FLAG_ANCHOR | EOF_BEAT_FLAG_START_2_4 | EOF_BEAT_FLAG_START_3_4 |
				EOF_BEAT_FLAG_START_4_4 | EOF_BEAT_FLAG_START_5_4 | EOF_BEAT_FLAG_START_6_4 | EOF_BEAT_FLAG_CUSTOM_TS);
			if(b == 0) eof_dtx_import_apply_ts_flag(eof_song->beat[index], count);
			index++;
		}
	}
	{
		double endunit = eof_dtx_import_measure_start_units(chart, chart->max_measure) +
			eof_dtx_import_measure_units(chart, chart->max_measure);
		double ms = eof_dtx_import_time_at_unit(chart, endunit);
		eof_song->beat[index]->fpos = ms;
		eof_song->beat[index]->pos = (unsigned long)floor(ms + 0.5);
		eof_song->beat[index]->flags &= ~(EOF_BEAT_FLAG_ANCHOR | EOF_BEAT_FLAG_START_2_4 | EOF_BEAT_FLAG_START_3_4 |
			EOF_BEAT_FLAG_START_4_4 | EOF_BEAT_FLAG_START_5_4 | EOF_BEAT_FLAG_START_6_4 | EOF_BEAT_FLAG_CUSTOM_TS);
		index++;
	}
	if(index < 2) return 0;
	if(index != eof_song->beats && !eof_song_resize_beats(eof_song, index)) return 0;

	for(measure = 0; measure < eof_song->beats; measure++)
	{
		double len;
		if(measure + 1 < eof_song->beats) len = eof_song->beat[measure + 1]->fpos - eof_song->beat[measure]->fpos;
		else len = previous_len > 0.0 ? previous_len : 500.0;
		if(len <= 0.0) len = 500.0;
		eof_song->beat[measure]->ppqn = (unsigned long)floor(len * 1000.0 + 0.5);
		if(measure == 0 || previous_len <= 0.0 || fabs(len - previous_len) > 0.5)
			eof_song->beat[measure]->flags |= EOF_BEAT_FLAG_ANCHOR;
		previous_len = len;
	}
	eof_song->tags->ogg[0].midi_offset = eof_song->beat[0]->pos;
	eof_chart_length = eof_song->beat[eof_song->beats - 1]->pos;
	eof_beat_stats_cached = 0;
	eof_process_beat_statistics(eof_song, EOF_TRACK_DRUM_DTX);
	(void)first_unit;
	return 1;
}

static EOF_PRO_GUITAR_TRACK *eof_dtx_import_track(void)
{
	unsigned long backing;
	if(!eof_song || EOF_TRACK_DRUM_DTX >= eof_song->tracks || !eof_song->track[EOF_TRACK_DRUM_DTX]) return NULL;
	backing = eof_song->track[EOF_TRACK_DRUM_DTX]->tracknum;
	if(backing >= eof_song->pro_guitar_tracks) return NULL;
	return eof_song->pro_guitar_track[backing];
}

static void eof_dtx_import_clear_track(void)
{
	EOF_PRO_GUITAR_TRACK *tp = eof_dtx_import_track();
	if(!tp) return;
	eof_menu_track_set_tech_view_state(eof_song, EOF_TRACK_DRUM_DTX, 0);
	while(tp->pgnotes) eof_pro_guitar_track_delete_note(tp, tp->pgnotes - 1UL);
	tp->note = tp->pgnote;
	tp->numstrings = 6;
	tp->numfrets = 127;
	tp->capo = 0;
	tp->ignore_tuning = 1;
}

static int eof_dtx_import_add_chart_notes(const EOF_DTX_IMPORT_CHART *source, const EOF_DTX_IMPORT_CHART *timing, int diff)
{
	EOF_PRO_GUITAR_TRACK *tp = eof_dtx_import_track();
	unsigned long i;
	EOF_PRO_GUITAR_NOTE *last = NULL;
	unsigned long lastpos = ULONG_MAX;
	if(!tp || !source || !timing || diff < 0 || diff >= EOF_DTX_IMPORT_DIFFS) return 0;
	for(i = 0; i < source->hits; i++)
	{
		unsigned long pos = (unsigned long)floor(eof_dtx_import_hit_time(timing, &source->hit[i]) + 0.5);
		unsigned slot;
		if(!last || lastpos != pos)
		{
			last = eof_pro_guitar_track_add_note(tp);
			if(!last) return 0;
			last->type = (unsigned char)diff;
			last->pos = pos;
			last->length = 1;
			last->note = 0;
			memset(last->frets, 0xFF, sizeof(last->frets));
			lastpos = pos;
		}
		for(slot = 0; slot < 6; slot++) if(!(last->note & (1U << slot))) break;
		if(slot >= 6)
		{
			eof_log("DTX import: more than six simultaneous drum objects; extra hit skipped.", 1);
			continue;
		}
		last->note |= 1U << slot;
		last->frets[slot] = source->hit[i].midi;
	}
	eof_pro_guitar_track_sort_notes(tp);
	return 1;
}

static int eof_dtx_import_copy_audio(const char *source, int diff, char *dest, size_t size)
{
	static const char *suffix[EOF_DTX_IMPORT_DIFFS] = {"bsc","adv","ext","mas","ult"};
	char name[64];
	if(!source || !source[0] || !exists(source) || !dest || !size || diff < 0 || diff >= EOF_DTX_IMPORT_DIFFS) return 0;
	snprintf(name, sizeof(name), "dtx_%s.ogg", suffix[diff]);
	append_filename(dest, eof_song_path, name, (int)size);
	if(ustricmp(source, dest) && !eof_copy_file(source, dest)) return 0;
	return exists(dest) ? 1 : 0;
}

static int eof_dtx_import_ratios_equal(const EOF_DTX_IMPORT_CHART *a, const EOF_DTX_IMPORT_CHART *b)
{
	unsigned long i, maxm;
	if(!a || !b) return 0;
	maxm = a->max_measure > b->max_measure ? a->max_measure : b->max_measure;
	for(i = 0; i <= maxm; i++)
	{
		double ar = (i < a->ratio_count) ? a->ratio[i] : 1.0;
		double br = (i < b->ratio_count) ? b->ratio[i] : 1.0;
		if(fabs(ar - br) > 0.000001) return 0;
	}
	return 1;
}

static int eof_dtx_import_read_set(const char *selected, char files[EOF_DTX_IMPORT_DIFFS][1024])
{
	char setpath[1024], folder[1024], line[4096];
	FILE *fp;
	int found = 0;
	replace_filename(folder, selected, "", sizeof(folder));
	append_filename(setpath, folder, "set.def", sizeof(setpath));
	if(!exists(setpath)) return 0;
	fp = fopen(setpath, "rb");
	if(!fp) return 0;
	while(fgets(line, sizeof(line), fp))
	{
		char *p = eof_dtx_import_trim(line), *colon, *value;
		int level;
		if(*p == '#') p++;
		if(toupper((unsigned char)p[0]) != 'L' || p[1] < '1' || p[1] > '5' || ustrnicmp(p + 2, "FILE", 4)) continue;
		level = p[1] - '1';
		colon = strchr(p, ':');
		if(!colon) continue;
		value = eof_dtx_import_trim(colon + 1);
		append_filename(files[level], folder, value, 1024);
		if(exists(files[level])) found = 1; else files[level][0] = '\0';
	}
	fclose(fp);
	return found;
}

static void eof_dtx_import_scan_siblings(const char *selected, char files[EOF_DTX_IMPORT_DIFFS][1024])
{
	static const char *suffix[EOF_DTX_IMPORT_DIFFS] = {"_bsc.dtx","_adv.dtx","_ext.dtx","_mas.dtx","_ult.dtx"};
	char folder[1024], stem[1024], base[1024], *dot;
	size_t len;
	int d;
	replace_filename(folder, selected, "", sizeof(folder));
	ustrzcpy(stem, sizeof(stem), get_filename(selected));
	dot = strrchr(stem, '.'); if(dot) *dot = '\0';
	len = strlen(stem);
	if(len >= 4)
	{
		const char *tail = stem + len - 4;
		if(!ustricmp(tail, "_bsc") || !ustricmp(tail, "_adv") || !ustricmp(tail, "_ext") || !ustricmp(tail, "_mas") || !ustricmp(tail, "_ult"))
			stem[len - 4] = '\0';
	}
	ustrzcpy(base, sizeof(base), stem);
	for(d = 0; d < EOF_DTX_IMPORT_DIFFS; d++)
	{
		char name[1024];
		if(files[d][0]) continue;
		snprintf(name, sizeof(name), "%s%s", base, suffix[d]);
		append_filename(files[d], folder, name, 1024);
		if(!exists(files[d])) files[d][0] = '\0';
	}
}

static NCDFS_FILTER_LIST *eof_dtx_import_get_filter(void)
{
	if(!eof_dtx_import_filter)
	{
		eof_dtx_import_filter = ncdfs_filter_list_create();
		if(eof_dtx_import_filter) (void)ncdfs_filter_list_add(eof_dtx_import_filter, "dtx", "DTXMania chart (*.dtx)", 1);
	}
	return eof_dtx_import_filter;
}

static void eof_dtx_import_reset_session(void)
{
	memset(&eof_dtx_import_session, 0, sizeof(eof_dtx_import_session));
	eof_dtx_import_session.current_diff = -1;
}

static void eof_dtx_import_clone_diff(unsigned source_diff)
{
	EOF_PRO_GUITAR_TRACK *tp = eof_dtx_import_track();
	unsigned long i, original;
	int d;
	if(!tp || source_diff >= EOF_DTX_IMPORT_DIFFS) return;
	for(d = 0; d < EOF_DTX_IMPORT_DIFFS; d++)
	{
		if(d == (int)source_diff || !eof_dtx_import_session.present[d]) continue;
		for(i = tp->pgnotes; i > 0; i--)
			if(tp->pgnote[i - 1] && tp->pgnote[i - 1]->type == (unsigned char)d)
				eof_pro_guitar_track_delete_note(tp, i - 1);
		original = tp->pgnotes;
		for(i = 0; i < original; i++)
		{
			EOF_PRO_GUITAR_NOTE *src = tp->pgnote[i], *dst;
			if(!src || src->type != source_diff) continue;
			dst = eof_pro_guitar_track_add_note(tp);
			if(!dst) break;
			memcpy(dst, src, sizeof(*dst));
			dst->type = (unsigned char)d;
		}
	}
	eof_pro_guitar_track_sort_notes(tp);
}

static void eof_dtx_import_scale_timeline(double old_bpm, double new_bpm)
{
	EOF_PRO_GUITAR_TRACK *tp = eof_dtx_import_track();
	double factor, origin;
	unsigned long i;
	if(!eof_song || !tp || old_bpm <= 0.0 || new_bpm <= 0.0 || fabs(old_bpm - new_bpm) < 0.0001 || !eof_song->beats) return;
	factor = old_bpm / new_bpm;
	origin = eof_song->beat[0]->fpos;
	for(i = 0; i < eof_song->beats; i++)
	{
		double p = origin + (eof_song->beat[i]->fpos - origin) * factor;
		eof_song->beat[i]->fpos = p;
		eof_song->beat[i]->pos = (unsigned long)floor(p + 0.5);
		eof_song->beat[i]->ppqn = (unsigned long)floor((double)eof_song->beat[i]->ppqn * factor + 0.5);
	}
	for(i = 0; i < tp->pgnotes; i++)
		if(tp->pgnote[i] && tp->pgnote[i]->pos >= (unsigned long)origin)
			tp->pgnote[i]->pos = (unsigned long)floor(origin + ((double)tp->pgnote[i]->pos - origin) * factor + 0.5);
	eof_song->tags->ogg[0].midi_offset = eof_song->beat[0]->pos;
	eof_chart_length = eof_song->beat[eof_song->beats - 1]->pos;
	eof_beat_stats_cached = 0;
	eof_pro_guitar_track_sort_notes(tp);
}

static void eof_dtx_import_load_diff_audio(int diff)
{
	unsigned long seek;
	if(diff < 0 || diff >= EOF_DTX_IMPORT_DIFFS || !eof_dtx_import_session.audio[diff][0] || !exists(eof_dtx_import_session.audio[diff])) return;
	if(!ustricmp(eof_loaded_ogg_name, eof_dtx_import_session.audio[diff])) return;
	seek = EOF_SEEK_POS;
	if(!eof_load_ogg_quick(eof_dtx_import_session.audio[diff]))
	{
		eof_log("DTX import: failed to switch to the OGG assigned to the selected difficulty.", 1);
		return;
	}
	ustrzcpy(eof_song->tags->ogg[0].filename, sizeof(eof_song->tags->ogg[0].filename), get_filename(eof_dtx_import_session.audio[diff]));
	if(seek > eof_music_length) seek = eof_music_length;
	alogg_seek_abs_msecs_ogg_ul(eof_music_track, seek + eof_av_delay);
	eof_music_actual_pos = seek + eof_av_delay;
	eof_set_music_pos(&eof_music_pos, (int)eof_music_actual_pos);
	eof_mix_seek(seek);
	eof_mix_find_claps();
	eof_mix_start_helper();
}

int eof_dtx_import_session_active(void)
{
	if(eof_dtx_import_session.active && eof_dtx_import_session.song != eof_song) eof_dtx_import_reset_session();
	return eof_dtx_import_session.active;
}

int eof_dtx_import_is_bpm_linked(void)
{
	return eof_dtx_import_session_active() ? eof_dtx_import_session.bpm_linked : 0;
}

int eof_dtx_import_active_difficulty(void)
{
	return eof_dtx_import_session_active() ? eof_dtx_import_session.current_diff : -1;
}

void eof_dtx_import_sync_editor_state(void)
{
	int diff, old;
	if(!eof_dtx_import_session_active() || eof_dtx_import_session.switching || eof_selected_track != EOF_TRACK_DRUM_DTX) return;
	diff = (eof_note_type < EOF_DTX_IMPORT_DIFFS) ? (int)eof_note_type : eof_dtx_import_session.current_diff;
	if(diff < 0 || diff >= EOF_DTX_IMPORT_DIFFS || !eof_dtx_import_session.present[diff]) return;
	old = eof_dtx_import_session.current_diff;
	if(old < 0) old = diff;

	eof_dtx_import_session.switching = 1;
	if(eof_dtx_import_session.bpm_linked && old >= 0 && old < EOF_DTX_IMPORT_DIFFS)
		eof_dtx_import_clone_diff((unsigned)old);
	if(diff != old)
	{
		if(eof_dtx_import_session.tempo_switchable)
			eof_dtx_import_scale_timeline(eof_dtx_import_session.bpm[old], eof_dtx_import_session.bpm[diff]);
		else if(fabs(eof_dtx_import_session.bpm[old] - eof_dtx_import_session.bpm[diff]) > 0.01)
			eof_log("DTX import: difficulty has an independent tempo map; audio switch kept on the current timing.", 1);
		eof_dtx_import_session.current_diff = diff;
		eof_dtx_import_load_diff_audio(diff);
	}
	if(eof_dtx_import_session.bpm_linked)
		eof_dtx_import_clone_diff((unsigned)diff);
	eof_dtx_import_session.switching = 0;
}

static int eof_dtx_import_prompt_grid_start(void)
{
	char temp[32];
	char *end;
	double seconds, old_ms, new_ms, delta;
	unsigned long i;
	if(!eof_song || !eof_song->beats) return 0;
	snprintf(eof_dtx_grid_start_text, sizeof(eof_dtx_grid_start_text), "%.3f", eof_song->beat[0]->fpos / 1000.0);
	eof_color_dialog(eof_dtx_grid_start_dialog, gui_fg_color, gui_bg_color);
	eof_conditionally_center_dialog(eof_dtx_grid_start_dialog);
	eof_clear_input();
	if(eof_popup_dialog(eof_dtx_grid_start_dialog, 2) != 3) return 0;
	ustrzcpy(temp, sizeof(temp), eof_dtx_grid_start_text);
	for(i = 0; temp[i]; i++) if(temp[i] == ',') temp[i] = '.';
	seconds = strtod(temp, &end);
	if(end == temp || *eof_dtx_import_trim(end) || !isfinite(seconds) || seconds < 0.0)
	{
		allegro_message("Enter a valid non-negative start time in seconds.");
		return 0;
	}
	old_ms = eof_song->beat[0]->fpos;
	new_ms = seconds * 1000.0;
	delta = new_ms - old_ms;
	if(fabs(delta) < 0.0005) return 1;
	eof_prepare_undo(EOF_UNDO_TYPE_NONE);
	for(i = 0; i < eof_song->beats; i++)
	{
		double p = eof_song->beat[i]->fpos + delta;
		if(p < 0.0) p = 0.0;
		eof_song->beat[i]->fpos = p;
		eof_song->beat[i]->pos = (unsigned long)floor(p + 0.5);
	}
	eof_song->tags->ogg[0].midi_offset = eof_song->beat[0]->pos;
	eof_beat_stats_cached = 0;
	eof_process_beat_statistics(eof_song, eof_selected_track);
	eof_changes = 1;
	eof_window_title_dirty = 1;
	return 1;
}

int eof_dtx_import_grid_start_click(unsigned long beat)
{
	clock_t now;
	long elapsed;
	if(!eof_song || beat != 0) { eof_dtx_grid_last_beat = (int)beat; return 0; }
	now = clock();
	elapsed = eof_dtx_grid_last_click ? (long)((now - eof_dtx_grid_last_click) * 1000 / CLOCKS_PER_SEC) : 9999L;
	if(eof_dtx_grid_last_beat == 0 && elapsed >= 0 && elapsed <= EOF_DTX_IMPORT_DOUBLECLICK_MS)
	{
		eof_dtx_grid_last_click = 0;
		eof_dtx_grid_last_beat = -1;
		return eof_dtx_import_prompt_grid_start();
	}
	eof_dtx_grid_last_click = now;
	eof_dtx_grid_last_beat = 0;
	return 0;
}

int eof_dtx_import_grid_line_mouse(int near_line, int pressed)
{
	static int was_down = 0;
	int triggered = 0;
	if(!pressed)
	{
		was_down = 0;
		return 0;
	}
	if(near_line && !was_down)
		triggered = eof_dtx_import_grid_start_click(0);
	was_down = 1;
	return triggered;
}

int eof_menu_file_dtx_import(void)
{
	char *selected;
	char selected_path[1024] = {0}, files[EOF_DTX_IMPORT_DIFFS][1024] = {{0}};
	EOF_DTX_IMPORT_CHART charts[EOF_DTX_IMPORT_DIFFS];
	EOF_DTX_IMPORT_CHART selected_chart;
	int have[EOF_DTX_IMPORT_DIFFS] = {0}, selected_diff, d, count = 0, linked = 1, switchable = 1;
	int result = D_O_K;
	memset(charts, 0, sizeof(charts));
	memset(&selected_chart, 0, sizeof(selected_chart));

	selected = ncd_file_select(0, eof_last_eof_path, "Import DTXMania chart", eof_dtx_import_get_filter());
	eof_clear_input();
	if(!selected) return D_O_K;
	ustrzcpy(selected_path, sizeof(selected_path), selected);
	if(!eof_dtx_import_parse(selected_path, &selected_chart))
	{
		allegro_message("Could not parse the selected DTX file.");
		return D_O_K;
	}
	if(!selected_chart.audio[0] || !exists(selected_chart.audio))
	{
		allegro_message("The DTX was parsed, but its referenced BGM audio could not be found.\\n\\nEOF expects #BGMWAV/#WAVxx to resolve to an existing audio file beside the DTX.");
		eof_dtx_import_chart_free(&selected_chart);
		return D_O_K;
	}
	selected_diff = selected_chart.difficulty;
	if(selected_diff < 0 || selected_diff >= EOF_DTX_IMPORT_DIFFS) selected_diff = 4;

	if(eof_dtx_import_read_set(selected_path, files))
	{
		/* set.def is authoritative about which L1..L5 difficulty the selected
		 * file represents.  Many existing DTX sets do not contain #DIFFICULTY
		 * inside each .dtx, so filename/default inference alone is insufficient. */
		for(d = 0; d < EOF_DTX_IMPORT_DIFFS; d++)
			if(files[d][0] && !ustricmp(files[d], selected_path))
			{ selected_diff = d; break; }
	}
	eof_dtx_import_scan_siblings(selected_path, files);
	if(!files[selected_diff][0]) ustrzcpy(files[selected_diff], sizeof(files[selected_diff]), selected_path);
	for(d = 0; d < EOF_DTX_IMPORT_DIFFS; d++)
	{
		if(!files[d][0]) continue;
		if(!ustricmp(files[d], selected_path))
		{
			charts[d] = selected_chart;
			memset(&selected_chart, 0, sizeof(selected_chart));
			charts[d].difficulty = d;
			have[d] = 1; count++;
		}
		else if(eof_dtx_import_parse(files[d], &charts[d]))
		{
			charts[d].difficulty = d;
			have[d] = 1; count++;
		}
	}
	if(!have[selected_diff])
	{
		charts[selected_diff] = selected_chart;
		memset(&selected_chart, 0, sizeof(selected_chart));
		have[selected_diff] = 1; count++;
	}
	eof_dtx_import_chart_free(&selected_chart);

	if(eof_song_loaded)
	{
		if(alert("DTX import creates a new EOF project.", "The currently loaded project will be replaced after the normal New Project prompts.", "Continue?", "&Yes", "&No", 'y', 'n') != 1)
			goto cleanup;
	}
	if(eof_new_chart(charts[selected_diff].audio))
		goto cleanup;

	eof_dtx_import_reset_session();
	if(charts[selected_diff].title[0]) ustrzcpy(eof_song->tags->title, sizeof(eof_song->tags->title), charts[selected_diff].title);
	if(charts[selected_diff].artist[0]) ustrzcpy(eof_song->tags->artist, sizeof(eof_song->tags->artist), charts[selected_diff].artist);
	eof_dtx_import_clear_track();
	if(!eof_dtx_import_build_beatmap(&charts[selected_diff]))
	{
		allegro_message("Could not build the EOF beat map from the DTX timing.");
		goto cleanup;
	}
	for(d = 0; d < EOF_DTX_IMPORT_DIFFS; d++)
	{
		if(!have[d]) continue;
		if(!eof_dtx_import_add_chart_notes(&charts[d], &charts[selected_diff], d))
		{
			allegro_message("DTX note import failed while creating PART_REAL_DRUM_DTX.");
			goto cleanup;
		}
		eof_dtx_import_session.present[d] = 1;
		eof_dtx_import_session.bpm[d] = charts[d].bpm;
		eof_dtx_import_session.pattern_hash[d] = charts[d].pattern_hash;
		if(charts[d].audio[0] && exists(charts[d].audio))
			(void)eof_dtx_import_copy_audio(charts[d].audio, d, eof_dtx_import_session.audio[d], sizeof(eof_dtx_import_session.audio[d]));
	}

	for(d = 0; d < EOF_DTX_IMPORT_DIFFS; d++)
	{
		if(!have[d] || d == selected_diff) continue;
		if(charts[d].pattern_hash != charts[selected_diff].pattern_hash) linked = 0;
		if(charts[d].has_complex_tempo || charts[selected_diff].has_complex_tempo || !eof_dtx_import_ratios_equal(&charts[d], &charts[selected_diff])) switchable = 0;
	}
	if(count < 2) linked = 0;
	if(charts[selected_diff].has_complex_tempo) switchable = 0;

	eof_dtx_import_session.active = 1;
	eof_dtx_import_session.song = eof_song;
	eof_dtx_import_session.current_diff = selected_diff;
	eof_dtx_import_session.bpm_linked = linked;
	eof_dtx_import_session.tempo_switchable = switchable;
	eof_dtx_import_session.switching = 1;
	if(eof_dtx_import_session.audio[selected_diff][0])
		eof_dtx_import_load_diff_audio(selected_diff);
	eof_dtx_import_session.switching = 0;

	(void)eof_menu_track_selected_track_number(EOF_TRACK_DRUM_DTX, 1);
	eof_note_type = (unsigned char)selected_diff;
	eof_note_type_i = (unsigned char)selected_diff;
	(void)eof_detect_difficulties(eof_song, EOF_TRACK_DRUM_DTX);
	eof_changes = 1;
	(void)eof_menu_file_quick_save();
	eof_fix_window_title();
	eof_render();
	if(linked)
		allegro_message("DTX import completed.\\n\\nThe difficulty files have the same drum pattern and differ only in timing/BPM. Edits in the active difficulty will be mirrored to the linked difficulties.");
	else
		allegro_message("DTX import completed.\\n\\nPART_REAL_DRUM_DTX is selected and the DTX difficulty audio is associated with the imported project.");
	result = D_O_K;

cleanup:
	for(d = 0; d < EOF_DTX_IMPORT_DIFFS; d++) eof_dtx_import_chart_free(&charts[d]);
	return result;
}

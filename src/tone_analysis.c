/*
 * tone_analysis.c
 *
 * Experimental Rocksmith tone-change estimator for the currently selected pro
 * guitar/bass arrangement.  The estimator is deliberately conservative because
 * EOF normally has a full song mix, not an isolated guitar stem.
 *
 * Research basis:
 * - Jurgens/Hinrichs/Ostermann, DAFx 2020: MFCC/spectral/RMS/phase-derived
 *   features are effective for guitar-effect recognition.
 * - Hinrichs et al., EURASIP JASMP 2022: effect classification can also work
 *   on instrument mixes, but a learned model is needed for their reported
 *   accuracy.
 * - Guo/McFee, DAFx 2023: 5 s windows and multi-label models are robust for
 *   cascaded effects.  Here we keep the 5 s temporal context but use a compact
 *   deterministic feature model because EOF has no ML runtime/model weights.
 *
 * The generated labels are broad Rocksmith tone families, not claims about a
 * particular pedal/amp model: Clean, Drive, Modulation and Delay-Reverb.
 */

#include <allegro.h>
#include <alogg.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "song.h"
#include "note.h"
#include "undo.h"
#include "utility.h"
#include "menu/track.h"
#include "tone_analysis.h"

#define TA_RATE 11025UL
#define TA_FFT 2048U
#define TA_WINDOW_MS 5000UL
#define TA_HOP_MS 1000UL
#define TA_MIN_RUN_FRAMES 5U
#define TA_MAX_TONE_MENU 8
#define TA_CLASSES 4
#define TA_PI 3.14159265358979323846
#define TA_AUDIO_BUFFER_SAMPLES 65536

enum
{
	TA_CLEAN = 0,
	TA_DRIVE,
	TA_MOD,
	TA_SPACE
};

static const char *ta_tone_names[TA_CLASSES] =
{
	"AI Clean",
	"AI Drive",
	"AI Modulation",
	"AI Delay-Reverb"
};

typedef struct
{
	short *sample;
	size_t samples;
	size_t capacity;
	unsigned long source_rate;
	unsigned source_channels;
	unsigned long phase;
	int failed;
} TA_PCM;

typedef struct
{
	unsigned long *start;
	unsigned long *end;
	unsigned long count;
} TA_NOTES;

typedef struct
{
	unsigned long start_ms;
	unsigned long end_ms;
	unsigned long notes;
	double rms;
	double crest;
	double zcr;
	double hf_ratio;
	double centroid;
	double flatness;
	double spectral_motion;
	double env_mod;
	double echo;
	double tail;
	double probability[TA_CLASSES];
	int state;
	double confidence;
} TA_FRAME;

static TA_PCM *ta_decode_ctx = NULL;
static MENU ta_tone_menu[TA_MAX_TONE_MENU];
static int ta_menu_installed = 0;
extern MENU eof_track_rocksmith_tone_change_menu[];

static void ta_log(const char *text)
{
	if(text)
		eof_log(text, 1);
}

static int ta_pcm_append(TA_PCM *pcm, short value)
{
	short *resized;
	size_t newcap;
	if(!pcm || pcm->failed)
		return 0;
	if(pcm->samples >= pcm->capacity)
	{
		newcap = pcm->capacity ? pcm->capacity * 2U : (size_t)TA_RATE * 30U;
		if(newcap <= pcm->capacity)
		{
			pcm->failed = 1;
			return 0;
		}
		resized = realloc(pcm->sample, newcap * sizeof(short));
		if(!resized)
		{
			pcm->failed = 1;
			return 0;
		}
		pcm->sample = resized;
		pcm->capacity = newcap;
	}
	pcm->sample[pcm->samples++] = value;
	return 1;
}

static void ta_decode_callback(void *buf, int nsamples, int stereo)
{
	TA_PCM *ctx = ta_decode_ctx;
	unsigned short *src = (unsigned short *)buf;
	int channels = stereo ? 2 : 1;
	int frames, frame;
	if(!ctx || !src || (nsamples <= 0) || ctx->failed)
		return;
	frames = nsamples / channels;
	for(frame = 0; frame < frames; frame++)
	{
		long mono;
		int ch;
		long sum = 0;
		for(ch = 0; ch < channels; ch++)
			sum += (long)src[frame * channels + ch] - 0x8000L;
		mono = sum / channels;
		ctx->phase += TA_RATE;
		while(ctx->phase >= ctx->source_rate)
		{
			if(mono > 32767L) mono = 32767L;
			if(mono < -32768L) mono = -32768L;
			if(!ta_pcm_append(ctx, (short)mono))
				return;
			ctx->phase -= ctx->source_rate;
		}
	}
}

static int ta_decode_loaded_audio(TA_PCM *pcm)
{
	void *data = NULL;
	ALOGG_OGG *ogg = NULL;
	long long filesize;
	unsigned long duration_ms, rate;
	unsigned channels;
	int result = 0;
	char logbuf[1024];

	if(!pcm || !eof_loaded_ogg_name[0] || !exists(eof_loaded_ogg_name))
		return 0;
	memset(pcm, 0, sizeof(*pcm));
	filesize = file_size_ex(eof_loaded_ogg_name);
	if((filesize <= 0) || (filesize > INT_MAX))
		return 0;
	data = eof_buffer_file(eof_loaded_ogg_name, 0, 0);
	if(!data)
		return 0;
	ogg = alogg_create_ogg_from_buffer(data, (int)filesize);
	if(!ogg)
		goto cleanup;
	duration_ms = alogg_get_length_msecs_ogg_ul(ogg);
	rate = (unsigned long)alogg_get_wave_freq_ogg(ogg);
	channels = alogg_get_wave_is_stereo_ogg(ogg) ? 2U : 1U;
	if(!duration_ms || !rate)
		goto cleanup;

	pcm->source_rate = rate;
	pcm->source_channels = channels;
	pcm->capacity = (size_t)(((double)duration_ms / 1000.0) * (double)TA_RATE + (double)TA_RATE);
	pcm->sample = malloc(pcm->capacity * sizeof(short));
	if(!pcm->sample)
		goto cleanup;

	ta_decode_ctx = pcm;
	result = alogg_process_ogg(ogg, ta_decode_callback, TA_AUDIO_BUFFER_SAMPLES,
		0.0, (double)duration_ms / 1000.0);
	ta_decode_ctx = NULL;
	if(!result || pcm->failed || !pcm->samples)
	{
		free(pcm->sample);
		memset(pcm, 0, sizeof(*pcm));
		result = 0;
		goto cleanup;
	}
	(void)snprintf(logbuf, sizeof(logbuf) - 1,
		"Tone analysis: decoded %lu ms to %lu Hz mono (%lu samples)",
		duration_ms, TA_RATE, (unsigned long)pcm->samples);
	ta_log(logbuf);
	result = 1;

cleanup:
	ta_decode_ctx = NULL;
	if(ogg)
		alogg_destroy_ogg(ogg);
	free(data);
	return result;
}

static void ta_pcm_free(TA_PCM *pcm)
{
	if(!pcm)
		return;
	free(pcm->sample);
	memset(pcm, 0, sizeof(*pcm));
}

static int ta_build_note_map(EOF_PRO_GUITAR_TRACK *tp, TA_NOTES *notes)
{
	unsigned long i, count = 0;
	if(!tp || !notes || !tp->pgnotes)
		return 0;
	memset(notes, 0, sizeof(*notes));
	notes->start = malloc(tp->pgnotes * sizeof(unsigned long));
	notes->end = malloc(tp->pgnotes * sizeof(unsigned long));
	if(!notes->start || !notes->end)
	{
		free(notes->start);
		free(notes->end);
		memset(notes, 0, sizeof(*notes));
		return 0;
	}
	for(i = 0; i < tp->pgnotes; i++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[i];
		unsigned long end;
		if(!np)
			continue;
		end = np->pos + ((np->length > 0) ? (unsigned long)np->length : 1UL);
		if(count && (notes->start[count - 1] == np->pos))
		{
			if(end > notes->end[count - 1])
				notes->end[count - 1] = end;
			continue;
		}
		notes->start[count] = np->pos;
		notes->end[count] = end;
		count++;
	}
	notes->count = count;
	return count ? 1 : 0;
}

static void ta_free_note_map(TA_NOTES *notes)
{
	if(!notes)
		return;
	free(notes->start);
	free(notes->end);
	memset(notes, 0, sizeof(*notes));
}

static unsigned long ta_count_notes(const TA_NOTES *notes, unsigned long start, unsigned long end)
{
	unsigned long i, count = 0;
	if(!notes)
		return 0;
	for(i = 0; i < notes->count; i++)
	{
		if(notes->start[i] >= end)
			break;
		if(notes->start[i] >= start)
			count++;
	}
	return count;
}

static double ta_rms_range(const TA_PCM *pcm, size_t first, size_t last)
{
	double sum = 0.0;
	size_t i, count;
	if(!pcm || !pcm->sample || (first >= pcm->samples))
		return 0.0;
	if(last > pcm->samples) last = pcm->samples;
	if(last <= first) return 0.0;
	count = last - first;
	for(i = first; i < last; i++)
	{
		double v = (double)pcm->sample[i] / 32768.0;
		sum += v * v;
	}
	return sqrt(sum / (double)count);
}

static double ta_note_tail_feature(const TA_PCM *pcm, const TA_NOTES *notes,
	unsigned long start_ms, unsigned long end_ms)
{
	unsigned long i, used = 0;
	double total = 0.0;
	if(!pcm || !notes)
		return 0.0;
	for(i = 0; i < notes->count; i++)
	{
		unsigned long e = notes->end[i], next = (i + 1 < notes->count) ? notes->start[i + 1] : e + 1000UL;
		size_t pre1, pre2, post1, post2;
		double pre, post, ratio;
		if(e < start_ms || e >= end_ms)
			continue;
		if(next < e + 350UL)
			continue;
		pre1 = (size_t)(((e > 120UL ? e - 120UL : 0UL) * TA_RATE) / 1000UL);
		pre2 = (size_t)((e * TA_RATE) / 1000UL);
		post1 = (size_t)(((e + 100UL) * TA_RATE) / 1000UL);
		post2 = (size_t)(((e + 350UL) * TA_RATE) / 1000UL);
		pre = ta_rms_range(pcm, pre1, pre2);
		post = ta_rms_range(pcm, post1, post2);
		if(pre < 0.001)
			continue;
		ratio = post / pre;
		if(ratio > 2.0) ratio = 2.0;
		total += ratio;
		used++;
	}
	return used ? total / (double)used : 0.0;
}

static void ta_fft(double *re, double *im, unsigned n)
{
	unsigned i, j, len;
	for(i = 1, j = 0; i < n; i++)
	{
		unsigned bit = n >> 1;
		for(; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if(i < j)
		{
			double tr = re[i], ti = im[i];
			re[i] = re[j]; im[i] = im[j];
			re[j] = tr; im[j] = ti;
		}
	}
	for(len = 2; len <= n; len <<= 1)
	{
		double angle = -2.0 * TA_PI / (double)len;
		double wlenr = cos(angle), wleni = sin(angle);
		unsigned half = len >> 1;
		for(i = 0; i < n; i += len)
		{
			double wr = 1.0, wi = 0.0;
			for(j = 0; j < half; j++)
			{
				unsigned a = i + j, b = a + half;
				double vr = re[b] * wr - im[b] * wi;
				double vi = re[b] * wi + im[b] * wr;
				double ur = re[a], ui = im[a];
				double nwr;
				re[a] = ur + vr; im[a] = ui + vi;
				re[b] = ur - vr; im[b] = ui - vi;
				nwr = wr * wlenr - wi * wleni;
				wi = wr * wleni + wi * wlenr;
				wr = nwr;
			}
		}
	}
}

static void ta_spectral_features(const TA_PCM *pcm, size_t first, size_t last,
	double *hf_ratio, double *centroid, double *flatness, double *motion)
{
	double re[TA_FFT], im[TA_FFT];
	double cents[5] = {0.0};
	double hf = 0.0, cent = 0.0, flat = 0.0;
	unsigned frames = 0, f;
	size_t span;
	if(hf_ratio) *hf_ratio = 0.0;
	if(centroid) *centroid = 0.0;
	if(flatness) *flatness = 0.0;
	if(motion) *motion = 0.0;
	if(!pcm || !pcm->sample || (last <= first + TA_FFT))
		return;
	span = last - first;
	for(f = 0; f < 5; f++)
	{
		size_t center = first + (span * (size_t)(f + 1)) / 6U;
		size_t start = (center > TA_FFT / 2U) ? center - TA_FFT / 2U : first;
		double total = 0.0, high = 0.0, csum = 0.0, logsum = 0.0, mean;
		unsigned k, bins = 0;
		if(start < first) start = first;
		if(start + TA_FFT > last) start = last - TA_FFT;
		for(k = 0; k < TA_FFT; k++)
		{
			double window = 0.5 - 0.5 * cos((2.0 * TA_PI * (double)k) / (double)(TA_FFT - 1U));
			re[k] = ((double)pcm->sample[start + k] / 32768.0) * window;
			im[k] = 0.0;
		}
		ta_fft(re, im, TA_FFT);
		for(k = 1; k < TA_FFT / 2U; k++)
		{
			double p = re[k] * re[k] + im[k] * im[k] + 1.0e-18;
			double freq = ((double)k * (double)TA_RATE) / (double)TA_FFT;
			total += p;
			csum += p * freq;
			if(freq >= 2000.0) high += p;
			logsum += log(p);
			bins++;
		}
		if(total <= 0.0 || !bins)
			continue;
		mean = total / (double)bins;
		hf += high / total;
		cents[frames] = (csum / total) / ((double)TA_RATE / 2.0);
		cent += cents[frames];
		flat += exp(logsum / (double)bins) / (mean + 1.0e-18);
		frames++;
	}
	if(frames)
	{
		double avg = cent / (double)frames, var = 0.0;
		for(f = 0; f < frames; f++)
		{
			double d = cents[f] - avg;
			var += d * d;
		}
		if(hf_ratio) *hf_ratio = hf / (double)frames;
		if(centroid) *centroid = avg;
		if(flatness) *flatness = flat / (double)frames;
		if(motion) *motion = sqrt(var / (double)frames);
	}
}

static void ta_time_features(const TA_PCM *pcm, size_t first, size_t last,
	double *rms, double *crest, double *zcr, double *env_mod, double *echo)
{
	double sumsq = 0.0, peak = 0.0;
	unsigned long crossings = 0;
	size_t i, count;
	unsigned block = (unsigned)(TA_RATE / 50UL); /* 20ms envelope */
	double *env = NULL, envmean = 0.0, envvar = 0.0;
	unsigned envn = 0, b;
	if(rms) *rms = 0.0;
	if(crest) *crest = 0.0;
	if(zcr) *zcr = 0.0;
	if(env_mod) *env_mod = 0.0;
	if(echo) *echo = 0.0;
	if(!pcm || !pcm->sample || last <= first)
		return;
	if(last > pcm->samples) last = pcm->samples;
	count = last - first;
	for(i = first; i < last; i++)
	{
		double v = (double)pcm->sample[i] / 32768.0;
		double av = fabs(v);
		sumsq += v * v;
		if(av > peak) peak = av;
		if(i > first && ((pcm->sample[i - 1] < 0) != (pcm->sample[i] < 0))) crossings++;
	}
	if(count)
	{
		double rr = sqrt(sumsq / (double)count);
		if(rms) *rms = rr;
		if(crest) *crest = rr > 1.0e-9 ? peak / rr : 0.0;
		if(zcr) *zcr = (double)crossings / (double)count;
	}
	if(block < 1U) block = 1U;
	envn = (unsigned)(count / block);
	if(envn < 8U)
		return;
	env = malloc(envn * sizeof(double));
	if(!env)
		return;
	for(b = 0; b < envn; b++)
	{
		double ss = 0.0;
		unsigned j;
		size_t pos = first + (size_t)b * block;
		for(j = 0; j < block; j++)
		{
			double v = (double)pcm->sample[pos + j] / 32768.0;
			ss += v * v;
		}
		env[b] = sqrt(ss / (double)block);
		envmean += env[b];
	}
	envmean /= (double)envn;
	for(b = 0; b < envn; b++)
	{
		double d = env[b] - envmean;
		envvar += d * d;
	}
	if(envvar > 1.0e-12)
	{
		double bestmod = 0.0, beste = 0.0;
		unsigned lag;
		/* 2-10 Hz amplitude modulation (5..25 blocks at 50 Hz). */
		for(lag = 5; lag <= 25 && lag < envn; lag++)
		{
			double corr = 0.0, a = 0.0, c = 0.0;
			for(b = lag; b < envn; b++)
			{
				double x = env[b] - envmean, y = env[b - lag] - envmean;
				corr += x * y; a += x * x; c += y * y;
			}
			if(a > 0.0 && c > 0.0)
			{
				double n = corr / sqrt(a * c);
				if(n > bestmod) bestmod = n;
			}
		}
		/* 100-700 ms envelope echoes.  Longer lags are weighted up so ordinary
		 * attack autocorrelation is less dominant. */
		for(lag = 5; lag <= 35 && lag < envn; lag += 2)
		{
			double corr = 0.0, a = 0.0, c = 0.0;
			for(b = lag; b < envn; b++)
			{
				double x = env[b] - envmean, y = env[b - lag] - envmean;
				corr += x * y; a += x * x; c += y * y;
			}
			if(a > 0.0 && c > 0.0)
			{
				double n = corr / sqrt(a * c);
				n *= 0.7 + 0.3 * ((double)lag / 35.0);
				if(n > beste) beste = n;
			}
		}
		if(env_mod) *env_mod = bestmod;
		if(echo) *echo = beste;
	}
	free(env);
}

static int ta_extract_frames(const TA_PCM *pcm, const TA_NOTES *notes, TA_FRAME **out, unsigned *outcount)
{
	unsigned long duration_ms, start;
	unsigned count = 0, capacity, index = 0;
	TA_FRAME *frames;
	if(out) *out = NULL;
	if(outcount) *outcount = 0;
	if(!pcm || !pcm->samples || !out || !outcount)
		return 0;
	duration_ms = (unsigned long)(((double)pcm->samples * 1000.0) / (double)TA_RATE);
	if(duration_ms < 1000UL)
		return 0;
	capacity = (unsigned)(duration_ms / TA_HOP_MS + 2UL);
	frames = calloc(capacity, sizeof(*frames));
	if(!frames)
		return 0;
	for(start = 0; start < duration_ms; start += TA_HOP_MS)
	{
		unsigned long end = start + TA_WINDOW_MS;
		size_t first, last;
		TA_FRAME *fr;
		if(end > duration_ms) end = duration_ms;
		if(end <= start + 500UL)
			break;
		fr = &frames[index++];
		fr->start_ms = start;
		fr->end_ms = end;
		fr->notes = ta_count_notes(notes, start, end);
		first = (size_t)(((double)start * (double)TA_RATE) / 1000.0);
		last = (size_t)(((double)end * (double)TA_RATE) / 1000.0);
		if(last > pcm->samples) last = pcm->samples;
		ta_time_features(pcm, first, last, &fr->rms, &fr->crest, &fr->zcr, &fr->env_mod, &fr->echo);
		ta_spectral_features(pcm, first, last, &fr->hf_ratio, &fr->centroid, &fr->flatness, &fr->spectral_motion);
		fr->tail = ta_note_tail_feature(pcm, notes, start, end);
		if(index >= capacity)
			break;
	}
	count = index;
	if(!count)
	{
		free(frames);
		return 0;
	}
	*out = frames;
	*outcount = count;
	return 1;
}

static double ta_z(double value, double mean, double sd)
{
	if(sd < 1.0e-9)
		return 0.0;
	return (value - mean) / sd;
}

static void ta_classify_frames(TA_FRAME *frames, unsigned count)
{
	double mean[8] = {0.0}, sd[8] = {0.0};
	unsigned i, j;
	if(!frames || !count)
		return;
	for(i = 0; i < count; i++)
	{
		double v[8] = {frames[i].crest, frames[i].zcr, frames[i].hf_ratio, frames[i].centroid,
			frames[i].flatness, frames[i].spectral_motion, frames[i].env_mod, frames[i].tail + 0.5 * frames[i].echo};
		for(j = 0; j < 8; j++) mean[j] += v[j];
	}
	for(j = 0; j < 8; j++) mean[j] /= (double)count;
	for(i = 0; i < count; i++)
	{
		double v[8] = {frames[i].crest, frames[i].zcr, frames[i].hf_ratio, frames[i].centroid,
			frames[i].flatness, frames[i].spectral_motion, frames[i].env_mod, frames[i].tail + 0.5 * frames[i].echo};
		for(j = 0; j < 8; j++) { double d = v[j] - mean[j]; sd[j] += d * d; }
	}
	for(j = 0; j < 8; j++) sd[j] = sqrt(sd[j] / (double)count);

	for(i = 0; i < count; i++)
	{
		double zcrest = ta_z(frames[i].crest, mean[0], sd[0]);
		double zzcr = ta_z(frames[i].zcr, mean[1], sd[1]);
		double zhf = ta_z(frames[i].hf_ratio, mean[2], sd[2]);
		double zcent = ta_z(frames[i].centroid, mean[3], sd[3]);
		double zflat = ta_z(frames[i].flatness, mean[4], sd[4]);
		double zmotion = ta_z(frames[i].spectral_motion, mean[5], sd[5]);
		double zmod = ta_z(frames[i].env_mod, mean[6], sd[6]);
		double zspace = ta_z(frames[i].tail + 0.5 * frames[i].echo, mean[7], sd[7]);
		double score[TA_CLASSES], maxscore = -DBL_MAX, sum = 0.0, best = -1.0;
		int beststate = TA_CLEAN;

		/* These broad discriminants intentionally combine absolute cues with
		 * song-relative z-scores.  Absolute terms keep an all-distorted song from
		 * being normalized into "clean" merely because every window is similar. */
		score[TA_DRIVE] = 1.00 * zhf + 0.65 * zflat + 0.45 * zzcr + 0.30 * zcent - 0.45 * zcrest
			+ 1.4 * (frames[i].hf_ratio - 0.16) + 0.8 * (frames[i].flatness - 0.10);
		score[TA_MOD] = 1.20 * zmod + 0.85 * zmotion + 0.30 * zcent
			+ 0.7 * (frames[i].env_mod - 0.35);
		score[TA_SPACE] = 1.15 * zspace + 0.55 * ta_z(frames[i].echo, 0.30, 0.18) - 0.20 * zcrest
			+ 0.7 * (frames[i].tail - 0.45);
		score[TA_CLEAN] = 0.55 * zcrest - 0.45 * zhf - 0.40 * zflat - 0.35 * zmod - 0.35 * zspace;
		for(j = 0; j < TA_CLASSES; j++) if(score[j] > maxscore) maxscore = score[j];
		for(j = 0; j < TA_CLASSES; j++)
		{
			frames[i].probability[j] = exp((score[j] - maxscore) / 1.15);
			sum += frames[i].probability[j];
		}
		for(j = 0; j < TA_CLASSES; j++)
		{
			frames[i].probability[j] /= sum;
			if(frames[i].probability[j] > best)
			{
				best = frames[i].probability[j];
				beststate = (int)j;
			}
		}
		frames[i].state = beststate;
		frames[i].confidence = best;
	}
}

static int ta_long_rest_near(const TA_NOTES *notes, unsigned long pos)
{
	unsigned long i, prev = 0, next = ULONG_MAX;
	if(!notes || !notes->count)
		return 0;
	for(i = 0; i < notes->count; i++)
	{
		if(notes->start[i] <= pos) prev = notes->end[i];
		if(notes->start[i] > pos) { next = notes->start[i]; break; }
	}
	return (next != ULONG_MAX && next > prev + 700UL && pos >= prev && pos <= next) ? 1 : 0;
}

static void ta_viterbi(TA_FRAME *frames, unsigned count, const TA_NOTES *notes)
{
	double *dp;
	unsigned char *back;
	unsigned i, s, p;
	int state;
	if(!frames || !count)
		return;
	dp = malloc((size_t)count * TA_CLASSES * sizeof(double));
	back = malloc((size_t)count * TA_CLASSES);
	if(!dp || !back)
	{
		free(dp); free(back);
		return;
	}
	for(s = 0; s < TA_CLASSES; s++)
	{
		dp[s] = log(frames[0].probability[s] + 1.0e-9);
		back[s] = 0;
	}
	for(i = 1; i < count; i++)
	{
		double penalty = ta_long_rest_near(notes, frames[i].start_ms) ? 0.35 : 0.85;
		for(s = 0; s < TA_CLASSES; s++)
		{
			double best = -DBL_MAX;
			unsigned bestp = 0;
			double emission = log(frames[i].probability[s] + 1.0e-9);
			if(!frames[i].notes) emission *= 0.55;
			for(p = 0; p < TA_CLASSES; p++)
			{
				double candidate = dp[(i - 1U) * TA_CLASSES + p] - ((p == s) ? 0.0 : penalty);
				if(candidate > best) { best = candidate; bestp = p; }
			}
			dp[i * TA_CLASSES + s] = best + emission;
			back[i * TA_CLASSES + s] = (unsigned char)bestp;
		}
	}
	state = 0;
	for(s = 1; s < TA_CLASSES; s++)
		if(dp[(count - 1U) * TA_CLASSES + s] > dp[(count - 1U) * TA_CLASSES + state]) state = (int)s;
	for(i = count; i > 0; i--)
	{
		frames[i - 1U].state = state;
		state = back[(i - 1U) * TA_CLASSES + state];
	}
	free(dp); free(back);
}

static void ta_merge_short_runs(TA_FRAME *frames, unsigned count, const TA_NOTES *notes, unsigned long duration_ms)
{
	int changed = 1;
	unsigned maxruns = (unsigned)(duration_ms / 30000UL) + 3U;
	unsigned pass;
	if(maxruns < 4U) maxruns = 4U;
	if(maxruns > 11U) maxruns = 11U;
	for(pass = 0; pass < 8U && changed; pass++)
	{
		unsigned i = 0, runs = 0;
		changed = 0;
		while(i < count)
		{
			unsigned start = i, end, nnotes;
			int state = frames[i].state;
			while(i < count && frames[i].state == state) i++;
			end = i;
			runs++;
			nnotes = (unsigned)ta_count_notes(notes, frames[start].start_ms, frames[end - 1U].end_ms);
			if((end - start < TA_MIN_RUN_FRAMES || nnotes < 2U) && (start > 0 || end < count))
			{
				int replacement;
				if(start == 0) replacement = frames[end].state;
				else if(end == count) replacement = frames[start - 1U].state;
				else
				{
					double left = 0.0, right = 0.0;
					unsigned k;
					int ls = frames[start - 1U].state, rs = frames[end].state;
					for(k = start; k < end; k++) { left += frames[k].probability[ls]; right += frames[k].probability[rs]; }
					replacement = (right > left) ? rs : ls;
				}
				while(start < end) frames[start++].state = replacement;
				changed = 1;
			}
		}
		if(!changed && runs > maxruns)
		{
			/* Merge the weakest internal run until the chart is not flooded with
			 * changes.  Note count is the primary support term. */
			unsigned bests = 0, beste = 0;
			double weakest = DBL_MAX;
			i = 0;
			while(i < count)
			{
				unsigned start = i, end, k;
				int st = frames[i].state;
				double conf = 0.0, strength;
				unsigned long nn;
				while(i < count && frames[i].state == st) { conf += frames[i].probability[st]; i++; }
				end = i;
				if(start == 0 || end == count) continue;
				nn = ta_count_notes(notes, frames[start].start_ms, frames[end - 1U].end_ms);
				for(k = start; k < end; k++) (void)k;
				strength = (conf / (double)(end - start)) * (1.0 + log(1.0 + (double)nn)) * (double)(end - start);
				if(strength < weakest) { weakest = strength; bests = start; beste = end; }
			}
			if(beste > bests)
			{
				int ls = frames[bests - 1U].state, rs = frames[beste].state;
				double left = 0.0, right = 0.0;
				unsigned k;
				for(k = bests; k < beste; k++) { left += frames[k].probability[ls]; right += frames[k].probability[rs]; }
				for(k = bests; k < beste; k++) frames[k].state = (right > left) ? rs : ls;
				changed = 1;
			}
		}
	}
}

static unsigned long ta_snap_change_to_note(const TA_NOTES *notes, unsigned long boundary)
{
	unsigned long i, best = boundary, bestdist = ULONG_MAX;
	if(!notes || !notes->count)
		return boundary;
	for(i = 0; i < notes->count; i++)
	{
		unsigned long p = notes->start[i];
		unsigned long dist = (p > boundary) ? p - boundary : boundary - p;
		if(dist < bestdist && dist <= 2500UL)
		{
			bestdist = dist;
			best = p ? p - 1UL : 0UL;
		}
		if(p > boundary + 2500UL) break;
	}
	return best;
}

static int ta_is_bass_track(EOF_PRO_GUITAR_TRACK *tp)
{
	if(!tp)
		return 0;
	if(tp->arrangement == EOF_BASS_ARRANGEMENT)
		return 1;
	return (eof_selected_track == EOF_TRACK_PRO_BASS || eof_selected_track == EOF_TRACK_PRO_BASS_22) ? 1 : 0;
}

static int ta_add_tone(EOF_PRO_GUITAR_TRACK *tp, int state, int default_state, unsigned long pos)
{
	int isdefault = (state == default_state) ? 1 : 0;
	if(!tp || state < 0 || state >= TA_CLASSES)
		return 0;
	return eof_track_add_section(eof_song, eof_selected_track, EOF_RS_TONE_CHANGE, 0,
		pos, (unsigned long)isdefault, 0, (char *)ta_tone_names[state]);
}

static int ta_write_tones(EOF_PRO_GUITAR_TRACK *tp, TA_FRAME *frames, unsigned count,
	const TA_NOTES *notes, int bass)
{
	unsigned i, first_supported = 0, added = 0;
	int default_state, current;
	char logbuf[512];
	if(!tp || !frames || !count)
		return 0;
	while(first_supported + 1U < count && !frames[first_supported].notes) first_supported++;
	default_state = frames[first_supported].state;

	while(tp->tonechanges)
		eof_track_pro_guitar_delete_tone_change(tp, tp->tonechanges - 1UL);
	tp->defaulttone[0] = '\0';
	(void)ustrzcpy(tp->defaulttone, sizeof(tp->defaulttone), ta_tone_names[default_state]);
	if(ta_add_tone(tp, default_state, default_state, 0UL))
		added++;
	current = default_state;

	if(bass)
	{
		/* Bass is intentionally restricted to the default plus one actual tone
		 * change.  Pick the first strongly supported persistent departure. */
		for(i = first_supported + 1U; i < count; )
		{
			unsigned start = i, end;
			int state = frames[i].state;
			double conf = 0.0;
			unsigned long nn;
			while(i < count && frames[i].state == state) { conf += frames[i].probability[state]; i++; }
			end = i;
			nn = ta_count_notes(notes, frames[start].start_ms, frames[end - 1U].end_ms);
			if(state != default_state && end - start >= 7U && nn >= 4UL && conf / (double)(end - start) >= 0.38)
			{
				unsigned long pos = ta_snap_change_to_note(notes, frames[start].start_ms);
				if(ta_add_tone(tp, state, default_state, pos)) added++;
				break;
			}
		}
	}
	else
	{
		for(i = first_supported + 1U; i < count; )
		{
			unsigned start = i, end;
			int state = frames[i].state;
			double conf = 0.0;
			unsigned long nn;
			while(i < count && frames[i].state == state) { conf += frames[i].probability[state]; i++; }
			end = i;
			if(state == current)
				continue;
			nn = ta_count_notes(notes, frames[start].start_ms, frames[end - 1U].end_ms);
			if(nn < 2UL || end - start < TA_MIN_RUN_FRAMES || conf / (double)(end - start) < 0.34)
				continue;
			if(ta_add_tone(tp, state, default_state, ta_snap_change_to_note(notes, frames[start].start_ms)))
			{
				added++;
				current = state;
			}
		}
	}
	eof_track_pro_guitar_sort_tone_changes(tp);
	(void)snprintf(logbuf, sizeof(logbuf) - 1,
		"Tone analysis: generated %u tone references; default='%s'%s",
		added, tp->defaulttone, bass ? " (bass limited to one non-default change)" : "");
	ta_log(logbuf);
	return (int)added;
}

int eof_tone_analysis_menu(void)
{
	EOF_PRO_GUITAR_TRACK *tp;
	unsigned long tracknum, duration_ms;
	TA_PCM pcm;
	TA_NOTES notes;
	TA_FRAME *frames = NULL;
	unsigned framecount = 0;
	int bass, result = D_O_K, generated;
	char msg[256];

	if(!eof_song || !eof_song_loaded || !eof_track_is_pro_guitar_track(eof_song, eof_selected_track))
	{
		allegro_message("Select a Rocksmith pro guitar or bass track first.");
		return D_O_K;
	}
	if(!eof_loaded_ogg_name[0] || !exists(eof_loaded_ogg_name))
	{
		allegro_message("No loaded OGG audio is available for tone analysis.");
		return D_O_K;
	}
	tracknum = eof_song->track[eof_selected_track]->tracknum;
	if(tracknum >= eof_song->pro_guitar_tracks || !eof_song->pro_guitar_track[tracknum])
		return D_O_K;
	tp = eof_song->pro_guitar_track[tracknum];
	if(!tp->pgnotes)
	{
		allegro_message("This arrangement has no notes. Add/import notes before analyzing tones.");
		return D_O_K;
	}

	if(alert("Experimental mixed-audio guitar-effect estimate.",
		"Results can be wrong; no minimum accuracy can be guaranteed.",
		"Review all generated Rocksmith tones before export.", "&Analyze", "&Cancel", 'a', 27) != 1)
		return D_O_K;
	if(tp->tonechanges)
	{
		if(alert("This arrangement already has tone changes.", "Automatic analysis will replace them.", NULL,
			"&Replace", "&Cancel", 'r', 27) != 1)
			return D_O_K;
	}

	eof_cursor_visible = 0;
	eof_pen_visible = 0;
	eof_render();
	ta_log("Tone analysis: starting research-driven mixed-audio estimator");
	memset(&pcm, 0, sizeof(pcm));
	memset(&notes, 0, sizeof(notes));
	if(!ta_build_note_map(tp, &notes))
	{
		allegro_message("Could not build a note map for this arrangement.");
		goto cleanup;
	}
	if(!ta_decode_loaded_audio(&pcm))
	{
		allegro_message("Could not decode the loaded OGG for tone analysis.");
		goto cleanup;
	}
	if(!ta_extract_frames(&pcm, &notes, &frames, &framecount))
	{
		allegro_message("Could not extract audio-effect features.");
		goto cleanup;
	}
	ta_classify_frames(frames, framecount);
	ta_viterbi(frames, framecount, &notes);
	duration_ms = (unsigned long)(((double)pcm.samples * 1000.0) / (double)TA_RATE);
	ta_merge_short_runs(frames, framecount, &notes, duration_ms);
	bass = ta_is_bass_track(tp);

	eof_prepare_undo(EOF_UNDO_TYPE_NONE);
	generated = ta_write_tones(tp, frames, framecount, &notes, bass);
	if(generated > 0)
	{
		eof_changes = 1;
		(void)snprintf(msg, sizeof(msg) - 1,
			"Estimated %d tone reference%s. Default: %s\n\nThese are broad effect-family estimates from the full mix and may be wrong.",
			generated, generated == 1 ? "" : "s", tp->defaulttone);
		allegro_message("%s", msg);
	}
	else
	{
		(void)eof_remove_undo();
		allegro_message("No sufficiently stable tone pattern was found.");
	}

cleanup:
	free(frames);
	ta_pcm_free(&pcm);
	ta_free_note_map(&notes);
	eof_cursor_visible = 1;
	eof_pen_visible = 1;
	eof_show_mouse(NULL);
	eof_render();
	return result;
}

void eof_tone_analysis_install_menu(void)
{
	unsigned in = 0, out = 0, i;
	if(ta_menu_installed)
		return;
	while(eof_track_rocksmith_tone_change_menu[in].text && out + 2U < TA_MAX_TONE_MENU)
		ta_tone_menu[out++] = eof_track_rocksmith_tone_change_menu[in++];
	{
		MENU item = {"Detect from &audio (experimental)", eof_tone_analysis_menu, NULL, 0, NULL};
		ta_tone_menu[out++] = item;
	}
	memset(&ta_tone_menu[out], 0, sizeof(MENU));
	for(i = 0; eof_track_rocksmith_menu[i].text; i++)
	{
		if(eof_track_rocksmith_menu[i].child == eof_track_rocksmith_tone_change_menu)
		{
			eof_track_rocksmith_menu[i].child = ta_tone_menu;
			ta_menu_installed = 1;
			break;
		}
	}
	if(ta_menu_installed)
		ta_log("Tone analysis: installed Track > Rocksmith > Tone change > Detect from audio");
}

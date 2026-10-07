/*
 * High-quality mixed-audio guitar effect estimator.
 *
 * Feature layout is inspired by Sony CSL Paris AutoFX (MIT licensed):
 * https://github.com/SonyCSLParis/AutoFX
 *
 * AutoFX trains a 163-feature MLP on isolated monophonic guitar notes.  Its
 * trained checkpoint/scaler are not published, so EOF cannot reproduce that
 * classifier directly.  This implementation ports the useful signal-analysis
 * ideas to C and adapts them to EOF's much harder input: a complete song mix.
 *
 * In particular EOF computes an AutoFX-shaped 163-value descriptor for each
 * context window (spectral moments/functionals, derivatives and MFCC summary),
 * supplements it with delay/reverb/modulation cues, and uses chart timing plus
 * the authored tuning/frets as the pitch reference.  Classification remains a
 * deterministic estimator, not the unpublished AutoFX neural network.
 */

#include <allegro.h>
#include <alogg.h>
#include <fftw3.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "song.h"
#include "undo.h"
#include "utility.h"
#include "menu/track.h"
#include "tone_analysis_hq.h"

#define HQ_RATE 22050UL
#define HQ_FFT 8192U
#define HQ_FFT_BINS (HQ_FFT / 2U + 1U)
#define HQ_STFT_HOP 512U
#define HQ_WINDOW_MS 6000UL
#define HQ_STEP_MS 500UL
#define HQ_SPEC_STREAMS 8U
#define HQ_MFCCS 10U
#define HQ_MEL_BANDS 40U
#define HQ_AUTOFX_VECTOR 163U
#define HQ_CLASSES 11U
#define HQ_AUDIO_BUFFER_SAMPLES 65536
#define HQ_PI 3.14159265358979323846

enum
{
	HQ_DRY = 0,
	HQ_FEEDBACK_DELAY,
	HQ_SLAPBACK_DELAY,
	HQ_REVERB,
	HQ_CHORUS,
	HQ_FLANGER,
	HQ_PHASER,
	HQ_TREMOLO,
	HQ_VIBRATO,
	HQ_DISTORTION,
	HQ_OVERDRIVE
};

static const char *hq_names[HQ_CLASSES] =
{
	"AI Dry",
	"AI Feedback Delay",
	"AI Slapback Delay",
	"AI Reverb",
	"AI Modulation Chorus",
	"AI Modulation Flanger",
	"AI Modulation Phaser",
	"AI Modulation Tremolo",
	"AI Modulation Vibrato",
	"AI Distortion",
	"AI Overdrive"
};

typedef struct
{
	short *sample;
	size_t samples;
	size_t capacity;
	unsigned long source_rate;
	unsigned long phase;
	int failed;
} HQ_PCM;

typedef struct
{
	unsigned long *start;
	unsigned long *end;
	double *pitch;
	unsigned long count;
} HQ_NOTES;

typedef struct
{
	unsigned long ms;
	/* AutoFX spectral streams:
	 * centroid, spread, skewness, kurtosis, flux, rolloff, slope, flatness. */
	double s[HQ_SPEC_STREAMS];
	double mfcc[HQ_MFCCS];
	/* Additional robust mixed-audio cues. */
	double rms;
	double crest;
	double zcr;
	double high_ratio;
	double peakiness;
	double flux_norm;
} HQ_SPEC;

typedef struct
{
	unsigned long start_ms;
	unsigned long end_ms;
	unsigned long notes;
	double pitch;
	/* AutoFX-shaped descriptor.  Kept explicitly so a published/trained model
	 * can be dropped into the inference path later without redesigning feature
	 * extraction again. */
	double afx[HQ_AUTOFX_VECTOR];
	double spec_mean[HQ_SPEC_STREAMS];
	double spec_std[HQ_SPEC_STREAMS];
	double spec_delta_std[HQ_SPEC_STREAMS];
	double mfcc_mean[HQ_MFCCS];
	double mfcc_max[HQ_MFCCS];
	double rms;
	double crest;
	double zcr;
	double high_ratio;
	double peakiness;
	double flux_norm;
	double amp_mod;
	double amp_mod_hz;
	double spec_mod;
	double spec_mod_hz;
	double echo_peak;
	double echo_ms;
	double echo_broad;
	double tail;
	double p[HQ_CLASSES];
	int state;
} HQ_WINDOW;

static HQ_PCM *hq_decode_ctx = NULL;
static EOF_SONG *hq_session_song = NULL;
static unsigned char hq_session_analyzed[EOF_TRACKS_MAX];

static void hq_reset_session_if_needed(EOF_SONG *sp)
{
	if(hq_session_song != sp)
	{
		hq_session_song = sp;
		memset(hq_session_analyzed, 0, sizeof(hq_session_analyzed));
	}
}

static void hq_log(const char *text)
{
	if(text)
		eof_log(text, 1);
}

static int hq_pcm_append(HQ_PCM *pcm, short value)
{
	short *resized;
	size_t newcap;
	if(!pcm || pcm->failed)
		return 0;
	if(pcm->samples >= pcm->capacity)
	{
		newcap = pcm->capacity ? pcm->capacity * 2U : (size_t)HQ_RATE * 60U;
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

static void hq_decode_callback(void *buf, int nsamples, int stereo)
{
	HQ_PCM *ctx = hq_decode_ctx;
	unsigned short *src = (unsigned short *)buf;
	int channels = stereo ? 2 : 1;
	int frames, frame;
	if(!ctx || !src || nsamples <= 0 || ctx->failed)
		return;
	frames = nsamples / channels;
	for(frame = 0; frame < frames; frame++)
	{
		long sum = 0, mono;
		int ch;
		for(ch = 0; ch < channels; ch++)
			sum += (long)src[frame * channels + ch] - 0x8000L;
		mono = sum / channels;
		ctx->phase += HQ_RATE;
		while(ctx->phase >= ctx->source_rate)
		{
			if(mono > 32767L) mono = 32767L;
			if(mono < -32768L) mono = -32768L;
			if(!hq_pcm_append(ctx, (short)mono))
				return;
			ctx->phase -= ctx->source_rate;
		}
	}
}

static int hq_decode_audio(HQ_PCM *pcm)
{
	void *data = NULL;
	ALOGG_OGG *ogg = NULL;
	long long filesize;
	unsigned long duration_ms, rate;
	int result = 0;
	char logbuf[256];
	if(!pcm || !eof_loaded_ogg_name[0] || !exists(eof_loaded_ogg_name))
		return 0;
	memset(pcm, 0, sizeof(*pcm));
	filesize = file_size_ex(eof_loaded_ogg_name);
	if(filesize <= 0 || filesize > INT_MAX)
		return 0;
	data = eof_buffer_file(eof_loaded_ogg_name, 0, 0);
	if(!data)
		return 0;
	ogg = alogg_create_ogg_from_buffer(data, (int)filesize);
	if(!ogg)
		goto cleanup;
	duration_ms = alogg_get_length_msecs_ogg_ul(ogg);
	rate = (unsigned long)alogg_get_wave_freq_ogg(ogg);
	if(!duration_ms || !rate)
		goto cleanup;
	pcm->source_rate = rate;
	pcm->capacity = (size_t)(((double)duration_ms / 1000.0) * (double)HQ_RATE + (double)HQ_RATE);
	pcm->sample = malloc(pcm->capacity * sizeof(short));
	if(!pcm->sample)
		goto cleanup;
	hq_decode_ctx = pcm;
	result = alogg_process_ogg(ogg, hq_decode_callback, HQ_AUDIO_BUFFER_SAMPLES,
		0.0, (double)duration_ms / 1000.0);
	hq_decode_ctx = NULL;
	if(!result || pcm->failed || !pcm->samples)
	{
		free(pcm->sample);
		memset(pcm, 0, sizeof(*pcm));
		result = 0;
		goto cleanup;
	}
	(void)snprintf(logbuf, sizeof(logbuf) - 1,
		"Tone AutoFX-HQ: decoded %lu ms at %lu Hz (%lu mono samples)",
		duration_ms, HQ_RATE, (unsigned long)pcm->samples);
	hq_log(logbuf);
	result = 1;
cleanup:
	hq_decode_ctx = NULL;
	if(ogg)
		alogg_destroy_ogg(ogg);
	free(data);
	return result;
}

static void hq_free_pcm(HQ_PCM *pcm)
{
	if(!pcm) return;
	free(pcm->sample);
	memset(pcm, 0, sizeof(*pcm));
}

static double hq_midi_to_hz(double midi)
{
	return 440.0 * pow(2.0, (midi - 69.0) / 12.0);
}

static double hq_note_pitch(EOF_PRO_GUITAR_TRACK *tp, EOF_PRO_GUITAR_NOTE *np)
{
	static const int guitar_midi[6] = {40, 45, 50, 55, 59, 64};
	static const int bass_midi[6] = {28, 33, 38, 43, 47, 52};
	const int *base;
	double logsum = 0.0;
	unsigned used = 0, s;
	if(!tp || !np)
		return 0.0;
	base = (tp->arrangement == EOF_BASS_ARRANGEMENT) ? bass_midi : guitar_midi;
	for(s = 0; s < tp->numstrings && s < 6U; s++)
	{
		unsigned char rawfret;
		int fret, midi;
		double hz;
		if(!(np->note & (1U << s)))
			continue;
		rawfret = np->frets[s];
		if(rawfret == 0xFF || (rawfret & 0x80))
			continue;
		fret = rawfret & 0x7F;
		midi = base[s] + (int)((signed char)tp->tuning[s]) + fret + (int)tp->capo;
		hz = hq_midi_to_hz((double)midi);
		if(hz > 0.0)
		{
			logsum += log(hz);
			used++;
		}
	}
	return used ? exp(logsum / (double)used) : 0.0;
}

static int hq_build_notes(EOF_PRO_GUITAR_TRACK *tp, HQ_NOTES *notes)
{
	unsigned long i, count = 0;
	if(!tp || !notes || !tp->pgnotes)
		return 0;
	memset(notes, 0, sizeof(*notes));
	notes->start = malloc(tp->pgnotes * sizeof(unsigned long));
	notes->end = malloc(tp->pgnotes * sizeof(unsigned long));
	notes->pitch = malloc(tp->pgnotes * sizeof(double));
	if(!notes->start || !notes->end || !notes->pitch)
	{
		free(notes->start);
		free(notes->end);
		free(notes->pitch);
		memset(notes, 0, sizeof(*notes));
		return 0;
	}
	for(i = 0; i < tp->pgnotes; i++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[i];
		unsigned long end;
		double pitch;
		if(!np) continue;
		end = np->pos + (np->length > 0 ? (unsigned long)np->length : 1UL);
		pitch = hq_note_pitch(tp, np);
		if(count && notes->start[count - 1UL] == np->pos)
		{
			if(end > notes->end[count - 1UL]) notes->end[count - 1UL] = end;
			if(pitch > 0.0)
			{
				if(notes->pitch[count - 1UL] > 0.0)
					notes->pitch[count - 1UL] = sqrt(notes->pitch[count - 1UL] * pitch);
				else
					notes->pitch[count - 1UL] = pitch;
			}
			continue;
		}
		notes->start[count] = np->pos;
		notes->end[count] = end;
		notes->pitch[count] = pitch;
		count++;
	}
	notes->count = count;
	return count ? 1 : 0;
}

static void hq_free_notes(HQ_NOTES *notes)
{
	if(!notes) return;
	free(notes->start);
	free(notes->end);
	free(notes->pitch);
	memset(notes, 0, sizeof(*notes));
}

static unsigned long hq_count_notes(const HQ_NOTES *notes, unsigned long start, unsigned long end)
{
	unsigned long i, count = 0;
	if(!notes) return 0;
	for(i = 0; i < notes->count; i++)
	{
		if(notes->start[i] >= end) break;
		if(notes->start[i] >= start) count++;
	}
	return count;
}

static double hq_window_pitch(const HQ_NOTES *notes, unsigned long start, unsigned long end)
{
	unsigned long i, used = 0;
	double logsum = 0.0;
	if(!notes) return 0.0;
	for(i = 0; i < notes->count; i++)
	{
		if(notes->start[i] >= end) break;
		if(notes->start[i] < start || notes->pitch[i] <= 0.0) continue;
		logsum += log(notes->pitch[i]);
		used++;
	}
	return used ? exp(logsum / (double)used) : 220.0;
}

static double hq_rms_range(const HQ_PCM *pcm, size_t first, size_t last)
{
	double sum = 0.0;
	size_t i;
	if(!pcm || !pcm->sample || first >= pcm->samples)
		return 0.0;
	if(last > pcm->samples) last = pcm->samples;
	if(last <= first) return 0.0;
	for(i = first; i < last; i++)
	{
		double v = (double)pcm->sample[i] / 32768.0;
		sum += v * v;
	}
	return sqrt(sum / (double)(last - first));
}

static double hq_tail_cue(const HQ_PCM *pcm, const HQ_NOTES *notes,
	unsigned long start_ms, unsigned long end_ms)
{
	unsigned long i, used = 0;
	double total = 0.0;
	if(!pcm || !notes) return 0.0;
	for(i = 0; i < notes->count; i++)
	{
		unsigned long e = notes->end[i];
		unsigned long next = (i + 1UL < notes->count) ? notes->start[i + 1UL] : e + 1400UL;
		double pre, post1, post2, post3, ratio;
		size_t a, b;
		if(e < start_ms || e >= end_ms || next < e + 900UL)
			continue;
		a = (size_t)(((e > 160UL ? e - 160UL : 0UL) * HQ_RATE) / 1000UL);
		b = (size_t)((e * HQ_RATE) / 1000UL);
		pre = hq_rms_range(pcm, a, b);
		a = (size_t)(((e + 120UL) * HQ_RATE) / 1000UL);
		b = (size_t)(((e + 360UL) * HQ_RATE) / 1000UL);
		post1 = hq_rms_range(pcm, a, b);
		a = (size_t)(((e + 380UL) * HQ_RATE) / 1000UL);
		b = (size_t)(((e + 620UL) * HQ_RATE) / 1000UL);
		post2 = hq_rms_range(pcm, a, b);
		a = (size_t)(((e + 640UL) * HQ_RATE) / 1000UL);
		b = (size_t)(((e + 880UL) * HQ_RATE) / 1000UL);
		post3 = hq_rms_range(pcm, a, b);
		if(pre < 0.001) continue;
		ratio = (0.50 * post1 + 0.30 * post2 + 0.20 * post3) / pre;
		if(ratio > 3.0) ratio = 3.0;
		total += ratio;
		used++;
	}
	return used ? total / (double)used : 0.0;
}

static void hq_envelope_cues(const HQ_PCM *pcm, size_t first, size_t last,
	double *mod, double *mod_hz, double *echo_peak, double *echo_ms, double *echo_broad)
{
	unsigned block = (unsigned)(HQ_RATE / 100UL); /* 10 ms envelope */
	unsigned n, i, lag;
	double *env = NULL, mean = 0.0, var = 0.0;
	double bestmod = 0.0, bestecho = 0.0, broad = 0.0;
	unsigned bestmodlag = 0, bestecholag = 0, broadn = 0;
	if(mod) *mod = 0.0;
	if(mod_hz) *mod_hz = 0.0;
	if(echo_peak) *echo_peak = 0.0;
	if(echo_ms) *echo_ms = 0.0;
	if(echo_broad) *echo_broad = 0.0;
	if(!pcm || !pcm->sample || last <= first || block < 1U)
		return;
	if(last > pcm->samples) last = pcm->samples;
	n = (unsigned)((last - first) / block);
	if(n < 80U) return;
	env = malloc(n * sizeof(double));
	if(!env) return;
	for(i = 0; i < n; i++)
	{
		double ss = 0.0;
		unsigned j;
		size_t p = first + (size_t)i * block;
		for(j = 0; j < block; j++)
		{
			double v = (double)pcm->sample[p + j] / 32768.0;
			ss += v * v;
		}
		env[i] = sqrt(ss / (double)block);
		mean += env[i];
	}
	mean /= (double)n;
	for(i = 0; i < n; i++)
	{
		double d = env[i] - mean;
		var += d * d;
	}
	if(var < 1.0e-12)
	{
		free(env);
		return;
	}
	for(lag = 4; lag <= 140 && lag < n; lag++)
	{
		double corr = 0.0, a = 0.0, b = 0.0, norm;
		for(i = lag; i < n; i++)
		{
			double x = env[i] - mean, y = env[i - lag] - mean;
			corr += x * y;
			a += x * x;
			b += y * y;
		}
		if(a <= 0.0 || b <= 0.0) continue;
		norm = corr / sqrt(a * b);
		if(lag >= 8 && lag <= 100 && norm > bestmod)
		{
			bestmod = norm;
			bestmodlag = lag;
		}
		/* For delay, reward a sharp repeat but retain the average positive
		 * correlation as a separate reverb-like diffuse-tail cue. */
		if(lag >= 4 && lag <= 120)
		{
			if(norm > bestecho)
			{
				bestecho = norm;
				bestecholag = lag;
			}
			if(norm > 0.0) broad += norm;
			broadn++;
		}
	}
	if(mod) *mod = bestmod;
	if(mod_hz && bestmodlag) *mod_hz = 100.0 / (double)bestmodlag;
	if(echo_peak) *echo_peak = bestecho;
	if(echo_ms && bestecholag) *echo_ms = 10.0 * (double)bestecholag;
	if(echo_broad) *echo_broad = broadn ? broad / (double)broadn : 0.0;
	free(env);
}

static double hq_hz_to_mel(double hz)
{
	return 2595.0 * log10(1.0 + hz / 700.0);
}

static double hq_mel_to_hz(double mel)
{
	return 700.0 * (pow(10.0, mel / 2595.0) - 1.0);
}

static void hq_build_mel_bins(unsigned start[HQ_MEL_BANDS], unsigned center[HQ_MEL_BANDS], unsigned end[HQ_MEL_BANDS])
{
	double melmax = hq_hz_to_mel((double)HQ_RATE / 2.0);
	unsigned b;
	for(b = 0; b < HQ_MEL_BANDS; b++)
	{
		double m0 = melmax * (double)b / (double)(HQ_MEL_BANDS + 1U);
		double m1 = melmax * (double)(b + 1U) / (double)(HQ_MEL_BANDS + 1U);
		double m2 = melmax * (double)(b + 2U) / (double)(HQ_MEL_BANDS + 1U);
		start[b] = (unsigned)(hq_mel_to_hz(m0) * (double)HQ_FFT / (double)HQ_RATE + 0.5);
		center[b] = (unsigned)(hq_mel_to_hz(m1) * (double)HQ_FFT / (double)HQ_RATE + 0.5);
		end[b] = (unsigned)(hq_mel_to_hz(m2) * (double)HQ_FFT / (double)HQ_RATE + 0.5);
		if(center[b] <= start[b]) center[b] = start[b] + 1U;
		if(end[b] <= center[b]) end[b] = center[b] + 1U;
		if(end[b] >= HQ_FFT_BINS) end[b] = HQ_FFT_BINS - 1U;
	}
}

static int hq_extract_spectra(const HQ_PCM *pcm, HQ_SPEC **out, unsigned long *outcount)
{
	double *input = NULL, *window = NULL, *prev = NULL, *cur = NULL;
	fftw_complex *freq = NULL;
	fftw_plan plan = NULL;
	HQ_SPEC *spec = NULL;
	unsigned mel_start[HQ_MEL_BANDS], mel_center[HQ_MEL_BANDS], mel_end[HQ_MEL_BANDS];
	unsigned long frames, frame;
	unsigned k;
	if(out) *out = NULL;
	if(outcount) *outcount = 0;
	if(!pcm || !pcm->sample || pcm->samples < HQ_FFT || !out || !outcount)
		return 0;
	frames = (unsigned long)((pcm->samples - HQ_FFT) / HQ_STFT_HOP + 1U);
	spec = calloc(frames, sizeof(*spec));
	input = fftw_malloc(sizeof(double) * HQ_FFT);
	freq = fftw_malloc(sizeof(fftw_complex) * HQ_FFT_BINS);
	window = malloc(sizeof(double) * HQ_FFT);
	prev = calloc(HQ_FFT_BINS, sizeof(double));
	cur = calloc(HQ_FFT_BINS, sizeof(double));
	if(!spec || !input || !freq || !window || !prev || !cur)
		goto fail;
	for(k = 0; k < HQ_FFT; k++)
		window[k] = 0.5 - 0.5 * cos((2.0 * HQ_PI * (double)k) / (double)(HQ_FFT - 1U));
	hq_build_mel_bins(mel_start, mel_center, mel_end);
	plan = fftw_plan_dft_r2c_1d(HQ_FFT, input, freq, FFTW_MEASURE);
	if(!plan) goto fail;

	for(frame = 0; frame < frames; frame++)
	{
		size_t pos = (size_t)frame * HQ_STFT_HOP;
		double sumsq = 0.0, peak = 0.0, magsum = 0.0, powersum = 0.0;
		double freqsum = 0.0, centroid, spread = 0.0, skew = 0.0, kurt = 0.0;
		double logsum = 0.0, flux = 0.0, fluxnorm = 0.0;
		double high = 0.0, peakpower = 0.0, rolloff = 0.0, slope = 0.0;
		double sumfreq = 0.0, sumfreq2 = 0.0, sumfreqmag = 0.0;
		double mel[HQ_MEL_BANDS] = {0.0};
		unsigned crossings = 0, rollbin = 0, b;
		for(k = 0; k < HQ_FFT; k++)
		{
			double v = (double)pcm->sample[pos + k] / 32768.0;
			double av = fabs(v);
			input[k] = v * window[k];
			sumsq += v * v;
			if(av > peak) peak = av;
			if(k && ((pcm->sample[pos + k - 1U] < 0) != (pcm->sample[pos + k] < 0))) crossings++;
		}
		fftw_execute(plan);
		for(k = 0; k < HQ_FFT_BINS; k++)
		{
			double hz = ((double)k * (double)HQ_RATE) / (double)HQ_FFT;
			double p = freq[k][0] * freq[k][0] + freq[k][1] * freq[k][1] + 1.0e-30;
			double m = sqrt(p);
			cur[k] = m;
			magsum += m;
			powersum += p;
			freqsum += m * hz;
			logsum += log(m + 1.0e-20);
			sumfreq += hz;
			sumfreq2 += hz * hz;
			sumfreqmag += hz * m;
			if(hz >= 2500.0) high += p;
			if(k > 0U && k + 1U < HQ_FFT_BINS)
			{
				double lp = freq[k - 1U][0] * freq[k - 1U][0] + freq[k - 1U][1] * freq[k - 1U][1];
				double rp = freq[k + 1U][0] * freq[k + 1U][0] + freq[k + 1U][1] * freq[k + 1U][1];
				if(p > lp && p > rp && p > peakpower) peakpower = p;
			}
			if(frame)
				flux += fabs(m - prev[k]);
		}
		if(magsum <= 0.0) magsum = 1.0e-20;
		if(powersum <= 0.0) powersum = 1.0e-20;
		centroid = freqsum / magsum;
		for(k = 0; k < HQ_FFT_BINS; k++)
		{
			double hz = ((double)k * (double)HQ_RATE) / (double)HQ_FFT;
			double d = hz - centroid;
			double q = cur[k] / magsum;
			double d2 = d * d;
			spread += q * d2;
			skew += q * d2 * d;
			kurt += q * d2 * d2;
		}
		{
			double accum = 0.0, target = powersum * 0.95;
			for(k = 0; k < HQ_FFT_BINS; k++)
			{
				double p = freq[k][0] * freq[k][0] + freq[k][1] * freq[k][1];
				accum += p;
				if(accum >= target) { rollbin = k; break; }
			}
			rolloff = ((double)rollbin * (double)HQ_RATE) / (double)HQ_FFT;
		}
		{
			double nbin = (double)HQ_FFT_BINS;
			double denom = nbin * sumfreq2 - sumfreq * sumfreq;
			if(fabs(denom) > 1.0e-20)
				slope = (nbin * sumfreqmag - sumfreq * magsum) / denom;
		}
		fluxnorm = flux / (magsum + 1.0e-20);

		/* Approximate torchaudio's MFCC idea locally: power mel filterbank,
		 * logarithm and DCT.  AutoFX uses 10 coefficients and later summarizes
		 * each coefficient by mean and max. */
		for(b = 0; b < HQ_MEL_BANDS; b++)
		{
			unsigned left = mel_start[b], mid = mel_center[b], right = mel_end[b];
			if(left >= HQ_FFT_BINS) continue;
			if(mid >= HQ_FFT_BINS) mid = HQ_FFT_BINS - 1U;
			if(right >= HQ_FFT_BINS) right = HQ_FFT_BINS - 1U;
			for(k = left; k < mid; k++)
			{
				double w = (double)(k - left) / (double)((mid > left) ? mid - left : 1U);
				double p = freq[k][0] * freq[k][0] + freq[k][1] * freq[k][1];
				mel[b] += p * w;
			}
			for(k = mid; k <= right; k++)
			{
				double w = (double)(right - k) / (double)((right > mid) ? right - mid : 1U);
				double p = freq[k][0] * freq[k][0] + freq[k][1] * freq[k][1];
				mel[b] += p * w;
				if(k == right) break;
			}
			mel[b] = log(mel[b] + 1.0e-20);
		}
		for(k = 0; k < HQ_MFCCS; k++)
		{
			double c = 0.0;
			for(b = 0; b < HQ_MEL_BANDS; b++)
				c += mel[b] * cos(HQ_PI * (double)k * ((double)b + 0.5) / (double)HQ_MEL_BANDS);
			spec[frame].mfcc[k] = c;
		}

		spec[frame].ms = (unsigned long)(((double)(pos + HQ_FFT / 2U) * 1000.0) / (double)HQ_RATE);
		spec[frame].s[0] = centroid;
		spec[frame].s[1] = spread;
		spec[frame].s[2] = skew;
		spec[frame].s[3] = kurt;
		spec[frame].s[4] = flux;
		spec[frame].s[5] = rolloff;
		spec[frame].s[6] = slope;
		spec[frame].s[7] = exp(logsum / (double)HQ_FFT_BINS) / (magsum / (double)HQ_FFT_BINS + 1.0e-20);
		spec[frame].rms = sqrt(sumsq / (double)HQ_FFT);
		spec[frame].crest = spec[frame].rms > 1.0e-9 ? peak / spec[frame].rms : 0.0;
		spec[frame].zcr = (double)crossings / (double)HQ_FFT;
		spec[frame].high_ratio = high / powersum;
		spec[frame].peakiness = peakpower / powersum;
		spec[frame].flux_norm = fluxnorm;
		memcpy(prev, cur, sizeof(double) * HQ_FFT_BINS);
	}
	fftw_destroy_plan(plan);
	fftw_free(input);
	fftw_free(freq);
	free(window); free(prev); free(cur);
	*out = spec;
	*outcount = frames;
	return 1;
fail:
	if(plan) fftw_destroy_plan(plan);
	if(input) fftw_free(input);
	if(freq) fftw_free(freq);
	free(window); free(prev); free(cur); free(spec);
	return 0;
}

static void hq_stream_stats(const HQ_SPEC *spec, unsigned long first, unsigned long last,
	unsigned stream, double divisor, int derivative, double out[6])
{
	unsigned long i, start, n = 0;
	double mean = 0.0, var = 0.0, third = 0.0, fourth = 0.0;
	double minv = DBL_MAX, maxv = -DBL_MAX;
	if(out) memset(out, 0, 6 * sizeof(double));
	if(!spec || !out || stream >= HQ_SPEC_STREAMS || last <= first)
		return;
	if(divisor <= 1.0e-12) divisor = 1.0;
	start = derivative ? first + 1UL : first;
	if(start >= last) return;
	for(i = start; i < last; i++)
	{
		double v = derivative ? spec[i].s[stream] - spec[i - 1UL].s[stream] : spec[i].s[stream];
		v /= divisor;
		if(!isfinite(v)) continue;
		mean += v;
		if(v < minv) minv = v;
		if(v > maxv) maxv = v;
		n++;
	}
	if(!n) return;
	mean /= (double)n;
	for(i = start; i < last; i++)
	{
		double v = derivative ? spec[i].s[stream] - spec[i - 1UL].s[stream] : spec[i].s[stream];
		double d;
		v /= divisor;
		if(!isfinite(v)) continue;
		d = v - mean;
		var += d * d;
		third += d * d * d;
		fourth += d * d * d * d;
	}
	var /= (double)n;
	out[0] = mean;
	out[1] = sqrt(var);
	if(out[1] > 1.0e-20)
	{
		double sd3 = out[1] * out[1] * out[1];
		double sd4 = sd3 * out[1];
		out[2] = (third / (double)n) / sd3;
		out[3] = (fourth / (double)n) / sd4;
	}
	out[4] = (minv == DBL_MAX) ? 0.0 : minv;
	out[5] = (maxv == -DBL_MAX) ? 0.0 : maxv;
}

static void hq_spectral_modulation(const HQ_SPEC *spec, unsigned long first, unsigned long last,
	double pitch, double *strength, double *hz)
{
	unsigned long n, lag, i;
	double mean = 0.0, best = 0.0;
	unsigned long bestlag = 0;
	if(strength) *strength = 0.0;
	if(hz) *hz = 0.0;
	if(!spec || last <= first + 8UL) return;
	if(pitch <= 1.0e-9) pitch = 220.0;
	n = last - first;
	for(i = first; i < last; i++) mean += spec[i].s[0] / pitch;
	mean /= (double)n;
	for(lag = 4; lag <= 120 && lag < n / 2UL; lag++)
	{
		double corr = 0.0, a = 0.0, b = 0.0;
		double rate_hz = (double)HQ_RATE / ((double)HQ_STFT_HOP * (double)lag);
		if(rate_hz < 0.20 || rate_hz > 10.0) continue;
		for(i = first + lag; i < last; i++)
		{
			double x = spec[i].s[0] / pitch - mean;
			double y = spec[i - lag].s[0] / pitch - mean;
			corr += x * y;
			a += x * x;
			b += y * y;
		}
		if(a > 0.0 && b > 0.0)
		{
			double v = corr / sqrt(a * b);
			if(v > best)
			{
				best = v;
				bestlag = lag;
			}
		}
	}
	if(strength) *strength = best;
	if(hz && bestlag) *hz = (double)HQ_RATE / ((double)HQ_STFT_HOP * (double)bestlag);
}

static int hq_build_windows(const HQ_PCM *pcm, const HQ_NOTES *notes,
	const HQ_SPEC *spec, unsigned long speccount, HQ_WINDOW **out, unsigned *outcount)
{
	unsigned long duration_ms, start, si = 0;
	unsigned capacity, wi = 0;
	HQ_WINDOW *win;
	if(out) *out = NULL;
	if(outcount) *outcount = 0;
	if(!pcm || !notes || !spec || !speccount || !out || !outcount)
		return 0;
	duration_ms = (unsigned long)(((double)pcm->samples * 1000.0) / (double)HQ_RATE);
	capacity = (unsigned)(duration_ms / HQ_STEP_MS + 2UL);
	win = calloc(capacity, sizeof(*win));
	if(!win) return 0;

	for(start = 0; start < duration_ms; start += HQ_STEP_MS)
	{
		unsigned long end = start + HQ_WINDOW_MS, sj, n;
		double temp[144];
		unsigned tpos = 0, apos = 0, s, j;
		size_t firstsample, lastsample;
		if(end > duration_ms) end = duration_ms;
		if(end <= start + 1000UL) break;
		while(si < speccount && spec[si].ms < start) si++;
		sj = si;
		while(sj < speccount && spec[sj].ms < end) sj++;
		n = sj - si;
		if(n < 4UL) continue;
		win[wi].start_ms = start;
		win[wi].end_ms = end;
		win[wi].notes = hq_count_notes(notes, start, end);
		win[wi].pitch = hq_window_pitch(notes, start, end);

		/* Same feature organization as AutoFX: raw moments, pitch-normalized
		 * moments, four other spectral features, then their derivatives. */
		for(s = 0; s < 4U; s++)
		{
			double st[6];
			hq_stream_stats(spec, si, sj, s, 1.0, 0, st);
			for(j = 0; j < 6U; j++) temp[tpos++] = st[j];
			win[wi].spec_mean[s] = st[0];
			win[wi].spec_std[s] = st[1];
		}
		for(s = 0; s < 4U; s++)
		{
			double st[6];
			hq_stream_stats(spec, si, sj, s, win[wi].pitch, 0, st);
			for(j = 0; j < 6U; j++) temp[tpos++] = st[j];
		}
		for(s = 4U; s < 8U; s++)
		{
			double st[6];
			hq_stream_stats(spec, si, sj, s, 1.0, 0, st);
			for(j = 0; j < 6U; j++) temp[tpos++] = st[j];
			win[wi].spec_mean[s] = st[0];
			win[wi].spec_std[s] = st[1];
		}
		for(s = 0; s < 4U; s++)
		{
			double st[6];
			hq_stream_stats(spec, si, sj, s, 1.0, 1, st);
			for(j = 0; j < 6U; j++) temp[tpos++] = st[j];
			win[wi].spec_delta_std[s] = st[1];
		}
		for(s = 0; s < 4U; s++)
		{
			double st[6];
			hq_stream_stats(spec, si, sj, s, win[wi].pitch, 1, st);
			for(j = 0; j < 6U; j++) temp[tpos++] = st[j];
		}
		for(s = 4U; s < 8U; s++)
		{
			double st[6];
			hq_stream_stats(spec, si, sj, s, 1.0, 1, st);
			for(j = 0; j < 6U; j++) temp[tpos++] = st[j];
			win[wi].spec_delta_std[s] = st[1];
		}

		/* AutoFX drops the minimum of spectral flux (conceptual index 52),
		 * because it is always zero in their extraction. */
		for(j = 0; j < 144U; j++)
			if(j != 52U && apos < 143U) win[wi].afx[apos++] = temp[j];

		for(s = 0; s < HQ_MFCCS; s++)
		{
			double sum = 0.0, maxv = -DBL_MAX;
			for(j = (unsigned)si; j < (unsigned)sj; j++)
			{
				double v = spec[j].mfcc[s];
				sum += v;
				if(v > maxv) maxv = v;
			}
			win[wi].mfcc_mean[s] = sum / (double)n;
			win[wi].mfcc_max[s] = (maxv == -DBL_MAX) ? 0.0 : maxv;
			if(apos < HQ_AUTOFX_VECTOR) win[wi].afx[apos++] = win[wi].mfcc_mean[s];
		}
		for(s = 0; s < HQ_MFCCS; s++)
			if(apos < HQ_AUTOFX_VECTOR) win[wi].afx[apos++] = win[wi].mfcc_max[s];

		for(j = (unsigned)si; j < (unsigned)sj; j++)
		{
			win[wi].rms += spec[j].rms;
			win[wi].crest += spec[j].crest;
			win[wi].zcr += spec[j].zcr;
			win[wi].high_ratio += spec[j].high_ratio;
			win[wi].peakiness += spec[j].peakiness;
			win[wi].flux_norm += spec[j].flux_norm;
		}
		win[wi].rms /= (double)n;
		win[wi].crest /= (double)n;
		win[wi].zcr /= (double)n;
		win[wi].high_ratio /= (double)n;
		win[wi].peakiness /= (double)n;
		win[wi].flux_norm /= (double)n;
		firstsample = (size_t)(((double)start * (double)HQ_RATE) / 1000.0);
		lastsample = (size_t)(((double)end * (double)HQ_RATE) / 1000.0);
		hq_envelope_cues(pcm, firstsample, lastsample,
			&win[wi].amp_mod, &win[wi].amp_mod_hz,
			&win[wi].echo_peak, &win[wi].echo_ms, &win[wi].echo_broad);
		hq_spectral_modulation(spec, si, sj, win[wi].pitch, &win[wi].spec_mod, &win[wi].spec_mod_hz);
		win[wi].tail = hq_tail_cue(pcm, notes, start, end);
		wi++;
		if(wi >= capacity) break;
	}
	if(!wi) { free(win); return 0; }
	*out = win;
	*outcount = wi;
	return 1;
}

static double hq_z(double v, double mean, double sd)
{
	return sd > 1.0e-12 ? (v - mean) / sd : 0.0;
}

static double hq_metric(const HQ_WINDOW *w, unsigned metric)
{
	if(!w) return 0.0;
	switch(metric)
	{
		case 0: return w->crest;
		case 1: return w->high_ratio;
		case 2: return w->spec_mean[7];
		case 3: return w->flux_norm;
		case 4: return sqrt(fabs(w->spec_mean[1])) / 5000.0;
		case 5: return w->spec_std[0] / 1500.0;
		case 6: return w->spec_delta_std[6] * 100000.0;
		case 7: return w->amp_mod;
		case 8: return w->spec_mod;
		case 9: return w->echo_peak;
		case 10: return w->echo_broad;
		case 11: return w->tail;
		case 12: return w->peakiness;
		case 13: return w->spec_mean[5] / ((double)HQ_RATE / 2.0);
		default: return 0.0;
	}
}

static void hq_classify(HQ_WINDOW *w, unsigned count)
{
	#define HQ_METRICS 14U
	double mean[HQ_METRICS] = {0.0}, sd[HQ_METRICS] = {0.0};
	unsigned i, m, c;
	if(!w || !count) return;
	for(i = 0; i < count; i++)
		for(m = 0; m < HQ_METRICS; m++) mean[m] += hq_metric(&w[i], m);
	for(m = 0; m < HQ_METRICS; m++) mean[m] /= (double)count;
	for(i = 0; i < count; i++)
		for(m = 0; m < HQ_METRICS; m++)
		{
			double d = hq_metric(&w[i], m) - mean[m];
			sd[m] += d * d;
		}
	for(m = 0; m < HQ_METRICS; m++) sd[m] = sqrt(sd[m] / (double)count);

	for(i = 0; i < count; i++)
	{
		double crest = hq_metric(&w[i], 0), high = hq_metric(&w[i], 1), flat = hq_metric(&w[i], 2);
		double flux = hq_metric(&w[i], 3), spread = hq_metric(&w[i], 4), centstd = hq_metric(&w[i], 5);
		double slopestd = hq_metric(&w[i], 6), ampmod = hq_metric(&w[i], 7), specmod = hq_metric(&w[i], 8);
		double echo = hq_metric(&w[i], 9), broad = hq_metric(&w[i], 10), tail = hq_metric(&w[i], 11);
		double peak = hq_metric(&w[i], 12), roll = hq_metric(&w[i], 13);
		double zcrest = hq_z(crest, mean[0], sd[0]);
		double zhigh = hq_z(high, mean[1], sd[1]);
		double zflat = hq_z(flat, mean[2], sd[2]);
		double zflux = hq_z(flux, mean[3], sd[3]);
		double zspread = hq_z(spread, mean[4], sd[4]);
		double zcentstd = hq_z(centstd, mean[5], sd[5]);
		double zslope = hq_z(slopestd, mean[6], sd[6]);
		double zampmod = hq_z(ampmod, mean[7], sd[7]);
		double zspecmod = hq_z(specmod, mean[8], sd[8]);
		double zecho = hq_z(echo, mean[9], sd[9]);
		double zbroad = hq_z(broad, mean[10], sd[10]);
		double ztail = hq_z(tail, mean[11], sd[11]);
		double zpeak = hq_z(peak, mean[12], sd[12]);
		double zroll = hq_z(roll, mean[13], sd[13]);
		double score[HQ_CLASSES], maxs = -DBL_MAX, sum = 0.0;
		double drive, modcommon, sharp_echo, delay_lag;
		int best = HQ_DRY;

		drive = 0.95 * zhigh + 0.80 * zflat + 0.55 * zroll + 0.35 * zflux - 0.70 * zcrest
			+ 2.0 * (high - 0.12) + 1.4 * (flat - 0.08);
		modcommon = 0.85 * zspecmod + 0.55 * zcentstd + 0.40 * zflux + 0.25 * zspread;
		sharp_echo = echo - 1.6 * broad;
		if(sharp_echo < 0.0) sharp_echo = 0.0;
		delay_lag = w[i].echo_ms;

		memset(score, 0, sizeof(score));
		score[HQ_DISTORTION] = drive + 0.75 * zhigh + 0.35 * zflat - 0.20 * zpeak
			+ 1.2 * (high - 0.18) + 0.8 * (flat - 0.12);
		score[HQ_OVERDRIVE] = 0.72 * drive + 0.30 * zroll - 0.15 * zflat + 0.20 * zcrest
			+ 0.45 * (high - 0.09);

		score[HQ_TREMOLO] = 1.50 * zampmod - 0.35 * zspecmod + 1.15 * (ampmod - 0.45);
		score[HQ_VIBRATO] = 1.45 * zspecmod - 0.30 * zampmod + 0.65 * zcentstd
			+ 0.95 * (specmod - 0.40);
		score[HQ_CHORUS] = modcommon + 0.55 * zspread + 0.25 * zslope
			+ 0.55 * (specmod - 0.32);
		score[HQ_FLANGER] = modcommon + 0.65 * zflux + 0.55 * zslope + 0.30 * zhigh
			+ 0.45 * (specmod - 0.34);
		score[HQ_PHASER] = modcommon + 0.75 * zslope - 0.20 * zhigh - 0.10 * zspread
			+ 0.35 * (specmod - 0.30);
		/* Gentle rate priors.  They do not decide the class alone. */
		if(w[i].amp_mod_hz >= 2.0 && w[i].amp_mod_hz <= 9.0) score[HQ_TREMOLO] += 0.35;
		if(w[i].spec_mod_hz >= 2.0 && w[i].spec_mod_hz <= 9.0) score[HQ_VIBRATO] += 0.25;
		if(w[i].spec_mod_hz >= 0.15 && w[i].spec_mod_hz <= 4.0)
		{
			score[HQ_CHORUS] += 0.18;
			score[HQ_FLANGER] += 0.18;
			score[HQ_PHASER] += 0.18;
		}

		score[HQ_REVERB] = 1.15 * ztail + 1.00 * zbroad + 0.35 * zecho - 0.30 * zcrest
			+ 1.20 * (tail - 0.42) + 0.70 * (broad - 0.08) - 0.35 * sharp_echo;
		score[HQ_SLAPBACK_DELAY] = 1.15 * zecho + 0.50 * ztail - 0.45 * zbroad + 1.50 * sharp_echo;
		score[HQ_FEEDBACK_DELAY] = 1.20 * zecho + 0.45 * ztail + 0.30 * zbroad + 1.25 * sharp_echo;
		if(delay_lag >= 40.0 && delay_lag <= 180.0)
		{
			score[HQ_SLAPBACK_DELAY] += 0.80;
			score[HQ_FEEDBACK_DELAY] -= 0.20;
		}
		else if(delay_lag > 180.0 && delay_lag <= 900.0)
		{
			score[HQ_FEEDBACK_DELAY] += 0.60;
			score[HQ_SLAPBACK_DELAY] -= 0.25;
		}

		score[HQ_DRY] = 0.85 * zcrest + 0.55 * zpeak - 0.65 * zhigh - 0.55 * zflat
			- 0.55 * zampmod - 0.55 * zspecmod - 0.45 * zecho - 0.45 * ztail
			+ 0.35 * (crest - 3.0);

		/* Windows without authored guitar notes are still useful for reverb/delay
		 * tails, but should not dominate timbral classes. */
		if(!w[i].notes)
		{
			for(c = 0; c < HQ_CLASSES; c++)
				if(c != HQ_REVERB && c != HQ_FEEDBACK_DELAY && c != HQ_SLAPBACK_DELAY)
					score[c] *= 0.55;
		}

		for(c = 0; c < HQ_CLASSES; c++) if(score[c] > maxs) maxs = score[c];
		for(c = 0; c < HQ_CLASSES; c++)
		{
			w[i].p[c] = exp((score[c] - maxs) / 1.20);
			sum += w[i].p[c];
		}
		for(c = 0; c < HQ_CLASSES; c++)
		{
			w[i].p[c] /= sum;
			if(w[i].p[c] > w[i].p[best]) best = (int)c;
		}
		w[i].state = best;
	}

	/* 2-second probability smoother.  The source mix contains drums/vocals that
	 * can create isolated false positives, whereas a guitar rig normally stays
	 * stable for longer than one 500 ms decision step. */
	{
		int *state = malloc(count * sizeof(int));
		if(state)
		{
			for(i = 0; i < count; i++)
			{
				double bestv = -1.0;
				int bestc = HQ_DRY;
				for(c = 0; c < HQ_CLASSES; c++)
				{
					double s = 0.0;
					int d;
					for(d = -4; d <= 4; d++)
					{
						long q = (long)i + d;
						if(q >= 0 && q < (long)count)
							s += w[q].p[c] * (1.0 - 0.10 * fabs((double)d));
					}
					if(s > bestv) { bestv = s; bestc = (int)c; }
				}
				state[i] = bestc;
			}
			for(i = 0; i < count; i++) w[i].state = state[i];
			free(state);
		}
	}
	#undef HQ_METRICS
}

static void hq_pick_two(const HQ_WINDOW *w, unsigned count, const HQ_NOTES *notes,
	int *base, int *other, unsigned long *change)
{
	double totals[HQ_CLASSES] = {0.0};
	unsigned i, c;
	int b = HQ_DRY, o = HQ_DRY;
	if(base) *base = HQ_DRY;
	if(other) *other = HQ_DRY;
	if(change) *change = (notes && notes->count) ? notes->start[0] : 1UL;
	if(!w || !count) return;
	for(i = 0; i < count; i++)
	{
		double weight = w[i].notes ? (1.0 + 0.16 * (double)((w[i].notes > 10UL) ? 10UL : w[i].notes)) : 0.12;
		for(c = 0; c < HQ_CLASSES; c++) totals[c] += w[i].p[c] * weight;
	}
	for(c = 1; c < HQ_CLASSES; c++) if(totals[c] > totals[b]) b = (int)c;
	o = (b == HQ_DRY) ? HQ_OVERDRIVE : HQ_DRY;
	for(c = 0; c < HQ_CLASSES; c++)
		if((int)c != b && totals[c] > totals[o]) o = (int)c;
	if(totals[o] < totals[b] * 0.34) o = b;

	if(o != b && change)
	{
		for(i = 0; i + 7U < count; i++)
		{
			unsigned j, support = 0;
			if(w[i].start_ms < 1000UL) continue;
			for(j = i; j < i + 8U; j++)
				if(w[j].state == o) support++;
			if(support >= 6U && hq_count_notes(notes, w[i].start_ms, w[i + 7U].end_ms) >= 3UL)
			{
				unsigned long boundary = w[i].start_ms, best = boundary, bestdist = ULONG_MAX, n;
				for(n = 0; notes && n < notes->count; n++)
				{
					unsigned long p = notes->start[n];
					unsigned long d = (p > boundary) ? p - boundary : boundary - p;
					if(d < bestdist && d <= 2000UL) { bestdist = d; best = p; }
					if(p > boundary + 2000UL) break;
				}
				*change = best ? best : 1UL;
				break;
			}
		}
	}
	if(base) *base = b;
	if(other) *other = o;
}

static int hq_analyze(EOF_SONG *sp, unsigned long track, int *base, int *other, unsigned long *change)
{
	unsigned long ptrack;
	EOF_PRO_GUITAR_TRACK *tp;
	HQ_PCM pcm;
	HQ_NOTES notes;
	HQ_SPEC *spec = NULL;
	HQ_WINDOW *win = NULL;
	unsigned long speccount = 0;
	unsigned wincount = 0;
	int ok = 0;
	char logbuf[512];
	if(!sp || !track || track >= sp->tracks || !eof_track_is_pro_guitar_track(sp, track)) return 0;
	if(track == EOF_TRACK_DRUM_DTX) return 0;
	ptrack = sp->track[track]->tracknum;
	if(ptrack >= sp->pro_guitar_tracks || !sp->pro_guitar_track[ptrack]) return 0;
	tp = sp->pro_guitar_track[ptrack];
	if(!tp->pgnotes) return 0;
	memset(&pcm, 0, sizeof(pcm));
	memset(&notes, 0, sizeof(notes));
	if(!hq_build_notes(tp, &notes)) goto cleanup;
	if(!hq_decode_audio(&pcm)) goto cleanup;
	hq_log("Tone AutoFX-HQ: computing dense 8192-point STFT, spectral moments and MFCC map");
	if(!hq_extract_spectra(&pcm, &spec, &speccount)) goto cleanup;
	(void)snprintf(logbuf, sizeof(logbuf) - 1,
		"Tone AutoFX-HQ: %lu STFT frames; building AutoFX-shaped %u-value descriptors",
		speccount, HQ_AUTOFX_VECTOR);
	hq_log(logbuf);
	if(!hq_build_windows(&pcm, &notes, spec, speccount, &win, &wincount)) goto cleanup;
	hq_classify(win, wincount);
	hq_pick_two(win, wincount, &notes, base, other, change);
	(void)snprintf(logbuf, sizeof(logbuf) - 1,
		"Tone AutoFX-HQ: %u context windows classified; dominant='%s', alternate='%s'",
		wincount, hq_names[*base], hq_names[*other]);
	hq_log(logbuf);
	ok = 1;
cleanup:
	free(spec);
	free(win);
	hq_free_pcm(&pcm);
	hq_free_notes(&notes);
	return ok;
}

static void hq_make_secondary_name(int base, int other, char *out, size_t outsz)
{
	if(!out || !outsz) return;
	if(other == base)
		(void)snprintf(out, outsz - 1, "%s Alt", hq_names[other]);
	else
		ustrzcpy(out, (int)outsz, hq_names[other]);
}

int eof_tone_analysis_hq_track_has_results(EOF_SONG *sp, unsigned long track)
{
	unsigned long ptrack, i;
	EOF_PRO_GUITAR_TRACK *tp;
	if(!sp || !track || track >= sp->tracks || !eof_track_is_pro_guitar_track(sp, track)) return 0;
	hq_reset_session_if_needed(sp);
	if(track < EOF_TRACKS_MAX && hq_session_analyzed[track]) return 1;
	ptrack = sp->track[track]->tracknum;
	if(ptrack >= sp->pro_guitar_tracks || !sp->pro_guitar_track[ptrack]) return 0;
	tp = sp->pro_guitar_track[ptrack];
	if(!strncmp(tp->defaulttone, "AI ", 3)) return 1;
	for(i = 0; i < tp->tonechanges; i++)
		if(!strncmp(tp->tonechange[i].name, "AI ", 3)) return 1;
	return 0;
}

int eof_tone_analysis_hq_estimate(EOF_SONG *sp, unsigned long track,
	char *base, size_t basesz, char *other, size_t othersz)
{
	int b = HQ_DRY, o = HQ_DRY;
	unsigned long change = 1UL;
	if(base && basesz) base[0] = '\0';
	if(other && othersz) other[0] = '\0';
	if(!base || !basesz || !other || !othersz) return 0;
	if(!hq_analyze(sp, track, &b, &o, &change)) return 0;
	ustrzcpy(base, (int)basesz, hq_names[b]);
	hq_make_secondary_name(b, o, other, othersz);
	return 1;
}

int eof_tone_analysis_hq_menu(void)
{
	unsigned long ptrack, change = 1UL;
	EOF_PRO_GUITAR_TRACK *tp;
	int b = HQ_DRY, o = HQ_DRY;
	char othername[EOF_SECTION_NAME_LENGTH + 1] = {0};
	char msg[768];
	if(!eof_song || !eof_song_loaded || !eof_track_is_pro_guitar_track(eof_song, eof_selected_track) || eof_selected_track == EOF_TRACK_DRUM_DTX)
	{
		allegro_message("Select a populated Rocksmith pro guitar or bass arrangement first.");
		return D_O_K;
	}
	if(!eof_loaded_ogg_name[0] || !exists(eof_loaded_ogg_name))
	{
		allegro_message("No loaded OGG audio is available for tone analysis.");
		return D_O_K;
	}
	ptrack = eof_song->track[eof_selected_track]->tracknum;
	if(ptrack >= eof_song->pro_guitar_tracks || !eof_song->pro_guitar_track[ptrack]) return D_O_K;
	tp = eof_song->pro_guitar_track[ptrack];
	if(!tp->pgnotes)
	{
		allegro_message("This arrangement has no notes.");
		return D_O_K;
	}
	if(alert("AutoFX-inspired mixed-audio guitar-effect estimate.",
		"It computes dense 8192-point STFT, 163-value descriptors and MFCCs over the whole song.",
		"The original AutoFX trained weights are not published; full-mix results still require review.",
		"&Analyze", "&Cancel", 'a', 27) != 1)
		return D_O_K;
	if(tp->tonechanges && alert("This arrangement already has tone changes.",
		"Analysis will replace them with exactly two references.", NULL,
		"&Replace", "&Cancel", 'r', 27) != 1)
		return D_O_K;

	eof_cursor_visible = 0;
	eof_pen_visible = 0;
	eof_render();
	hq_log("Tone AutoFX-HQ: starting full-song analysis");
	if(!hq_analyze(eof_song, eof_selected_track, &b, &o, &change))
	{
		allegro_message("AutoFX-inspired effect analysis failed. See eof_log.txt for details.");
		goto done;
	}
	hq_make_secondary_name(b, o, othername, sizeof(othername));
	eof_prepare_undo(EOF_UNDO_TYPE_NONE);
	while(tp->tonechanges)
		eof_track_pro_guitar_delete_tone_change(tp, tp->tonechanges - 1UL);
	ustrzcpy(tp->defaulttone, sizeof(tp->defaulttone), hq_names[b]);
	if(!eof_track_add_section(eof_song, eof_selected_track, EOF_RS_TONE_CHANGE, 0, 0UL, 1UL, 0, (char *)hq_names[b]) ||
	   !eof_track_add_section(eof_song, eof_selected_track, EOF_RS_TONE_CHANGE, 0, change ? change : 1UL, 0UL, 0, othername))
	{
		(void)eof_remove_undo();
		allegro_message("Could not write the two estimated tone references.");
		goto done;
	}
	eof_track_pro_guitar_sort_tone_changes(tp);
	eof_changes = 1;
	eof_project_unsaved = 1;
	hq_reset_session_if_needed(eof_song);
	if(eof_selected_track < EOF_TRACKS_MAX) hq_session_analyzed[eof_selected_track] = 1;
	(void)snprintf(msg, sizeof(msg) - 1,
		"AutoFX-inspired estimate complete.\n\nDefault: %s\nSecond: %s\n\n"
		"The 163-feature layout follows AutoFX concepts, but the published repository does not include its trained checkpoint/scaler. "
		"EOF therefore uses its own mixed-audio classifier and the result remains an estimate.",
		tp->defaulttone, othername);
	allegro_message("%s", msg);
done:
	eof_cursor_visible = 1;
	eof_pen_visible = 1;
	eof_show_mouse(NULL);
	eof_render();
	return D_O_K;
}

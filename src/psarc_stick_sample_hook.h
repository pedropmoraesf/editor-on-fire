#ifndef EOF_PSARC_STICK_SAMPLE_HOOK_H
#define EOF_PSARC_STICK_SAMPLE_HOOK_H

/*
 * Bundled stick-count sample backend for PSARC export.
 *
 * The legacy psarc_export.c flow still builds a tiny MIDI/count-in command, but
 * this hook deliberately bypasses FluidSynth for stick count-ins.  Instead it:
 *   1. uses the bundled tools/psarc/stick_countin.wav sample;
 *   2. derives the INITIAL tempo from beat 0;
 *   3. finds the predominant time signature by chart duration;
 *   4. prepends exactly one measure at that tempo/meter;
 *   5. places one copy of the stick sample on every meter beat;
 *   6. shifts the RS XML by exactly the same one-measure duration.
 *
 * The bundled WAV is a short mono PCM extract of the user-provided TuxGuitar
 * stick sound.  Only its useful transient is kept; the several seconds of
 * trailing silence from the original export are intentionally omitted.
 */

#include "midi.h"
#include "audio_normalize.h"

#ifdef system
#undef system
#endif

#ifdef eof_popup_dialog
#undef eof_popup_dialog
#endif

#define EOF_PSARC_STICK_DIALOG_INDEX 22
#define EOF_PSARC_STICK_SAMPLE_NAME "stick_countin.wav"

typedef struct
{
	unsigned num;
	unsigned den;
	unsigned long duration;
} EOF_PSARC_METER_ACCUM;

typedef struct
{
	unsigned channels;
	unsigned bits;
	unsigned long rate;
	unsigned long frames;
	short *samples;
} EOF_PSARC_STICK_PCM;

static unsigned long eof_psarc_stick_le16(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8);
}

static unsigned long eof_psarc_stick_le32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
	       ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int eof_psarc_stick_find_sample(char *out, size_t outsz)
{
	char exe[1024] = {0}, dir[1024] = {0}, candidate[1024] = {0};
	if(!out || !outsz)
		return 0;
	out[0] = '\0';
	get_executable_name(exe, sizeof(exe));
	ustrzcpy(dir, sizeof(dir), exe);
	*get_filename(dir) = '\0';

	(void)snprintf(candidate, sizeof(candidate) - 1,
		"%stools%cpsarc%c%s", dir, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR,
		EOF_PSARC_STICK_SAMPLE_NAME);
	fix_filename_slashes(candidate);
	if(exists(candidate))
	{
		ustrzcpy(out, (int)outsz, candidate);
		return 1;
	}

	(void)snprintf(candidate, sizeof(candidate) - 1,
		"%s..%ctools%cpsarc%c%s", dir, OTHER_PATH_SEPARATOR,
		OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR, EOF_PSARC_STICK_SAMPLE_NAME);
	fix_filename_slashes(candidate);
	if(exists(candidate))
	{
		ustrzcpy(out, (int)outsz, candidate);
		return 1;
	}

	(void)snprintf(candidate, sizeof(candidate) - 1,
		"tools%cpsarc%c%s", OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR,
		EOF_PSARC_STICK_SAMPLE_NAME);
	fix_filename_slashes(candidate);
	if(exists(candidate))
	{
		ustrzcpy(out, (int)outsz, candidate);
		return 1;
	}
	return 0;
}

static void eof_psarc_stick_predominant_meter(EOF_SONG *sp, unsigned *num, unsigned *den)
{
	EOF_PSARC_METER_ACCUM meters[32];
	unsigned used = 0, best = 0;
	unsigned long beat;

	if(num) *num = 4;
	if(den) *den = 4;
	if(!sp || !sp->beats)
		return;
	memset(meters, 0, sizeof(meters));

	for(beat = 0; beat < sp->beats; beat++)
	{
		unsigned n = 4, d = 4, slot;
		unsigned long span = 1;
		if(!eof_get_effective_ts(sp, &n, &d, beat, 0) || !n || !d)
		{
			n = 4;
			d = 4;
		}
		if(beat + 1UL < sp->beats && sp->beat[beat + 1UL]->pos > sp->beat[beat]->pos)
			span = sp->beat[beat + 1UL]->pos - sp->beat[beat]->pos;
		else if(beat && sp->beat[beat]->pos > sp->beat[beat - 1UL]->pos)
			span = sp->beat[beat]->pos - sp->beat[beat - 1UL]->pos;

		for(slot = 0; slot < used; slot++)
			if(meters[slot].num == n && meters[slot].den == d)
				break;
		if(slot == used && used < 32U)
		{
			meters[used].num = n;
			meters[used].den = d;
			meters[used].duration = 0;
			used++;
		}
		if(slot < used)
			meters[slot].duration += span;
	}

	if(!used)
		return;
	for(best = 0; best + 1U < used; )
	{
		unsigned i, winner = best;
		for(i = best + 1U; i < used; i++)
			if(meters[i].duration > meters[winner].duration)
				winner = i;
		if(winner == best)
			break;
		best = winner;
	}
	if(num) *num = meters[best].num;
	if(den) *den = meters[best].den;
}

static double eof_psarc_stick_initial_quarter_ms(EOF_SONG *sp)
{
	if(sp && sp->beats && sp->beat[0] && sp->beat[0]->ppqn)
		return (double)sp->beat[0]->ppqn / 1000.0;
	if(sp && sp->beats > 1UL && sp->beat[0] && sp->beat[1] &&
	   sp->beat[1]->fpos > sp->beat[0]->fpos)
		return sp->beat[1]->fpos - sp->beat[0]->fpos;
	return 500.0;
}

static unsigned long eof_psarc_stick_measure_ms(EOF_SONG *sp, unsigned *outnum, unsigned *outden)
{
	unsigned num = 4, den = 4;
	double quarter_ms, beat_ms, measure_ms;
	eof_psarc_stick_predominant_meter(sp, &num, &den);
	quarter_ms = eof_psarc_stick_initial_quarter_ms(sp);
	if(!den) den = 4;
	beat_ms = quarter_ms * 4.0 / (double)den;
	measure_ms = beat_ms * (double)num;
	if(measure_ms < 100.0) measure_ms = 100.0;
	if(measure_ms > 30000.0) measure_ms = 30000.0;
	if(outnum) *outnum = num;
	if(outden) *outden = den;
	return (unsigned long)(measure_ms + 0.5);
}

static unsigned long eof_psarc_stick_intro_strtoul(const char *text, char **end, int base, int sticks)
{
	unsigned long normal = strtoul(text, end, base);
	if(sticks)
	{
		unsigned long existing = 0, measure;
		if(eof_song && eof_song->beats && eof_song->beat[0])
			existing = eof_song->beat[0]->pos;
		measure = eof_psarc_stick_measure_ms(eof_song, NULL, NULL);
		return existing + measure; /* legacy code subtracts existing again */
	}
	return normal;
}

static int eof_psarc_stick_load_wav(const char *path, EOF_PSARC_STICK_PCM *pcm)
{
	FILE *fp;
	unsigned char head[12], chunk[8], fmt[32];
	unsigned long chunk_size, data_pos = 0, data_size = 0;
	unsigned format = 0, channels = 0, bits = 0;
	unsigned long rate = 0;
	short *samples = NULL;

	if(!path || !pcm)
		return 0;
	memset(pcm, 0, sizeof(*pcm));
	fp = fopen(path, "rb");
	if(!fp)
		return 0;
	if(fread(head, 1, 12, fp) != 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4))
	{
		fclose(fp);
		return 0;
	}

	while(fread(chunk, 1, 8, fp) == 8)
	{
		chunk_size = eof_psarc_stick_le32(chunk + 4);
		if(!memcmp(chunk, "fmt ", 4))
		{
			unsigned long want = (chunk_size < sizeof(fmt)) ? chunk_size : (unsigned long)sizeof(fmt);
			memset(fmt, 0, sizeof(fmt));
			if(fread(fmt, 1, want, fp) != want)
				break;
			if(chunk_size > want)
				fseek(fp, (long)(chunk_size - want), SEEK_CUR);
			if(want >= 16UL)
			{
				format = (unsigned)eof_psarc_stick_le16(fmt);
				channels = (unsigned)eof_psarc_stick_le16(fmt + 2);
				rate = eof_psarc_stick_le32(fmt + 4);
				bits = (unsigned)eof_psarc_stick_le16(fmt + 14);
			}
		}
		else if(!memcmp(chunk, "data", 4))
		{
			data_pos = (unsigned long)ftell(fp);
			data_size = chunk_size;
			fseek(fp, (long)chunk_size, SEEK_CUR);
		}
		else
		{
			fseek(fp, (long)chunk_size, SEEK_CUR);
		}
		if(chunk_size & 1UL)
			fseek(fp, 1L, SEEK_CUR);
		if(data_pos && format)
			break;
	}

	if(format != 1U || (channels != 1U && channels != 2U) || bits != 16U || !rate || !data_pos || data_size < channels * 2U)
	{
		fclose(fp);
		return 0;
	}
	samples = (short *)malloc(data_size);
	if(!samples)
	{
		fclose(fp);
		return 0;
	}
	fseek(fp, (long)data_pos, SEEK_SET);
	if(fread(samples, 1, data_size, fp) != data_size)
	{
		free(samples);
		fclose(fp);
		return 0;
	}
	fclose(fp);

	pcm->channels = channels;
	pcm->bits = bits;
	pcm->rate = rate;
	pcm->frames = data_size / (channels * 2UL);
	pcm->samples = samples;
	return 1;
}

static void eof_psarc_stick_free_wav(EOF_PSARC_STICK_PCM *pcm)
{
	if(!pcm) return;
	free(pcm->samples);
	memset(pcm, 0, sizeof(*pcm));
}

static int eof_psarc_stick_mix_into_prefix(const char *wav, unsigned long prefix_ms)
{
	char sample_path[1024] = {0};
	EOF_PSARC_STICK_PCM stick;
	FILE *fp = NULL;
	unsigned char header[44];
	unsigned long rate, prefix_frames, beat_frames, dest_frames;
	unsigned num = 4, den = 4, click;
	double quarter_ms, beat_ms, bpm;
	short *mix = NULL;
	int ok = 0;

	if(!wav || !prefix_ms || !eof_psarc_stick_find_sample(sample_path, sizeof(sample_path)))
	{
		eof_log("PSARC stick count-in: bundled stick_countin.wav was not found.", 1);
		return 0;
	}
	if(!eof_psarc_stick_load_wav(sample_path, &stick))
	{
		eof_log("PSARC stick count-in: bundled WAV is not readable PCM16 audio.", 1);
		return 0;
	}

	fp = fopen(wav, "rb+");
	if(!fp || fread(header, 1, sizeof(header), fp) != sizeof(header) ||
	   memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4) ||
	   eof_psarc_stick_le16(header + 22) != 2UL || eof_psarc_stick_le16(header + 34) != 16UL)
		goto cleanup;
	rate = eof_psarc_stick_le32(header + 24);
	if(!rate)
		goto cleanup;

	prefix_frames = (unsigned long)(((double)prefix_ms * (double)rate) / 1000.0 + 0.5);
	if(!prefix_frames)
		goto cleanup;
	mix = (short *)calloc(prefix_frames * 2UL, sizeof(short));
	if(!mix)
		goto cleanup;

	eof_psarc_stick_predominant_meter(eof_song, &num, &den);
	quarter_ms = eof_psarc_stick_initial_quarter_ms(eof_song);
	beat_ms = quarter_ms * 4.0 / (double)(den ? den : 4U);
	beat_frames = (unsigned long)(((double)rate * beat_ms) / 1000.0 + 0.5);
	if(!beat_frames) beat_frames = 1UL;

	/* Resample only this tiny click sample by nearest frame.  It is intentionally
	 * short, so this is cheaper and more reliable than invoking a converter. */
	dest_frames = (unsigned long)(((double)stick.frames * (double)rate) / (double)stick.rate + 0.5);
	for(click = 0; click < num; click++)
	{
		unsigned long start = (unsigned long)click * beat_frames;
		unsigned long j;
		if(start >= prefix_frames)
			break;
		for(j = 0; j < dest_frames && start + j < prefix_frames; j++)
		{
			unsigned long src = (unsigned long)(((double)j * (double)stick.rate) / (double)rate);
			long left, right;
			long dl, dr;
			if(src >= stick.frames) src = stick.frames - 1UL;
			if(stick.channels == 1U)
				left = right = stick.samples[src];
			else
			{
				left = stick.samples[src * 2UL];
				right = stick.samples[src * 2UL + 1UL];
			}
			dl = (long)mix[(start + j) * 2UL] + left;
			dr = (long)mix[(start + j) * 2UL + 1UL] + right;
			if(dl > 32767L) dl = 32767L; else if(dl < -32768L) dl = -32768L;
			if(dr > 32767L) dr = 32767L; else if(dr < -32768L) dr = -32768L;
			mix[(start + j) * 2UL] = (short)dl;
			mix[(start + j) * 2UL + 1UL] = (short)dr;
		}
	}

	fseek(fp, 44L, SEEK_SET);
	if(fwrite(mix, sizeof(short), prefix_frames * 2UL, fp) != prefix_frames * 2UL)
		goto cleanup;
	ok = 1;
	bpm = quarter_ms > 0.0 ? 60000.0 / quarter_ms : 120.0;
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"PSARC stick count-in: bundled sample, initial BPM %.2f, predominant meter %u/%u, %u clicks, %lu ms",
		bpm, num, den, num, prefix_ms);
	eof_log(eof_log_string, 1);

cleanup:
	if(fp) fclose(fp);
	free(mix);
	eof_psarc_stick_free_wav(&stick);
	if(!ok)
		eof_log("PSARC stick count-in: could not splice bundled click sample into staged WAV.", 1);
	return ok;
}

static int eof_psarc_stick_prepare_placeholder(const char *command)
{
	char placeholder[1024] = {0}, sample[1024] = {0};
	if(!eof_psarc_get_quoted_arg_compat(command, 3, placeholder, sizeof(placeholder)) ||
	   !eof_psarc_stick_find_sample(sample, sizeof(sample)))
		return -1;
	return eof_copy_file(sample, placeholder) ? 0 : -1;
}

static int eof_psarc_stick_audio_command(const char *command)
{
	char source[1024] = {0}, output[1024] = {0};
	unsigned long prefix_ms;
	int result;

	if(!eof_psarc_get_input_arg_compat(command, 1, source, sizeof(source)) ||
	   !eof_psarc_get_last_quoted_arg_compat(command, output, sizeof(output)))
	{
		eof_log("PSARC stick count-in: could not parse source/output paths.", 1);
		return -1;
	}
	prefix_ms = eof_psarc_stick_measure_ms(eof_song, NULL, NULL);

	/* First create the same canonical staged WAV as the silence path, then write
	 * the bundled clicks into its silent prefix. */
	result = eof_psarc_internal_ogg_to_wav_compat(source, output, prefix_ms, 0, 120) ? 0 : -1;
	if(result != 0)
		return result;
	if(!eof_psarc_stick_mix_into_prefix(output, prefix_ms))
		return -1;

	if(eof_audio_normalize_psarc_checked)
	{
		if(!eof_audio_normalize_wav_in_place(output, eof_audio_normalize_target_value()))
		{
			eof_log("PSARC normalize: loudness normalization of stick-count staged WAV failed.", 1);
			return -1;
		}
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"PSARC staged stick-count WAV normalized to %.2f LUFS: %.800s",
			eof_audio_normalize_target_value(), output);
		eof_log(eof_log_string, 1);
	}
	return 0;
}

static int eof_psarc_stick_system_compat(const char *command)
{
	if(!command)
		return -1;
	if(strstr(command, "\"" EOF_PSARC_INTERNAL_FLUIDSYNTH "\"") == command)
		return eof_psarc_stick_prepare_placeholder(command);
	if(strstr(command, "\"" EOF_PSARC_INTERNAL_AUDIO "\"") == command &&
	   strstr(command, "atrim=duration="))
		return eof_psarc_stick_audio_command(command);
	return eof_audio_normalize_psarc_system_compat(command);
}

static int eof_psarc_stick_popup_compat(DIALOG *dialog, int focus)
{
	if(dialog && dialog[0].dp && !strcmp((const char *)dialog[0].dp, "Export Rocksmith 2014 PSARC"))
	{
		/* Keep the UI aligned with the new deterministic sample backend. */
		dialog[20].dp = (void *)"Silence ms:";
		dialog[EOF_PSARC_STICK_DIALOG_INDEX].dp = (void *)"Stick count-in at initial BPM";
	}
	return eof_psarc_popup_dialog(dialog, focus);
}

#define system(command) eof_psarc_stick_system_compat((command))
#define eof_popup_dialog(dialog, focus) eof_psarc_stick_popup_compat((dialog), (focus))

/* psarc_export.c has exactly one strtoul() use: conversion of the intro target.
 * For stick mode, feed the legacy subtraction logic an absolute target that
 * produces one full measure of new prefix.  Silence mode keeps the user's value. */
#define strtoul(text, end, base) eof_psarc_stick_intro_strtoul((text), (end), (base), \
	((eof_psarc_dialog[EOF_PSARC_STICK_DIALOG_INDEX].flags & D_SELECTED) ? 1 : 0))

#endif

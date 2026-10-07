#ifndef EOF_PSARC_FLUIDSYNTH_COUNTIN_HOOK_H
#define EOF_PSARC_FLUIDSYNTH_COUNTIN_HOOK_H

/*
 * PSARC count-in V8
 * -----------------
 * Prefer a real GM side-stick rendered by the same FluidSynth/SoundFont pair
 * configured for EOF's Guitar Pro audio workflow.  The old bundled WAV remains
 * a fallback only, so a missing/broken FluidSynth setup does not make PSARC
 * export unusable.
 *
 * V8 writes the count-in MIDI from the predominant time-signature numerator,
 * not from a rounded millisecond duration.  The old duration/ceil calculation
 * could turn a 4/4 measure into five hits when floating-point rounding made the
 * measured duration fractionally longer than four beats.
 *
 * This header is force-included after psarc_stick_sample_v2_hook.h.  That lets
 * us reuse its already-tested timing/WAV helpers while replacing only the call
 * made by psarc_export.c.  The complete FluidSynth render (including the final
 * stick's decay) is mixed into the lead-in; unlike the short bundled transient,
 * it is not cut at the song downbeat.
 */

#include "gp_synth_process_hook.h"

/* Defined later in psarc_export.c.  Static prototype is intentional because
 * this hook is part of the same translation unit. */
static int eof_psarc_prompt_path(const char *section, const char *key, const char *title, const char *ext, char *out, size_t outsz);

static void eof_psarc_fs_put_be32(FILE *fp, unsigned long value)
{
	fputc((int)((value >> 24) & 0xFFUL), fp);
	fputc((int)((value >> 16) & 0xFFUL), fp);
	fputc((int)((value >> 8) & 0xFFUL), fp);
	fputc((int)(value & 0xFFUL), fp);
}

static int eof_psarc_fs_append_byte(unsigned char *buffer, size_t capacity, unsigned long *pos, unsigned char value)
{
	if(!buffer || !pos || *pos >= capacity)
		return 0;
	buffer[(*pos)++] = value;
	return 1;
}

static int eof_psarc_fs_append_vlq(unsigned char *buffer, size_t capacity, unsigned long *pos, unsigned long value)
{
	unsigned char tmp[5];
	int n = 0, i;
	tmp[n++] = (unsigned char)(value & 0x7FUL);
	while((value >>= 7) != 0UL)
		tmp[n++] = (unsigned char)((value & 0x7FUL) | 0x80U);
	for(i = n - 1; i >= 0; i--)
		if(!eof_psarc_fs_append_byte(buffer, capacity, pos, tmp[i]))
			return 0;
	return 1;
}

static int eof_psarc_fs_write_exact_click_midi(const char *path, int bpm, unsigned num, unsigned den)
{
	unsigned char trackdata[4096];
	unsigned long pos = 0, mpqn, beat_ticks, note_ticks, gap_ticks;
	unsigned i, den_power = 0, den_check = 1;
	FILE *fp;

	if(!path || bpm <= 0)
		return 0;
	if(num < 1U) num = 4U;
	if(num > 32U) num = 32U;
	if(!den) den = 4U;
	while(den_check < den && den_power < 7U)
	{
		den_check <<= 1;
		den_power++;
	}
	if(den_check != den)
	{
		den = 4U;
		den_power = 2U;
	}

	mpqn = (unsigned long)(60000000.0 / (double)bpm + 0.5);
	beat_ticks = (480UL * 4UL) / (unsigned long)den;
	if(!beat_ticks) beat_ticks = 1UL;
	note_ticks = beat_ticks / 8UL;
	if(note_ticks < 1UL) note_ticks = 1UL;
	if(note_ticks > 60UL) note_ticks = 60UL;
	gap_ticks = (beat_ticks > note_ticks) ? beat_ticks - note_ticks : 0UL;

#define FS_MIDI_BYTE(v) do { if(!eof_psarc_fs_append_byte(trackdata, sizeof(trackdata), &pos, (unsigned char)(v))) return 0; } while(0)
	/* Exact meter metadata. */
	FS_MIDI_BYTE(0); FS_MIDI_BYTE(0xFF); FS_MIDI_BYTE(0x58); FS_MIDI_BYTE(4);
	FS_MIDI_BYTE(num); FS_MIDI_BYTE(den_power); FS_MIDI_BYTE(24); FS_MIDI_BYTE(8);
	/* Quarter-note tempo. */
	FS_MIDI_BYTE(0); FS_MIDI_BYTE(0xFF); FS_MIDI_BYTE(0x51); FS_MIDI_BYTE(3);
	FS_MIDI_BYTE((mpqn >> 16) & 0xFFUL); FS_MIDI_BYTE((mpqn >> 8) & 0xFFUL); FS_MIDI_BYTE(mpqn & 0xFFUL);

	for(i = 0; i < num; i++)
	{
		if(!eof_psarc_fs_append_vlq(trackdata, sizeof(trackdata), &pos, (i == 0U) ? 0UL : gap_ticks)) return 0;
		FS_MIDI_BYTE(0x99); FS_MIDI_BYTE(37); FS_MIDI_BYTE(112);
		if(!eof_psarc_fs_append_vlq(trackdata, sizeof(trackdata), &pos, note_ticks)) return 0;
		FS_MIDI_BYTE(0x89); FS_MIDI_BYTE(37); FS_MIDI_BYTE(0);
	}
	/* Leave the remainder of the last meter beat for FluidSynth's natural tail,
	 * but never create an extra note on the following downbeat. */
	if(!eof_psarc_fs_append_vlq(trackdata, sizeof(trackdata), &pos, gap_ticks)) return 0;
	FS_MIDI_BYTE(0xFF); FS_MIDI_BYTE(0x2F); FS_MIDI_BYTE(0);
#undef FS_MIDI_BYTE

	fp = fopen(path, "wb");
	if(!fp) return 0;
	fwrite("MThd", 1, 4, fp);
	eof_psarc_fs_put_be32(fp, 6UL);
	fputc(0, fp); fputc(0, fp);
	fputc(0, fp); fputc(1, fp);
	fputc(1, fp); fputc(0xE0, fp); /* PPQN = 480 */
	fwrite("MTrk", 1, 4, fp);
	eof_psarc_fs_put_be32(fp, pos);
	fwrite(trackdata, 1, pos, fp);
	fclose(fp);
	return 1;
}

static int eof_psarc_fluidsynth_mix_wav(const char *music_wav, const char *click_wav, unsigned long prefix_ms)
{
	EOF_PSARC_STICK_PCM click;
	FILE *fp = NULL;
	unsigned char header[44];
	unsigned long rate, data_bytes, total_frames, start_frame, click_out_frames, j;
	unsigned num = 4, den = 4;
	double quarter_ms, beat_ms, measure_ms, downbeat_ms, start_ms;
	short *music = NULL;
	int ok = 0;

	memset(&click, 0, sizeof(click));
	if(!music_wav || !click_wav || !eof_psarc_stick_load_wav(click_wav, &click))
	{
		eof_log("PSARC FluidSynth count-in: rendered click WAV is not readable PCM16 audio.", 1);
		return 0;
	}

	fp = fopen(music_wav, "rb+");
	if(!fp || fread(header, 1, sizeof(header), fp) != sizeof(header) ||
	   memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4) ||
	   eof_psarc_stick_le16(header + 20) != 1UL ||
	   eof_psarc_stick_le16(header + 22) != 2UL ||
	   eof_psarc_stick_le16(header + 34) != 16UL ||
	   memcmp(header + 36, "data", 4))
		goto cleanup;

	rate = eof_psarc_stick_le32(header + 24);
	data_bytes = eof_psarc_stick_le32(header + 40);
	if(!rate || data_bytes < 4UL)
		goto cleanup;
	total_frames = data_bytes / 4UL;

	eof_psarc_stick_predominant_meter(eof_song, &num, &den);
	quarter_ms = eof_psarc_stick_initial_quarter_ms(eof_song);
	if(!den) den = 4;
	beat_ms = quarter_ms * 4.0 / (double)den;
	measure_ms = beat_ms * (double)num;
	downbeat_ms = (double)prefix_ms;
	if(eof_song && eof_song->beats && eof_song->beat[0])
		downbeat_ms += (double)eof_song->beat[0]->pos;
	start_ms = (downbeat_ms > measure_ms) ? downbeat_ms - measure_ms : 0.0;
	start_frame = (unsigned long)(start_ms * (double)rate / 1000.0 + 0.5);
	if(start_frame >= total_frames)
		goto cleanup;

	music = (short *)malloc((size_t)total_frames * 2U * sizeof(short));
	if(!music)
		goto cleanup;
	if(fseek(fp, 44L, SEEK_SET))
		goto cleanup;
	if(fread(music, sizeof(short), total_frames * 2UL, fp) != total_frames * 2UL)
		goto cleanup;

	click_out_frames = (unsigned long)((double)click.frames * (double)rate / (double)click.rate + 0.5);
	if(!click_out_frames)
		goto cleanup;

	/* Mix the whole FluidSynth render, including the tail after the last stick.
	 * That small natural overlap is deliberate: clipping at the downbeat was the
	 * audible defect of the sampled-count-in implementation. */
	for(j = 0; j < click_out_frames && start_frame + j < total_frames; j++)
	{
		unsigned long src = (unsigned long)((double)j * (double)click.rate / (double)rate);
		long sl, sr, dl, dr;
		if(src >= click.frames)
			break;
		if(click.channels == 1U)
			sl = sr = click.samples[src];
		else
		{
			sl = click.samples[src * 2UL];
			sr = click.samples[src * 2UL + 1UL];
		}
		dl = (long)music[(start_frame + j) * 2UL] + sl;
		dr = (long)music[(start_frame + j) * 2UL + 1UL] + sr;
		if(dl > 32767L) dl = 32767L; else if(dl < -32768L) dl = -32768L;
		if(dr > 32767L) dr = 32767L; else if(dr < -32768L) dr = -32768L;
		music[(start_frame + j) * 2UL] = (short)dl;
		music[(start_frame + j) * 2UL + 1UL] = (short)dr;
	}

	if(fseek(fp, 44L, SEEK_SET))
		goto cleanup;
	if(fwrite(music, sizeof(short), total_frames * 2UL, fp) != total_frames * 2UL)
		goto cleanup;
	ok = 1;
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"PSARC FluidSynth count-in V8: mixed full rendered side-stick WAV at %.1f ms; song downbeat %.1f ms",
		start_ms, downbeat_ms);
	eof_log(eof_log_string, 1);

cleanup:
	if(fp) fclose(fp);
	free(music);
	eof_psarc_stick_free_wav(&click);
	if(!ok)
		eof_log("PSARC FluidSynth count-in V8: failed while mixing rendered click WAV.", 1);
	return ok;
}

static int eof_psarc_fluidsynth_countin(const char *music_wav, unsigned long prefix_ms)
{
	char dir[1024] = {0}, midi[1024] = {0}, clickwav[1024] = {0};
	char fluid[1024] = {0}, soundfont[1024] = {0};
	const char *saved;
	wchar_t wfluid[2048] = {0}, wsf[2048] = {0}, wmidi[2048] = {0}, wwav[2048] = {0};
	const wchar_t *argv[13] = {0};
	unsigned long measure_ms;
	unsigned num = 4, den = 4;
	double quarter_ms;
	int bpm;
	intptr_t result;

	if(!music_wav || !music_wav[0])
		return 0;

	ustrzcpy(dir, sizeof(dir), music_wav);
	*get_filename(dir) = '\0';
	(void)snprintf(midi, sizeof(midi) - 1, "%spsarc_countin.mid", dir);
	(void)snprintf(clickwav, sizeof(clickwav) - 1, "%spsarc_countin.wav", dir);
	delete_file(midi);
	delete_file(clickwav);

	saved = get_config_string("gp_synth", "fluidsynth_path", "");
	if(saved && saved[0] && exists(saved))
		ustrzcpy(fluid, sizeof(fluid), saved);
	else if(!eof_psarc_prompt_path("gp_synth", "fluidsynth_path", "Locate fluidsynth.exe for PSARC count-in", "exe", fluid, sizeof(fluid)))
		return 0;

	saved = get_config_string("gp_synth", "soundfont_path", "");
	if(saved && saved[0] && exists(saved))
		ustrzcpy(soundfont, sizeof(soundfont), saved);
	else if(!eof_psarc_prompt_path("gp_synth", "soundfont_path", "Locate GM SoundFont for PSARC count-in", "sf2;sf3", soundfont, sizeof(soundfont)))
		return 0;

	quarter_ms = eof_psarc_stick_initial_quarter_ms(eof_song);
	bpm = (quarter_ms > 0.0) ? (int)(60000.0 / quarter_ms + 0.5) : 120;
	if(bpm < 20) bpm = 20;
	if(bpm > 400) bpm = 400;
	eof_psarc_stick_predominant_meter(eof_song, &num, &den);
	measure_ms = eof_psarc_stick_measure_ms(eof_song, NULL, NULL);
	if(!eof_psarc_fs_write_exact_click_midi(midi, bpm, num, den))
	{
		eof_log("PSARC FluidSynth count-in V8: could not write exact-meter side-stick MIDI.", 1);
		return 0;
	}

	(void)uconvert(fluid, U_UTF8, (char *)wfluid, U_UNICODE, sizeof(wfluid));
	(void)uconvert(soundfont, U_UTF8, (char *)wsf, U_UNICODE, sizeof(wsf));
	(void)uconvert(midi, U_UTF8, (char *)wmidi, U_UNICODE, sizeof(wmidi));
	(void)uconvert(clickwav, U_UTF8, (char *)wwav, U_UNICODE, sizeof(wwav));
	argv[9] = wwav;
	argv[10] = wsf;
	argv[11] = wmidi;

	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"PSARC FluidSynth count-in V8: rendering GM side stick, BPM %d, predominant %u/%u, exact hits %u, measure %lu ms",
		bpm, num, den, num, measure_ms);
	eof_log(eof_log_string, 1);
#ifdef _WIN32
	result = gp_synth_raw_spawnv(_P_WAIT, wfluid, argv);
#else
	/* PSARC export is Windows-only.  Keep the Linux translation unit buildable
	 * and let the already existing bundled-sample fallback handle this path. */
	result = (intptr_t)-1;
#endif
	if(result != 0 || !exists(clickwav) || file_size_ex(clickwav) <= 44)
	{
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"PSARC FluidSynth count-in V8: renderer failed (%ld); using bundled sample fallback",
			(long)result);
		eof_log(eof_log_string, 1);
		return 0;
	}

	return eof_psarc_fluidsynth_mix_wav(music_wav, clickwav, prefix_ms);
}

static int eof_psarc_fluidsynth_or_sample_countin(const char *music_wav, unsigned long prefix_ms)
{
	if(eof_psarc_fluidsynth_countin(music_wav, prefix_ms))
		return 1;
	eof_log("PSARC count-in V8: FluidSynth unavailable/failed; falling back to bundled drumstick.wav.", 1);
	return eof_psarc_stick_v2_mix_to_downbeat(music_wav, prefix_ms);
}

/* Replace only psarc_export.c's later call.  The original V2 function above is
 * already compiled and is still directly callable by the fallback worker. */
#define eof_psarc_stick_v2_mix_to_downbeat(wav, prefix_ms) \
	eof_psarc_fluidsynth_or_sample_countin((wav), (prefix_ms))

#endif

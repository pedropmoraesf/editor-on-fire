#ifndef EOF_PSARC_STICK_SAMPLE_V2_HOOK_H
#define EOF_PSARC_STICK_SAMPLE_V2_HOOK_H

/*
 * Refine the bundled-stick backend defined by psarc_stick_sample_hook.h.
 *
 * Keep the user's normal "Target ms" lead-in calculation.  The count-in is
 * mixed into the complete pre-song region so its final measure ends exactly on
 * EOF's first chart beat.  This avoids adding a full measure and then leaving
 * the project's original pre-roll as an extra silent gap.
 */

#ifdef system
#undef system
#endif

/* psarc_stick_sample_hook.h used this macro to force a full-measure prefix.
 * V2 intentionally restores the real strtoul() so the existing target-ms logic
 * remains authoritative and XML/audio shifting stays unchanged. */
#ifdef strtoul
#undef strtoul
#endif

#define EOF_PSARC_DRUMSTICK_SAMPLE_NAME "drumstick.wav"

static int eof_psarc_stick_v2_find_sample(char *out, size_t outsz)
{
	char exe[1024] = {0}, dir[1024] = {0}, candidate[1024] = {0};
	const char *relative[] = {
		"tools/psarc/" EOF_PSARC_DRUMSTICK_SAMPLE_NAME,
		"../tools/psarc/" EOF_PSARC_DRUMSTICK_SAMPLE_NAME
	};
	unsigned i;

	if(!out || !outsz) return 0;
	out[0] = '\0';
	get_executable_name(exe, sizeof(exe));
	ustrzcpy(dir, sizeof(dir), exe);
	*get_filename(dir) = '\0';
	for(i = 0; i < sizeof(relative) / sizeof(relative[0]); i++)
	{
		(void)snprintf(candidate, sizeof(candidate) - 1, "%s%s", dir, relative[i]);
		fix_filename_slashes(candidate);
		if(exists(candidate))
		{
			ustrzcpy(out, (int)outsz, candidate);
			return 1;
		}
	}
	for(i = 0; i < sizeof(relative) / sizeof(relative[0]); i++)
	{
		ustrzcpy(candidate, sizeof(candidate), relative[i]);
		fix_filename_slashes(candidate);
		if(exists(candidate))
		{
			ustrzcpy(out, (int)outsz, candidate);
			return 1;
		}
	}
	return 0;
}

static int eof_psarc_stick_v2_mix_to_downbeat(const char *wav, unsigned long prefix_ms)
{
	char sample_path[1024] = {0};
	EOF_PSARC_STICK_PCM stick;
	FILE *fp = NULL;
	unsigned char header[44];
	unsigned long rate, data_bytes, total_frames, lead_frames, beat_frames, sample_out_frames;
	unsigned long click, j;
	unsigned num = 4, den = 4;
	double quarter_ms, beat_ms, measure_ms, downbeat_ms, start_ms, bpm;
	short *lead = NULL;
	int ok = 0;

	memset(&stick, 0, sizeof(stick));
	if(!wav || !eof_psarc_stick_v2_find_sample(sample_path, sizeof(sample_path)))
	{
		eof_log("PSARC stick count-in: tools/psarc/drumstick.wav was not found.", 1);
		return 0;
	}
	if(!eof_psarc_stick_load_wav(sample_path, &stick))
	{
		eof_log("PSARC stick count-in: drumstick.wav is not readable PCM16 audio.", 1);
		return 0;
	}

	fp = fopen(wav, "rb+");
	if(!fp || fread(header, 1, sizeof(header), fp) != sizeof(header) ||
	   memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4) ||
	   eof_psarc_stick_le16(header + 20) != 1UL ||
	   eof_psarc_stick_le16(header + 22) != 2UL ||
	   eof_psarc_stick_le16(header + 34) != 16UL ||
	   memcmp(header + 36, "data", 4))
		goto cleanup;

	rate = eof_psarc_stick_le32(header + 24);
	data_bytes = eof_psarc_stick_le32(header + 40);
	if(!rate || data_bytes < 4UL) goto cleanup;
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
	beat_frames = (unsigned long)(beat_ms * (double)rate / 1000.0 + 0.5);
	if(!beat_frames) beat_frames = 1UL;
	lead_frames = (unsigned long)(downbeat_ms * (double)rate / 1000.0 + 0.5);
	if(lead_frames > total_frames) lead_frames = total_frames;
	if(!lead_frames) goto cleanup;

	lead = (short *)malloc((size_t)lead_frames * 2U * sizeof(short));
	if(!lead) goto cleanup;
	if(fseek(fp, 44L, SEEK_SET)) goto cleanup;
	if(fread(lead, sizeof(short), lead_frames * 2UL, fp) != lead_frames * 2UL) goto cleanup;

	sample_out_frames = (unsigned long)((double)stick.frames * (double)rate / (double)stick.rate + 0.5);
	if(!sample_out_frames) goto cleanup;
	for(click = 0; click < (unsigned long)num; click++)
	{
		double hit_ms = start_ms + (double)click * beat_ms;
		unsigned long start = (unsigned long)(hit_ms * (double)rate / 1000.0 + 0.5);
		if(start >= lead_frames) break;
		for(j = 0; j < sample_out_frames && start + j < lead_frames; j++)
		{
			unsigned long src = (unsigned long)((double)j * (double)stick.rate / (double)rate);
			long sl, sr, dl, dr;
			if(src >= stick.frames) break;
			if(stick.channels == 1U)
				sl = sr = stick.samples[src];
			else
			{
				sl = stick.samples[src * 2UL];
				sr = stick.samples[src * 2UL + 1UL];
			}
			dl = (long)lead[(start + j) * 2UL] + sl;
			dr = (long)lead[(start + j) * 2UL + 1UL] + sr;
			if(dl > 32767L) dl = 32767L; else if(dl < -32768L) dl = -32768L;
			if(dr > 32767L) dr = 32767L; else if(dr < -32768L) dr = -32768L;
			lead[(start + j) * 2UL] = (short)dl;
			lead[(start + j) * 2UL + 1UL] = (short)dr;
		}
	}

	if(fseek(fp, 44L, SEEK_SET)) goto cleanup;
	if(fwrite(lead, sizeof(short), lead_frames * 2UL, fp) != lead_frames * 2UL) goto cleanup;
	ok = 1;
	bpm = quarter_ms > 0.0 ? 60000.0 / quarter_ms : 120.0;
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"PSARC stick count-in V2: drumstick.wav, initial BPM %.2f, predominant %u/%u, count-in %.1f..%.1f ms, prefix %lu ms",
		bpm, num, den, start_ms, downbeat_ms, prefix_ms);
	eof_log(eof_log_string, 1);

cleanup:
	if(fp) fclose(fp);
	free(lead);
	eof_psarc_stick_free_wav(&stick);
	if(!ok) eof_log("PSARC stick count-in V2: failed while mixing drumstick.wav into staged WAV.", 1);
	return ok;
}

static int eof_psarc_stick_v2_placeholder(const char *command)
{
	char placeholder[1024] = {0}, sample[1024] = {0};
	FILE *fp;
	if(!eof_psarc_get_quoted_arg_compat(command, 3, placeholder, sizeof(placeholder)) &&
	   !eof_psarc_get_last_quoted_arg_compat(command, placeholder, sizeof(placeholder)))
		return -1;
	if(!eof_psarc_stick_v2_find_sample(sample, sizeof(sample))) return -1;
	/* The legacy caller only tests for existence.  Do not copy/convert the real
	 * sample here; create a tiny marker and use drumstick.wav directly below. */
	fp = fopen(placeholder, "wb");
	if(!fp) return -1;
	fwrite("EOF drumstick sample", 1, 20, fp);
	fclose(fp);
	return 0;
}

static int eof_psarc_stick_v2_audio_command(const char *command)
{
	char output[1024] = {0};
	const char *duration;
	unsigned long prefix_ms = 0UL;
	double seconds;

	if(!command) return -1;
	duration = strstr(command, "atrim=duration=");
	if(duration)
	{
		seconds = strtod(duration + strlen("atrim=duration="), NULL);
		if(seconds > 0.0) prefix_ms = (unsigned long)(seconds * 1000.0 + 0.5);
	}
	if(!eof_psarc_get_last_quoted_arg_compat(command, output, sizeof(output)))
	{
		eof_log("PSARC stick count-in V2: could not determine staged WAV output.", 1);
		return -1;
	}
	if(!eof_psarc_internal_ogg_to_wav_compat(eof_loaded_ogg_name, output, prefix_ms, 0, 120))
		return -1;
	if(!eof_psarc_stick_v2_mix_to_downbeat(output, prefix_ms))
	{
		remove(output);
		return -1;
	}

	/* Pipeline V3: the internal decoder plus drumstick mixer have already made
	 * the exact PCM16 WAV Wwise needs.  Do not send this intermediate file back
	 * through FFmpeg loudnorm.  On this path loudnorm is optional post-processing
	 * and must never be able to invalidate an otherwise valid PSARC export. */
	if(eof_audio_normalize_psarc_checked)
		eof_log("PSARC audio pipeline V3: sampled count-in WAV ready; skipping pre-Wwise FFmpeg loudnorm.", 1);
	if(!exists(output))
	{
		eof_log("PSARC audio pipeline V3: staged WAV disappeared after count-in mixing.", 1);
		return -1;
	}
	return 0;
}

static int eof_psarc_stick_v2_system_compat(const char *command)
{
	if(!command) return -1;
	if(strstr(command, "\"" EOF_PSARC_INTERNAL_FLUIDSYNTH "\"") == command)
		return eof_psarc_stick_v2_placeholder(command);
	if(strstr(command, "\"" EOF_PSARC_INTERNAL_AUDIO "\"") == command && strstr(command, "atrim=duration="))
		return eof_psarc_stick_v2_audio_command(command);
	return eof_audio_normalize_psarc_system_compat(command);
}

#define system(command) eof_psarc_stick_v2_system_compat((command))

#endif

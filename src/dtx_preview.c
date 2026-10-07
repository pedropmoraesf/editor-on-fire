#include <allegro.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alogg.h>

#include "main.h"
#include "utility.h"
#include "dtx_preview.h"

#define EOF_DTX_PREVIEW_SECONDS 25.0
#define EOF_DTX_PREVIEW_STEP_SECONDS 5.0
#define EOF_DTX_PREVIEW_FADE_SECONDS 2.0
#define EOF_DTX_PREVIEW_BUFFER_SAMPLES 65536

typedef struct
{
	void *data;
	ALOGG_OGG *ogg;
	unsigned long duration_ms;
	unsigned long sample_rate;
	unsigned channels;
} EOF_DTX_PREVIEW_SOURCE;

typedef struct
{
	double sumsq;
	unsigned long samples;
} EOF_DTX_PREVIEW_ENERGY;

typedef struct
{
	FILE *fp;
	unsigned long sample_rate;
	unsigned channels;
	unsigned long total_frames;
	unsigned long written_samples;
	int failed;
} EOF_DTX_PREVIEW_WRITER;

static EOF_DTX_PREVIEW_ENERGY *eof_dtx_preview_energy_ctx = NULL;
static EOF_DTX_PREVIEW_WRITER *eof_dtx_preview_writer_ctx = NULL;

static int dtx_preview_copy_quoted(const char *start, char *out, size_t outsize)
{
	const char *end;
	size_t len;
	if(!start || !out || (outsize < 2))
		return 0;
	end = strchr(start, '"');
	if(!end)
		return 0;
	len = (size_t)(end - start);
	if(len >= outsize)
		return 0;
	memcpy(out, start, len);
	out[len] = '\0';
	return 1;
}

static int dtx_preview_extract_paths(const char *command, char *ffmpeg, size_t ffsize, char *audio, size_t audiosize, char *dest, size_t destsize)
{
	const char *p, *last, *prev;
	if(!command || (command[0] != '"'))
		return 0;
	if(!dtx_preview_copy_quoted(command + 1, ffmpeg, ffsize))
		return 0;
	p = strstr(command, " -i \"");
	if(!p || !dtx_preview_copy_quoted(p + 5, audio, audiosize))
		return 0;
	last = strrchr(command, '"');
	if(!last || (last == command))
		return 0;
	prev = last - 1;
	while((prev > command) && (*prev != '"'))
		prev--;
	if(*prev != '"')
		return 0;
	if((size_t)(last - prev - 1) >= destsize)
		return 0;
	memcpy(dest, prev + 1, (size_t)(last - prev - 1));
	dest[last - prev - 1] = '\0';
	return 1;
}

static int dtx_preview_bundled_tool(const char *name, char *out, size_t outsize)
{
	char executable[2048] = {0};
	char folder[2048] = {0};
	if(!name || !out || !outsize)
		return 0;
	out[0] = '\0';
	get_executable_name(executable, sizeof(executable));
	if(!executable[0])
		return 0;
	replace_filename(folder, executable, "", sizeof(folder));
	replace_filename(out, folder, name, (int)outsize);
	if(exists(out))
		return 1;
	out[0] = '\0';
	return 0;
}

static int dtx_preview_is_ffmpeg_token(const char *token)
{
	const char *name;
	if(!token || !token[0])
		return 0;
	name = get_filename(token);
	return (!ustricmp(name, "ffmpeg") || !ustricmp(name, "ffmpeg.exe"));
}

/* Keep the rest of the DTX exporter unchanged.  Its loudness-normalization
 * commands may still use FFmpeg, but if ffmpeg.exe is installed beside eof.exe
 * force those calls to that exact binary.  Preview generation itself no longer
 * depends on FFmpeg at all. */
static int dtx_preview_run_system(const char *command)
{
	char token[2048] = {0}, local[2048] = {0}, rewritten[8192] = {0};
	const char *tail = NULL;
	int result;

	if(!command)
		return -1;
	if(command[0] == '"')
	{
		const char *end = strchr(command + 1, '"');
		size_t len;
		if(!end)
			return eof_system(command);
		len = (size_t)(end - command - 1);
		if(len >= sizeof(token)) len = sizeof(token) - 1;
		memcpy(token, command + 1, len);
		token[len] = '\0';
		tail = end + 1;
	}
	else
	{
		const char *end = strchr(command, ' ');
		size_t len = end ? (size_t)(end - command) : strlen(command);
		if(len >= sizeof(token)) len = sizeof(token) - 1;
		memcpy(token, command, len);
		token[len] = '\0';
		tail = end ? end : command + strlen(command);
	}

	if(dtx_preview_is_ffmpeg_token(token) && dtx_preview_bundled_tool("ffmpeg.exe", local, sizeof(local)))
	{
		snprintf(rewritten, sizeof(rewritten), "\"%s\"%s", local, tail ? tail : "");
		result = eof_system(rewritten);
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"DTX audio normalization: bundled FFmpeg returned %d", result);
		eof_log(eof_log_string, 1);
		return result;
	}
	return eof_system(command);
}

static int dtx_preview_open_source(const char *filename, EOF_DTX_PREVIEW_SOURCE *src)
{
	long long size;
	if(!filename || !src || !exists(filename))
		return 0;
	memset(src, 0, sizeof(*src));
	size = file_size_ex(filename);
	if((size <= 0) || (size > INT_MAX))
		return 0;
	src->data = eof_buffer_file(filename, 0, 0);
	if(!src->data)
		return 0;
	src->ogg = alogg_create_ogg_from_buffer(src->data, (int)size);
	if(!src->ogg)
	{
		free(src->data);
		src->data = NULL;
		return 0;
	}
	src->duration_ms = alogg_get_length_msecs_ogg_ul(src->ogg);
	src->sample_rate = (unsigned long)alogg_get_wave_freq_ogg(src->ogg);
	src->channels = alogg_get_wave_is_stereo_ogg(src->ogg) ? 2U : 1U;
	if(!src->duration_ms || !src->sample_rate)
	{
		alogg_destroy_ogg(src->ogg);
		free(src->data);
		memset(src, 0, sizeof(*src));
		return 0;
	}
	return 1;
}

static void dtx_preview_close_source(EOF_DTX_PREVIEW_SOURCE *src)
{
	if(!src)
		return;
	if(src->ogg)
		alogg_destroy_ogg(src->ogg);
	free(src->data);
	memset(src, 0, sizeof(*src));
}

/* ALOGG returns unsigned 16-bit PCM centered at 0x8000.  Pydub/AudioSegment.rms
 * works on signed sample amplitudes, so subtracting 0x8000 gives the same RMS
 * domain.  We compare mean-square instead of sqrt(mean-square), which selects
 * exactly the same hottest window. */
static void dtx_preview_energy_callback(void *buf, int nsamples, int stereo)
{
	unsigned short *pcm = (unsigned short *)buf;
	int i;
	(void)stereo;
	if(!eof_dtx_preview_energy_ctx || !pcm || (nsamples <= 0))
		return;
	for(i = 0; i < nsamples; i++)
	{
		double sample = (double)((long)pcm[i] - 0x8000L);
		eof_dtx_preview_energy_ctx->sumsq += sample * sample;
	}
	eof_dtx_preview_energy_ctx->samples += (unsigned long)nsamples;
}

static int dtx_preview_window_energy(EOF_DTX_PREVIEW_SOURCE *src, double start, double end, double *energy)
{
	EOF_DTX_PREVIEW_ENERGY ctx;
	int ret;
	if(!src || !src->ogg || !energy || (end <= start))
		return 0;
	memset(&ctx, 0, sizeof(ctx));
	eof_dtx_preview_energy_ctx = &ctx;
	ret = alogg_process_ogg(src->ogg, dtx_preview_energy_callback,
		EOF_DTX_PREVIEW_BUFFER_SAMPLES, start, end);
	eof_dtx_preview_energy_ctx = NULL;
	if(!ret || !ctx.samples)
		return 0;
	*energy = ctx.sumsq / (double)ctx.samples;
	return 1;
}

static int dtx_preview_find_hottest_start(EOF_DTX_PREVIEW_SOURCE *src, double *best_start)
{
	double duration, start, best_energy = -1.0;
	if(!src || !best_start)
		return 0;
	duration = (double)src->duration_ms / 1000.0;
	*best_start = 0.0;
	if(duration <= EOF_DTX_PREVIEW_SECONDS)
		return 1;

	/* This mirrors the Python/Pydub script: 25-second windows, advancing five
	 * seconds at a time, with the first window winning ties. */
	for(start = 0.0; start + EOF_DTX_PREVIEW_SECONDS <= duration + 0.0005; start += EOF_DTX_PREVIEW_STEP_SECONDS)
	{
		double energy;
		if(!dtx_preview_window_energy(src, start, start + EOF_DTX_PREVIEW_SECONDS, &energy))
			return 0;
		if(energy > best_energy)
		{
			best_energy = energy;
			*best_start = start;
		}
	}
	return (best_energy >= 0.0);
}

static void dtx_preview_put_le16(unsigned char *p, unsigned value)
{
	p[0] = (unsigned char)(value & 0xFFU);
	p[1] = (unsigned char)((value >> 8) & 0xFFU);
}

static void dtx_preview_put_le32(unsigned char *p, unsigned long value)
{
	p[0] = (unsigned char)(value & 0xFFUL);
	p[1] = (unsigned char)((value >> 8) & 0xFFUL);
	p[2] = (unsigned char)((value >> 16) & 0xFFUL);
	p[3] = (unsigned char)((value >> 24) & 0xFFUL);
}

static int dtx_preview_write_wav_header(FILE *fp, unsigned long rate, unsigned channels, unsigned long data_bytes)
{
	unsigned char h[44];
	if(!fp || !rate || !channels)
		return 0;
	memset(h, 0, sizeof(h));
	memcpy(h, "RIFF", 4);
	dtx_preview_put_le32(h + 4, 36UL + data_bytes);
	memcpy(h + 8, "WAVEfmt ", 8);
	dtx_preview_put_le32(h + 16, 16);
	dtx_preview_put_le16(h + 20, 1);
	dtx_preview_put_le16(h + 22, channels);
	dtx_preview_put_le32(h + 24, rate);
	dtx_preview_put_le32(h + 28, rate * (unsigned long)channels * 2UL);
	dtx_preview_put_le16(h + 32, channels * 2U);
	dtx_preview_put_le16(h + 34, 16);
	memcpy(h + 36, "data", 4);
	dtx_preview_put_le32(h + 40, data_bytes);
	return (fwrite(h, 1, sizeof(h), fp) == sizeof(h)) ? 1 : 0;
}

static double dtx_preview_fade_gain(unsigned long frame, unsigned long total_frames, unsigned long fade_frames)
{
	double gain = 1.0;
	if(!total_frames || !fade_frames)
		return gain;

	/* AudioSegment.fade_in()/fade_out() use a dB ramp.  Pydub's practical
	 * silence floor is -120 dB, so reproduce that curve instead of using a
	 * simple linear-amplitude fade. */
	if(frame < fade_frames)
	{
		double pct = (double)frame / (double)fade_frames;
		double db = -120.0 + (120.0 * pct);
		gain *= pow(10.0, db / 20.0);
	}
	if(frame + fade_frames >= total_frames)
	{
		unsigned long fade_start = (total_frames > fade_frames) ? total_frames - fade_frames : 0;
		double pct = (total_frames > fade_start) ?
			(double)(frame - fade_start) / (double)(total_frames - fade_start) : 1.0;
		double db;
		if(pct < 0.0) pct = 0.0;
		if(pct > 1.0) pct = 1.0;
		db = -120.0 * pct;
		gain *= pow(10.0, db / 20.0);
	}
	return gain;
}

static void dtx_preview_write_callback(void *buf, int nsamples, int stereo)
{
	EOF_DTX_PREVIEW_WRITER *ctx = eof_dtx_preview_writer_ctx;
	unsigned short *pcm = (unsigned short *)buf;
	unsigned char *out;
	unsigned long total_samples, remaining, count, i, fade_frames;
	(void)stereo;
	if(!ctx || ctx->failed || !ctx->fp || !pcm || (nsamples <= 0))
		return;
	total_samples = ctx->total_frames * (unsigned long)ctx->channels;
	if(ctx->written_samples >= total_samples)
		return;
	remaining = total_samples - ctx->written_samples;
	count = ((unsigned long)nsamples < remaining) ? (unsigned long)nsamples : remaining;
	out = malloc(count * 2UL);
	if(!out)
	{
		ctx->failed = 1;
		return;
	}
	fade_frames = (unsigned long)(EOF_DTX_PREVIEW_FADE_SECONDS * (double)ctx->sample_rate + 0.5);
	if(fade_frames > ctx->total_frames)
		fade_frames = ctx->total_frames;

	for(i = 0; i < count; i++)
	{
		unsigned long absolute_sample = ctx->written_samples + i;
		unsigned long frame = absolute_sample / (unsigned long)ctx->channels;
		double gain = dtx_preview_fade_gain(frame, ctx->total_frames, fade_frames);
		long signed_sample = (long)pcm[i] - 0x8000L;
		long scaled = (long)((double)signed_sample * gain);
		unsigned short raw;
		if(scaled > 32767L) scaled = 32767L;
		if(scaled < -32768L) scaled = -32768L;
		raw = (unsigned short)(short)scaled;
		out[i * 2UL] = (unsigned char)(raw & 0xFFU);
		out[i * 2UL + 1] = (unsigned char)((raw >> 8) & 0xFFU);
	}
	if(fwrite(out, 2, count, ctx->fp) != count)
		ctx->failed = 1;
	else
		ctx->written_samples += count;
	free(out);
}

static int dtx_preview_render_wav(EOF_DTX_PREVIEW_SOURCE *src, double start, const char *wav)
{
	EOF_DTX_PREVIEW_WRITER ctx;
	double duration, preview_duration, end;
	unsigned char blank[44] = {0};
	unsigned long data_bytes;
	int ret;

	if(!src || !src->ogg || !wav)
		return 0;
	duration = (double)src->duration_ms / 1000.0;
	if(start < 0.0) start = 0.0;
	if(start >= duration) start = 0.0;
	preview_duration = duration - start;
	if(preview_duration > EOF_DTX_PREVIEW_SECONDS)
		preview_duration = EOF_DTX_PREVIEW_SECONDS;
	if(preview_duration <= 0.0)
		return 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.sample_rate = src->sample_rate;
	ctx.channels = src->channels;
	ctx.total_frames = (unsigned long)(preview_duration * (double)src->sample_rate + 0.5);
	if(!ctx.total_frames)
		return 0;
	end = start + ((double)ctx.total_frames / (double)src->sample_rate);
	ctx.fp = fopen(wav, "wb+");
	if(!ctx.fp)
		return 0;
	if(fwrite(blank, 1, sizeof(blank), ctx.fp) != sizeof(blank))
	{
		fclose(ctx.fp);
		remove(wav);
		return 0;
	}

	eof_dtx_preview_writer_ctx = &ctx;
	ret = alogg_process_ogg(src->ogg, dtx_preview_write_callback,
		EOF_DTX_PREVIEW_BUFFER_SAMPLES, start, end);
	eof_dtx_preview_writer_ctx = NULL;
	if(!ret || ctx.failed || !ctx.written_samples)
	{
		fclose(ctx.fp);
		remove(wav);
		return 0;
	}
	data_bytes = ctx.written_samples * 2UL;
	if(fseek(ctx.fp, 0, SEEK_SET) || !dtx_preview_write_wav_header(ctx.fp, src->sample_rate, src->channels, data_bytes))
	{
		fclose(ctx.fp);
		remove(wav);
		return 0;
	}
	fclose(ctx.fp);
	return 1;
}

static int dtx_preview_encode_ogg(const char *wav, const char *dest)
{
	char oggenc[2048] = {0}, command[8192];
	int result;
	if(!wav || !dest)
		return 0;
	if(!dtx_preview_bundled_tool("oggenc2.exe", oggenc, sizeof(oggenc)))
		ustrzcpy(oggenc, sizeof(oggenc), "oggenc2");
	remove(dest);

	/* Pydub exported the preview at 128 kbps.  OggEnc's -b 128 is the closest
	 * direct equivalent and is already one of EOF's supported external tools. */
	snprintf(command, sizeof(command), "\"%s\" --quiet -b 128 -s 0 \"%s\" -o \"%s\"",
		oggenc, wav, dest);
	result = eof_system(command);
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"DTX preview: native AudioSegment-style OggEnc render returned %d, output=%s",
		result, exists(dest) ? "present" : "missing");
	eof_log(eof_log_string, 1);
	return (result == 0 && exists(dest) && (file_size_ex(dest) > 0)) ? 1 : 0;
}

static int dtx_preview_generate_native(const char *audio, const char *dest)
{
	EOF_DTX_PREVIEW_SOURCE src;
	char wav[4096];
	double best_start = 0.0;
	int success = 0;

	if(!audio || !dest)
		return -1;
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"DTX preview: native AudioSegment-style generation from '%s'", audio);
	eof_log(eof_log_string, 1);
	if(!dtx_preview_open_source(audio, &src))
	{
		eof_log("DTX preview: ALOGG could not decode source OGG", 1);
		return -1;
	}
	if(!dtx_preview_find_hottest_start(&src, &best_start))
	{
		eof_log("DTX preview: native RMS analysis failed", 1);
		goto cleanup;
	}
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"DTX preview: native RMS selected start %.3f s (duration %.3f s)",
		best_start, (double)src.duration_ms / 1000.0);
	eof_log(eof_log_string, 1);

	snprintf(wav, sizeof(wav), "%s.tmp.wav", dest);
	remove(wav);
	remove(dest);
	if(!dtx_preview_render_wav(&src, best_start, wav))
	{
		eof_log("DTX preview: native WAV render/fades failed", 1);
		goto cleanup;
	}
	if(!dtx_preview_encode_ogg(wav, dest))
	{
		/* Keep the WAV on encoder failure: it proves the AudioSegment-equivalent
		 * decode/RMS/fade path succeeded and makes any remaining OggEnc problem
		 * directly inspectable. */
		eof_log("DTX preview: OggEnc failed; temporary faded WAV kept beside preview.ogg", 1);
		goto cleanup;
	}
	remove(wav);
	success = 1;

cleanup:
	dtx_preview_close_source(&src);
	return success ? 0 : -1;
}

int eof_dtx_system_hook(const char *command)
{
	char ffmpeg[2048], audio[4096], dest[4096];
	if(!command)
		return -1;

	/* dtx_export_v2.c still constructs its historical FFmpeg preview command.
	 * Recognize that call signature, extract only input/output paths and replace
	 * the operation completely with the native C equivalent of the supplied
	 * Python/Pydub AudioSegment algorithm.  FFmpeg is not invoked here. */
	if(strstr(command, " -t 25 -af afade=t=in:st=0:d=2,afade=t=out:st=23:d=2 ") &&
	   dtx_preview_extract_paths(command, ffmpeg, sizeof(ffmpeg), audio, sizeof(audio), dest, sizeof(dest)))
	{
		(void)ffmpeg;
		return dtx_preview_generate_native(audio, dest);
	}

	return dtx_preview_run_system(command);
}

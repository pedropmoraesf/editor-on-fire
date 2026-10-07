#include <allegro.h>
#include "vorbis/vorbisfile.h"
#include "vorbis/codec.h"
#include <math.h>	//For sqrt()
#include <stdint.h>
#include "main.h"
#include "utility.h"
#include "beat.h"
#include "mix.h"
#include "tuning.h"	//For eof_lookup_default_string_tuning_absolute()

#ifdef USEMEMWATCH
#include "memwatch.h"
#endif

//AUDIOSTREAM * eof_mix_stream = NULL;
SAMPLE *    eof_sound_clap = NULL;
SAMPLE *    eof_sound_metronome = NULL;
SAMPLE *    eof_sound_metronome_low = NULL;
SAMPLE *    eof_sound_grid_snap = NULL;
SAMPLE *    eof_sound_note[EOF_MAX_VOCAL_TONES] = {NULL};
SAMPLE *    eof_sound_chosen_percussion = NULL;	//The user-selected percussion sound
SAMPLE *    eof_sound_cowbell = NULL;
SAMPLE *    eof_sound_tambourine1 = NULL;
SAMPLE *    eof_sound_tambourine2 = NULL;
SAMPLE *    eof_sound_tambourine3 = NULL;
SAMPLE *    eof_sound_triangle1 = NULL;
SAMPLE *    eof_sound_triangle2 = NULL;
SAMPLE *    eof_sound_woodblock1 = NULL;
SAMPLE *    eof_sound_woodblock2 = NULL;
SAMPLE *    eof_sound_woodblock3 = NULL;
SAMPLE *    eof_sound_woodblock4 = NULL;
SAMPLE *    eof_sound_woodblock5 = NULL;
SAMPLE *    eof_sound_woodblock6 = NULL;
SAMPLE *    eof_sound_woodblock7 = NULL;
SAMPLE *    eof_sound_woodblock8 = NULL;
SAMPLE *    eof_sound_woodblock9 = NULL;
SAMPLE *    eof_sound_woodblock10 = NULL;
SAMPLE *    eof_sound_clap1 = NULL;
SAMPLE *    eof_sound_clap2 = NULL;
SAMPLE *    eof_sound_clap3 = NULL;
SAMPLE *    eof_sound_clap4 = NULL;
SAMPLE *    eof_sound_gas1 = NULL;
SAMPLE *    eof_sound_gas2 = NULL;
SAMPLE *    eof_sound_gas3 = NULL;

/* Low-latency PART_REAL_DRUM_DTX preview samples.  These are generated once
 * at startup and mixed directly in EOF's OGG callback, avoiding the latency
 * and scheduling jitter of the operating system MIDI synthesizer. */
enum
{
	EOF_DRUM_SAMPLE_KICK = 0,
	EOF_DRUM_SAMPLE_SNARE,
	EOF_DRUM_SAMPLE_SIDE,
	EOF_DRUM_SAMPLE_HIHAT_CLOSED,
	EOF_DRUM_SAMPLE_HIHAT_OPEN,
	EOF_DRUM_SAMPLE_TOM_LOW,
	EOF_DRUM_SAMPLE_TOM_MID,
	EOF_DRUM_SAMPLE_TOM_HIGH,
	EOF_DRUM_SAMPLE_CRASH,
	EOF_DRUM_SAMPLE_RIDE,
	EOF_DRUM_SAMPLE_COUNT
};
static SAMPLE *eof_sound_drum[EOF_DRUM_SAMPLE_COUNT] = {NULL};
static unsigned eof_mix_drum_voice_cursor = 0;
static uint32_t eof_drum_noise_state = 0x13579BDFU;

EOF_MIX_VOICE eof_voice[EOF_MIX_MAX_CHANNELS];	//eof_voice[0] is "clap", eof_voice[1] is "metronome", eof_voice[2] is "vocal tone", eof_voice[3] is "vocal percussion"
char eof_mix_claps_enabled = 0;
char eof_mix_metronome_enabled = 0;
char eof_mix_claps_note = 63; /* enable all by default */
char eof_mix_vocal_tones_enabled = 0;
char eof_mix_midi_tones_enabled = 0;
char eof_mix_drum_tones_enabled = 0;	//GM percussion preview for PART_REAL_DRUM_DTX
char eof_mix_percussion_enabled = 0;
int eof_selected_percussion_cue = 17;	//The user selected percussion sound (cowbell by default), corresponds to the radio button in the eof_audio_cues_dialog[] array

int eof_chart_volume = 100;	//Stores the volume level for the chart audio, specified as a percentage
double eof_chart_volume_multiplier = 1.0;	//This is the value sqrt(volume/100.0), which must be multiplied to the voice's amplitude to adjust for the specified volume
int eof_clap_volume = 100;	//Stores the volume level for the clap cue, specified as a percentage
int eof_tick_volume = 100;	//Stores the volume level for the tick cue, specified as a percentage
int eof_tone_volume = 100;	//Stores the volume level for the vocal tone cue, specified as a percentage
int eof_percussion_volume = 100;	//Stores the volume level for the vocal percussion cue, specified as a percentage
int eof_midi_tone_volume = 100;	//Stores the volume level for the MIDI tones, specified as a percentage
int eof_clap_for_mutes = 1;			//Specifies whether fully string muted notes trigger the clap sound cue
int eof_clap_for_ghosts = 1;		//Specifies whether ghosted pro guitar or drum notes trigger the clap sound cue
int eof_multi_pitch_metronome = 1;	//Specifies whether the metronome will use a lower tick for beats that aren't the first in a measure
int eof_min_midi_tone_length = 100;	//The MIDI tones feature will pad each queued MIDI tone to at least this length

int           eof_mix_speed = 1000;
char          eof_mix_speed_ticker;
unsigned long eof_mix_sample_count = 0;
double        eof_mix_sample_increment = 1.0;
unsigned long eof_mix_next_guitar_note;
unsigned long eof_mix_next_drum_note;
unsigned long eof_mix_next_clap;
unsigned long eof_mix_next_metronome;
char eof_mix_next_metronome_pitch;
unsigned long eof_mix_next_note;
unsigned long eof_mix_next_percussion;

unsigned long eof_mix_clap_pos[EOF_MAX_NOTES] = {0};
int eof_mix_claps = 0;
int eof_mix_current_clap = 0;

typedef struct {
	unsigned long pos;
	unsigned char channel;
	int note;
	int tone;
	unsigned long length;
} guitar_midi_note;

guitar_midi_note eof_guitar_notes[EOF_MAX_NOTES * 6] = {{0,0,0,0,0}};	//Each note can have 6 pitches (one on each string)
int eof_mix_guitar_notes = 0;
int eof_mix_current_guitar_note = 0;

typedef struct {
	unsigned long pos;
	unsigned char note;
} drum_midi_note;

drum_midi_note eof_drum_notes[EOF_MAX_NOTES * 6] = {{0,0}};	//Each DTX event can have up to six simultaneous GM percussion notes
int eof_mix_drum_notes = 0;
int eof_mix_current_drum_note = 0;

unsigned long eof_mix_note_pos[EOF_MAX_NOTES] = {0};
unsigned long eof_mix_note_note[EOF_MAX_NOTES] = {0};
unsigned long eof_mix_note_ms_pos[EOF_MAX_NOTES] = {0};	//Used to store the start positions of notes (for MIDI playback)
unsigned long eof_mix_note_ms_end[EOF_MAX_NOTES] = {0};	//Used to store the end positions of notes (for MIDI playback)
int eof_mix_notes = 0;
int eof_mix_current_note = 0;

unsigned long eof_mix_metronome_pos[EOF_MAX_BEATS] = {0};
char eof_mix_metronome_pos_pitch[EOF_MAX_BEATS] = {0};	//Will track whether the pitch of each beat's metronome tick should be high (nonzero, first beat in a measure) or low (zero, any other beat)
int eof_mix_metronomes = 0;
int eof_mix_current_metronome = 0;

unsigned long eof_mix_percussion_pos[EOF_MAX_NOTES] = {0};
int eof_mix_percussions = 0;
int eof_mix_current_percussion = 0;

static double eof_drum_noise(void)
{
	uint32_t x = eof_drum_noise_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	eof_drum_noise_state = x;
	return ((double)(x & 0xFFFFU) / 32767.5) - 1.0;
}

static SAMPLE *eof_create_drum_sample(int type)
{
	const int rate = 44100;
	double seconds = 0.4, phase = 0.0, previous_noise = 0.0;
	unsigned long i, length;
	SAMPLE *sp;
	unsigned short *pcm;

	switch(type)
	{
		case EOF_DRUM_SAMPLE_KICK: seconds = 0.42; break;
		case EOF_DRUM_SAMPLE_SNARE: seconds = 0.38; break;
		case EOF_DRUM_SAMPLE_SIDE: seconds = 0.16; break;
		case EOF_DRUM_SAMPLE_HIHAT_CLOSED: seconds = 0.16; break;
		case EOF_DRUM_SAMPLE_HIHAT_OPEN: seconds = 0.72; break;
		case EOF_DRUM_SAMPLE_TOM_LOW: seconds = 0.58; break;
		case EOF_DRUM_SAMPLE_TOM_MID: seconds = 0.50; break;
		case EOF_DRUM_SAMPLE_TOM_HIGH: seconds = 0.44; break;
		case EOF_DRUM_SAMPLE_CRASH: seconds = 1.35; break;
		case EOF_DRUM_SAMPLE_RIDE: seconds = 1.10; break;
		default: break;
	}
	length = (unsigned long)(seconds * rate);
	sp = create_sample(16, 0, rate, length);
	if(!sp || !sp->data)
		return sp;
	pcm = (unsigned short *)sp->data;

	for(i = 0; i < length; i++)
	{
		double t = (double)i / (double)rate;
		double n = eof_drum_noise();
		double hp = n - previous_noise;
		double signal = 0.0;
		double freq;
		previous_noise = n;

		switch(type)
		{
			case EOF_DRUM_SAMPLE_KICK:
				freq = 46.0 + 92.0 * exp(-t * 18.0);
				phase += 2.0 * 3.14159265358979323846 * freq / rate;
				signal = 0.96 * sin(phase) * exp(-t * 10.0);
			break;
			case EOF_DRUM_SAMPLE_SNARE:
				signal = 0.68 * hp * exp(-t * 16.0) +
				         0.28 * sin(2.0 * 3.14159265358979323846 * 185.0 * t) * exp(-t * 22.0);
			break;
			case EOF_DRUM_SAMPLE_SIDE:
				signal = 0.82 * hp * exp(-t * 34.0) +
				         0.18 * sin(2.0 * 3.14159265358979323846 * 720.0 * t) * exp(-t * 30.0);
			break;
			case EOF_DRUM_SAMPLE_HIHAT_CLOSED:
				signal = 0.58 * hp * exp(-t * 46.0) +
				         0.12 * sin(2.0 * 3.14159265358979323846 * 6100.0 * t) * exp(-t * 38.0);
			break;
			case EOF_DRUM_SAMPLE_HIHAT_OPEN:
				signal = 0.48 * hp * exp(-t * 7.0) +
				         0.10 * sin(2.0 * 3.14159265358979323846 * 5700.0 * t) * exp(-t * 8.0);
			break;
			case EOF_DRUM_SAMPLE_TOM_LOW:
			case EOF_DRUM_SAMPLE_TOM_MID:
			case EOF_DRUM_SAMPLE_TOM_HIGH:
			{
				double base = (type == EOF_DRUM_SAMPLE_TOM_LOW) ? 105.0 :
				              ((type == EOF_DRUM_SAMPLE_TOM_MID) ? 145.0 : 205.0);
				signal = (0.78 * sin(2.0 * 3.14159265358979323846 * base * t) +
				          0.18 * sin(2.0 * 3.14159265358979323846 * base * 1.96 * t)) * exp(-t * 8.5) +
				         0.08 * n * exp(-t * 22.0);
			}
			break;
			case EOF_DRUM_SAMPLE_CRASH:
				signal = 0.42 * hp * exp(-t * 2.8) +
				         (0.16 * sin(2.0 * 3.14159265358979323846 * 430.0 * t) +
				          0.13 * sin(2.0 * 3.14159265358979323846 * 671.0 * t) +
				          0.10 * sin(2.0 * 3.14159265358979323846 * 953.0 * t)) * exp(-t * 3.1);
			break;
			case EOF_DRUM_SAMPLE_RIDE:
				signal = 0.20 * hp * exp(-t * 4.0) +
				         (0.24 * sin(2.0 * 3.14159265358979323846 * 510.0 * t) +
				          0.18 * sin(2.0 * 3.14159265358979323846 * 785.0 * t) +
				          0.12 * sin(2.0 * 3.14159265358979323846 * 1120.0 * t)) * exp(-t * 3.7);
			break;
			default:
				signal = 0.4 * hp * exp(-t * 18.0);
			break;
		}

		if(signal > 1.0) signal = 1.0;
		if(signal < -1.0) signal = -1.0;
		pcm[i] = (unsigned short)(32768.0 + signal * 30000.0);
	}
	return sp;
}

static int eof_drum_sample_for_gm(unsigned char note)
{
	switch(note)
	{
		case 35: case 36:
			return EOF_DRUM_SAMPLE_KICK;
		case 37:
			return EOF_DRUM_SAMPLE_SIDE;
		case 38: case 39: case 40:
			return EOF_DRUM_SAMPLE_SNARE;
		case 42: case 44: case 54:
			return EOF_DRUM_SAMPLE_HIHAT_CLOSED;
		case 46:
			return EOF_DRUM_SAMPLE_HIHAT_OPEN;
		case 41: case 43:
			return EOF_DRUM_SAMPLE_TOM_LOW;
		case 45: case 47:
			return EOF_DRUM_SAMPLE_TOM_MID;
		case 48: case 50:
			return EOF_DRUM_SAMPLE_TOM_HIGH;
		case 51: case 53: case 59:
			return EOF_DRUM_SAMPLE_RIDE;
		case 49: case 52: case 55: case 57:
			return EOF_DRUM_SAMPLE_CRASH;
		default:
			if(note < 41) return EOF_DRUM_SAMPLE_SNARE;
			if(note < 49) return EOF_DRUM_SAMPLE_HIHAT_CLOSED;
			return EOF_DRUM_SAMPLE_CRASH;
	}
}

static void eof_mix_trigger_drum_sample(unsigned char note)
{
	unsigned attempt, channel = EOF_MIX_DRUM_FIRST_CHANNEL;
	int sample_index = eof_drum_sample_for_gm(note);
	SAMPLE *sp = eof_sound_drum[sample_index];

	if(!sp)
		return;
	for(attempt = 0; attempt < EOF_MIX_DRUM_CHANNELS; attempt++)
	{
		unsigned candidate = EOF_MIX_DRUM_FIRST_CHANNEL +
			((eof_mix_drum_voice_cursor + attempt) % EOF_MIX_DRUM_CHANNELS);
		if(!eof_voice[candidate].playing)
		{
			channel = candidate;
			break;
		}
	}
	if(attempt == EOF_MIX_DRUM_CHANNELS)
		channel = EOF_MIX_DRUM_FIRST_CHANNEL + (eof_mix_drum_voice_cursor % EOF_MIX_DRUM_CHANNELS);

	eof_mix_drum_voice_cursor = (channel - EOF_MIX_DRUM_FIRST_CHANNEL + 1U) % EOF_MIX_DRUM_CHANNELS;
	eof_voice[channel].sp = sp;
	eof_voice[channel].pos = 0;
	eof_voice[channel].fpos = 0.0;
	/* Volume/multiplier were precomputed in eof_mix_start(); keep the real-time
	 * trigger path to pointer/index assignments only. */
	eof_voice[channel].playing = 1;
}

void eof_mix_callback_common(void)
{
	/* increment the sample and check sound triggers */
	eof_mix_sample_count++;

	//Mix claps
	if((eof_mix_sample_count >= eof_mix_next_clap) && (eof_mix_current_clap < eof_mix_claps))
	{
		if(eof_mix_claps_enabled)
		{
			eof_voice[0].sp = eof_sound_clap;
			eof_voice[0].pos = 0;
			eof_voice[0].fpos = 0.0;
			eof_voice[0].playing = 1;
		}
		eof_mix_current_clap++;
		eof_mix_next_clap = eof_mix_clap_pos[eof_mix_current_clap];
	}

	//Mix metronome
	if((eof_mix_sample_count >= eof_mix_next_metronome) && (eof_mix_current_metronome < eof_mix_metronomes))
	{
		if(eof_mix_metronome_enabled)
		{
			if(eof_mix_next_metronome_pitch || !eof_multi_pitch_metronome)
			{	//If this beat was determined to be the first beat in its measure, or if multi-pitched metronome isn't enabled
				eof_voice[1].sp = eof_sound_metronome;	//Use the high pitch metronome tick
			}
			else
			{	//Low pitch metronome tick
				eof_voice[1].sp = eof_sound_metronome_low;
			}
			eof_voice[1].pos = 0;
			eof_voice[1].fpos = 0.0;
			eof_voice[1].playing = 1;
		}
		eof_mix_current_metronome++;
		eof_mix_next_metronome = eof_mix_metronome_pos[eof_mix_current_metronome];
		eof_mix_next_metronome_pitch = eof_mix_metronome_pos_pitch[eof_mix_current_metronome];
	}

	//Mix vocal tones
	if((eof_mix_sample_count >= eof_mix_next_note) && (eof_mix_current_note < eof_mix_notes))
	{
		if(eof_mix_vocal_tones_enabled && eof_sound_note[eof_mix_note_note[eof_mix_current_note]])
		{
			eof_voice[2].sp = eof_sound_note[eof_mix_note_note[eof_mix_current_note]];
			eof_voice[2].pos = 0;
			eof_voice[2].fpos = 0.0;
			eof_voice[2].playing = 1;
		}
		eof_mix_current_note++;
		eof_mix_next_note = eof_mix_note_pos[eof_mix_current_note];
	}
	if((eof_mix_sample_count >= eof_mix_next_percussion) && (eof_mix_current_percussion < eof_mix_notes))
	{
		if(eof_mix_percussion_enabled)
		{
			eof_voice[3].sp = eof_sound_chosen_percussion;
			eof_voice[3].pos = 0;
			eof_voice[3].fpos = 0.0;
			eof_voice[3].playing = 1;
		}
		eof_mix_current_percussion++;
		eof_mix_next_percussion = eof_mix_percussion_pos[eof_mix_current_percussion];
	}

	//Trigger instrument previews
	eof_play_queued_midi_tones();	//Pro guitar/bass still uses the configured MIDI synth
	eof_play_queued_drum_tones();	//DTX GM note numbers select sample-synchronous PCM drum sounds
}

void eof_mix_callback_stereo(void * buf, int length)
{
	unsigned long bytes_left;
	unsigned short * buffer;
	long sum=0,sum2=0;	//Use a signed long integer to allow the clipping logic to be more efficient
	long cuesample;		//Used to apply a volume to cues, where the appropriate amplitude multiplier for changing the cue's loudness to X% is to multiply its amplitudes by sqrt(X/100)
	unsigned long i, j;

	bytes_left = length >> 1;	//Divide by two (length refers to the number of bytes to process, but buffer will be accessed as an array of 16 bit integer values)
	buffer = (unsigned short *)buf;

	/* add audio data to the buffer */
	for(i = 0; i < bytes_left; i += 2)
	{
		/* store original sample values */
		sum = buffer[i] - 32768;		//Convert to signed sample
		sum2 = buffer[i+1] - 32768;		//Repeat for the other channel of a stereo sample

		/* perform phase cancellation if enabled */
		if(eof_phase_cancellation)
		{
			long sum3 = (sum - sum2) / 2;		//Subtract one channel's amplitude from the other and divide by two

			sum = sum3;		//And use that for each channel's amplitude
			sum2 = sum3;
		}

		/* perform center isolation if enabled */
		else if(eof_center_isolation)
		{
			long sum3 = (sum + sum2) / 2;		//Add the two channels' amplitudes and divide by two

			sum = sum3;		//And use that for each channel's amplitude
			sum2 = sum3;
		}

		/* apply volume multiplier */
		if(eof_chart_volume != 100)		//If the chart volume is to be less than 100%
		{
			sum *= eof_chart_volume_multiplier;
			sum2 *= eof_chart_volume_multiplier;	//If this is a stereo audio file, apply the volume to the other channel as well
		}

		/* mix voices */
		for(j = 0; j < EOF_MIX_MAX_CHANNELS; j++)
		{
			if(!eof_voice[j].playing)
				continue;	//If this voice isn't playing, skip it

			cuesample = ((unsigned short *)(eof_voice[j].sp->data))[(unsigned long)eof_voice[j].pos] - 32768;
			if(eof_voice[j].volume != 100)
				cuesample *= eof_voice[j].multiplier;	//Change the cue to the specified loudness

			sum += cuesample;
			sum2 += cuesample;	//If this is a stereo audio file, mix the voice into the other channel as well

			eof_voice[j].fpos += eof_mix_sample_increment;
			eof_voice[j].pos = eof_voice[j].fpos + 0.5;	//Round to nearest full sample number
			if(eof_voice[j].pos >= eof_voice[j].sp->len)
			{
				eof_voice[j].playing = 0;
			}
		}

		/* Apply the floor and ceiling for 16 bit sample data as necessary */
		if(sum < -32768)
			sum = -32768;
		else if(sum > 32767)
			sum = 32767;
		buffer[i] = sum + 32768;		//Convert the summed PCM samples to unsigned and store into buffer

		if(sum2 < -32768)
			sum2 = -32768;
		else if(sum2 > 32767)
			sum2 = 32767;
		buffer[i+1] = sum2 + 32768;	//Convert the summed PCM samples to unsigned and store into buffer

		eof_mix_callback_common();	//Increment the sample and check sound triggers
	}
	eof_just_played = 1;
}

void eof_mix_callback_mono(void * buf, int length)
{
	unsigned long bytes_left;
	unsigned short * buffer;
	long sum=0;			//Use a signed long integer to allow the clipping logic to be more efficient
	long cuesample;		//Used to apply a volume to cues, where the appropriate amplitude multiplier for changing the cue's loudness to X% is to multiply its amplitudes by sqrt(X/100)
	unsigned long i, j;

	bytes_left = length >> 1;	//Divide by two (length refers to the number of bytes to process, but buffer will be accessed as an array of 16 bit integer values)
	buffer = (unsigned short *)buf;

	/* add audio data to the buffer */
	for(i = 0; i < bytes_left; i++)
	{
		/* store original sample values */
		sum = buffer[i] - 32768;	//Convert to signed sample

		/* apply volume multiplier */
		if(eof_chart_volume != 100)		//If the chart volume is to be less than 100%
		{
			sum *= eof_chart_volume_multiplier;
		}

		/* mix voices */
		for(j = 0; j < EOF_MIX_MAX_CHANNELS; j++)
		{
			if(eof_voice[j].playing)
			{
				cuesample = ((unsigned short *)(eof_voice[j].sp->data))[(unsigned long)eof_voice[j].pos] - 32768;
				if(eof_voice[j].volume != 100)
					cuesample *= eof_voice[j].multiplier;	//Change the cue to the specified loudness

				sum += cuesample;

				eof_voice[j].fpos += eof_mix_sample_increment;
				eof_voice[j].pos = eof_voice[j].fpos + 0.5;	//Round to nearest full sample number
				if(eof_voice[j].pos >= eof_voice[j].sp->len)
				{
					eof_voice[j].playing = 0;
				}
			}
		}

		/* Apply the floor and ceiling for 16 bit sample data as necessary */
		if(sum < -32768)
			sum = -32768;
		else if(sum > 32767)
			sum = 32767;
		buffer[i] = sum + 32768;		//Convert the summed PCM samples to unsigned and store into buffer

		eof_mix_callback_common();		//Increment the sample and check sound triggers
	}
	eof_just_played = 1;
}

unsigned long eof_mix_msec_to_sample(unsigned long msec, int freq)
{
	unsigned long sample;
	double second = (double)msec / (double)1000.0;

	if(eof_playback_time_stretch)
	{
		sample = (unsigned long)(second * (double)freq * (1000.0 / (double)eof_mix_speed));
	}
	else
	{
		sample = (unsigned long)(second * (double)freq);
	}
	return sample;
}

void eof_mix_find_claps(void)
{
	unsigned long i, bitmask;
	unsigned long tracknum;
	EOF_PRO_GUITAR_TRACK *tp = NULL;

	if(!eof_music_track)
		return;
	eof_log("eof_mix_find_claps() entered", 2);

	eof_mix_claps = 0;
	eof_mix_current_clap = 0;
	tracknum = eof_song->track[eof_selected_track]->tracknum;
	if((eof_selected_track != EOF_TRACK_DRUM_DTX) && eof_track_is_pro_guitar_track(eof_song, eof_selected_track))
	{	//If a normal pro guitar/bass track is active
		tp = eof_song->pro_guitar_track[eof_song->track[eof_selected_track]->tracknum];
	}

	//Queue claps
	if(eof_vocals_selected)
	{
		for(i = 0; i < eof_song->vocal_track[tracknum]->lyrics; i++)
		{
			eof_mix_clap_pos[eof_mix_claps] = eof_mix_msec_to_sample(eof_song->vocal_track[tracknum]->lyric[i]->pos, alogg_get_wave_freq_ogg(eof_music_track));
			eof_mix_claps++;
		}
	}
	else
	{	//If a vocal track is not selected
		for(i = 0; i < eof_get_track_size(eof_song, eof_selected_track); i++)
		{	//For each note in the track
			unsigned char note, ghost;

			note = eof_get_note_note(eof_song, eof_selected_track, i);
			if((eof_get_note_type(eof_song, eof_selected_track, i) != eof_note_type) || !(note & eof_mix_claps_note))
			{	//If the note is not in the active track difficulty or the clap sound cue doesn't apply to at least one gem used in the note
				continue;	//Skip it
			}
			ghost = eof_get_note_ghost(eof_song, eof_selected_track, i);
			if(!eof_clap_for_ghosts && !(note & ~ghost))
			{	//If clap cues should not trigger for ghosted gems, or if this gem doesn't have any non-ghosted gems
				continue;	//Skip this note
			}
			if(tp)
			{	//If a pro guitar track is active
				if(!eof_clap_for_mutes && (eof_get_note_flags(eof_song, eof_selected_track, i) & EOF_PRO_GUITAR_NOTE_FLAG_STRING_MUTE))
				{	//If clap cues should not trigger for fully string muted notes, and this note is
					continue;	//Skip this note
				}
			}
			eof_mix_clap_pos[eof_mix_claps] = eof_mix_msec_to_sample(eof_get_note_pos(eof_song, eof_selected_track, i), alogg_get_wave_freq_ogg(eof_music_track));
			eof_mix_claps++;
		}
	}

	//Queue metronome
	eof_mix_metronomes = 0;
	eof_mix_current_metronome = 0;
	for(i = 0; i < eof_song->beats; i++)
	{
		char pitch = 1;	//By default, the standard metronome tick sound will be used

		eof_mix_metronome_pos[eof_mix_metronomes] = eof_mix_msec_to_sample(eof_song->beat[i]->pos, alogg_get_wave_freq_ogg(eof_music_track));
		if(eof_song->beat[i]->has_ts && eof_song->beat[i]->beat_within_measure)
		{	//If there is a time signature in effect at this beat and this is NOT the first beat in its measure
			pitch = 0;	//Use the low pitched metronome tick sound
		}
		eof_mix_metronome_pos_pitch[eof_mix_metronomes] = pitch;
		eof_mix_metronomes++;
	}

	//Queue MIDI tones for pro guitar notes
	eof_mix_guitar_notes = 0;
	eof_mix_current_guitar_note = 0;
	if(eof_track_is_pro_guitar_track(eof_song, eof_selected_track))
	{	//If a pro guitar/bass track is active
		int tone;
		EOF_PRO_GUITAR_TRACK *track = eof_song->pro_guitar_track[eof_song->track[eof_selected_track]->tracknum];

		eof_midi_reset_instrument = 1;	//Ensure the active track's MIDI instrument gets set for MIDI channels 0 through 5
		for(i = 0; i < eof_get_track_size(eof_song, eof_selected_track); i++)
		{	//For each note in the track
			int j = 0;
			unsigned long pos;
			EOF_PRO_GUITAR_NOTE *note;

			if(eof_get_note_type(eof_song, eof_selected_track, i) != eof_note_type)
				continue;	//If the note is not in the active track difficulty, skip it

			pos = eof_mix_msec_to_sample(eof_get_note_pos(eof_song, eof_selected_track, i) + eof_av_delay - eof_midi_tone_delay, alogg_get_wave_freq_ogg(eof_music_track));
			note = track->note[i];

			tone = eof_lookup_midi_tone(eof_song, eof_selected_track, i);
			for(j = 0, bitmask = 1; j < 6; j++, bitmask <<= 1)
			{	//For each of the 6 supported strings
				if((note->note & bitmask) && !(note->frets[j] & 0x80) && !(note->ghost & bitmask))
				{	//If the string is used (and not muted or ghosted)
					eof_guitar_notes[eof_mix_guitar_notes].pos = pos;
					eof_guitar_notes[eof_mix_guitar_notes].channel = j;
					eof_guitar_notes[eof_mix_guitar_notes].note = track->tuning[j] + eof_lookup_default_string_tuning_absolute(track, eof_selected_track, j) + note->frets[j] + track->capo;
					eof_guitar_notes[eof_mix_guitar_notes].tone = tone;
					eof_guitar_notes[eof_mix_guitar_notes].length = track->note[i]->length;
					if(eof_guitar_notes[eof_mix_guitar_notes].length < eof_min_midi_tone_length)
						eof_guitar_notes[eof_mix_guitar_notes].length = eof_min_midi_tone_length;	//Ensure that queued MIDI tones are at least this long
					eof_mix_guitar_notes++;
				}
			}
		}
	}

	//Queue General MIDI percussion for PART_REAL_DRUM_DTX.
	//The DTX track stores the actual GM percussion note in each used string's fret byte.
	eof_mix_drum_notes = 0;
	eof_mix_current_drum_note = 0;
	if((eof_selected_track == EOF_TRACK_DRUM_DTX) && (eof_selected_track < eof_song->tracks) &&
	   eof_song->track[eof_selected_track] && (eof_song->track[eof_selected_track]->track_format == EOF_PRO_GUITAR_TRACK_FORMAT))
	{
		EOF_PRO_GUITAR_TRACK *track = eof_song->pro_guitar_track[eof_song->track[eof_selected_track]->tracknum];

		if(track)
		{
			for(i = 0; (i < track->notes) && (eof_mix_drum_notes < EOF_MAX_NOTES * 6); i++)
			{
				unsigned j;
				unsigned long bitmask;
				unsigned long pos;
				EOF_PRO_GUITAR_NOTE *note = track->note[i];

				if(!note || (note->type != eof_note_type))
					continue;

				/* PCM drum tones are mixed inside the audio callback, so no MIDI
				 * latency compensation is needed.  Queue the hit at the chart's
				 * exact note timestamp just like clap/metronome cues. */
				pos = eof_mix_msec_to_sample(note->pos, alogg_get_wave_freq_ogg(eof_music_track));

				for(j = 0, bitmask = 1; (j < 6U) && (eof_mix_drum_notes < EOF_MAX_NOTES * 6); j++, bitmask <<= 1)
				{
					unsigned midi_note;
					if(!(note->note & bitmask))
						continue;
					midi_note = note->frets[j] & 0x7FU;
					eof_drum_notes[eof_mix_drum_notes].pos = pos;
					eof_drum_notes[eof_mix_drum_notes].note = (unsigned char)midi_note;
					eof_mix_drum_notes++;
				}
			}
		}
	}

	//Queue vocal tones
	eof_mix_notes = 0;
	eof_mix_current_note = 0;
	eof_mix_percussions = 0;
	for(i = 0; i < eof_song->vocal_track[0]->lyrics; i++)
	{
		if((eof_song->vocal_track[0]->lyric[i]->note >= 36) && (eof_song->vocal_track[0]->lyric[i]->note <= 84))
		{	//This is a vocal pitch
			eof_mix_note_pos[eof_mix_notes] = eof_mix_msec_to_sample(eof_song->vocal_track[0]->lyric[i]->pos, alogg_get_wave_freq_ogg(eof_music_track));
			eof_mix_note_note[eof_mix_notes] = eof_song->vocal_track[0]->lyric[i]->note;
			eof_mix_note_ms_pos[eof_mix_notes] = eof_song->vocal_track[0]->lyric[i]->pos;
			eof_mix_note_ms_end[eof_mix_notes] = eof_song->vocal_track[0]->lyric[i]->pos + eof_song->vocal_track[0]->lyric[i]->length;
			eof_mix_notes++;
		}
		else if(eof_song->vocal_track[0]->lyric[i]->note == EOF_LYRIC_PERCUSSION)
		{	//This is vocal percussion
			eof_mix_percussion_pos[eof_mix_percussions] = eof_mix_msec_to_sample(eof_song->vocal_track[0]->lyric[i]->pos, alogg_get_wave_freq_ogg(eof_music_track));
			eof_mix_percussions++;
		}
	}
}

SAMPLE *eof_load_wav(char *filename)
{
	char path[1024];

	if(!filename)
		return NULL;	//Invalid parameter

	//Check for the specified file in the resources folder
	(void) snprintf(path, sizeof(path) - 1, "resources");
	put_backslash(path);	//Append a file separator
	(void) eof_strncat(path, filename, sizeof(path));	//Append the file name

	if(exists(path))
	{
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tLoading custom WAV \"%s\"", path);
		eof_log(eof_log_string, 1);
	}
	else
	{	//If the file doesn't exist within /resources, change the path to within eof.dat
		(void) snprintf(path, sizeof(path) - 1, "eof.dat#%s", filename);
	}

	return load_wav(path);
}

void eof_mix_init(void)
{
	int i;
	char fbuffer[1024] = {0};

	eof_log("eof_mix_init() entered", 1);
	eof_log("\tLoading audio samples", 1);

	eof_sound_clap = eof_load_wav("clap.wav");
	if(!eof_sound_clap)
	{
		allegro_message("Couldn't load clap sound!");
	}
	eof_sound_metronome = eof_load_wav("metronome.wav");
	if(!eof_sound_metronome)
	{
		allegro_message("Couldn't load metronome sound!");
	}
	eof_sound_metronome_low = eof_load_wav("metronome_low.wav");
	if(!eof_sound_metronome_low)
	{
		allegro_message("Couldn't load low pitched metronome sound!");
	}
	eof_sound_grid_snap = eof_load_wav("gridsnap.wav");
	if(!eof_sound_grid_snap)
	{
		allegro_message("Couldn't load seek sound!");
	}
	for(i = 36; i < 85; i++)
	{	//Load piano tones (only tones numbered 36 through 84 are included in the DAT file
		(void) snprintf(fbuffer, sizeof(fbuffer) - 1, "eof.dat#piano.esp/NOTE_%02d_OGG", i);
		eof_sound_note[i] = eof_mix_load_ogg_sample(fbuffer);
	}
	eof_sound_cowbell = eof_mix_load_ogg_sample("percussion.dat#cowbell.ogg");
	eof_sound_chosen_percussion = eof_sound_cowbell;	//Until the user specifies otherwise, make cowbell the default percussion
	eof_sound_tambourine1 = eof_mix_load_ogg_sample("percussion.dat#tambourine1.ogg");
	eof_sound_tambourine2 = eof_mix_load_ogg_sample("percussion.dat#tambourine2.ogg");
	eof_sound_tambourine3 = eof_mix_load_ogg_sample("percussion.dat#tambourine3.ogg");
	eof_sound_triangle1 = eof_mix_load_ogg_sample("percussion.dat#triangle1.ogg");
	eof_sound_triangle2 = eof_mix_load_ogg_sample("percussion.dat#triangle2.ogg");
	eof_sound_woodblock1 = eof_mix_load_ogg_sample("percussion.dat#woodblock1.ogg");
	eof_sound_woodblock2 = eof_mix_load_ogg_sample("percussion.dat#woodblock2.ogg");
	eof_sound_woodblock3 = eof_mix_load_ogg_sample("percussion.dat#woodblock3.ogg");
	eof_sound_woodblock4 = eof_mix_load_ogg_sample("percussion.dat#woodblock4.ogg");
	eof_sound_woodblock5 = eof_mix_load_ogg_sample("percussion.dat#woodblock5.ogg");
	eof_sound_woodblock6 = eof_mix_load_ogg_sample("percussion.dat#woodblock6.ogg");
	eof_sound_woodblock7 = eof_mix_load_ogg_sample("percussion.dat#woodblock7.ogg");
	eof_sound_woodblock8 = eof_mix_load_ogg_sample("percussion.dat#woodblock8.ogg");
	eof_sound_woodblock9 = eof_mix_load_ogg_sample("percussion.dat#woodblock9.ogg");
	eof_sound_woodblock10 = eof_mix_load_ogg_sample("percussion.dat#woodblock10.ogg");
	eof_sound_clap1 = eof_mix_load_ogg_sample("percussion.dat#clap1.ogg");
	eof_sound_clap2 = eof_mix_load_ogg_sample("percussion.dat#clap2.ogg");
	eof_sound_clap3 = eof_mix_load_ogg_sample("percussion.dat#clap3.ogg");
	eof_sound_clap4 = eof_mix_load_ogg_sample("percussion.dat#clap4.ogg");
	eof_sound_gas1 = eof_load_wav("gas1.wav");
	if(!eof_sound_gas1)
	{
		allegro_message("Couldn't load gas sound!");
	}
	eof_sound_gas2 = eof_load_wav("gas2.wav");
	if(!eof_sound_gas2)
	{
		allegro_message("Couldn't load gas sound!");
	}
	eof_sound_gas3 = eof_load_wav("gas3.wav");
	if(!eof_sound_gas3)
	{
		allegro_message("Couldn't load gas sound!");
	}

	for(i = 0; i < EOF_DRUM_SAMPLE_COUNT; i++)
	{
		eof_sound_drum[i] = eof_create_drum_sample(i);
		if(!eof_sound_drum[i])
			eof_log("	Warning: Couldn't create a low-latency drum preview sample", 1);
	}
}

SAMPLE *eof_mix_load_ogg_sample(char *fn)
{
//	eof_log("eof_mix_load_ogg_sample() entered");

	ALOGG_OGG * temp_ogg = NULL;
	char * buffer = NULL;
	SAMPLE *loadedsample = NULL;

	if(fn == NULL)
	{
		return NULL;
	}
	buffer = eof_buffer_file(fn, 0, 0);
	if(buffer)
	{
		temp_ogg = alogg_create_ogg_from_buffer(buffer, file_size_ex(fn));
		if(temp_ogg)
		{
			loadedsample = alogg_create_sample_from_ogg(temp_ogg);
			alogg_destroy_ogg(temp_ogg);
		}
		if(loadedsample == NULL)
			allegro_message("Couldn't load sample %s!",fn);
		else if(loadedsample->len == 0)
		{
			allegro_message("Couldn't process sample %s!",fn);
			destroy_sample(loadedsample);
		}
		free(buffer);
	}

	return loadedsample;
}

void eof_mix_exit(void)
{
	int i;

	eof_log("eof_mix_exit() entered", 1);

	destroy_sample(eof_sound_clap);
	eof_sound_clap=NULL;
	destroy_sample(eof_sound_metronome);
	eof_sound_metronome=NULL;
	destroy_sample(eof_sound_metronome_low);
	eof_sound_metronome_low=NULL;
	destroy_sample(eof_sound_grid_snap);
	eof_sound_grid_snap=NULL;
	destroy_sample(eof_sound_cowbell);
	eof_sound_cowbell=NULL;
	destroy_sample(eof_sound_tambourine1);
	eof_sound_tambourine1=NULL;
	destroy_sample(eof_sound_tambourine2);
	eof_sound_tambourine2=NULL;
	destroy_sample(eof_sound_tambourine3);
	eof_sound_tambourine3=NULL;
	destroy_sample(eof_sound_triangle1);
	eof_sound_triangle1=NULL;
	destroy_sample(eof_sound_triangle2);
	eof_sound_triangle2=NULL;
	destroy_sample(eof_sound_woodblock1);
	eof_sound_woodblock1=NULL;
	destroy_sample(eof_sound_woodblock2);
	eof_sound_woodblock2=NULL;
	destroy_sample(eof_sound_woodblock3);
	eof_sound_woodblock3=NULL;
	destroy_sample(eof_sound_woodblock4);
	eof_sound_woodblock4=NULL;
	destroy_sample(eof_sound_woodblock5);
	eof_sound_woodblock5=NULL;
	destroy_sample(eof_sound_woodblock6);
	eof_sound_woodblock6=NULL;
	destroy_sample(eof_sound_woodblock7);
	eof_sound_woodblock7=NULL;
	destroy_sample(eof_sound_woodblock8);
	eof_sound_woodblock8=NULL;
	destroy_sample(eof_sound_woodblock9);
	eof_sound_woodblock9=NULL;
	destroy_sample(eof_sound_woodblock10);
	eof_sound_woodblock10=NULL;
	destroy_sample(eof_sound_clap1);
	eof_sound_clap1=NULL;
	destroy_sample(eof_sound_clap2);
	eof_sound_clap2=NULL;
	destroy_sample(eof_sound_clap3);
	eof_sound_clap3=NULL;
	destroy_sample(eof_sound_clap4);
	eof_sound_clap4=NULL;
	destroy_sample(eof_sound_gas1);
	eof_sound_gas1=NULL;
	destroy_sample(eof_sound_gas2);
	eof_sound_gas2=NULL;
	destroy_sample(eof_sound_gas3);
	eof_sound_gas3=NULL;

	for(i = 0; i < EOF_DRUM_SAMPLE_COUNT; i++)
	{
		if(eof_sound_drum[i])
		{
			destroy_sample(eof_sound_drum[i]);
			eof_sound_drum[i] = NULL;
		}
	}

	for(i = 0; i < EOF_MAX_VOCAL_TONES; i++)
	{
		if(eof_sound_note[i] != NULL)
		{
			destroy_sample(eof_sound_note[i]);
			eof_sound_note[i]=NULL;
		}
	}
}

void eof_mix_start_helper(void)
{
	int i;

	eof_log("eof_mix_start_helper() entered", 2);

	if(!eof_music_track)
		return;

	eof_mix_find_claps();
	eof_mix_current_clap = -1;
	eof_mix_next_clap = -1;
	for(i = 0; i < eof_mix_claps; i++)
	{
		if(eof_mix_clap_pos[i] >= eof_mix_sample_count)
		{
			eof_mix_current_clap = i;
			eof_mix_next_clap = eof_mix_clap_pos[i];
			break;
		}
	}
	eof_mix_current_metronome = -1;
	eof_mix_next_metronome = -1;
	for(i = 0; i < eof_mix_metronomes; i++)
	{
		if(eof_mix_metronome_pos[i] >= eof_mix_sample_count)
		{
			eof_mix_current_metronome = i;
			eof_mix_next_metronome = eof_mix_metronome_pos[i];
			eof_mix_next_metronome_pitch = eof_mix_metronome_pos_pitch[i];	//Track which pitch the metronome tick should be
			break;
		}
	}
	eof_mix_current_note = -1;
	eof_mix_next_note = -1;
	for(i = 0; i < eof_mix_notes; i++)
	{
		if(eof_mix_note_pos[i] >= eof_mix_sample_count)
		{
			eof_mix_current_note = i;
			eof_mix_next_note = eof_mix_note_pos[i];
			break;
		}
	}
	eof_mix_current_percussion = -1;
	eof_mix_next_percussion = -1;
	for(i = 0; i < eof_mix_percussions; i++)
	{
		if(eof_mix_percussion_pos[i] >= eof_mix_sample_count)
		{
			eof_mix_current_percussion = i;
			eof_mix_next_percussion = eof_mix_percussion_pos[i];
			break;
		}
	}
	eof_mix_current_guitar_note = -1;
	eof_mix_next_guitar_note = -1;
	for(i = 0; i < eof_mix_guitar_notes; i++)
	{
		if(eof_guitar_notes[i].pos >= eof_mix_sample_count)
		{
			eof_mix_current_guitar_note = i;
			eof_mix_next_guitar_note = eof_guitar_notes[i].pos;
			break;
		}
	}
	eof_mix_current_drum_note = -1;
	eof_mix_next_drum_note = -1;
	for(i = 0; i < eof_mix_drum_notes; i++)
	{
		if(eof_drum_notes[i].pos >= eof_mix_sample_count)
		{
			eof_mix_current_drum_note = i;
			eof_mix_next_drum_note = eof_drum_notes[i].pos;
			break;
		}
	}

	if(eof_disable_sound_processing)
	{	//If callback processing is disabled
		alogg_set_buffer_callback(NULL);	//Alogg will not invoke a callback if it is NULL
	}
	else if(alogg_get_wave_is_stereo_ogg(eof_music_track))
	{	//If the chart audio is in stereo, use the stereo callback function
		alogg_set_buffer_callback(eof_mix_callback_stereo);
	}
	else
	{	//Otherwise use the mono callback function
		alogg_set_buffer_callback(eof_mix_callback_mono);
	}
}

void eof_mix_start(int speed)
{
	unsigned long i;
	int drum_volume;

	eof_log("eof_mix_start() entered", 1);

	eof_mix_next_clap = -1;
	eof_mix_next_metronome = -1;
	for(i = 0; i < EOF_MIX_MAX_CHANNELS; i++)
	{
		eof_voice[i].sp = NULL;
		eof_voice[i].pos = 0;
		eof_voice[i].fpos = 0.0;
		eof_voice[i].playing = 0;
		eof_voice[i].volume = 100;		//Default to 100% volume
		eof_voice[i].multiplier = 1.0;	//Default to 100% volume
	}
	eof_voice[0].volume = eof_clap_volume;	//Put the clap volume into effect
	eof_voice[0].multiplier = sqrt(eof_clap_volume/100.0);	//Store this math so it only needs to be performed once

	eof_voice[1].volume = eof_tick_volume;	//Put the tick volume into effect
	eof_voice[1].multiplier = sqrt(eof_tick_volume/100.0);	//Store this math so it only needs to be performed once

	eof_voice[2].volume = eof_tone_volume;	//Put the tone volume into effect
	eof_voice[2].multiplier = sqrt(eof_tone_volume/100.0);	//Store this math so it only needs to be performed once

	eof_voice[3].volume = eof_percussion_volume;	//Put the percussion volume into effect
	eof_voice[3].multiplier = sqrt(eof_percussion_volume/100.0);	//Store this math so it only needs to be performed once

	drum_volume = eof_midi_tone_volume;
	if(drum_volume < 0)
		drum_volume = 0;
	else if(drum_volume > 100)
		drum_volume = 100;
	for(i = EOF_MIX_DRUM_FIRST_CHANNEL; i < EOF_MIX_MAX_CHANNELS; i++)
	{
		eof_voice[i].volume = drum_volume;
		eof_voice[i].multiplier = sqrt(drum_volume / 100.0);
	}
	eof_mix_drum_voice_cursor = 0;

	eof_mix_speed = speed;
	eof_mix_speed_ticker = 0;
	eof_mix_sample_count = eof_mix_msec_to_sample(alogg_get_pos_msecs_ogg_ul(eof_music_track), alogg_get_wave_freq_ogg(eof_music_track));
	eof_mix_sample_increment = (1.0) * (44100.0 / (double)alogg_get_wave_freq_ogg(eof_music_track));
	eof_mix_start_helper();

	for(i = 1; i < eof_song->tracks; i++)
	{	//Pre-process all tracks so that switching tracks during playback doesn't cause the playback to lag
		eof_determine_phrase_status(eof_song, i);
	}
}

void eof_mix_seek(unsigned long pos)
{
	int i;

//	eof_log("eof_mix_seek() entered", 3);

	eof_mix_next_clap = -1;
	eof_mix_next_metronome = -1;
	eof_mix_next_note = -1;
	eof_mix_next_percussion = -1;
	eof_mix_next_guitar_note = -1;
	eof_mix_next_drum_note = -1;

	if(eof_disable_sound_processing)	//If sound cues are disabled
		return;							//Don't do anything here

	if(eof_silence_loaded)	//If no chart audio is actually loaded
		return;				//Also do nothing because playback can't occur anyway

	eof_mix_sample_count = eof_mix_msec_to_sample(pos, alogg_get_wave_freq_ogg(eof_music_track));
	for(i = 0; i < eof_mix_claps; i++)
	{
		if(eof_mix_clap_pos[i] >= eof_mix_sample_count)
		{
			eof_mix_current_clap = i;
			eof_mix_next_clap = eof_mix_clap_pos[i];
			break;
		}
	}
	for(i = 0; i < eof_mix_metronomes; i++)
	{
		if(eof_mix_metronome_pos[i] >= eof_mix_sample_count)
		{
			eof_mix_current_metronome = i;
			eof_mix_next_metronome = eof_mix_metronome_pos[i];
			eof_mix_next_metronome_pitch = eof_mix_metronome_pos_pitch[i];	//Track which pitch the metronome tick should be
			break;
		}
	}
	for(i = 0; i < eof_mix_notes; i++)
	{
		if(eof_mix_note_pos[i] >= eof_mix_sample_count)
		{
			eof_mix_current_note = i;
			eof_mix_next_note = eof_mix_note_pos[i];
			break;
		}
	}
	for(i = 0; i < eof_mix_percussions; i++)
	{
		if(eof_mix_percussion_pos[i] >= eof_mix_sample_count)
		{
			eof_mix_current_percussion = i;
			eof_mix_next_percussion = eof_mix_percussion_pos[i];
			break;
		}
	}
	for(i = 0; i < eof_mix_guitar_notes; i++)
	{
		if(eof_guitar_notes[i].pos >= eof_mix_sample_count)
		{
			eof_mix_current_guitar_note = i;
			eof_mix_next_guitar_note = eof_guitar_notes[i].pos;
			break;
		}
	}
	for(i = 0; i < eof_mix_drum_notes; i++)
	{
		if(eof_drum_notes[i].pos >= eof_mix_sample_count)
		{
			eof_mix_current_drum_note = i;
			eof_mix_next_drum_note = eof_drum_notes[i].pos;
			break;
		}
	}
}

void eof_mix_play_note(int note)
{
	if((note < EOF_MAX_VOCAL_TONES) && eof_sound_note[note])
	{
		(void) play_sample(eof_sound_note[note], 255.0 * (eof_tone_volume / 100.0), 127, 1000 + eof_audio_fine_tune, 0);	//Play the tone at the user specified cue volume
	}
}

EOF_MIDI_PLAYBACK_STATUS_STRUCT eof_midi_channel_status[16] = {0};
unsigned char eof_midi_reset_instrument = 1;

void eof_midi_play_note_ex(int note, unsigned char channel, unsigned char patch, unsigned long duration)
{
	unsigned char NOTE_ON_DATA[3] = {0x90, 0x0, 127};	//Data sequence for a Note On, channel 1, Note 0
	unsigned char NOTE_OFF_DATA[3] = {0x80, 0x0, 127};	//Data sequence for a Note Off, channel 1, Note 0
	char debug[101];

	if(midi_driver == NULL)
	{	//Ensure Allegro's MIDI driver is loaded
		return;
	}
	if(channel > 15)
	{	//Bounds check
		channel = 15;
	}
	if(eof_midi_reset_instrument)
	{	//If this function is signalled to set the instrument number on channels 0 through 5
		//Manually reset the instrument voice in effect to avoid the possibility of playing with the wrong voice if multiple EOF instances are in use
		unsigned char SET_PATCH_DATA[12] = {0xC0, 0, 0xC0 | 1, 0, 0xC0 | 2, 0, 0xC0 | 3, 0, 0xC0 | 4, 0, 0xC0 | 5, 0};	//Commands to set the instrument for channels 0 through 5
		SET_PATCH_DATA[1] = SET_PATCH_DATA[3] = SET_PATCH_DATA[5] = SET_PATCH_DATA[7] = SET_PATCH_DATA[9] = SET_PATCH_DATA[11] = patch;	//Apply the instrument number
		midi_out(SET_PATCH_DATA, 12);	//Send the twelve bytes of commands
		eof_midi_reset_instrument = 0;
	}

	NOTE_ON_DATA[0] = 0x90 | channel;
	NOTE_OFF_DATA[0] = 0x80 | channel;

	if(note < EOF_MAX_VOCAL_TONES)
	{	//If the note is valid
		if(eof_midi_channel_status[channel].on)
		{	//If the note last played on this channel has not been stopped
			(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tForcing MIDI channel %u note %u off before playing note %d", channel, eof_midi_channel_status[channel].note, note);
			eof_log(eof_log_string, 3);
			NOTE_OFF_DATA[1] = eof_midi_channel_status[channel].note;	//The note number in question
			midi_out(NOTE_OFF_DATA, 3);	//Signal the MIDI controller to turn the note off
		}
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tPlaying MIDI channel %u note %d", channel, note);
		eof_log(eof_log_string, 3);
		NOTE_ON_DATA[1] = note;		//Alter the data sequence to be the appropriate note number
		midi_out(NOTE_ON_DATA, 3);	//Turn on this note
		eof_midi_channel_status[channel].note = note;	//Track the state of this MIDI note
		eof_midi_channel_status[channel].on = 1;
		eof_midi_channel_status[channel].stop_time = clock() + (duration * CLOCKS_PER_SEC / 1000);
		if(eof_expand_notes_window_macro("ACTIVE_MIDI_TONES", debug, sizeof(debug), eof_info_panel) == 1)
		{	//Dump the MIDI channel statuses to log
			eof_log(debug, 3);
		}
	}
}

void eof_midi_set_pan(unsigned value)
{
	unsigned char PAN_DATA[18] = {0xB0, 0xA, 0, 0xB0 | 1, 0xA, 0, 0xB0 | 2, 0xA, 0, 0xB0 | 3, 0xA, 0, 0xB0 | 4, 0xA, 0, 0xB0 | 5, 0xA, 0};	//Commands to set the pan value for channels 0 through 5

	if(midi_driver == NULL)
	{	//Ensure Allegro's MIDI driver is loaded
		return;
	}

	if(value > 100)
		value = 100;
	value = (((double)value * 127.0) / 100.0) + 0.5;
	PAN_DATA[2] = PAN_DATA[5] = PAN_DATA[8] = PAN_DATA[11] = PAN_DATA[14] = PAN_DATA[17] = value;

	(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tSending MIDI pan settings: %X, %X, %u,  %X, %X, %u,  %X, %X, %u,  %X, %X, %u,  %X, %X, %u,  %X, %X, %u", PAN_DATA[0], PAN_DATA[1], PAN_DATA[2], PAN_DATA[3], PAN_DATA[4], PAN_DATA[5], PAN_DATA[6], PAN_DATA[7], PAN_DATA[8], PAN_DATA[9], PAN_DATA[10], PAN_DATA[11], PAN_DATA[12], PAN_DATA[13], PAN_DATA[14], PAN_DATA[15], PAN_DATA[16], PAN_DATA[17]);
	eof_log(eof_log_string, 1);
	midi_out(PAN_DATA, 18);	//Send the eighteen bytes of commands
}

void eof_midi_set_volume(unsigned value)
{
	unsigned char VOL_DATA[18] = {0xB0, 0x7, 0, 0xB0 | 1, 0x7, 0, 0xB0 | 2, 0x7, 0, 0xB0 | 3, 0x7, 0, 0xB0 | 4, 0x7, 0, 0xB0 | 5, 0x7, 0};	//Commands to set the volume for channels 0 through 5

	if(midi_driver == NULL)
	{	//Ensure Allegro's MIDI driver is loaded
		return;
	}

	if(value > 100)
		value = 100;
	value = (((double)value * 127.0) / 100.0) + 0.5;
	VOL_DATA[2] = VOL_DATA[5] = VOL_DATA[8] = VOL_DATA[11] = VOL_DATA[14] = VOL_DATA[17] = value;

	(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tSending MIDI volume settings: %X, %X, %u,  %X, %X, %u,  %X, %X, %u,  %X, %X, %u,  %X, %X, %u,  %X, %X, %u", VOL_DATA[0], VOL_DATA[1], VOL_DATA[2], VOL_DATA[3], VOL_DATA[4], VOL_DATA[5], VOL_DATA[6], VOL_DATA[7], VOL_DATA[8], VOL_DATA[9], VOL_DATA[10], VOL_DATA[11], VOL_DATA[12], VOL_DATA[13], VOL_DATA[14], VOL_DATA[15], VOL_DATA[16], VOL_DATA[17]);
	eof_log(eof_log_string, 1);
	midi_out(VOL_DATA, 18);	//Send the eighteen bytes of commands
}

void eof_update_midi_timers(void)
{
	unsigned char NOTE_OFF_DATA[3] = {0x80, 0x0, 127};	//Data sequence for a Note Off, channel 1, Note 0
	clock_t current_time = clock();
	unsigned channel;

	if(midi_driver == NULL)
	{	//Ensure Allegro's MIDI driver is loaded
		return;
	}

	for(channel = 0; channel < 6; channel++)
	{	//For each of the MIDI channels being tracked in eof_midi_channel_status
		if(eof_midi_channel_status[channel].on)
		{	//If a note on this channel is still on
			if(current_time >= eof_midi_channel_status[channel].stop_time)
			{	//If it is at or beyond the time to stop the note
				(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tGracefully ending MIDI channel %u note %d", channel, eof_midi_channel_status[channel].note);
				eof_log(eof_log_string, 3);
				NOTE_OFF_DATA[0] = 0x80 | channel;
				NOTE_OFF_DATA[1] = eof_midi_channel_status[channel].note;	//The note number in question
				midi_out(NOTE_OFF_DATA, 3);	//Signal the MIDI controller to turn the note off
				eof_midi_channel_status[channel].on = 0;
			}
		}
	}
}

void eof_play_pro_guitar_note_midi(EOF_SONG *sp, unsigned long track, unsigned long note)
{
	EOF_PRO_GUITAR_TRACK *tp;
	unsigned long tracknum, ctr, bitmask;
	int tone;

	if(!sp || !track || (track >= sp->tracks) || (!eof_track_is_pro_guitar_track(sp, track)))
		return;	//Invalid parameters

	tracknum = sp->track[track]->tracknum;
	tp = sp->pro_guitar_track[tracknum];

	if(note >= tp->notes)
		return;	//Invalid parameters
	tone = eof_lookup_midi_tone(sp, track, note);

	eof_midi_reset_instrument = 1;	//Ensure the active track's MIDI instrument gets set for MIDI channels 0 through 5
	for(ctr = 0, bitmask = 1; ctr < 6; ctr++, bitmask <<= 1)
	{	//For each of the 6 supported strings
		if((tp->note[note]->note & bitmask) && !(tp->note[note]->frets[ctr] & 0x80))
		{	//If this string is used (and not muted)
			//This note is found by adding default tuning for the string, the offset defining the current tuning and the fret number being played
			eof_midi_play_note_ex(tp->tuning[ctr] + eof_lookup_default_string_tuning_absolute(tp, track, ctr) + tp->note[note]->frets[ctr] + tp->capo, ctr, tone, 1000);	//Play the MIDI note for 1000ms
		}
	}
}

int eof_lookup_midi_tone(EOF_SONG *sp, unsigned long track, unsigned long note)
{
	EOF_PRO_GUITAR_TRACK *tp;
	unsigned long tracknum;

	if(!sp || !track || (track >= sp->tracks) || (!eof_track_is_pro_guitar_track(sp, track)))
		return 0;	//Invalid parameters

	tracknum = sp->track[track]->tracknum;
	tp = sp->pro_guitar_track[tracknum];
	if(note >= tp->notes)
		return 0;	//Invalid parameters
	if(tp->arrangement == EOF_BASS_ARRANGEMENT)
	{	//If this track's arrangement type is bass
		return eof_midi_synth_instrument_bass;
	}
	if(tp->note[note]->flags & EOF_PRO_GUITAR_NOTE_FLAG_PALM_MUTE)
	{	//If this guitar note is palm muted
		return eof_midi_synth_instrument_guitar_muted;
	}
	if(tp->note[note]->flags & EOF_PRO_GUITAR_NOTE_FLAG_HARMONIC)
	{	//If this guitar note is played as a harmonic
		return eof_midi_synth_instrument_guitar_harm;
	}
	return eof_midi_synth_instrument_guitar;
}

/*
struct ALOGG_OGG {
  // data info
  void *data;                      // ogg data
  char *data_cursor;               // pointer to data being read
  int data_len;                    // size of the data
  // decoder info
  OggVorbis_File vf;
  int current_section;
  // playing info
  AUDIOSTREAM *audiostream;        // the audiostream we are using to play
                                   // also used to know when it's playing
  int audiostream_buffer_len;      // len of the audiostream buffer
  int stereo, freq, loop;          // audio general info
  int auto_polling;                // set if the ogg is auto polling
  int auto_poll_speed;             // auto poll speed in msecs
  int wait_for_audio_stop;         // set if we are just waiting for the
                                   // audiobuffer to stop plaing the last
                                   // frame
};
*/

void eof_set_seek_position(unsigned long pos)
{
	if(eof_music_track)
	{	//If chart audio is loaded
		alogg_seek_abs_msecs_ogg_ul(eof_music_track, pos);
		eof_set_music_pos(&eof_music_pos, pos);
		eof_music_actual_pos = eof_music_pos.value;
		eof_mix_seek(eof_music_actual_pos);
		eof_reset_lyric_preview_lines();
	}
}

void eof_play_queued_midi_tones(void)
{
	while((eof_mix_sample_count >= eof_mix_next_guitar_note) && (eof_mix_current_guitar_note < eof_mix_guitar_notes))
	{	// Using a while loop to allow all notes in a chord to fire at the same time
		if(eof_mix_midi_tones_enabled)
		{
			unsigned long length = eof_guitar_notes[eof_mix_current_guitar_note].length;	//By default, play the MIDI tone at the originating note's length
			if(length < 250)
				length = 250;	//Unless it's shorter than 250ms, in which case pad it so it plays long enough to be heard
			eof_midi_play_note_ex(eof_guitar_notes[eof_mix_current_guitar_note].note, eof_guitar_notes[eof_mix_current_guitar_note].channel, eof_guitar_notes[eof_mix_current_guitar_note].tone, length);	//Play the MIDI note
		}
		eof_mix_current_guitar_note++;
		eof_mix_next_guitar_note = eof_guitar_notes[eof_mix_current_guitar_note].pos;
	}
}

void eof_midi_play_drum_note(unsigned char note)
{
	/* Kept as the public immediate-preview entry point, but deliberately does
	 * not use midi_out(): the OS MIDI synth adds audible latency. */
	if(eof_midi_tone_volume > 0)
		eof_mix_trigger_drum_sample(note);
}

void eof_play_queued_drum_tones(void)
{
	while((eof_mix_sample_count >= eof_mix_next_drum_note) && (eof_mix_current_drum_note < eof_mix_drum_notes))
	{	//A while loop lets every piece in a simultaneous DTX hit fire on the exact same audio sample.
		if(eof_mix_drum_tones_enabled)
			eof_midi_play_drum_note(eof_drum_notes[eof_mix_current_drum_note].note);
		eof_mix_current_drum_note++;
		if(eof_mix_current_drum_note < eof_mix_drum_notes)
			eof_mix_next_drum_note = eof_drum_notes[eof_mix_current_drum_note].pos;
		else
			eof_mix_next_drum_note = (unsigned long)-1;
	}
}

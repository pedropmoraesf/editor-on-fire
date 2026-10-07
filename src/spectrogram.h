#ifndef EOF_SPECTROGRAM_H
#define EOF_SPECTROGRAM_H
#include <fftw3.h>

#define DEFAULT_STARTFREQ 27.5
#define DEFAULT_ENDFREQ 4186
#define MINFREQ 27.5
#define EOF_EXPERIMENTAL_SPECTROGRAM_WINDOWSIZE 4096
#define EOF_EXPERIMENTAL_SPECTROGRAM_HOPSIZE 1024
#define EOF_EXPERIMENTAL_SPECTROGRAM_DB_RANGE 90.0

struct spectrogramslice
{
	double *amplist;	 //The list of absolute amplitudes for each frequency band
};

struct spectrogramchanneldata
{
	struct spectrogramslice *slices;	//The spectrogram data for this channel
	unsigned maxamp;					//The highest amplitude of samples in this channel
	unsigned long yaxis;				//The y coordinate representing the y axis the channel's graph will render to
	unsigned long height;				//The height of this channel's graph
	unsigned long halfheight;			//One half of the channel's graph height (cached in eof_render_spectrogram() to avoid recalculating for each line)
	double logheight;					//The logarithm of the height (cached in eof_render_spectrogram() to avoid recalculating for each column)
};

//A cached map of pixels to frequency bins
struct spectrogrampxtofreq
{
	unsigned long *map;

	char dirty;
	int height;
	char is_log;
};

struct spectrogramstruct
{
	char *oggfilename;
	double *buffin;						//Allow multiple input/output buffers, for overlap
	double *buffout;
	short int numbuff;					//Number of buffers to overlap
	int windowsize;						//FFT analysis window size used to build this spectrogram
	int hopsize;							//Samples advanced between successive analysis frames
	char window_function;				//0 = rectangular (legacy), 1 = Hann (experimental)
	double windowlength;				//The length of one slice of the graph in milliseconds
	unsigned long numslices;			//The number of spectrogram structures in the arrays below
	unsigned int zeroamp;				//The amplitude representing a 0 amplitude for this spectrogram (32768 for 16 bit audio samples, 128 for 8 bit audio samples)
	char is_stereo;						//This OGG has two audio channels
	double destmax;
	double displaymax;					//Robust high-percentile reference used by the experimental display
	unsigned char *experimental_cache;		//Pre-rendered HQ log-frequency intensity cache for lag-free playback
	unsigned long experimental_cache_bands;
	double experimental_cache_fmin;
	double experimental_cache_fmax;
	double log_max;						//Will store the logarithm of the product of the window size and the the spectrogram's zeroamp value
	long rate;

	struct spectrogrampxtofreq px_to_freq;

	struct spectrogramchanneldata left;	//The amplitude and graph data for the audio's left channel
	struct spectrogramchanneldata right;	//The amplitude and graph data for the audio's right channel (if applicable)
};

//A colortable for a color scale, to map values to colors
struct spectrogramcolorscalestruct
{
	int scalenum;
	int maxval;
	int *colortable;
};

extern struct spectrogramstruct *eof_spectrogram;	//Stores the spectrogram data
extern struct spectrogramcolorscalestruct *eof_spectrogram_colorscale;  //Stores the current colorscale being used
extern char eof_display_spectrogram;				//Specifies whether the spectrogram display is enabled
extern char eof_display_spectrogram_experimental;	//Specifies whether the experimental spectrogram display is enabled
extern char eof_spectrogram_renderlocation;			//Specifies where and how high the graph will render (0 = fretboard area, 1 = editor window)
extern char eof_spectrogram_renderleftchannel;		//Specifies whether the left channel's graph should render
extern char eof_spectrogram_renderrightchannel;		//Specifies whether the right channel's graph should render
extern char eof_spectrogram_colorscheme;			//Specifies the index of the colorscale being used
extern int eof_spectrogram_windowsize;				//Specifies the window size for the spectrogram
extern double eof_half_spectrogram_windowsize;		//Caches the value of eof_spectrogram_windowsize / 2
extern double eof_spectrogram_startfreq;			//For displaying a subset of the frequency range, the lower frequency
extern double eof_spectrogram_endfreq;				//For displaying a subset of the frequency range, the higher frequency
extern int eof_spectrogram_userange;				//Specifies whether to display the selected subset
extern int eof_spectrogram_logplot;					//Specifies whether to graph the y-axis on a log scale
extern int eof_spectrogram_avgbins;					//Specifies whether to average the bins within a pixel

//Axis helper functions
double eof_spectrogram_y_from_freq(long rate, double freq);
	//Converts a given frequency to a y value from 0 to 1
double eof_spectrogram_freq_from_y(long rate, double y);
	//Converts a given y value to a frequency
void eof_spectrogram_calculate_px_to_freq(struct spectrogramstruct *spectrogram);
	//Regenerates the px_to_freq table

void eof_destroy_spectrogram(struct spectrogramstruct *ptr);
	//frees memory used by the specified spectrogram structure
int eof_render_spectrogram(struct spectrogramstruct *spectrogram);
	//Renders the left channel spectrogram into the editor window, taking the zoom level into account
	//Returns nonzero on failure
int eof_render_spectrogram_experimental(struct spectrogramstruct *spectrogram);
	//Renders the HQ experimental spectrogram (4096-point Hann STFT, 75% overlap, robust 99.5% ceiling and precomputed adaptive contrast cache)
int eof_spectrogram_auto_sync_notes(const char *audio_filename, unsigned long track, unsigned char diff, char selected_only, unsigned long max_shift_ms, unsigned long *events_considered, unsigned long *events_moved, double *mean_abs_shift);
	//High-resolution conservative timing refinement: SuperFlux-style novelty, harmonic/percussive cues, pitch-class evidence, original-timing prior and anchor-constrained sequence alignment.
void eof_render_spectrogram_line(struct spectrogramstruct *spectrogram,struct spectrogramchanneldata *channel,unsigned amp,unsigned long x,int color);
	//Debugging function used for testing a colorscale, draws a line of a given color on the spectrogram
void eof_render_spectrogram_col(struct spectrogramstruct *spectrogram,struct spectrogramchanneldata *channel,struct spectrogramslice *ampdata, unsigned long x, unsigned long curms);
	//Given the amplitude and the channel and spectrogram to process, renders a column of the channel's spectrogram at x for the time curms
int eof_color_scale(double value, double max, short int scalenum);
	//Accessor for the color scale
void eof_generate_colorscale(char scalenum);
	//Generates color scales

struct spectrogramstruct *eof_create_spectrogram(char *oggfilename);
struct spectrogramstruct *eof_create_spectrogram_experimental(char *oggfilename);
	//Creates the experimental 4096-point Hann/75%-overlap spectrogram and its precomputed HQ display cache
	//Decompresses the specified OGG file into memory and creates spectrogram data
	//windowlength is the length of one spectrogram graph slice in milliseconds
	//The correct number of samples are used to represent each column (slice) of the graph
	//The spectrogram data is returned, otherwise NULL is returned upon error

int eof_process_next_spectrogram_slice(struct spectrogramstruct *spectrogram,SAMPLE *audio,unsigned long slicenum,fftw_plan fftplan);
	//Processes spectrogram->slicesize number of audio samples, or if there are not enough, the remainder of the samples, storing the peak amplitude and RMS into spectrogram->slices[slicenum]
	//If the audio is stereo, the data for the right channel is likewise processed and stored into spectrogram->slices2[slicenum]
	//Returns 0 on success, 1 when all samples are exhausted or -1 on error

#endif

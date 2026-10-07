#include <math.h>	//For sqrt()
#include <limits.h>
#include <stdlib.h>
#include <allegro.h>
#include "utility.h"
#include "spectrogram.h"
#include "song.h"
#include "main.h"
#include "undo.h"
#include "midi.h"
#include "dtx_integration.h"

#ifdef USEMEMWATCH
#include "memwatch.h"
#endif

struct spectrogramstruct *eof_spectrogram = NULL;	//Stores the spectrogram data
struct spectrogramcolorscalestruct *eof_spectrogram_colorscale=NULL;	//Stores the current colorscale being used
char eof_display_spectrogram = 0;					//Specifies whether the spectrogram display is enabled
char eof_display_spectrogram_experimental = 0;		//Experimental music-oriented spectrogram display
static unsigned long *eof_spectrogram_experimental_binmap = NULL;
static int eof_spectrogram_experimental_binmap_height = 0;
static int eof_spectrogram_experimental_binmap_windowsize = 0;
static long eof_spectrogram_experimental_binmap_rate = 0;
char eof_spectrogram_renderlocation = 0;			//Specifies where and how high the graph will render (0 = fretboard area, 1 = editor window)
char eof_spectrogram_renderleftchannel = 1;			//Specifies whether the left channel's graph should render
char eof_spectrogram_renderrightchannel = 0;		//Specifies whether the right channel's graph should render
char eof_spectrogram_colorscheme = 1;				//Specifies the index of the colorscale being used
int eof_spectrogram_windowsize = 1024;				//Specifies the window size for the spectrogram
double eof_half_spectrogram_windowsize = 512.0;		//Caches the value of eof_spectrogram_windowsize / 2
double eof_spectrogram_startfreq = DEFAULT_STARTFREQ;	//For displaying a subset of the frequency range, the lower frequency
double eof_spectrogram_endfreq = DEFAULT_ENDFREQ;		//For displaying a subset of the frequency range, the higher frequency
int eof_spectrogram_userange = 0;					//Specifies whether to display the selected subset
int eof_spectrogram_logplot = 1;					//Specifies whether to graph the y-axis on a log scale
int eof_spectrogram_avgbins = 0;					//Specifies whether to average the bins within a pixel

void eof_destroy_spectrogram(struct spectrogramstruct *ptr)
{
	unsigned long ctr;

 	eof_log("eof_destroy_spectrogram() entered", 1);

	if(ptr)
	{
		if(ptr->buffin)
			fftw_free(ptr->buffin);
		if(ptr->buffout)
			fftw_free(ptr->buffout);
		if(ptr->oggfilename)
			free(ptr->oggfilename);
		if(ptr->left.slices)
		{
			for(ctr = 0; ctr < ptr->numslices; ctr++)
			{	//For each slice of spectrogram data
				if(ptr->left.slices[ctr].amplist)
					free(ptr->left.slices[ctr].amplist);
			}
			free(ptr->left.slices);
		}
		if(ptr->right.slices)
		{
			for(ctr = 0; ctr < ptr->numslices; ctr++)
			{	//For each slice of spectrogram data
				if(ptr->right.slices[ctr].amplist)
					free(ptr->right.slices[ctr].amplist);
			}
			free(ptr->right.slices);
		}
		if(ptr->px_to_freq.map)
			free(ptr->px_to_freq.map);
		if(ptr->experimental_cache)
			free(ptr->experimental_cache);
		free(ptr);
	}

	//Destroy any colorscale data
	if(eof_spectrogram_colorscale != NULL)
	{
		if(eof_spectrogram_colorscale->colortable != NULL)
		{
			free(eof_spectrogram_colorscale->colortable);
		}
		free(eof_spectrogram_colorscale);
	}
	eof_spectrogram_colorscale = NULL;
	if(eof_spectrogram_experimental_binmap)
	{
		free(eof_spectrogram_experimental_binmap);
		eof_spectrogram_experimental_binmap = NULL;
	}
	eof_spectrogram_experimental_binmap_height = 0;
	eof_spectrogram_experimental_binmap_windowsize = 0;
	eof_spectrogram_experimental_binmap_rate = 0;
}

int eof_render_spectrogram(struct spectrogramstruct *spectrogram)
{
	unsigned long x,startpixel;
	unsigned long ycoord1,ycoord2;	//Stores the Y coordinates of graph 1's and 2's Y axis
	unsigned long height;		//Stores the heigth of the fretboard area
	unsigned long top,bottom;	//Stores the top and bottom coordinates for the area the graph will render to
	char numgraphs;				//Stores the number of channels to render
	unsigned long pos = eof_music_pos.value / eof_zoom;
	unsigned long curms;

//	eof_log("eof_render_spectrogram() entered", 1);

//validate input
	if(!eof_song_loaded || !spectrogram)
		return 1;	//Return error
	if(!spectrogram->left.slices)
		return 1;	//Return error if the spectrogram graph has no left/mono channel data
	if(spectrogram->is_stereo && !spectrogram->right.slices)
		return 1;	//Return error if the stereo spectrogram graph has no right channel data

//determine how many channels will be graphed
	if(spectrogram->is_stereo)
	{	//Take both channels into account
		numgraphs = eof_spectrogram_renderleftchannel + eof_spectrogram_renderrightchannel;
	}
	else
	{	//Take only the first channel into account
		numgraphs = eof_spectrogram_renderrightchannel;
	}
	if(numgraphs == 0)	//If user specified not to render either channel, or to render the right channel on a mono audio file
		return 0;

//determine timestamp of the left visible edge of the piano roll, which will be in ms, the same as the length of each spectrogram slice
	curms = eof_determine_piano_roll_left_edge();

//determine which pixel is the left visible edge of the piano roll
	if(pos < 300)
	{
		startpixel = 20;
	}
	else if(pos < 320)
	{
		startpixel = 320 - pos;
	}
	else
	{
		startpixel = 0;
	}

//determine the top and bottom boundary for the graphing area
	if(eof_spectrogram_renderlocation == 0)
	{	//Render one or both channels' graphs into the fretboard area
		if(eof_selected_track == EOF_TRACK_VOCALS)
		{	//Set the top boundary 1 pixel below the lyric lane (at the top of the fretboard area in the vocal editor)
			top = EOF_EDITOR_RENDER_OFFSET + 15 + eof_screen_layout.lyric_y + 1 + 16 + 1;
		}
		else
		{	//Set the top boundary to the top of the fretboard area
			top = EOF_EDITOR_RENDER_OFFSET + 25;
		}

		bottom = EOF_EDITOR_RENDER_OFFSET + eof_screen_layout.fretboard_h - 1;	//Set the bottom boundary to the bottom of the fretboard area
	}
	else
	{	//Render one or both channels' graphs into the editor window
		top = 32;												//Set the top of the editor window
		bottom = eof_screen_layout.scrollbar_y - 1;				//Set the bottom boundary to just above the scroll bar
	}

//determine the y axis location and graph height of each channel's graph
	height = (bottom - top) / numgraphs;
	ycoord1 = top + (height / 2);	//The first graph will render with respect to the top of the graphing area
	if(numgraphs == 1)
	{
		if(eof_spectrogram_renderleftchannel)
		{	//If only rendering the left channel
			spectrogram->left.height = height;
			spectrogram->left.yaxis = ycoord1;
			spectrogram->left.halfheight = height / 2;
			spectrogram->left.logheight = log(height);
		}
		else
		{	//If only rendering the right channel
			spectrogram->right.height = height;
			spectrogram->right.yaxis = ycoord1;
			spectrogram->right.halfheight = height / 2;
			spectrogram->right.logheight = log(height);
		}
	}
	else if(numgraphs == 2)
	{
		ycoord2 = bottom - (height / 2);	//This graph will take 1/2 the entire graphing area, oriented at the bottom

		spectrogram->left.height = height;
		spectrogram->left.yaxis = ycoord1;
		spectrogram->right.height = height;
		spectrogram->right.yaxis = ycoord2;
		spectrogram->left.halfheight = spectrogram->right.halfheight = height / 2;
		spectrogram->left.logheight = spectrogram->right.logheight = log(height / 2);
	}
	else
	{	//Do not render anything unless it's the graph for 1 or 2 channels
		return 1;
	}

	//Calculate the y axis bins
	eof_spectrogram_calculate_px_to_freq(spectrogram);

//render graph from left to right, one pixel at a time (each pixel represents eof_zoom number of milliseconds of audio)
	//for(x=startpixel;x < eof_window_editor->w;x++)
	for(x=startpixel;x < eof_window_editor->w;x++,curms+=eof_zoom)
	{	//for each pixel in the piano roll's visible width
		if(eof_spectrogram_renderleftchannel)
		{	//If the left channel rendering is enabled
			eof_render_spectrogram_col(spectrogram,&spectrogram->left,spectrogram->left.slices,x,curms);
		}

		if(eof_spectrogram_renderrightchannel)
		{	//If the right channel rendering is enabled
			eof_render_spectrogram_col(spectrogram,&spectrogram->right,spectrogram->right.slices,x,curms);
		}
	}

	return 0;
}

/**
 * Pre-calculate a table that maps pixels on the y-axis to frequency bins
 *
 * This function populates the px_to_freq table in the spectrogram that is
 * used to avoid converting pixels to frequencies repeatedly, which is costly
 * especially in the case of log plots.
 *
 * This function checks itself if the table needs to be recalculated and simply
 * returns if the old version is usable, so it can be called on each render
 *
 * @param spectrogram The spectrogramstruct of the given spectrogram
 */
void eof_spectrogram_calculate_px_to_freq(struct spectrogramstruct *spectrogram)
{
	int height = 0, y;
	double binsize, a, b, yfrac, freq;
	double startfreq = DEFAULT_STARTFREQ;
	double endfreq = DEFAULT_ENDFREQ;

	//Determine the height we should use from the channels
	if(spectrogram->left.height)
	{
		height = spectrogram->left.height;
	}
	if(spectrogram->right.height > height)
	{
		height = spectrogram->right.height;
	}
	if(!height)
		return;

	//Just in case it didn't get marked
	if(height != spectrogram->px_to_freq.height)
	{
		spectrogram->px_to_freq.dirty = 1;
	}

	spectrogram->px_to_freq.height = height;

	if(spectrogram->px_to_freq.dirty == 0)
	{
		//eof_log("Using existing px_to_freq table",1);
		return;
	}

	eof_log("Recreating px_to_freq table",1);
	spectrogram->px_to_freq.dirty = 0;

	if(spectrogram->px_to_freq.map)
		free(spectrogram->px_to_freq.map);

	spectrogram->px_to_freq.map = (unsigned long *)malloc((height+1) * sizeof(unsigned long));
	if(spectrogram->px_to_freq.map == NULL)
	{
		eof_log("Couldn't allocate memory for px_to_freq map!",1);
		return;
	}

	binsize=((double)spectrogram->rate/2.0)/(double)spectrogram->windowsize;

	if(eof_spectrogram_userange)
	{ //Assign custom start/end if we're using it
		startfreq = eof_spectrogram_startfreq;
		endfreq = eof_spectrogram_endfreq;
	}

	a = eof_spectrogram_y_from_freq(spectrogram->rate, startfreq);
	b = eof_spectrogram_y_from_freq(spectrogram->rate, endfreq);
	yfrac = (b-a)/height;

	for(y=0;y <= height;y++)
	{
		freq = eof_spectrogram_freq_from_y(spectrogram->rate, a + (double)y*yfrac);
		spectrogram->px_to_freq.map[y] = (unsigned long)ceil(freq/binsize);
	}
}

/**
 * A safe accessor for the px_to_freq table
 *
 * Ensures the given pixel value is in the range of available values
 * and that the px_to_freq map is populated properly
 *
 * @param spectrogram The spectrogram data structure
 * @param px The pixel value to be looked up
 * @return The frequency bin represented by that pixel
 */
unsigned long spectrogram_get_freq_from_px(struct spectrogramstruct *spectrogram, int px)
{
	if(spectrogram->px_to_freq.map == NULL)
	{
		return 0;
	}

	if(px > 0 && px <= spectrogram->px_to_freq.height)
	{
		return spectrogram->px_to_freq.map[px];
	}

	return 0;
}

/**
 * Helper function to calculate the frequency given a percentage of the y axis
 *
 * Checks for log scale or not and calculates accordingly
 *
 * @param rate The sampling rate of the audio, which determines the max
 * @param y A value from 0 to 1 representing the position on the y axis
 * @return The frequency at that position
 */
double eof_spectrogram_freq_from_y(long rate, double y)
{
	if(eof_spectrogram_logplot)
	{
		return MINFREQ * pow(((double)rate/2.0) / MINFREQ, y);
	}

	return MINFREQ + y * (((double)rate/2.0) - MINFREQ);
}

/**
 * Helper function to calculate the y position for a given frequency
 *
 * Checks for log scale or not and calculates accordingly
 *
 * @param rate The sampling rate of the audio, which determines the max
 * @param freq The frequency being plotted
 * @return The position on the y axis, from 0 to 1, of that frequency
 */
double eof_spectrogram_y_from_freq(long rate, double freq)
{
	double lograte;
	if(eof_spectrogram_logplot)
	{
		lograte = log(((double)rate/2.0) / MINFREQ);	//Cache this to avoid Splint's nag about multiple calls to log() in one line of code, obscuring which one may have returned an error through errno
		return log(freq / MINFREQ) / lograte;
	}

	return (freq - MINFREQ) / (((double)rate/2.0) - MINFREQ);
}

void eof_render_spectrogram_col(struct spectrogramstruct *spectrogram,struct spectrogramchanneldata *channel,struct spectrogramslice *ampdata, unsigned long x, unsigned long curms)
{
	unsigned long yoffset;	//The offset from the y axis coordinate to render the line to
	unsigned long curslice;
	unsigned long actualzero;
	double val;
	unsigned long cursamp;
	unsigned long nextsamp;
	unsigned long sampoffset;

	if(spectrogram != NULL)
	{
		actualzero = channel->yaxis + channel->halfheight;
		curslice = curms / spectrogram->windowlength;
		if(curslice >= spectrogram->numslices)
		{	//Avoid a buffer overread
			return;
		}
		for(yoffset=0;yoffset < channel->height-1;yoffset++)
		{
			//Find the bins for these frequencies
			cursamp = spectrogram_get_freq_from_px(spectrogram, yoffset);
			if(eof_spectrogram_avgbins)
			{
				nextsamp = spectrogram_get_freq_from_px(spectrogram, yoffset+1);
				if(cursamp == nextsamp)
				{
					nextsamp = cursamp + 1;
				}

				//Average the samples to get a gray value
				val = 0.0;
				for(sampoffset = cursamp; sampoffset < nextsamp; sampoffset++)
				{	//Break from loop before a buffer overread can occur
					val += ampdata[curslice].amplist[sampoffset];
				}
				val = val/(double)(nextsamp - cursamp);
			}
			else
			{
				val = ampdata[curslice].amplist[cursamp];
			}

			putpixel(eof_window_editor->screen, x, actualzero - yoffset, eof_color_scale(log(val),spectrogram->log_max,eof_spectrogram_colorscheme));
			//To test a color scale
			//putpixel(eof_window_editor->screen, x, actualzero - yoffset, eof_color_scale(yoffset,channel->height,eof_spectrogram_colorscheme));
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Experimental, music-oriented spectrogram display                           */
/* ------------------------------------------------------------------------- */

#define EOF_EXPERIMENTAL_SPECTROGRAM_CACHE_BANDS 768UL

static int eof_spectrogram_experimental_palette(double value)
{
	static int palette[256];
	static char initialized = 0;
	static const int anchors[6][3] =
	{
		{0, 0, 4},
		{47, 17, 92},
		{116, 31, 109},
		{184, 55, 86},
		{245, 135, 48},
		{252, 253, 191}
	};
	int index;

	if(!initialized)
	{
		int ctr;
		for(ctr = 0; ctr < 256; ctr++)
		{
			double scaled = ((double)ctr / 255.0) * 5.0;
			double fraction;
			int anchor = (int)scaled;
			int r, g, b;
			if(anchor >= 5)
			{
				palette[ctr] = makecol(anchors[5][0], anchors[5][1], anchors[5][2]);
				continue;
			}
			fraction = scaled - (double)anchor;
			r = (int)(anchors[anchor][0] + (anchors[anchor + 1][0] - anchors[anchor][0]) * fraction + 0.5);
			g = (int)(anchors[anchor][1] + (anchors[anchor + 1][1] - anchors[anchor][1]) * fraction + 0.5);
			b = (int)(anchors[anchor][2] + (anchors[anchor + 1][2] - anchors[anchor][2]) * fraction + 0.5);
			palette[ctr] = makecol(r, g, b);
		}
		initialized = 1;
	}

	if(value < 0.0) value = 0.0;
	if(value > 1.0) value = 1.0;
	index = (int)floor(value * 255.0 + 0.5);
	if(index < 0) index = 0;
	if(index > 255) index = 255;
	return palette[index];
}

static double eof_spectrogram_experimental_band_rms(struct spectrogramstruct *spectrogram, unsigned long slice, unsigned long firstbin, unsigned long endbin)
{
	unsigned long bin, halfsize, count = 0;
	double power = 0.0, peak = 0.0, left, right, magnitude;

	if(!spectrogram || (slice >= spectrogram->numslices) || !spectrogram->left.slices[slice].amplist)
		return 0.0;
	halfsize = (unsigned long)spectrogram->windowsize / 2UL;
	if(firstbin < 1UL) firstbin = 1UL;
	if(firstbin > halfsize) firstbin = halfsize;
	if(endbin <= firstbin) endbin = firstbin + 1UL;
	if(endbin > halfsize + 1UL) endbin = halfsize + 1UL;

	for(bin = firstbin; bin < endbin; bin++)
	{
		left = spectrogram->left.slices[slice].amplist[bin];
		if(spectrogram->is_stereo && spectrogram->right.slices && spectrogram->right.slices[slice].amplist)
		{
			right = spectrogram->right.slices[slice].amplist[bin];
			magnitude = sqrt((left * left + right * right) * 0.5);
		}
		else
		{
			magnitude = fabs(left);
		}
		power += magnitude * magnitude;
		if(magnitude > peak)
			peak = magnitude;
		count++;
	}
	if(!count)
		return 0.0;
	/* RMS keeps broadband/percussive energy honest.  A small peak component
	 * prevents narrow guitar/bass harmonics from disappearing when several FFT
	 * bins are collapsed into one logarithmic display pixel. */
	return 0.84 * sqrt(power / (double)count) + 0.16 * peak;
}

static int eof_spectrogram_experimental_prepare_binmap(struct spectrogramstruct *spectrogram, int height, double fmin, double fmax)
{
	int y;
	unsigned long halfsize, bin;
	double fraction, frequency;

	if(!spectrogram || (height < 2) || (fmax <= fmin))
		return 0;
	if(eof_spectrogram_experimental_binmap &&
	   (eof_spectrogram_experimental_binmap_height == height) &&
	   (eof_spectrogram_experimental_binmap_windowsize == spectrogram->windowsize) &&
	   (eof_spectrogram_experimental_binmap_rate == spectrogram->rate))
		return 1;

	if(eof_spectrogram_experimental_binmap)
		free(eof_spectrogram_experimental_binmap);
	eof_spectrogram_experimental_binmap = (unsigned long *)malloc((height + 1) * sizeof(unsigned long));
	if(!eof_spectrogram_experimental_binmap)
		return 0;

	halfsize = (unsigned long)spectrogram->windowsize / 2UL;
	for(y = 0; y <= height; y++)
	{
		fraction = (double)y / (double)height;
		frequency = fmin * pow(fmax / fmin, fraction);
		bin = (unsigned long)(frequency * (double)spectrogram->windowsize / (double)spectrogram->rate);
		if(bin < 1UL) bin = 1UL;
		if(bin > halfsize) bin = halfsize;
		eof_spectrogram_experimental_binmap[y] = bin;
	}
	eof_spectrogram_experimental_binmap_height = height;
	eof_spectrogram_experimental_binmap_windowsize = spectrogram->windowsize;
	eof_spectrogram_experimental_binmap_rate = spectrogram->rate;
	return 1;
}

static void eof_spectrogram_experimental_draw_guides(int top, int bottom, unsigned long startpixel, double fmin, double fmax)
{
	static const double guidefreq[] = {32.7032, 65.4064, 130.8128, 261.6256, 523.2511, 1046.5023, 2093.0045, 4186.0090, 8372.0181, 16744.0362};
	static const char *guidename[] = {"C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9", "C10"};
	unsigned long i;
	int y, height, labelright, legendx, legendtop, legendbottom, py;
	double fraction, norm;

	height = bottom - top;
	if(height < 2)
		return;

	for(i = 0; i < sizeof(guidefreq) / sizeof(guidefreq[0]); i++)
	{
		if((guidefreq[i] < fmin) || (guidefreq[i] > fmax))
			continue;
		fraction = log(guidefreq[i] / fmin) / log(fmax / fmin);
		y = bottom - (int)(fraction * (double)height + 0.5);
		hline(eof_window_editor->screen, (int)startpixel, y, eof_window_editor->w - 1, makecol(62, 62, 72));
		rectfill(eof_window_editor->screen, (int)startpixel + 2, y - 5, (int)startpixel + 65, y + 5, makecol(4, 4, 8));
		textprintf_ex(eof_window_editor->screen, font, (int)startpixel + 4, y - 4, makecol(220, 220, 225), -1, "%s %.0fHz", guidename[i], guidefreq[i]);
	}

	labelright = (int)startpixel + 300;
	if(labelright > eof_window_editor->w - 70)
		labelright = eof_window_editor->w - 70;
	if(labelright > (int)startpixel + 20)
	{
		rectfill(eof_window_editor->screen, (int)startpixel + 3, top + 2, labelright, top + 14, makecol(4, 4, 8));
		textprintf_ex(eof_window_editor->screen, font, (int)startpixel + 5, top + 4, makecol(230, 230, 235), -1,
			"EXP HQ: 4096 Hann/75%% | cache 768 bandas | %.0f dB | freq. log", EOF_EXPERIMENTAL_SPECTROGRAM_DB_RANGE);
	}

	legendx = eof_window_editor->w - 12;
	legendtop = top + 22;
	legendbottom = bottom - 6;
	if((legendbottom - legendtop) > 40)
	{
		for(py = legendtop; py <= legendbottom; py++)
		{
			norm = 1.0 - (double)(py - legendtop) / (double)(legendbottom - legendtop);
			hline(eof_window_editor->screen, legendx, py, eof_window_editor->w - 3, eof_spectrogram_experimental_palette(norm));
		}
		rectfill(eof_window_editor->screen, eof_window_editor->w - 50, legendtop - 2, eof_window_editor->w - 14, legendtop + 8, makecol(4, 4, 8));
		textout_ex(eof_window_editor->screen, font, "0 dB", eof_window_editor->w - 48, legendtop, makecol(230, 230, 235), -1);
		rectfill(eof_window_editor->screen, eof_window_editor->w - 55, legendbottom - 8, eof_window_editor->w - 14, legendbottom + 2, makecol(4, 4, 8));
		textprintf_ex(eof_window_editor->screen, font, eof_window_editor->w - 58, legendbottom - 7,
			makecol(230, 230, 235), -1, "-%.0f dB", EOF_EXPERIMENTAL_SPECTROGRAM_DB_RANGE);
	}
}

int eof_render_spectrogram_experimental(struct spectrogramstruct *spectrogram)
{
	unsigned long x, startpixel, curms, slice0, slice1, bin0, bin1;
	unsigned long pos = eof_music_pos.value / eof_zoom;
	int top, bottom, height, yoffset, color, ystart, yend, yscan;
	double fmin = MINFREQ, fmax, samplepos, slicepos, blend, val0, val1, value;
	double db, absolute_norm, local_db, local_norm, neighborhood, contrast_db, contrast_norm;
	double transient_db, transient_norm, normalized, framepeak, sumlog, display_reference;
	double *column = NULL, *previous = NULL;

	if(!eof_song_loaded || !spectrogram || !spectrogram->left.slices || (spectrogram->destmax <= 0.0))
		return 1;
	if(spectrogram->is_stereo && !spectrogram->right.slices)
		return 1;

	curms = eof_determine_piano_roll_left_edge();
	if(pos < 300)
		startpixel = 20;
	else if(pos < 320)
		startpixel = 320 - pos;
	else
		startpixel = 0;

	top = 32;
	bottom = eof_screen_layout.scrollbar_y - 1;
	height = bottom - top;
	if(height < 2)
		return 1;

	fmax = (double)spectrogram->rate / 2.0;
	if(fmax > 20000.0)
		fmax = 20000.0;
	if(fmax <= fmin)
		return 1;

	/* Fast path: all expensive RMS, spectral-whitening and transient work was
	 * computed once at load time.  During playback this is only bilinear
	 * interpolation in a compact 768-band cache. */
	if(spectrogram->experimental_cache && (spectrogram->experimental_cache_bands > 1UL))
	{
		unsigned long bands = spectrogram->experimental_cache_bands;

		for(x = startpixel; x < (unsigned long)eof_window_editor->w; x++, curms += eof_zoom)
		{
			unsigned long band0, band1;
			double bandpos, bandblend;

			samplepos = ((double)curms * (double)spectrogram->rate) / 1000.0;
			slicepos = (samplepos - ((double)spectrogram->windowsize * 0.5)) / (double)spectrogram->hopsize;
			if(slicepos <= 0.0)
			{
				slice0 = 0;
				slice1 = (spectrogram->numslices > 1UL) ? 1UL : 0UL;
				blend = 0.0;
			}
			else
			{
				slice0 = (unsigned long)slicepos;
				if(slice0 >= spectrogram->numslices)
					continue;
				slice1 = (slice0 + 1UL < spectrogram->numslices) ? slice0 + 1UL : slice0;
				blend = slicepos - (double)slice0;
			}

			for(yoffset = 0; yoffset < height; yoffset++)
			{
				double a0, a1, cached;

				bandpos = ((double)yoffset / (double)(height - 1)) * (double)(bands - 1UL);
				band0 = (unsigned long)bandpos;
				band1 = (band0 + 1UL < bands) ? band0 + 1UL : band0;
				bandblend = bandpos - (double)band0;

				a0 = (double)spectrogram->experimental_cache[(size_t)slice0 * bands + band0] * (1.0 - bandblend) +
					(double)spectrogram->experimental_cache[(size_t)slice0 * bands + band1] * bandblend;
				a1 = (double)spectrogram->experimental_cache[(size_t)slice1 * bands + band0] * (1.0 - bandblend) +
					(double)spectrogram->experimental_cache[(size_t)slice1 * bands + band1] * bandblend;
				cached = ((1.0 - blend) * a0 + blend * a1) / 255.0;
				putpixel(eof_window_editor->screen, (int)x, bottom - yoffset, eof_spectrogram_experimental_palette(cached));
			}
		}

		eof_spectrogram_experimental_draw_guides(top, bottom, startpixel,
			spectrogram->experimental_cache_fmin, spectrogram->experimental_cache_fmax);
		return 0;
	}

	/* Allocation-failure fallback keeps the previous full-quality renderer. */
	if(!eof_spectrogram_experimental_prepare_binmap(spectrogram, height, fmin, fmax))
		return 1;
	display_reference = (spectrogram->displaymax > 0.0) ? spectrogram->displaymax : spectrogram->destmax;
	if(display_reference <= 0.0)
		return 1;

	column = (double *)malloc(sizeof(double) * (size_t)height);
	previous = (double *)calloc((size_t)height, sizeof(double));
	if(!column || !previous)
	{
		if(column) free(column);
		if(previous) free(previous);
		return 1;
	}

	for(x = startpixel; x < (unsigned long)eof_window_editor->w; x++, curms += eof_zoom)
	{
		/* STFT magnitudes represent the center of their analysis windows.
		 * Compensate N/2 here so visible attacks line up with the audio. */
		samplepos = ((double)curms * (double)spectrogram->rate) / 1000.0;
		slicepos = (samplepos - ((double)spectrogram->windowsize * 0.5)) / (double)spectrogram->hopsize;
		if(slicepos <= 0.0)
		{
			slice0 = 0;
			slice1 = (spectrogram->numslices > 1UL) ? 1UL : 0UL;
			blend = 0.0;
		}
		else
		{
			slice0 = (unsigned long)slicepos;
			if(slice0 >= spectrogram->numslices)
				continue;
			slice1 = (slice0 + 1UL < spectrogram->numslices) ? slice0 + 1UL : slice0;
			blend = slicepos - (double)slice0;
		}

		framepeak = 0.0;
		for(yoffset = 0; yoffset < height; yoffset++)
		{
			bin0 = eof_spectrogram_experimental_binmap[yoffset];
			bin1 = eof_spectrogram_experimental_binmap[yoffset + 1];
			val0 = eof_spectrogram_experimental_band_rms(spectrogram, slice0, bin0, bin1);
			val1 = eof_spectrogram_experimental_band_rms(spectrogram, slice1, bin0, bin1);
			value = sqrt((1.0 - blend) * val0 * val0 + blend * val1 * val1);
			column[yoffset] = value;
			if(value > framepeak)
				framepeak = value;
		}

		for(yoffset = 0; yoffset < height; yoffset++)
		{
			value = column[yoffset];
			if(value <= 0.0)
			{
				normalized = 0.0;
			}
			else
			{
				/* Keep a song-global dB reference so intensity remains
				 * comparable between passages. */
				db = 20.0 * log10(value / display_reference);
				if(db <= -EOF_EXPERIMENTAL_SPECTROGRAM_DB_RANGE)
					absolute_norm = 0.0;
				else
					absolute_norm = (db + EOF_EXPERIMENTAL_SPECTROGRAM_DB_RANGE) / EOF_EXPERIMENTAL_SPECTROGRAM_DB_RANGE;
				if(absolute_norm < 0.0) absolute_norm = 0.0;
				if(absolute_norm > 1.0) absolute_norm = 1.0;

				/* Add a bounded local reference so quiet musical detail is
				 * still visible, without promoting noise to full brightness. */
				if(framepeak > 0.0)
				{
					local_db = 20.0 * log10(value / framepeak);
					local_norm = (local_db + 55.0) / 55.0;
					if(local_norm < 0.0) local_norm = 0.0;
					if(local_norm > 1.0) local_norm = 1.0;
				}
				else
					local_norm = 0.0;

				/* Local spectral whitening highlights tonal ridges against
				 * broad-band energy.  Geometric mean is appropriate in dB. */
				ystart = yoffset - 3;
				if(ystart < 0) ystart = 0;
				yend = yoffset + 3;
				if(yend >= height) yend = height - 1;
				sumlog = 0.0;
				for(yscan = ystart; yscan <= yend; yscan++)
					sumlog += log(column[yscan] + 1.0);
				neighborhood = exp(sumlog / (double)(yend - ystart + 1)) - 1.0;
				if(neighborhood > 0.0)
					contrast_db = 20.0 * log10((value + 1.0) / (neighborhood + 1.0));
				else
					contrast_db = 0.0;
				contrast_norm = contrast_db / 18.0;
				if(contrast_norm < 0.0) contrast_norm = 0.0;
				if(contrast_norm > 1.0) contrast_norm = 1.0;

				/* Positive temporal contrast marks attacks and percussion. */
				if(previous[yoffset] > 0.0)
					transient_db = 20.0 * log10((value + 1.0) / (previous[yoffset] + 1.0));
				else
					transient_db = 0.0;
				transient_norm = transient_db / 18.0;
				if(transient_norm < 0.0) transient_norm = 0.0;
				if(transient_norm > 1.0) transient_norm = 1.0;

				normalized = 0.66 * absolute_norm +
					0.22 * local_norm * sqrt(absolute_norm) +
					0.08 * transient_norm * sqrt(absolute_norm) +
					0.12 * contrast_norm * sqrt(absolute_norm);
				if(normalized > 1.0) normalized = 1.0;
			}

			color = eof_spectrogram_experimental_palette(normalized);
			putpixel(eof_window_editor->screen, (int)x, bottom - yoffset, color);
		}
		memcpy(previous, column, sizeof(double) * (size_t)height);
	}

	free(column);
	free(previous);
	eof_spectrogram_experimental_draw_guides(top, bottom, startpixel, fmin, fmax);
	return 0;
}

#define EOF_SPECTROGRAM_AUTOSYNC_HQ_MAX_BANDS 216
#define EOF_SPECTROGRAM_AUTOSYNC_HQ_MIN_BANDS 72
#define EOF_SPECTROGRAM_AUTOSYNC_HQ_MATRIX_BUDGET (128UL * 1024UL * 1024UL)
#define EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW 2048
#define EOF_SPECTROGRAM_AUTOSYNC_HQ_HOP 256
#define EOF_SPECTROGRAM_AUTOSYNC_HQ_DIFF_FRAMES 2UL
#define EOF_SPECTROGRAM_AUTOSYNC_HQ_TIME_RADIUS 17
#define EOF_SPECTROGRAM_AUTOSYNC_HQ_FREQ_RADIUS 2

struct eof_spectrogram_autosync_hq_peak
{
	double pos;
	double strength;
	double prominence;
	double support;
};

struct eof_spectrogram_autosync_hq_analysis
{
	double *envelope;
	double *support;
	double *low_band;
	double *mid_band;
	double *high_band;
	double *cymbal_band;
	double *chroma;
	unsigned long frames;
	unsigned long hopsize;
	unsigned long windowsize;
	long rate;
};

static int eof_spectrogram_autosync_hq_double_compare(const void *e1, const void *e2)
{
	double a = *(const double *)e1;
	double b = *(const double *)e2;

	if(a < b) return -1;
	if(a > b) return 1;
	return 0;
}

static double eof_spectrogram_autosync_hq_percentile(const double *data, unsigned long count, double fraction)
{
	double *copy, value;
	unsigned long index;

	if(!data || !count)
		return 0.0;
	copy = (double *)malloc(sizeof(double) * (size_t)count);
	if(!copy)
		return 0.0;
	memcpy(copy, data, sizeof(double) * (size_t)count);
	qsort(copy, (size_t)count, sizeof(double), eof_spectrogram_autosync_hq_double_compare);
	if(fraction < 0.0) fraction = 0.0;
	if(fraction > 1.0) fraction = 1.0;
	index = (unsigned long)floor(fraction * (double)(count - 1UL) + 0.5);
	value = copy[index];
	free(copy);
	return value;
}

static double eof_spectrogram_autosync_hq_small_median(double *values, unsigned count)
{
	unsigned i, j;
	double key;

	if(!values || !count)
		return 0.0;
	for(i = 1; i < count; i++)
	{
		key = values[i];
		j = i;
		while(j && (values[j - 1] > key))
		{
			values[j] = values[j - 1];
			j--;
		}
		values[j] = key;
	}
	if(count & 1U)
		return values[count / 2U];
	return (values[count / 2U - 1U] + values[count / 2U]) * 0.5;
}

static double eof_spectrogram_autosync_hq_mono_sample(const SAMPLE *audio, char stereo, unsigned long frame)
{
	unsigned bytes, stride;
	size_t offset;
	unsigned long left, right;
	long lsample, rsample;

	if(!audio || !audio->data || (frame >= audio->len))
		return 0.0;

	bytes = (unsigned)audio->bits / 8U;
	stride = bytes * (stereo ? 2U : 1U);
	offset = (size_t)frame * (size_t)stride;
	left = ((const unsigned char *)audio->data)[offset];
	if(bytes > 1U)
		left |= (unsigned long)((const unsigned char *)audio->data)[offset + 1U] << 8;
	lsample = (long)left - ((audio->bits == 8) ? 128L : 32768L);
	if(!stereo)
		return (double)lsample;

	offset += bytes;
	right = ((const unsigned char *)audio->data)[offset];
	if(bytes > 1U)
		right |= (unsigned long)((const unsigned char *)audio->data)[offset + 1U] << 8;
	rsample = (long)right - ((audio->bits == 8) ? 128L : 32768L);
	return ((double)lsample + (double)rsample) * 0.5;
}

static void eof_spectrogram_autosync_hq_destroy(struct eof_spectrogram_autosync_hq_analysis *analysis)
{
	if(!analysis)
		return;
	if(analysis->envelope) free(analysis->envelope);
	if(analysis->support) free(analysis->support);
	if(analysis->low_band) free(analysis->low_band);
	if(analysis->mid_band) free(analysis->mid_band);
	if(analysis->high_band) free(analysis->high_band);
	if(analysis->cymbal_band) free(analysis->cymbal_band);
	if(analysis->chroma) free(analysis->chroma);
	memset(analysis, 0, sizeof(*analysis));
}

static int eof_spectrogram_autosync_hq_build(const char *audio_filename, unsigned long track, struct eof_spectrogram_autosync_hq_analysis *analysis)
{
	ALOGG_OGG *oggstruct = NULL;
	SAMPLE *audio = NULL;
	void *oggbuffer = NULL;
	fftw_plan plan = NULL;
	double *fftin = NULL, *fftout = NULL, *matrix = NULL, *energy = NULL;
	double *mainflux = NULL, *lowflux = NULL, *midflux = NULL, *highflux = NULL, *cymbalflux = NULL, *energyflux = NULL;
	double *composite = NULL, *smooth = NULL, *prefix = NULL, *prefix2 = NULL;
	double bandpower[EOF_SPECTROGRAM_AUTOSYNC_HQ_MAX_BANDS];
	double chromapower[12];
	unsigned bandcount[EOF_SPECTROGRAM_AUTOSYNC_HQ_MAX_BANDS];
	unsigned char bandactive[EOF_SPECTROGRAM_AUTOSYNC_HQ_MAX_BANDS];
	double bandcenter[EOF_SPECTROGRAM_AUTOSYNC_HQ_MAX_BANDS];
	double timevalues[2 * EOF_SPECTROGRAM_AUTOSYNC_HQ_TIME_RADIUS + 1];
	double freqvalues[2 * EOF_SPECTROGRAM_AUTOSYNC_HQ_FREQ_RADIUS + 1];
	unsigned long frames, frame, sample, bin, halfsize, firstframe, lastframe, ctr;
	unsigned band, bands, firstband, lastband, bctr;
	unsigned lowcount = 0, midcount = 0, highcount = 0, cymbalcount = 0, activebands = 0;
	double fmin = 30.0, fmax, logrange, frequency, magnitude, power;
	double current, reference, difference, timemedian, freqmedian, percussive, weighted;
	double lo, mid, hi, cym, echange, scale_main, scale_low, scale_mid, scale_high, scale_cymbal, scale_energy;
	double nmain, nlow, nmid, nhigh, ncymbal, nenergy, maxgroup, scale_env;
	double sum, sumsq, mean, variance, threshold;
	char stereo, drum_mode = 0, use_chroma = 0;
	int ok = 0;
	size_t matrixcells;

	if(!analysis || !audio_filename || !audio_filename[0])
		return 0;
	memset(analysis, 0, sizeof(*analysis));
	set_window_title("Auto-sync: HQ audio analysis...");

	oggbuffer = eof_buffer_file((char *)audio_filename, 0, 0);
	if(!oggbuffer)
		goto cleanup;
	oggstruct = alogg_create_ogg_from_buffer(oggbuffer, (int)file_size_ex(audio_filename));
	if(!oggstruct)
		goto cleanup;
	audio = alogg_create_sample_from_ogg(oggstruct);
	if(!audio || !audio->len || !audio->freq || ((audio->bits != 8) && (audio->bits != 16)))
		goto cleanup;
	stereo = alogg_get_wave_is_stereo_ogg(oggstruct) ? 1 : 0;

	/* PART_REAL_DRUM_DTX deliberately uses EOF's pro-guitar carrier to store
	 * raw GM percussion numbers.  It must be treated as drums here and must
	 * never be interpreted as pitched pro-guitar data. */
	if((track == EOF_TRACK_DRUM_DTX) ||
	   (eof_song && (track < eof_song->tracks) && eof_song->track[track] &&
	    (eof_song->track[track]->track_behavior == EOF_DRUM_TRACK_BEHAVIOR)))
		drum_mode = 1;
	if(!drum_mode && eof_song && eof_track_is_pro_guitar_track(eof_song, track))
		use_chroma = 1;

	frames = ((unsigned long)audio->len + EOF_SPECTROGRAM_AUTOSYNC_HQ_HOP - 1UL) / EOF_SPECTROGRAM_AUTOSYNC_HQ_HOP;
	if(frames < 5UL)
		goto cleanup;

	/* SuperFlux performs best with a dense logarithmic filterbank (the public
	 * reference uses 24 bands per octave).  Target 216 bands across our ~9
	 * octave range, but reduce this automatically for long songs so the main
	 * analysis matrix stays near 128 MB even in a 32-bit EOF build. */
	bands = EOF_SPECTROGRAM_AUTOSYNC_HQ_MAX_BANDS;
	if(frames)
	{
		size_t memory_limited = (size_t)EOF_SPECTROGRAM_AUTOSYNC_HQ_MATRIX_BUDGET /
			((size_t)frames * sizeof(double));
		if(memory_limited < (size_t)bands)
		{
			bands = (unsigned)memory_limited;
			if(bands > EOF_SPECTROGRAM_AUTOSYNC_HQ_MIN_BANDS)
				bands -= bands % 12U;
		}
	}
	if(bands < EOF_SPECTROGRAM_AUTOSYNC_HQ_MIN_BANDS)
		bands = EOF_SPECTROGRAM_AUTOSYNC_HQ_MIN_BANDS;
	if(bands > EOF_SPECTROGRAM_AUTOSYNC_HQ_MAX_BANDS)
		bands = EOF_SPECTROGRAM_AUTOSYNC_HQ_MAX_BANDS;

	if((size_t)frames > ((size_t)-1) / (size_t)bands)
		goto cleanup;
	matrixcells = (size_t)frames * (size_t)bands;
	if(matrixcells > ((size_t)-1) / sizeof(double))
		goto cleanup;

	matrix = (double *)calloc(matrixcells, sizeof(double));
	energy = (double *)calloc((size_t)frames, sizeof(double));
	mainflux = (double *)calloc((size_t)frames, sizeof(double));
	lowflux = (double *)calloc((size_t)frames, sizeof(double));
	midflux = (double *)calloc((size_t)frames, sizeof(double));
	highflux = (double *)calloc((size_t)frames, sizeof(double));
	cymbalflux = (double *)calloc((size_t)frames, sizeof(double));
	energyflux = (double *)calloc((size_t)frames, sizeof(double));
	composite = (double *)calloc((size_t)frames, sizeof(double));
	smooth = (double *)calloc((size_t)frames, sizeof(double));
	prefix = (double *)calloc((size_t)frames + 1U, sizeof(double));
	prefix2 = (double *)calloc((size_t)frames + 1U, sizeof(double));
	analysis->envelope = (double *)calloc((size_t)frames, sizeof(double));
	analysis->support = (double *)calloc((size_t)frames, sizeof(double));
	analysis->low_band = (double *)calloc((size_t)frames, sizeof(double));
	analysis->mid_band = (double *)calloc((size_t)frames, sizeof(double));
	analysis->high_band = (double *)calloc((size_t)frames, sizeof(double));
	analysis->cymbal_band = (double *)calloc((size_t)frames, sizeof(double));
	fftin = (double *)fftw_malloc(sizeof(double) * EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW);
	fftout = (double *)fftw_malloc(sizeof(double) * EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW);
	if(!matrix || !energy || !mainflux || !lowflux || !midflux || !highflux || !cymbalflux || !energyflux ||
	   !composite || !smooth || !prefix || !prefix2 || !analysis->envelope || !analysis->support ||
	   !analysis->low_band || !analysis->mid_band || !analysis->high_band || !analysis->cymbal_band || !fftin || !fftout)
		goto cleanup;

	plan = fftw_plan_r2r_1d(EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW, fftin, fftout,
		FFTW_R2HC, FFTW_MEASURE);
	if(!plan)
		goto cleanup;

	fmax = (double)audio->freq * 0.5;
	if(fmax > 16000.0) fmax = 16000.0;
	if(fmax <= fmin)
		goto cleanup;
	logrange = log(fmax / fmin);
	halfsize = EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW / 2UL;
	memset(bandactive, 0, sizeof(bandactive));

	/* Mark only bands which actually receive at least one FFT bin.  This
	 * mirrors the "unique filters" behavior of logarithmic filterbanks and
	 * avoids letting duplicate/empty low-frequency bands dilute the novelty. */
	for(bin = 1; bin < halfsize; bin++)
	{
		int bandindex;
		frequency = (double)bin * (double)audio->freq / (double)EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW;
		if((frequency < fmin) || (frequency > fmax))
			continue;
		bandindex = (int)(log(frequency / fmin) / logrange * (double)bands);
		if(bandindex < 0) bandindex = 0;
		if(bandindex >= (int)bands) bandindex = (int)bands - 1;
		bandactive[bandindex] = 1;
	}

	for(band = 0; band < bands; band++)
	{
		double lowedge = fmin * exp(logrange * (double)band / (double)bands);
		double highedge = fmin * exp(logrange * (double)(band + 1U) / (double)bands);
		bandcenter[band] = sqrt(lowedge * highedge);
		if(!bandactive[band])
			continue;
		activebands++;
		if(bandcenter[band] < 200.0) lowcount++;
		else if(bandcenter[band] < 2200.0) midcount++;
		else highcount++;
		if(bandcenter[band] >= 5000.0) cymbalcount++;
	}
	if(!activebands)
		goto cleanup;

	(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"HQ Auto-sync analysis: %lu frames, %u logarithmic bands (%u active), SuperFlux lag %lu frames",
		frames, bands, activebands, EOF_SPECTROGRAM_AUTOSYNC_HQ_DIFF_FRAMES);
	eof_log(eof_log_string, 1);

	if(use_chroma)
	{
		if((size_t)frames > ((size_t)-1) / (12U * sizeof(double)))
			goto cleanup;
		analysis->chroma = (double *)calloc((size_t)frames * 12U, sizeof(double));
		if(!analysis->chroma)
			goto cleanup;
	}

	/* Dense short-hop STFT, collapsed immediately to an adaptive high-resolution
	 * logarithmic filterbank.  This gives ~5.8 ms hops at 44.1 kHz without
	 * storing a full raw FFT spectrogram for the entire song. */
	for(frame = 0; frame < frames; frame++)
	{
		unsigned long base = frame * EOF_SPECTROGRAM_AUTOSYNC_HQ_HOP;
		double samplevalue, hann, energysum = 0.0;

		for(sample = 0; sample < EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW; sample++)
		{
			samplevalue = (base + sample < audio->len) ?
				eof_spectrogram_autosync_hq_mono_sample(audio, stereo, base + sample) : 0.0;
			energysum += samplevalue * samplevalue;
			hann = 0.5 - 0.5 * cos((2.0 * 3.14159265358979323846 * (double)sample) /
				(double)(EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW - 1));
			fftin[sample] = samplevalue * hann;
		}
		energy[frame] = log(1.0 + sqrt(energysum / (double)EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW));
		fftw_execute(plan);
		memset(bandpower, 0, sizeof(bandpower));
		memset(chromapower, 0, sizeof(chromapower));
		memset(bandcount, 0, sizeof(bandcount));

		for(bin = 1; bin < halfsize; bin++)
		{
			double realpart, imagpart;
			int bandindex;

			frequency = (double)bin * (double)audio->freq / (double)EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW;
			if((frequency < fmin) || (frequency > fmax))
				continue;
			bandindex = (int)(log(frequency / fmin) / logrange * (double)bands);
			if(bandindex < 0) bandindex = 0;
			if(bandindex >= (int)bands)
				bandindex = (int)bands - 1;
			realpart = fftout[bin];
			imagpart = fftout[EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW - bin];
			power = realpart * realpart + imagpart * imagpart;
			bandpower[bandindex] += power;
			bandcount[bandindex]++;

			/* Secondary pitch-class evidence helps avoid snapping a guitar/bass
			 * note to a stronger drum or vocal transient in the full mix. */
			if(use_chroma && (frequency >= 45.0) && (frequency <= 5200.0))
			{
				double midinote = 69.0 + 12.0 * (log(frequency / 440.0) / log(2.0));
				double chromaweight = 1.0 / sqrt(frequency / 110.0);
				int pitchclass = ((int)floor(midinote + 0.5)) % 12;
				if(pitchclass < 0) pitchclass += 12;
				if(chromaweight < 0.35) chromaweight = 0.35;
				if(chromaweight > 1.80) chromaweight = 1.80;
				chromapower[pitchclass] += log(1.0 + sqrt(power)) * chromaweight;
			}
		}
		for(band = 0; band < bands; band++)
		{
			magnitude = bandcount[band] ? sqrt(bandpower[band] / (double)bandcount[band]) : 0.0;
			matrix[(size_t)frame * bands + band] = log(1.0 + magnitude);
		}
		if(use_chroma)
		{
			double chromasum = 0.0;
			unsigned pc;
			for(pc = 0; pc < 12U; pc++) chromasum += chromapower[pc];
			if(chromasum > 0.0)
			{
				for(pc = 0; pc < 12U; pc++)
					analysis->chroma[(size_t)frame * 12U + pc] = chromapower[pc] / chromasum;
			}
		}
	}

	/* SuperFlux-style maximum filtering suppresses vibrato-induced novelty.
	 * A median filter across time estimates sustained/harmonic energy, while
	 * a median across frequency estimates broad/percussive energy. */
	for(frame = EOF_SPECTROGRAM_AUTOSYNC_HQ_DIFF_FRAMES; frame < frames; frame++)
	{
		double main = 0.0;
		lo = mid = hi = cym = 0.0;

		for(band = 0; band < bands; band++)
		{
			if(!bandactive[band])
				continue;
			current = matrix[(size_t)frame * bands + band];
			/* SuperFlux reference: maximum of k-1,k,k+1 in a slightly older
			 * frame.  This suppresses vibrato-induced false novelty while the
			 * 2-frame lag better separates the attack from the reference. */
			firstband = (band > 0U) ? band - 1U : 0U;
			lastband = band + 1U;
			if(lastband >= bands)
				lastband = bands - 1U;
			reference = 0.0;
			for(bctr = firstband; bctr <= lastband; bctr++)
			{
				double candidate = matrix[(size_t)(frame - EOF_SPECTROGRAM_AUTOSYNC_HQ_DIFF_FRAMES) * bands + bctr];
				if(candidate > reference)
					reference = candidate;
			}
			difference = current - reference;
			if(difference <= 0.0)
				continue;

			firstframe = (frame > EOF_SPECTROGRAM_AUTOSYNC_HQ_TIME_RADIUS) ?
				frame - EOF_SPECTROGRAM_AUTOSYNC_HQ_TIME_RADIUS : 0UL;
			lastframe = frame + EOF_SPECTROGRAM_AUTOSYNC_HQ_TIME_RADIUS;
			if(lastframe >= frames) lastframe = frames - 1UL;
			ctr = 0;
			for(; firstframe <= lastframe; firstframe++)
				timevalues[ctr++] = matrix[(size_t)firstframe * bands + band];
			timemedian = eof_spectrogram_autosync_hq_small_median(timevalues, (unsigned)ctr);

			firstband = (band > EOF_SPECTROGRAM_AUTOSYNC_HQ_FREQ_RADIUS) ?
				band - EOF_SPECTROGRAM_AUTOSYNC_HQ_FREQ_RADIUS : 0U;
			lastband = band + EOF_SPECTROGRAM_AUTOSYNC_HQ_FREQ_RADIUS;
			if(lastband >= bands)
				lastband = bands - 1U;
			ctr = 0;
			for(bctr = firstband; bctr <= lastband; bctr++)
				freqvalues[ctr++] = matrix[(size_t)frame * bands + bctr];
			freqmedian = eof_spectrogram_autosync_hq_small_median(freqvalues, (unsigned)ctr);

			percussive = freqmedian / (freqmedian + timemedian + 0.000000001);
			weighted = difference * (0.65 + 0.70 * percussive);
			main += weighted;
			if(bandcenter[band] < 200.0) lo += weighted;
			else if(bandcenter[band] < 2200.0) mid += weighted;
			else hi += weighted;
			if(bandcenter[band] >= 5000.0) cym += weighted;
		}

		mainflux[frame] = main / (double)activebands;
		lowflux[frame] = lowcount ? lo / (double)lowcount : 0.0;
		midflux[frame] = midcount ? mid / (double)midcount : 0.0;
		highflux[frame] = highcount ? hi / (double)highcount : 0.0;
		cymbalflux[frame] = cymbalcount ? cym / (double)cymbalcount : highflux[frame];
		echange = energy[frame] - energy[frame - 1UL];
		energyflux[frame] = (echange > 0.0) ? echange : 0.0;
	}

	/* Robust percentile normalization prevents one huge crash/cymbal hit from
	 * making every quieter onset effectively invisible. */
	scale_main = eof_spectrogram_autosync_hq_percentile(mainflux, frames, 0.95);
	scale_low = eof_spectrogram_autosync_hq_percentile(lowflux, frames, 0.95);
	scale_mid = eof_spectrogram_autosync_hq_percentile(midflux, frames, 0.95);
	scale_high = eof_spectrogram_autosync_hq_percentile(highflux, frames, 0.95);
	scale_cymbal = eof_spectrogram_autosync_hq_percentile(cymbalflux, frames, 0.95);
	scale_energy = eof_spectrogram_autosync_hq_percentile(energyflux, frames, 0.95);
	if(scale_main <= 0.0)
		goto cleanup;
	if(scale_low <= 0.0) scale_low = scale_main;
	if(scale_mid <= 0.0) scale_mid = scale_main;
	if(scale_high <= 0.0) scale_high = scale_main;
	if(scale_cymbal <= 0.0) scale_cymbal = scale_high;
	if(scale_energy <= 0.0) scale_energy = 1.0;

	for(frame = 0; frame < frames; frame++)
	{
		nmain = mainflux[frame] / scale_main;
		nlow = lowflux[frame] / scale_low;
		nmid = midflux[frame] / scale_mid;
		nhigh = highflux[frame] / scale_high;
		ncymbal = cymbalflux[frame] / scale_cymbal;
		nenergy = energyflux[frame] / scale_energy;
		if(nmain > 2.0) nmain = 2.0;
		if(nlow > 2.0) nlow = 2.0;
		if(nmid > 2.0) nmid = 2.0;
		if(nhigh > 2.0) nhigh = 2.0;
		if(ncymbal > 2.0) ncymbal = 2.0;
		if(nenergy > 2.0) nenergy = 2.0;

		analysis->low_band[frame] = nlow;
		analysis->mid_band[frame] = nmid;
		analysis->high_band[frame] = nhigh;
		analysis->cymbal_band[frame] = ncymbal;

		if(drum_mode)
			composite[frame] = 0.30 * nmain + 0.20 * nlow + 0.10 * nmid + 0.18 * nhigh + 0.14 * ncymbal + 0.08 * nenergy;
		else
			composite[frame] = 0.44 * nmain + 0.10 * nlow + 0.24 * nmid + 0.14 * nhigh + 0.08 * nenergy;

		maxgroup = nlow;
		if(nmid > maxgroup) maxgroup = nmid;
		if(nhigh > maxgroup) maxgroup = nhigh;
		if(ncymbal > maxgroup) maxgroup = ncymbal;
		analysis->support[frame] = 0.55 * nmain + 0.45 * maxgroup;
		if(analysis->support[frame] > 1.0)
			analysis->support[frame] = 1.0;
	}

	/* Short multi-frame smoothing and adaptive local threshold. */
	for(frame = 0; frame < frames; frame++)
	{
		static const double kernel[5] = {1.0, 2.0, 3.0, 2.0, 1.0};
		double weighted_sum = 0.0, weights = 0.0;
		long offset;

		for(offset = -2; offset <= 2; offset++)
		{
			long index = (long)frame + offset;
			if((index < 0) || ((unsigned long)index >= frames))
				continue;
			weighted_sum += composite[index] * kernel[offset + 2];
			weights += kernel[offset + 2];
		}
		smooth[frame] = weights ? weighted_sum / weights : composite[frame];
		prefix[frame + 1UL] = prefix[frame] + smooth[frame];
		prefix2[frame + 1UL] = prefix2[frame] + smooth[frame] * smooth[frame];
	}

	for(frame = 0; frame < frames; frame++)
	{
		unsigned long radius = 30UL, count;
		firstframe = (frame > radius) ? frame - radius : 0UL;
		lastframe = frame + radius;
		if(lastframe >= frames) lastframe = frames - 1UL;
		count = lastframe - firstframe + 1UL;
		sum = prefix[lastframe + 1UL] - prefix[firstframe];
		sumsq = prefix2[lastframe + 1UL] - prefix2[firstframe];
		mean = sum / (double)count;
		variance = sumsq / (double)count - mean * mean;
		if(variance < 0.0) variance = 0.0;
		threshold = 0.55 * mean + 0.20 * sqrt(variance);
		analysis->envelope[frame] = smooth[frame] - threshold;
		if(analysis->envelope[frame] < 0.0)
			analysis->envelope[frame] = 0.0;
		analysis->envelope[frame] += 0.16 * smooth[frame];
	}

	scale_env = eof_spectrogram_autosync_hq_percentile(analysis->envelope, frames, 0.97);
	if(scale_env <= 0.0)
		goto cleanup;
	for(frame = 0; frame < frames; frame++)
	{
		analysis->envelope[frame] /= scale_env;
		if(analysis->envelope[frame] > 1.5)
			analysis->envelope[frame] = 1.5;
	}

	analysis->frames = frames;
	analysis->hopsize = EOF_SPECTROGRAM_AUTOSYNC_HQ_HOP;
	analysis->windowsize = EOF_SPECTROGRAM_AUTOSYNC_HQ_WINDOW;
	analysis->rate = audio->freq;
	ok = 1;

cleanup:
	if(plan) fftw_destroy_plan(plan);
	if(fftin) fftw_free(fftin);
	if(fftout) fftw_free(fftout);
	if(matrix) free(matrix);
	if(energy) free(energy);
	if(mainflux) free(mainflux);
	if(lowflux) free(lowflux);
	if(midflux) free(midflux);
	if(highflux) free(highflux);
	if(cymbalflux) free(cymbalflux);
	if(energyflux) free(energyflux);
	if(composite) free(composite);
	if(smooth) free(smooth);
	if(prefix) free(prefix);
	if(prefix2) free(prefix2);
	if(audio) destroy_sample(audio);
	if(oggstruct) alogg_destroy_ogg(oggstruct);
	if(oggbuffer) free(oggbuffer);
	if(!ok) eof_spectrogram_autosync_hq_destroy(analysis);
	return ok;
}

static double eof_spectrogram_autosync_hq_frame_time(const struct eof_spectrogram_autosync_hq_analysis *analysis, double frame)
{
	double samples;

	if(!analysis || !analysis->rate)
		return 0.0;
	samples = frame * (double)analysis->hopsize + ((double)analysis->windowsize * 0.5);
	return samples * 1000.0 / (double)analysis->rate;
}

static double eof_spectrogram_autosync_hq_curve_at(const struct eof_spectrogram_autosync_hq_analysis *analysis, const double *curve, double time_ms)
{
	double sample, framepos, fraction;
	unsigned long frame0, frame1;

	if(!analysis || !curve || !analysis->frames || !analysis->rate)
		return 0.0;
	sample = time_ms * (double)analysis->rate / 1000.0 - (double)analysis->windowsize * 0.5;
	framepos = sample / (double)analysis->hopsize;
	if(framepos <= 0.0)
		return curve[0];
	frame0 = (unsigned long)framepos;
	if(frame0 >= analysis->frames)
		return 0.0;
	frame1 = (frame0 + 1UL < analysis->frames) ? frame0 + 1UL : frame0;
	fraction = framepos - (double)frame0;
	return curve[frame0] * (1.0 - fraction) + curve[frame1] * fraction;
}

static unsigned long eof_spectrogram_autosync_hq_find_peaks(const struct eof_spectrogram_autosync_hq_analysis *analysis, double earliest, double latest, struct eof_spectrogram_autosync_hq_peak **peaklist)
{
	unsigned long frame, first, last, ctr, count = 0;
	double localmedian, value, prominence, denominator, delta, peakframe, peakpos, support;
	double score, previous_score, hop_ms, minimum_gap;
	double localvalues[37];
	struct eof_spectrogram_autosync_hq_peak *peaks;

	if(peaklist) *peaklist = NULL;
	if(!analysis || !analysis->envelope || !analysis->support || !peaklist || (analysis->frames < 5UL))
		return 0;

	peaks = (struct eof_spectrogram_autosync_hq_peak *)malloc(sizeof(*peaks) * (size_t)analysis->frames);
	if(!peaks)
		return 0;

	hop_ms = (double)analysis->hopsize * 1000.0 / (double)analysis->rate;
	minimum_gap = (hop_ms * 2.0 > 14.0) ? hop_ms * 2.0 : 14.0;

	for(frame = 2; frame + 2 < analysis->frames; frame++)
	{
		if((analysis->envelope[frame] < analysis->envelope[frame - 1UL]) ||
		   (analysis->envelope[frame] <= analysis->envelope[frame + 1UL]) ||
		   (analysis->envelope[frame] < analysis->envelope[frame - 2UL]) ||
		   (analysis->envelope[frame] < analysis->envelope[frame + 2UL]))
			continue;

		first = (frame > 18UL) ? frame - 18UL : 0UL;
		last = frame + 18UL;
		if(last >= analysis->frames) last = analysis->frames - 1UL;
		ctr = 0;
		while((first + ctr <= last) && (ctr < 37UL))
		{
			localvalues[ctr] = analysis->envelope[first + ctr];
			ctr++;
		}
		localmedian = eof_spectrogram_autosync_hq_small_median(localvalues, (unsigned)ctr);

		value = analysis->envelope[frame];
		prominence = value - localmedian;
		support = analysis->support[frame];
		/* A median floor is robust against one nearby crash/snare raising the
		 * threshold for quieter valid notes. */
		if((value < 0.055) || (prominence < 0.014 + 0.10 * localmedian) || (support < 0.035))
			continue;

		delta = 0.0;
		denominator = analysis->envelope[frame - 1UL] - 2.0 * analysis->envelope[frame] + analysis->envelope[frame + 1UL];
		if(fabs(denominator) > 0.0000001)
		{
			delta = 0.5 * (analysis->envelope[frame - 1UL] - analysis->envelope[frame + 1UL]) / denominator;
			if(delta < -0.5) delta = -0.5;
			if(delta > 0.5) delta = 0.5;
		}
		peakframe = (double)frame + delta;

		/* Spectral-flux maxima lag the perceptual attack slightly.  Backtrack
		 * toward the attack foot and blend it with the stable interpolated
		 * maximum.  Strong attacks trust the foot more; softer attacks remain
		 * closer to the maximum to avoid leading legato notes. */
		{
			unsigned long back = frame;
			unsigned long maxback = (unsigned long)(60.0 / hop_ms + 0.5);
			double footframe = peakframe;
			double footlevel = localmedian + 0.32 * (value - localmedian);

			if(maxback < 1UL) maxback = 1UL;
			while(back > 0UL && (frame - back < maxback))
			{
				if(analysis->envelope[back] <= footlevel)
				{
					footframe = (double)back;
					if((back + 1UL < analysis->frames) &&
					   (analysis->envelope[back + 1UL] > analysis->envelope[back]))
					{
						double footdenom = analysis->envelope[back + 1UL] - analysis->envelope[back];
						double fraction = (footlevel - analysis->envelope[back]) / footdenom;
						if(fraction < 0.0) fraction = 0.0;
						if(fraction > 1.0) fraction = 1.0;
						footframe += fraction;
					}
					break;
				}
				back--;
			}
			if(support >= 0.30)
				peakframe = 0.48 * peakframe + 0.52 * footframe;
			else
				peakframe = 0.67 * peakframe + 0.33 * footframe;
		}

		peakpos = eof_spectrogram_autosync_hq_frame_time(analysis, peakframe);
		if((peakpos < earliest) || (peakpos > latest))
			continue;

		score = value + 0.70 * prominence + 0.25 * support;
		if(count && ((peakpos - peaks[count - 1UL].pos) < minimum_gap))
		{
			previous_score = peaks[count - 1UL].strength + 0.70 * peaks[count - 1UL].prominence + 0.25 * peaks[count - 1UL].support;
			if(score > previous_score)
			{
				peaks[count - 1UL].pos = peakpos;
				peaks[count - 1UL].strength = value;
				peaks[count - 1UL].prominence = prominence;
				peaks[count - 1UL].support = support;
			}
			continue;
		}
		peaks[count].pos = peakpos;
		peaks[count].strength = value;
		peaks[count].prominence = prominence;
		peaks[count].support = support;
		count++;
	}

	if(!count)
	{
		free(peaks);
		return 0;
	}
	*peaklist = peaks;
	return count;
}

static double eof_spectrogram_autosync_hq_local_onset_evidence(const struct eof_spectrogram_autosync_hq_analysis *analysis, double time_ms)
{
	static const double offsets_ms[9] = {-24.0, -16.0, -8.0, 0.0, 8.0, 16.0, 24.0, 32.0, 40.0};
	double best = 0.0;
	unsigned ctr;

	if(!analysis || !analysis->envelope || !analysis->support)
		return 0.0;
	for(ctr = 0; ctr < 9U; ctr++)
	{
		double offset = offsets_ms[ctr];
		double t = time_ms + offset;
		double envelope, support, distance_weight, score;
		if(t < 0.0) continue;
		envelope = eof_spectrogram_autosync_hq_curve_at(analysis, analysis->envelope, t);
		support = eof_spectrogram_autosync_hq_curve_at(analysis, analysis->support, t);
		distance_weight = exp(-fabs(offset) / 28.0);
		score = envelope * (0.88 + 0.12 * support) * distance_weight;
		if(score > best) best = score;
	}
	return best;
}

static double eof_spectrogram_autosync_hq_local_curve_evidence(const struct eof_spectrogram_autosync_hq_analysis *analysis, const double *curve, double time_ms)
{
	static const double offsets_ms[7] = {-16.0, -8.0, 0.0, 8.0, 16.0, 24.0, 32.0};
	double best = 0.0;
	unsigned ctr;

	if(!analysis || !curve)
		return 0.0;
	for(ctr = 0; ctr < 7U; ctr++)
	{
		double offset = offsets_ms[ctr];
		double t = time_ms + offset;
		double weight, score;
		if(t < 0.0) continue;
		weight = exp(-fabs(offset) / 26.0);
		score = eof_spectrogram_autosync_hq_curve_at(analysis, curve, t) * weight;
		if(score > best) best = score;
	}
	return best;
}

static double eof_spectrogram_autosync_hq_dtx_timbre_score(const struct eof_spectrogram_autosync_hq_analysis *analysis, unsigned short mask, double time_ms)
{
	double low, mid, high, cymbal, score = 0.0;
	unsigned count = 0;
	unsigned short hhmask = (unsigned short)((1U << EOF_DTX_INT_HH) | (1U << EOF_DTX_INT_HH_OPEN));

	if(!analysis || !mask)
		return 0.0;
	low = eof_spectrogram_autosync_hq_local_curve_evidence(analysis, analysis->low_band, time_ms);
	mid = eof_spectrogram_autosync_hq_local_curve_evidence(analysis, analysis->mid_band, time_ms);
	high = eof_spectrogram_autosync_hq_local_curve_evidence(analysis, analysis->high_band, time_ms);
	cymbal = eof_spectrogram_autosync_hq_local_curve_evidence(analysis, analysis->cymbal_band, time_ms);

	if(mask & hhmask)
	{
		score += 0.04 * low + 0.10 * mid + 0.24 * high + 0.62 * cymbal;
		count++;
	}
	if(mask & (1U << EOF_DTX_INT_RIDE))
	{
		score += 0.06 * low + 0.18 * mid + 0.34 * high + 0.42 * cymbal;
		count++;
	}
	if(mask & (1U << EOF_DTX_INT_CRASH))
	{
		score += 0.05 * low + 0.12 * mid + 0.28 * high + 0.55 * cymbal;
		count++;
	}
	if(mask & (1U << EOF_DTX_INT_KICK))
	{
		score += 0.88 * low + 0.10 * mid + 0.02 * high;
		count++;
	}
	if(mask & (1U << EOF_DTX_INT_SNARE))
	{
		score += 0.10 * low + 0.55 * mid + 0.25 * high + 0.10 * cymbal;
		count++;
	}
	if(mask & (1U << EOF_DTX_INT_TOM1))
	{
		score += 0.12 * low + 0.70 * mid + 0.14 * high + 0.04 * cymbal;
		count++;
	}
	if(mask & (1U << EOF_DTX_INT_TOM2))
	{
		score += 0.28 * low + 0.64 * mid + 0.06 * high + 0.02 * cymbal;
		count++;
	}
	if(mask & (1U << EOF_DTX_INT_TOM3))
	{
		score += 0.50 * low + 0.47 * mid + 0.02 * high + 0.01 * cymbal;
		count++;
	}
	return count ? score / (double)count : 0.0;
}

static double eof_spectrogram_autosync_hq_dtx_event_score(const struct eof_spectrogram_autosync_hq_analysis *analysis, unsigned short mask, double time_ms)
{
	if(!analysis || !mask)
		return 0.0;
	return 0.46 * eof_spectrogram_autosync_hq_local_onset_evidence(analysis, time_ms) +
		0.36 * eof_spectrogram_autosync_hq_dtx_timbre_score(analysis, mask, time_ms) +
		0.18 * eof_spectrogram_autosync_hq_curve_at(analysis, analysis->support, time_ms);
}

static int eof_spectrogram_autosync_hq_dtx_same_conduction(unsigned short first, unsigned short second)
{
	unsigned short hhmask = (unsigned short)((1U << EOF_DTX_INT_HH) | (1U << EOF_DTX_INT_HH_OPEN));
	unsigned short metalmask = (unsigned short)((1U << EOF_DTX_INT_RIDE) | (1U << EOF_DTX_INT_CRASH));

	if((first & hhmask) && (second & hhmask))
		return 1;
	if((first & metalmask) && (second & metalmask))
		return 1;
	return 0;
}

static double eof_spectrogram_autosync_hq_chroma_score(const struct eof_spectrogram_autosync_hq_analysis *analysis, unsigned short pitchmask, double time_ms)
{
	static const double offsets_ms[5] = {-6.0, 6.0, 18.0, 30.0, 42.0};
	double best = 0.5;
	unsigned sampleidx, pc, pitchcount = 0;

	if(!analysis || !analysis->chroma || !pitchmask || !analysis->frames || !analysis->rate)
		return 0.5;
	for(pc = 0; pc < 12U; pc++)
		if(pitchmask & (1U << pc)) pitchcount++;
	if(!pitchcount || pitchcount >= 12U) return 0.5;

	for(sampleidx = 0; sampleidx < 5U; sampleidx++)
	{
		double t = time_ms + offsets_ms[sampleidx];
		double sample, framepos, frac, expected = 0.0, other, contrast, score;
		unsigned long frame0, frame1;

		if(t < 0.0) continue;
		sample = t * (double)analysis->rate / 1000.0 - (double)analysis->windowsize * 0.5;
		framepos = sample / (double)analysis->hopsize;
		if(framepos < 0.0) framepos = 0.0;
		frame0 = (unsigned long)framepos;
		if(frame0 >= analysis->frames) continue;
		frame1 = (frame0 + 1UL < analysis->frames) ? frame0 + 1UL : frame0;
		frac = framepos - (double)frame0;

		for(pc = 0; pc < 12U; pc++)
		{
			if(pitchmask & (1U << pc))
			{
				double v0 = analysis->chroma[(size_t)frame0 * 12U + pc];
				double v1 = analysis->chroma[(size_t)frame1 * 12U + pc];
				expected += v0 * (1.0 - frac) + v1 * frac;
			}
		}
		other = 1.0 - expected;
		contrast = expected / (double)pitchcount - other / (double)(12U - pitchcount);
		score = 0.5 + 3.0 * contrast;
		if(score < 0.0) score = 0.0;
		if(score > 1.0) score = 1.0;
		if(score > best) best = score;
	}
	return best;
}

static double eof_spectrogram_autosync_hq_global_shift(const struct eof_spectrogram_autosync_hq_analysis *analysis, const unsigned long *eventpos, const unsigned short *drum_masks, unsigned long eventcount, unsigned long max_shift_ms)
{
	double step, shift, score, bestscore = -1.0, bestshift = 0.0, meanscore = 0.0;
	unsigned long event, tested = 0;

	if(!analysis || !eventpos || !eventcount || !max_shift_ms)
		return 0.0;
	step = (double)analysis->hopsize * 500.0 / (double)analysis->rate;
	if(step < 1.0) step = 1.0;

	for(shift = -(double)max_shift_ms; shift <= (double)max_shift_ms; shift += step)
	{
		score = 0.0;
		for(event = 0; event < eventcount; event++)
		{
			double t = (double)eventpos[event] + shift;
			if(t < 0.0) continue;
			{
				double onset_score = eof_spectrogram_autosync_hq_curve_at(analysis, analysis->envelope, t) *
					(0.8 + 0.2 * eof_spectrogram_autosync_hq_curve_at(analysis, analysis->support, t));
				if(drum_masks && drum_masks[event])
					onset_score += 0.16 * eof_spectrogram_autosync_hq_dtx_timbre_score(analysis, drum_masks[event], t);
				score += onset_score;
			}
		}
		score /= (double)eventcount;
		meanscore += score;
		tested++;
		if(score > bestscore)
		{
			bestscore = score;
			bestshift = shift;
		}
	}
	if(tested) meanscore /= (double)tested;
	if((bestscore <= 0.03) || (bestscore < meanscore * 1.06))
		return 0.0;
	return bestshift;
}

int eof_spectrogram_auto_sync_notes(const char *audio_filename, unsigned long track, unsigned char diff, char selected_only, unsigned long max_shift_ms, unsigned long *events_considered, unsigned long *events_moved, double *mean_abs_shift)
{
	unsigned long tracksize, note, eventcount = 0, event, peakcount;
	unsigned long *eventpos = NULL, *eventtarget = NULL;
	unsigned short *event_pitchmask = NULL;
	unsigned short *event_drum_mask = NULL;
	unsigned char *anchor = NULL;
	long *note_event = NULL, *note_delta = NULL;
	double *confidence = NULL, *evidence_gain = NULL, *prevrow = NULL, *currow = NULL, *tmprow;
	struct eof_spectrogram_autosync_hq_peak *peaks = NULL;
	struct eof_spectrogram_autosync_hq_analysis analysis;
	EOF_PRO_GUITAR_TRACK *dtx_tp = NULL;
	unsigned char *trace = NULL;
	size_t rows, cols, cells, index;
	double earliest, latest, lower, upper, distance, predicted_distance, ratio, absolute_ratio;
	double score, match, best, global_shift;
	unsigned long i, j, moved = 0;
	unsigned long prior_rejected = 0, trajectory_rejected = 0, anchor_count = 0;
	unsigned long dtx_local_refined = 0, conduction_group_refined = 0, spacing_rejected = 0;
	double total_abs_shift = 0.0;
	int error = 0;

	memset(&analysis, 0, sizeof(analysis));
	if(events_considered) *events_considered = 0;
	if(events_moved) *events_moved = 0;
	if(mean_abs_shift) *mean_abs_shift = 0.0;

	if(!eof_song || !audio_filename || !audio_filename[0] || !track || (track >= eof_song->tracks) || !max_shift_ms)
		return 1;
	tracksize = eof_get_track_size(eof_song, track);
	if(!tracksize)
		return 1;
	if((track == EOF_TRACK_DRUM_DTX) && eof_song->track[track] &&
	   (eof_song->track[track]->tracknum < eof_song->pro_guitar_tracks))
		dtx_tp = eof_song->pro_guitar_track[eof_song->track[track]->tracknum];

	eventpos = (unsigned long *)malloc(sizeof(unsigned long) * (size_t)tracksize);
	eventtarget = (unsigned long *)malloc(sizeof(unsigned long) * (size_t)tracksize);
	event_pitchmask = (unsigned short *)calloc((size_t)tracksize, sizeof(unsigned short));
	event_drum_mask = (unsigned short *)calloc((size_t)tracksize, sizeof(unsigned short));
	anchor = (unsigned char *)calloc((size_t)tracksize, sizeof(unsigned char));
	note_event = (long *)malloc(sizeof(long) * (size_t)tracksize);
	note_delta = (long *)calloc((size_t)tracksize, sizeof(long));
	confidence = (double *)calloc((size_t)tracksize, sizeof(double));
	evidence_gain = (double *)calloc((size_t)tracksize, sizeof(double));
	if(!eventpos || !eventtarget || !event_pitchmask || !event_drum_mask || !anchor || !note_event || !note_delta || !confidence || !evidence_gain)
	{
		error = 1;
		goto cleanup;
	}
	for(note = 0; note < tracksize; note++)
		note_event[note] = -1L;

	for(note = 0; note < tracksize; note++)
	{
		unsigned long pos;

		if(eof_get_note_type(eof_song, track, note) != diff)
			continue;
		if(selected_only && ((eof_selection.track != track) || !eof_selection.multi[note]))
			continue;
		pos = eof_get_note_pos(eof_song, track, note);
		if(!eventcount || (eventpos[eventcount - 1UL] != pos))
		{
			eventpos[eventcount] = pos;
			eventtarget[eventcount] = ULONG_MAX;
			eventcount++;
		}
		note_event[note] = (long)(eventcount - 1UL);
		if((track != EOF_TRACK_DRUM_DTX) && eof_track_is_pro_guitar_track(eof_song, track))
		{
			unsigned char pitches[6] = {0};
			unsigned char pitchbits = eof_get_midi_pitches(eof_song, track, note, pitches, 0);
			unsigned stringnum;
			for(stringnum = 0; stringnum < 6U; stringnum++)
			{
				if(pitchbits & (1U << stringnum))
					event_pitchmask[eventcount - 1UL] |= (unsigned short)(1U << (pitches[stringnum] % 12U));
			}
		}
		if(dtx_tp && (note < dtx_tp->pgnotes) && dtx_tp->pgnote[note])
		{
			EOF_PRO_GUITAR_NOTE *dnp = dtx_tp->pgnote[note];
			unsigned stringnum, bitmask;
			for(stringnum = 0, bitmask = 1U; stringnum < 6U; stringnum++, bitmask <<= 1)
			{
				int channel;
				if(!(dnp->note & bitmask)) continue;
				channel = eof_dtx_integration_channel_from_midi(dnp->frets[stringnum] & 0x7F);
				if((channel >= 0) && (channel < EOF_DTX_INT_CHANNELS))
					event_drum_mask[eventcount - 1UL] |= (unsigned short)(1U << channel);
			}
		}

	}
	if(!eventcount)
	{
		error = 1;
		goto cleanup;
	}
	if(events_considered)
		*events_considered = eventcount;

	if(!eof_spectrogram_autosync_hq_build(audio_filename, track, &analysis))
	{
		error = 1;
		goto cleanup;
	}

	earliest = (eventpos[0] > max_shift_ms) ? (double)(eventpos[0] - max_shift_ms) : 0.0;
	latest = (double)eventpos[eventcount - 1UL] + (double)max_shift_ms;
	peakcount = eof_spectrogram_autosync_hq_find_peaks(&analysis, earliest, latest, &peaks);
	if(!peakcount)
		goto cleanup;

	global_shift = eof_spectrogram_autosync_hq_global_shift(&analysis, eventpos,
		(track == EOF_TRACK_DRUM_DTX) ? event_drum_mask : NULL, eventcount, max_shift_ms);
	(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"HQ Auto-sync: %lu chart event(s), %lu onset candidate(s), global shift %.1f ms",
		eventcount, peakcount, global_shift);
	eof_log(eof_log_string, 1);

	rows = (size_t)eventcount + 1U;
	cols = (size_t)peakcount + 1U;
	if(rows && (cols > ((size_t)-1) / rows))
	{
		error = 1;
		goto cleanup;
	}
	cells = rows * cols;
	trace = (unsigned char *)calloc(cells, sizeof(unsigned char));
	prevrow = (double *)calloc(cols, sizeof(double));
	currow = (double *)calloc(cols, sizeof(double));
	if(!trace || !prevrow || !currow)
	{
		error = 1;
		goto cleanup;
	}

	/* Sequence alignment: monotonic event/peak matching, but now informed by
	 * both local onset quality and an independently estimated global offset. */
	for(i = 1; i <= eventcount; i++)
	{
		currow[0] = 0.0;
		lower = (eventpos[i - 1UL] > max_shift_ms) ? (double)(eventpos[i - 1UL] - max_shift_ms) : 0.0;
		upper = (double)eventpos[i - 1UL] + (double)max_shift_ms;
		if(eof_song->beats && (lower < (double)eof_song->beat[0]->pos))
			lower = (double)eof_song->beat[0]->pos;
		if(i > 1UL)
		{
			double midpoint = ((double)eventpos[i - 2UL] + (double)eventpos[i - 1UL]) * 0.5;
			if(lower <= midpoint) lower = midpoint + 0.5;
		}
		if(i < eventcount)
		{
			double midpoint = ((double)eventpos[i - 1UL] + (double)eventpos[i]) * 0.5;
			if(upper >= midpoint) upper = midpoint - 0.5;
		}

		for(j = 1; j <= peakcount; j++)
		{
			index = (size_t)i * cols + (size_t)j;
			best = prevrow[j];
			trace[index] = 1;
			if(currow[j - 1UL] > best)
			{
				best = currow[j - 1UL];
				trace[index] = 2;
			}
			if((peaks[j - 1UL].pos >= lower) && (peaks[j - 1UL].pos <= upper))
			{
				distance = fabs(peaks[j - 1UL].pos - (double)eventpos[i - 1UL]);
				predicted_distance = fabs(peaks[j - 1UL].pos - ((double)eventpos[i - 1UL] + global_shift));
				ratio = predicted_distance / (double)max_shift_ms;
				absolute_ratio = distance / (double)max_shift_ms;
				{
					double chroma_score = eof_spectrogram_autosync_hq_chroma_score(&analysis, event_pitchmask[i - 1UL], peaks[j - 1UL].pos);
					double drum_score = eof_spectrogram_autosync_hq_dtx_timbre_score(&analysis, event_drum_mask[i - 1UL], peaks[j - 1UL].pos);
					score = 1.05 * peaks[j - 1UL].strength +
						0.75 * peaks[j - 1UL].prominence +
						0.30 * peaks[j - 1UL].support +
						0.30 * (chroma_score - 0.5) +
						0.28 * drum_score -
						0.40 * ratio - 0.24 * absolute_ratio - 0.08;
				}
				if(score > 0.0)
				{
					match = prevrow[j - 1UL] + score;
					if(match > best)
					{
						best = match;
						trace[index] = 3;
					}
				}
			}
			currow[j] = best;
		}
		tmprow = prevrow;
		prevrow = currow;
		currow = tmprow;
	}

	i = eventcount;
	j = peakcount;
	while(i && j)
	{
		unsigned char action = trace[(size_t)i * cols + (size_t)j];

		if(action == 3)
		{
			double target = peaks[j - 1UL].pos;
			if(target < 0.0) target = 0.0;
			eventtarget[i - 1UL] = (unsigned long)floor(target + 0.5);
			confidence[i - 1UL] = peaks[j - 1UL].strength +
				0.70 * peaks[j - 1UL].prominence + 0.25 * peaks[j - 1UL].support +
				0.20 * eof_spectrogram_autosync_hq_dtx_timbre_score(&analysis, event_drum_mask[i - 1UL], target);
			i--;
			j--;
		}
		else if(action == 2)
			j--;
		else
			i--;
	}

	/* DTX gets a second, instrument-aware fine search.  The global onset list can
	 * miss quiet hi-hat strokes under louder kick/snare transients, so search a
	 * narrow non-overlapping neighborhood around each imported timestamp using
	 * the corresponding drum sub-band profile. */
	if((track == EOF_TRACK_DRUM_DTX) && dtx_tp)
	{
		double step_ms = (double)analysis.hopsize * 500.0 / (double)analysis.rate;
		if(step_ms < 2.0) step_ms = 2.0;

		for(event = 0; event < eventcount; event++)
		{
			double original = (double)eventpos[event], lower_local, upper_local, candidate;
			double original_score, best_score, best_time, existing_score = -1.0;
			double radius = (max_shift_ms < 90UL) ? (double)max_shift_ms : 90.0;

			if(!event_drum_mask[event]) continue;
			lower_local = (original > radius) ? original - radius : 0.0;
			upper_local = original + radius;
			if(event > 0)
			{
				double midpoint = ((double)eventpos[event - 1UL] + original) * 0.5 + 1.0;
				if(lower_local < midpoint) lower_local = midpoint;
			}
			if(event + 1UL < eventcount)
			{
				double midpoint = (original + (double)eventpos[event + 1UL]) * 0.5 - 1.0;
				if(upper_local > midpoint) upper_local = midpoint;
			}
			if(upper_local <= lower_local) continue;

			original_score = eof_spectrogram_autosync_hq_dtx_event_score(&analysis, event_drum_mask[event], original);
			best_score = original_score;
			best_time = original;

			if(eventtarget[event] != ULONG_MAX)
			{
				double existing = (double)eventtarget[event];
				existing_score = eof_spectrogram_autosync_hq_dtx_event_score(&analysis, event_drum_mask[event], existing) -
					0.13 * (fabs(existing - original) / radius);
				if(existing_score > best_score)
				{
					best_score = existing_score;
					best_time = existing;
				}
			}

			for(candidate = lower_local; candidate <= upper_local; candidate += step_ms)
			{
				double candidate_score = eof_spectrogram_autosync_hq_dtx_event_score(&analysis, event_drum_mask[event], candidate) -
					0.13 * (fabs(candidate - original) / radius);
				if(candidate_score > best_score)
				{
					best_score = candidate_score;
					best_time = candidate;
				}
			}

			if((fabs(best_time - original) >= 2.0) &&
			   (best_score >= original_score + 0.028) &&
			   ((existing_score < 0.0) || (best_score >= existing_score + 0.010) ||
			    (fabs(best_time - original) < fabs((double)eventtarget[event] - original))))
			{
				eventtarget[event] = (unsigned long)floor(best_time + 0.5);
				confidence[event] = best_score;
				dtx_local_refined++;
			}
		}
	}

	/* Songsterr/GP timing is usually already close.  Treat it as a prior:
	 * a candidate must improve the audio evidence enough to justify its move. */
	for(event = 0; event < eventcount; event++)
	{
		long delta;
		double original_onset, target_onset, original_support, target_support;
		double original_chroma, target_chroma, original_drum, target_drum;
		double original_quality, target_quality, required_gain;

		if(eventtarget[event] == ULONG_MAX) continue;
		delta = (long)eventtarget[event] - (long)eventpos[event];
		original_onset = eof_spectrogram_autosync_hq_local_onset_evidence(&analysis, (double)eventpos[event]);
		target_onset = eof_spectrogram_autosync_hq_local_onset_evidence(&analysis, (double)eventtarget[event]);
		original_support = eof_spectrogram_autosync_hq_curve_at(&analysis, analysis.support, (double)eventpos[event]);
		target_support = eof_spectrogram_autosync_hq_curve_at(&analysis, analysis.support, (double)eventtarget[event]);
		original_chroma = eof_spectrogram_autosync_hq_chroma_score(&analysis, event_pitchmask[event], (double)eventpos[event]);
		target_chroma = eof_spectrogram_autosync_hq_chroma_score(&analysis, event_pitchmask[event], (double)eventtarget[event]);
		original_drum = eof_spectrogram_autosync_hq_dtx_timbre_score(&analysis, event_drum_mask[event], (double)eventpos[event]);
		target_drum = eof_spectrogram_autosync_hq_dtx_timbre_score(&analysis, event_drum_mask[event], (double)eventtarget[event]);

		original_quality = original_onset + 0.24 * original_support + 0.22 * (original_chroma - 0.5) + 0.30 * original_drum;
		target_quality = target_onset + 0.24 * target_support + 0.22 * (target_chroma - 0.5) + 0.30 * target_drum;
		evidence_gain[event] = target_quality - original_quality;

		required_gain = 0.010 + 0.085 * (fabs((double)delta) / (double)max_shift_ms);
		if(labs(delta) <= 10L) required_gain *= 0.35;
		else if(labs(delta) <= 30L) required_gain *= 0.70;
		if((target_chroma > original_chroma + 0.12) && event_pitchmask[event]) required_gain *= 0.80;
		if((target_drum > original_drum + 0.16) && event_drum_mask[event]) required_gain *= 0.76;

		if(evidence_gain[event] < required_gain)
		{
			eventtarget[event] = ULONG_MAX;
			prior_rejected++;
			continue;
		}
		if((labs(delta) > 90L) && (evidence_gain[event] < 0.11))
		{
			eventtarget[event] = ULONG_MAX;
			prior_rejected++;
			continue;
		}
		if((confidence[event] >= 0.58) && (evidence_gain[event] >= 0.035))
		{
			anchor[event] = 1;
			anchor_count++;
		}
	}

	/* Strong matches define a slowly varying local warp path.  Weak matches
	 * cannot jump away from that path just because another instrument has a
	 * nearby transient. */
	for(event = 0; event < eventcount; event++)
	{
		long previous_anchor = -1L, next_anchor = -1L, delta;
		double expected_shift = global_shift, disagreement;

		if(eventtarget[event] == ULONG_MAX || anchor[event]) continue;
		delta = (long)eventtarget[event] - (long)eventpos[event];
		for(i = event; i > 0; i--)
		{
			if(anchor[i - 1UL]) { previous_anchor = (long)(i - 1UL); break; }
		}
		for(i = event + 1UL; i < eventcount; i++)
		{
			if(anchor[i]) { next_anchor = (long)i; break; }
		}

		if((previous_anchor >= 0) && (next_anchor >= 0) &&
		   (eventpos[next_anchor] > eventpos[previous_anchor]) &&
		   (eventpos[next_anchor] - eventpos[previous_anchor] <= 4500UL))
		{
			double d0 = (double)((long)eventtarget[previous_anchor] - (long)eventpos[previous_anchor]);
			double d1 = (double)((long)eventtarget[next_anchor] - (long)eventpos[next_anchor]);
			double fraction = (double)(eventpos[event] - eventpos[previous_anchor]) /
				(double)(eventpos[next_anchor] - eventpos[previous_anchor]);
			expected_shift = d0 + (d1 - d0) * fraction;
		}
		else if((previous_anchor >= 0) && (eventpos[event] - eventpos[previous_anchor] <= 2200UL))
			expected_shift = (double)((long)eventtarget[previous_anchor] - (long)eventpos[previous_anchor]);
		else if((next_anchor >= 0) && (eventpos[next_anchor] - eventpos[event] <= 2200UL))
			expected_shift = (double)((long)eventtarget[next_anchor] - (long)eventpos[next_anchor]);

		disagreement = fabs((double)delta - expected_shift);
		if((disagreement > 38.0) && (evidence_gain[event] < 0.16))
		{
			eventtarget[event] = ULONG_MAX;
			trajectory_rejected++;
		}
	}

	(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"HQ Auto-sync refine: %lu strong anchor(s), %lu candidate(s) rejected by original-timing prior, %lu by local trajectory",
		anchor_count, prior_rejected, trajectory_rejected);
	eof_log(eof_log_string, 1);

	/* Reject weak isolated corrections that contradict nearby matched events.
	 * Strong transients are exempt so expressive timing is not over-smoothed. */
	for(event = 0; event < eventcount; event++)
	{
		long delta, previous_event = -1L, next_event = -1L;
		double expected, disagreement;

		if(eventtarget[event] == ULONG_MAX)
			continue;
		delta = (long)eventtarget[event] - (long)eventpos[event];
		if((fabs((double)delta - global_shift) > (double)max_shift_ms * 0.78) &&
		   (confidence[event] < 1.05))
		{
			eventtarget[event] = ULONG_MAX;
			continue;
		}

		for(i = event; i > 0; i--)
		{
			if(eventtarget[i - 1UL] != ULONG_MAX)
			{
				previous_event = (long)(i - 1UL);
				break;
			}
		}
		for(i = event + 1UL; i < eventcount; i++)
		{
			if(eventtarget[i] != ULONG_MAX)
			{
				next_event = (long)i;
				break;
			}
		}
		if((previous_event >= 0) && (next_event >= 0) &&
		   (eventpos[next_event] > eventpos[previous_event]) &&
		   (eventpos[next_event] - eventpos[previous_event] <= 1800UL))
		{
			double previous_delta = (double)((long)eventtarget[previous_event] - (long)eventpos[previous_event]);
			double next_delta = (double)((long)eventtarget[next_event] - (long)eventpos[next_event]);
			double fraction = (double)(eventpos[event] - eventpos[previous_event]) /
				(double)(eventpos[next_event] - eventpos[previous_event]);
			expected = previous_delta + (next_delta - previous_delta) * fraction;
			disagreement = fabs((double)delta - expected);
			if((disagreement > 65.0) && (confidence[event] < 1.18))
				eventtarget[event] = ULONG_MAX;
		}
	}

	/* Sequence-level DTX refinement.  Repetitive hi-hat/ride passages are more
	 * reliable as a pattern than as independent events.  Work in short windows
	 * so gradual tempo drift is retained, find the common shift which maximizes
	 * aggregate instrument-aware onset evidence, then allow only small residual
	 * per-hit deviations around that sequence shift. */
	if((track == EOF_TRACK_DRUM_DTX) && (eventcount >= 3UL))
	{
		unsigned long start = 0;
		double step_ms = (double)analysis.hopsize * 500.0 / (double)analysis.rate;
		if(step_ms < 2.0) step_ms = 2.0;

		while(start < eventcount)
		{
			unsigned long end = start, count, e;
			double min_original_gap = 1.0e30;

			while((end + 1UL < eventcount) && ((end - start + 1UL) < 16UL))
			{
				double gap = (double)(eventpos[end + 1UL] - eventpos[end]);
				if(!eof_spectrogram_autosync_hq_dtx_same_conduction(event_drum_mask[end], event_drum_mask[end + 1UL]))
					break;
				if((gap < 30.0) || (gap > 450.0))
					break;
				if((eventpos[end + 1UL] - eventpos[start]) > 1800UL)
					break;
				if(gap < min_original_gap) min_original_gap = gap;
				end++;
			}
			count = end - start + 1UL;
			if(count >= 3UL)
			{
				double radius = min_original_gap * 0.42;
				double zero_score = 0.0, best_score, best_shift = 0.0, shift;

				if(radius > 90.0) radius = 90.0;
				if(radius > (double)max_shift_ms) radius = (double)max_shift_ms;
				if(radius < 8.0) radius = 8.0;

				for(e = start; e <= end; e++)
					zero_score += eof_spectrogram_autosync_hq_dtx_event_score(&analysis, event_drum_mask[e], (double)eventpos[e]);
				zero_score /= (double)count;
				best_score = zero_score;

				for(shift = -radius; shift <= radius; shift += step_ms)
				{
					double sequence_score = 0.0;
					for(e = start; e <= end; e++)
					{
						double t = (double)eventpos[e] + shift;
						if(t < 0.0) continue;
						sequence_score += eof_spectrogram_autosync_hq_dtx_event_score(&analysis, event_drum_mask[e], t);
					}
					sequence_score /= (double)count;
					sequence_score -= 0.055 * (fabs(shift) / radius);
					if(sequence_score > best_score)
					{
						best_score = sequence_score;
						best_shift = shift;
					}
				}

				if((fabs(best_shift) >= 2.0) && (best_score >= zero_score + 0.022))
				{
					for(e = start; e <= end; e++)
					{
						double residual = 0.0, local_gap = min_original_gap;
						double residual_limit, target;
						if(eventtarget[e] != ULONG_MAX)
							residual = (double)((long)eventtarget[e] - (long)eventpos[e]) - best_shift;
						if(e > start)
						{
							double g = (double)(eventpos[e] - eventpos[e - 1UL]);
							if(g < local_gap) local_gap = g;
						}
						if(e < end)
						{
							double g = (double)(eventpos[e + 1UL] - eventpos[e]);
							if(g < local_gap) local_gap = g;
						}
						residual_limit = 4.0 + local_gap * 0.12;
						if(residual_limit < 7.0) residual_limit = 7.0;
						if(residual_limit > 20.0) residual_limit = 20.0;
						if(residual < -residual_limit) residual = -residual_limit;
						if(residual > residual_limit) residual = residual_limit;
						target = (double)eventpos[e] + best_shift + residual;
						if(target < 0.0) target = 0.0;
						if(fabs(target - (double)eventpos[e]) <= (double)max_shift_ms)
						{
							eventtarget[e] = (unsigned long)floor(target + 0.5);
							confidence[e] += 0.12 + (best_score - zero_score);
							conduction_group_refined++;
						}
					}
				}
			}
			start = (end > start) ? end + 1UL : start + 1UL;
		}
	}

	/* Final topology guard.  EOF's fixup merges notes which end up at the same
	 * timestamp.  For dense DTX timekeeping, also preserve the local inter-hit
	 * interval so alternating hi-hat/ride strokes cannot collapse into pairs. */
	if(eventcount > 1UL)
	{
		unsigned pass;
		for(pass = 0; pass < 3U; pass++)
		{
			int changed = 0;
			for(event = 1; event < eventcount; event++)
			{
				double original_gap = (double)(eventpos[event] - eventpos[event - 1UL]);
				double previous_pos = (eventtarget[event - 1UL] != ULONG_MAX) ?
					(double)eventtarget[event - 1UL] : (double)eventpos[event - 1UL];
				double current_pos = (eventtarget[event] != ULONG_MAX) ?
					(double)eventtarget[event] : (double)eventpos[event];
				double final_gap = current_pos - previous_pos;
				double min_gap = 2.0, max_gap = 1.0e30;
				int conduction = 0, invalid;
				int previous_moved = (eventtarget[event - 1UL] != ULONG_MAX);
				int current_moved = (eventtarget[event] != ULONG_MAX);

				if(original_gap <= 0.0) continue;
				if((track == EOF_TRACK_DRUM_DTX) &&
				   eof_spectrogram_autosync_hq_dtx_same_conduction(event_drum_mask[event - 1UL], event_drum_mask[event]) &&
				   (original_gap >= 30.0) && (original_gap <= 450.0))
				{
					conduction = 1;
					min_gap = original_gap * 0.68;
					max_gap = original_gap * 1.42;
				}
				invalid = (final_gap < min_gap) || (conduction && (final_gap > max_gap));
				if(!invalid || (!previous_moved && !current_moved))
					continue;

				if(previous_moved && !current_moved)
					eventtarget[event - 1UL] = ULONG_MAX;
				else if(!previous_moved && current_moved)
					eventtarget[event] = ULONG_MAX;
				else
				{
					double previous_reliability = evidence_gain[event - 1UL] + 0.12 * confidence[event - 1UL] + (anchor[event - 1UL] ? 0.08 : 0.0);
					double current_reliability = evidence_gain[event] + 0.12 * confidence[event] + (anchor[event] ? 0.08 : 0.0);
					if(previous_reliability < current_reliability)
						eventtarget[event - 1UL] = ULONG_MAX;
					else if(current_reliability < previous_reliability)
						eventtarget[event] = ULONG_MAX;
					else
					{
						double previous_delta = fabs((double)((long)eventtarget[event - 1UL] - (long)eventpos[event - 1UL]));
						double current_delta = fabs((double)((long)eventtarget[event] - (long)eventpos[event]));
						if(previous_delta > current_delta)
							eventtarget[event - 1UL] = ULONG_MAX;
						else
							eventtarget[event] = ULONG_MAX;
					}
				}
				spacing_rejected++;
				changed = 1;
			}
			if(!changed) break;
		}
	}

	(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"HQ Auto-sync DTX refine: %lu local correction(s), %lu conduction-group adjustment(s), %lu spacing/collision candidate(s) rejected",
		dtx_local_refined, conduction_group_refined, spacing_rejected);
	eof_log(eof_log_string, 1);

	for(note = 0; note < tracksize; note++)
	{
		long ev = note_event[note], delta;

		if((ev < 0) || (eventtarget[ev] == ULONG_MAX))
			continue;
		delta = (long)eventtarget[ev] - (long)eventpos[ev];
		if(labs(delta) < 3L)
			delta = 0L;
		note_delta[note] = delta;
	}

	for(event = 0; event < eventcount; event++)
	{
		long delta;

		if(eventtarget[event] == ULONG_MAX)
			continue;
		delta = (long)eventtarget[event] - (long)eventpos[event];
		if(labs(delta) < 3L)
			continue;
		moved++;
		total_abs_shift += (double)labs(delta);
	}
	if(!moved)
		goto cleanup;

	eof_prepare_undo(EOF_UNDO_TYPE_NONE);

	if(eof_technote_auto_adjust && eof_track_is_pro_guitar_track(eof_song, track))
	{
		EOF_PRO_GUITAR_TRACK *tp = eof_song->pro_guitar_track[eof_song->track[track]->tracknum];
		unsigned long tech, stringnum, bitmask;

		for(tech = 0; tech < tp->technotes; tech++)
		{
			long techdelta = 0L;
			char have_delta = 0, valid = 1;

			if(tp->technote[tech]->type != diff)
				continue;
			for(stringnum = 0, bitmask = 1UL; stringnum < 6UL; stringnum++, bitmask <<= 1)
			{
				unsigned long normalnote = 0;
				long delta;

				if(!(tp->technote[tech]->note & bitmask))
					continue;
				if(!eof_pro_guitar_tech_note_overlaps_a_note(tp, tech, bitmask, &normalnote) || (normalnote >= tracksize))
				{
					valid = 0;
					break;
				}
				delta = note_delta[normalnote];
				if(!delta)
				{
					valid = 0;
					break;
				}
				if(!have_delta)
				{
					techdelta = delta;
					have_delta = 1;
				}
				else if(delta != techdelta)
				{
					valid = 0;
					break;
				}
			}
			if(valid && have_delta)
			{
				long newpos = (long)tp->technote[tech]->pos + techdelta;
				if(newpos >= 0L)
					tp->technote[tech]->pos = (unsigned long)newpos;
			}
		}
	}

	for(note = 0; note < tracksize; note++)
	{
		long newpos;

		if(!note_delta[note])
			continue;
		newpos = (long)eof_get_note_pos(eof_song, track, note) + note_delta[note];
		if(newpos < 0L) newpos = 0L;
		eof_set_note_pos(eof_song, track, note, (unsigned long)newpos);
	}

	eof_notes_moved = 1;
	eof_track_sort_notes(eof_song, track);
	eof_track_fixup_notes(eof_song, track, 1);
	eof_determine_phrase_status(eof_song, track);
	eof_notes_moved = 0;

	if(eof_selection.track == track)
	{
		eof_selection.current = EOF_MAX_NOTES - 1;
		eof_selection.current_pos = 0;
		for(note = 0; note < eof_get_track_size(eof_song, track); note++)
		{
			if(eof_selection.multi[note] && (eof_get_note_type(eof_song, track, note) == diff))
			{
				eof_selection.current = note;
				eof_selection.current_pos = eof_get_note_pos(eof_song, track, note);
				break;
			}
		}
	}

	if(events_moved) *events_moved = moved;
	if(mean_abs_shift && moved)
		*mean_abs_shift = total_abs_shift / (double)moved;

cleanup:
	if(eventpos) free(eventpos);
	if(eventtarget) free(eventtarget);
	if(event_pitchmask) free(event_pitchmask);
	if(event_drum_mask) free(event_drum_mask);
	if(anchor) free(anchor);
	if(note_event) free(note_event);
	if(note_delta) free(note_delta);
	if(confidence) free(confidence);
	if(evidence_gain) free(evidence_gain);
	if(peaks) free(peaks);
	if(trace) free(trace);
	if(prevrow) free(prevrow);
	if(currow) free(currow);
	eof_spectrogram_autosync_hq_destroy(&analysis);
	return error;
}

/**
 * Returns a color for a given value on a scale
 *
 * @param value The value (from 0 to @max) to be represented
 * @param max The maximum value on the scale
 * @param scalenum The index of the scale being used
 * @return An integer representing a RGB color
 */
int eof_color_scale(double value, double max, short int scalenum)
{
	if(isnan(value) || isnan(max))
	{
		return 0;
	}
	eof_generate_colorscale(scalenum);

	if((eof_spectrogram_colorscale != NULL) && (eof_spectrogram_colorscale->colortable != NULL))
	{
		value = value/max;
		if(value < 0.0)
		{
			value = 0.0;
		}
		if(value > 1.0)
		{
			value = 1.0;
		}

		return eof_spectrogram_colorscale->colortable[(int)(value * eof_spectrogram_colorscale->maxval)];
	}

	return 0;
}

/**
 * Generates the given colorscale
 *
 * This function is where algorithms for the various colorscales are defined
 * It simply has a switch statement with a block for each scale
 * That creates a given colorscale in the colorscale datastructure
 *
 * @param scalenum The index number of the scale to generate
 */
void eof_generate_colorscale(char scalenum)
{
	int rgb[3] = {0,0,0};
	int cnt, scaledval;

	if((eof_spectrogram_colorscale != NULL) && (eof_spectrogram_colorscale->scalenum == scalenum))
		return;

	//Destroy any previous colorscale
	if(eof_spectrogram_colorscale != NULL)
	{
		if(eof_spectrogram_colorscale->colortable != NULL)
		{
			free(eof_spectrogram_colorscale->colortable);
		}
		free(eof_spectrogram_colorscale);
	}

	eof_spectrogram_colorscale = (struct spectrogramcolorscalestruct *)malloc(sizeof(struct spectrogramcolorscalestruct));

	if(eof_spectrogram_colorscale != NULL)
	{
		//Initialize the maximum value of the colorscale
		//Determined by the number of values available
		eof_spectrogram_colorscale->scalenum = scalenum;
		switch(scalenum)
		{
			case 0:
				eof_spectrogram_colorscale->maxval = 255.0;
				break;
			case 1:
				eof_spectrogram_colorscale->maxval = 1280.0;
				break;
			default:	//If scalenum is some unexpected value, use the default value of 1 for it
				eof_spectrogram_colorscale->maxval = 1280.0;
				break;
		}

		eof_spectrogram_colorscale->colortable = (int *)malloc((eof_spectrogram_colorscale->maxval+1) * sizeof(int));

		//Generate the color values
		for(scaledval = 0; scaledval <= eof_spectrogram_colorscale->maxval; scaledval++)
		{ //Loop through, calculating a color for each point on the scale
			switch(scalenum)
			{
				case 0:
					//Grayscale, just R=G=B=value
					for(cnt=0;cnt<3;cnt++)
					{
						rgb[cnt] = scaledval;
					}
				break;
				case 1:
					//More complex heatmap, rainbow (red->orange->green->blue)
					rgb[0] = 384.0 - fabs(scaledval-896.0);
					rgb[1] = 384.0 - fabs(scaledval-640.0);
					rgb[2] = 384.0 - fabs(scaledval-384.0);
					for(cnt=0;cnt<3;cnt++)
					{
						if(rgb[cnt] > 255)
						{
							rgb[cnt] = 255;
						}
						if(rgb[cnt] < 0)
						{
							rgb[cnt] = 0;
						}
					}
					break;
				default:
				break;
			}
			eof_spectrogram_colorscale->colortable[scaledval] = makecol(rgb[0],rgb[1],rgb[2]);
		}
	}
}

void eof_render_spectrogram_line(struct spectrogramstruct *spectrogram,struct spectrogramchanneldata *channel,unsigned amp,unsigned long x,int color)
{
	unsigned long maxampoffset;	//The difference between the zero amplitude and the channel's maximum amplitude
	unsigned long yoffset;	//The offset from the y axis coordinate to render the line to

	if(spectrogram != NULL)
	{
		if(channel->maxamp > spectrogram->zeroamp)
			maxampoffset = channel->maxamp - spectrogram->zeroamp;
		else
			maxampoffset = spectrogram->zeroamp - channel->maxamp;

		if(amp > spectrogram->zeroamp)	//Render positive amplitude
		{	//Transform y to fit between 0 and zeroamp, then scale to fit the graph
			yoffset=(amp - spectrogram->zeroamp) * (channel->height / 2) / maxampoffset;
			vline(eof_window_editor->screen, x, channel->yaxis, channel->yaxis - yoffset, color);
		}
		else
		{	//Correct the negative amplitude, then scale it to fit the graph
			yoffset=(spectrogram->zeroamp - amp) * (channel->height / 2) / maxampoffset;
			vline(eof_window_editor->screen, x, channel->yaxis, channel->yaxis + yoffset, color);
		}
	}
}

#define EOF_DEBUG_SPECTROGRAM
static struct spectrogramstruct *eof_create_spectrogram_internal(char *oggfilename, int windowsize, int hopsize, char window_function)
{
	ALOGG_OGG *oggstruct=NULL;
	SAMPLE *audio=NULL;
	void * oggbuffer = NULL;
	struct spectrogramstruct *spectrogram=NULL;
	static struct spectrogramstruct emptyspectrogram;	//all variables in this auto initialize to value 0
	char done=0;	//-1 on unsuccessful completion, 1 on successful completion
	unsigned long slicenum=0;
	clock_t starttime = 0, endtime = 0;

	fftw_plan fftplan = NULL;

	eof_log("\tGenerating spectrogram", 1);
	eof_log("eof_create_spectrogram() entered", 1);
	set_window_title("Generating Spectrogram...");
	starttime = clock();	//Get the start time of the spectrogram creation

	if(oggfilename == NULL)
	{
		#ifdef EOF_DEBUG_SPECTROGRAM
		allegro_message("Spectrogram: Invalid parameters");
		#endif
		return NULL;
	}

//Load OGG file into memory
	oggbuffer = eof_buffer_file(oggfilename, 0, 0);
	if(!oggbuffer)
	{
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Spectrogram: Failed to open input audio file: %s",strerror(errno));
		eof_log(eof_log_string, 1);
		return NULL;
	}
	oggstruct=alogg_create_ogg_from_buffer(oggbuffer, (int) file_size_ex(oggfilename));
	if(oggstruct == NULL)
	{
		eof_log("Spectrogram: ALOGG failed to open input audio file", 1);
		free(oggbuffer);
		return NULL;
	}

//Decode OGG into memory
	audio=alogg_create_sample_from_ogg(oggstruct);
	if(audio == NULL)
	{
		eof_log("Spectrogram: ALOGG failed to decode input audio file", 1);
		done=-1;
	}
	else if(audio->len == 0)
	{
		eof_log("Spectrogram: ALOGG failed to process input audio file", 1);
		done=-1;
	}
	else if((audio->bits != 8) && (audio->bits != 16))	//This logic currently only supports 8 and 16 bit audio
	{
		eof_log("Spectrogram: Invalid sample size", 1);
		done=-1;
	}
	else
	{
//Initialize spectrogram structure
		spectrogram=(struct spectrogramstruct *)malloc(sizeof(struct spectrogramstruct));
		if(spectrogram == NULL)
		{
			eof_log("Spectrogram: Unable to allocate memory for the spectrogram structure", 1);
			done=-1;
		}
		else
		{
			*spectrogram=emptyspectrogram;					//Set all variables to value zero
			spectrogram->windowsize = windowsize;
			spectrogram->hopsize = hopsize;
			spectrogram->window_function = window_function;
			spectrogram->numbuff = 1;
			if(alogg_get_wave_is_stereo_ogg(oggstruct))	//If this audio file has two audio channels
				spectrogram->is_stereo = 1;
			else
				spectrogram->is_stereo = 0;

			if(audio->bits == 8)
				spectrogram->zeroamp = 128;	//128 represents amplitude 0 for unsigned 8 bit audio samples
			else
				spectrogram->zeroamp = 32768;	//32768 represents amplitude 0 for unsigned 16 bit audio samples
			spectrogram->log_max = log(spectrogram->windowsize * spectrogram->zeroamp);	//Cache this value, since it is needed to render each pixel of the spectrogram

			spectrogram->oggfilename = (char *)malloc(strlen(oggfilename)+1);
			if(spectrogram->oggfilename == NULL)
			{
				eof_log("Spectrogram: Unable to allocate memory for the audio filename string", 1);
				done=-1;
			}
			else
			{
				spectrogram->rate = audio->freq;
				spectrogram->windowlength = (double)spectrogram->hopsize / (double)audio->freq * 1000.0;

				spectrogram->numslices = ((unsigned long)audio->len + (unsigned long)spectrogram->hopsize - 1UL) / (unsigned long)spectrogram->hopsize;

				strcpy(spectrogram->oggfilename,oggfilename);
				spectrogram->left.slices=(struct spectrogramslice *)calloc(spectrogram->numslices, sizeof(struct spectrogramslice));
				if(spectrogram->left.slices == NULL)
				{
					eof_log("Spectrogram: Unable to allocate memory for the left channel spectrogram data", 1);
					done=-1;
				}
				else if(spectrogram->is_stereo)	//If this OGG is stereo
				{				//Allocate memory for the right channel spectrogram data
					spectrogram->right.slices=(struct spectrogramslice *)calloc(spectrogram->numslices, sizeof(struct spectrogramslice));
					if(spectrogram->right.slices == NULL)
					{
						eof_log("Spectrogram: Unable to allocate memory for the right channel spectrogram data", 1);
						done=-1;
					}
				}
			}
		}
	}

	//Allocate memory for the buffer pointers
	if((done != -1) && spectrogram)
	{	//If there wasn't an error yet
		clock_t starttime2 = 0, endtime2 = 0;

		spectrogram->buffin=(double *) fftw_malloc(sizeof(double) * spectrogram->windowsize*spectrogram->numbuff);
		spectrogram->buffout=(double *) fftw_malloc(sizeof(double) * spectrogram->windowsize*spectrogram->numbuff);
		if(!spectrogram->buffin || !spectrogram->buffout)
		{
			eof_log("Spectrogram: FFTW buffer allocation failed", 1);
			done = -1;
		}
		else
		{
			starttime2 = clock();
			fftplan=fftw_plan_r2r_1d(spectrogram->windowsize,spectrogram->buffin,spectrogram->buffout,FFTW_R2HC,FFTW_DESTROY_INPUT | FFTW_EXHAUSTIVE);
			endtime2 = clock();
			if(!fftplan)
			{
				eof_log("Spectrogram: FFTW plan creation failed", 1);
				done = -1;
			}
			else
			{
				(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tFFTW plan generated in %f seconds.", (double)(endtime2 - starttime2) / (double)CLOCKS_PER_SEC);
				eof_log(eof_log_string, 1);
				eof_log("\tGenerating slices...", 1);
				while(!done)
				{
					done=eof_process_next_spectrogram_slice(spectrogram,audio,slicenum++,fftplan);
				}
			}
		}
	}

//Cleanup
	if(fftplan)
		fftw_destroy_plan(fftplan);
	if(spectrogram && spectrogram->buffin)
	{
		fftw_free(spectrogram->buffin);
		spectrogram->buffin = NULL;
	}
	if(spectrogram && spectrogram->buffout)
	{
		fftw_free(spectrogram->buffout);
		spectrogram->buffout = NULL;
	}
	alogg_destroy_ogg(oggstruct);
	if(audio != NULL)
		destroy_sample(audio);
	if(oggbuffer)
		free(oggbuffer);
	if(done == -1)
	{	//Unsuccessful completion
		eof_destroy_spectrogram(spectrogram);
		eof_log("Spectrogram: Failed to generate spectrogram", 1);
		return NULL;	//Return error
	}

	endtime = clock();	//Get the start time of the spectrogram creation
	(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tSpectrogram generated in %f seconds", (double)(endtime - starttime) / (double)CLOCKS_PER_SEC);
	eof_log(eof_log_string, 1);

	return spectrogram;	//Return spectrogram data
}

struct spectrogramstruct *eof_create_spectrogram(char *oggfilename)
{
	return eof_create_spectrogram_internal(oggfilename, eof_spectrogram_windowsize, eof_spectrogram_windowsize, 0);
}

static int eof_spectrogram_experimental_build_cache(struct spectrogramstruct *spectrogram)
{
	unsigned long band, slice, firstbin, endbin, halfsize, scan;
	unsigned long bands = EOF_EXPERIMENTAL_SPECTROGRAM_CACHE_BANDS;
	unsigned long *edges = NULL;
	double *current = NULL, *previous = NULL;
	unsigned char *cache = NULL;
	double fmin = MINFREQ, fmax, ratio, frequency, display_reference;
	size_t cells;

	if(!spectrogram || !spectrogram->left.slices || !spectrogram->numslices || !bands)
		return 0;
	if((size_t)spectrogram->numslices > ((size_t)-1) / (size_t)bands)
		return 0;
	cells = (size_t)spectrogram->numslices * (size_t)bands;

	fmax = (double)spectrogram->rate * 0.5;
	if(fmax > 20000.0) fmax = 20000.0;
	if(fmax <= fmin) return 0;
	display_reference = (spectrogram->displaymax > 0.0) ? spectrogram->displaymax : spectrogram->destmax;
	if(display_reference <= 0.0) return 0;

	edges = (unsigned long *)malloc(sizeof(unsigned long) * (size_t)(bands + 1UL));
	current = (double *)malloc(sizeof(double) * (size_t)bands);
	previous = (double *)calloc((size_t)bands, sizeof(double));
	cache = (unsigned char *)malloc(cells);
	if(!edges || !current || !previous || !cache)
		goto fail;

	halfsize = (unsigned long)spectrogram->windowsize / 2UL;
	ratio = pow(fmax / fmin, 1.0 / (double)bands);
	frequency = fmin;
	for(band = 0; band <= bands; band++)
	{
		unsigned long bin = (unsigned long)(frequency * (double)spectrogram->windowsize / (double)spectrogram->rate);
		if(bin < 1UL) bin = 1UL;
		if(bin > halfsize) bin = halfsize;
		edges[band] = bin;
		frequency *= ratio;
	}
	edges[bands] = halfsize;

	set_window_title("Precomputing HQ spectrogram display cache...");
	for(slice = 0; slice < spectrogram->numslices; slice++)
	{
		double framepeak = 0.0;

		for(band = 0; band < bands; band++)
		{
			firstbin = edges[band];
			endbin = edges[band + 1UL];
			current[band] = eof_spectrogram_experimental_band_rms(spectrogram, slice, firstbin, endbin);
			if(current[band] > framepeak) framepeak = current[band];
		}

		for(band = 0; band < bands; band++)
		{
			double value = current[band], normalized = 0.0;
			double db, absolute_norm, local_norm = 0.0, neighborhood, contrast_db, contrast_norm;
			double transient_db, transient_norm = 0.0, sumlog = 0.0;
			unsigned long startband, endband, count = 0;
			unsigned long radius = 6UL;

			if(value > 0.0)
			{
				db = 20.0 * log10(value / display_reference);
				absolute_norm = (db <= -EOF_EXPERIMENTAL_SPECTROGRAM_DB_RANGE) ? 0.0 :
					(db + EOF_EXPERIMENTAL_SPECTROGRAM_DB_RANGE) / EOF_EXPERIMENTAL_SPECTROGRAM_DB_RANGE;
				if(absolute_norm < 0.0) absolute_norm = 0.0;
				if(absolute_norm > 1.0) absolute_norm = 1.0;

				if(framepeak > 0.0)
				{
					double local_db = 20.0 * log10(value / framepeak);
					local_norm = (local_db + 55.0) / 55.0;
					if(local_norm < 0.0) local_norm = 0.0;
					if(local_norm > 1.0) local_norm = 1.0;
				}

				startband = (band > radius) ? band - radius : 0UL;
				endband = band + radius;
				if(endband >= bands) endband = bands - 1UL;
				for(scan = startband; scan <= endband; scan++)
				{
					sumlog += log(current[scan] + 1.0);
					count++;
				}
				neighborhood = count ? exp(sumlog / (double)count) - 1.0 : 0.0;
				contrast_db = (neighborhood > 0.0) ? 20.0 * log10((value + 1.0) / (neighborhood + 1.0)) : 0.0;
				contrast_norm = contrast_db / 18.0;
				if(contrast_norm < 0.0) contrast_norm = 0.0;
				if(contrast_norm > 1.0) contrast_norm = 1.0;

				if(previous[band] > 0.0)
				{
					transient_db = 20.0 * log10((value + 1.0) / (previous[band] + 1.0));
					transient_norm = transient_db / 18.0;
					if(transient_norm < 0.0) transient_norm = 0.0;
					if(transient_norm > 1.0) transient_norm = 1.0;
				}

				normalized = 0.66 * absolute_norm +
					0.22 * local_norm * sqrt(absolute_norm) +
					0.08 * transient_norm * sqrt(absolute_norm) +
					0.12 * contrast_norm * sqrt(absolute_norm);
				if(normalized > 1.0) normalized = 1.0;
			}
			cache[(size_t)slice * bands + band] = (unsigned char)floor(normalized * 255.0 + 0.5);
		}
		memcpy(previous, current, sizeof(double) * (size_t)bands);
	}

	spectrogram->experimental_cache = cache;
	spectrogram->experimental_cache_bands = bands;
	spectrogram->experimental_cache_fmin = fmin;
	spectrogram->experimental_cache_fmax = fmax;
	free(edges);
	free(current);
	free(previous);
	return 1;

fail:
	if(edges) free(edges);
	if(current) free(current);
	if(previous) free(previous);
	if(cache) free(cache);
	return 0;
}

static void eof_spectrogram_experimental_release_fft_bins(struct spectrogramstruct *spectrogram)
{
	unsigned long slice;

	if(!spectrogram) return;
	for(slice = 0; slice < spectrogram->numslices; slice++)
	{
		if(spectrogram->left.slices && spectrogram->left.slices[slice].amplist)
		{
			free(spectrogram->left.slices[slice].amplist);
			spectrogram->left.slices[slice].amplist = NULL;
		}
		if(spectrogram->right.slices && spectrogram->right.slices[slice].amplist)
		{
			free(spectrogram->right.slices[slice].amplist);
			spectrogram->right.slices[slice].amplist = NULL;
		}
	}
}

static double eof_spectrogram_experimental_robust_displaymax(struct spectrogramstruct *spectrogram)
{
	double *peaks;
	double value, left, right, peak;
	unsigned long slice, bin, halfsize, sample_step, count = 0, index;

	if(!spectrogram || !spectrogram->left.slices || !spectrogram->numslices)
		return 0.0;
	sample_step = (spectrogram->numslices > 12000UL) ? (spectrogram->numslices / 12000UL) : 1UL;
	peaks = (double *)malloc(sizeof(double) * ((spectrogram->numslices + sample_step - 1UL) / sample_step));
	if(!peaks)
		return spectrogram->destmax;

	halfsize = (unsigned long)spectrogram->windowsize / 2UL;
	for(slice = 0; slice < spectrogram->numslices; slice += sample_step)
	{
		peak = 0.0;
		for(bin = 1; bin <= halfsize; bin++)
		{
			left = spectrogram->left.slices[slice].amplist ? fabs(spectrogram->left.slices[slice].amplist[bin]) : 0.0;
			if(spectrogram->is_stereo && spectrogram->right.slices && spectrogram->right.slices[slice].amplist)
			{
				right = fabs(spectrogram->right.slices[slice].amplist[bin]);
				value = sqrt((left * left + right * right) * 0.5);
			}
			else
				value = left;
			if(value > peak) peak = value;
		}
		peaks[count++] = peak;
	}
	if(!count)
	{
		free(peaks);
		return spectrogram->destmax;
	}
	qsort(peaks, count, sizeof(double), eof_spectrogram_autosync_hq_double_compare);
	index = (unsigned long)floor((double)(count - 1UL) * 0.995);
	value = peaks[index];
	free(peaks);
	if(value <= 0.0) value = spectrogram->destmax;
	return value;
}

struct spectrogramstruct *eof_create_spectrogram_experimental(char *oggfilename)
{
	struct spectrogramstruct *spectrogram;

	spectrogram = eof_create_spectrogram_internal(oggfilename, EOF_EXPERIMENTAL_SPECTROGRAM_WINDOWSIZE, EOF_EXPERIMENTAL_SPECTROGRAM_HOPSIZE, 1);
	if(spectrogram)
	{
		set_window_title("Optimizing experimental spectrogram contrast...");
		spectrogram->displaymax = eof_spectrogram_experimental_robust_displaymax(spectrogram);
		if(eof_spectrogram_experimental_build_cache(spectrogram))
		{
			eof_log("Experimental spectrogram: HQ display cache ready; releasing raw FFT bins", 1);
			eof_spectrogram_experimental_release_fft_bins(spectrogram);
		}
		else
		{
			eof_log("Experimental spectrogram: display cache allocation failed; using slower live renderer", 1);
		}
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"Experimental spectrogram: robust 99.5%% display ceiling = %.3f (absolute max %.3f)",
			spectrogram->displaymax, spectrogram->destmax);
		eof_log(eof_log_string, 1);
	}
	return spectrogram;
}

int eof_process_next_spectrogram_slice(struct spectrogramstruct *spectrogram,SAMPLE *audio,unsigned long slicenum,fftw_plan fftplan)
{
	unsigned long sampleindex=0;	//The byte index into audio->data
	unsigned long startsample=0;	//The sample number of the first sample being processed
	unsigned long samplesize=0;	//Number of bytes for each sample: 1 for 8 bit audio, 2 for 16 bit audio.  Doubled for stereo
	unsigned long cursamp=0;
	unsigned long halfsize;
	long sample=0;
	char channel=0;
	struct spectrogramslice *dest;	//The structure to write this slice's data to
	char outofsamples=0;		//Will be set to 1 if all samples in the audio structure have been processed

//Validate parameters
	if((spectrogram == NULL) || (spectrogram->left.slices == NULL) || (audio == NULL))
	{
		eof_log("\tNo sound found", 1);
		return -1;	//Return error
	}

	if(spectrogram->is_stereo && (spectrogram->right.slices == NULL))
	{
		eof_log("\tStereo channel not found", 1);
		return -1;	//Return error
	}

	if((slicenum >= spectrogram->numslices))
	{	//If this is more than the number of slices that were supposed to be read
		eof_log("\tSlicenum is too far", 1);
		return 1;	//Return out of samples
	}

	samplesize=audio->bits / 8;
	if(spectrogram->is_stereo)		//Stereo data is interleaved as left channel, right channel, ...
		samplesize+=samplesize;	//Double the sample size

	for(channel=0;channel<=spectrogram->is_stereo;channel++)	//Process loop once for mono track, twice for stereo track
	{
//Initialize processing for this audio channel
		startsample=slicenum * spectrogram->hopsize; //This is the sample index for this slices starting sample
		sampleindex=startsample * samplesize;		//This is the byte index for this slice's starting sample number

		if(channel)							//If processing the sample for the right channel
			sampleindex+=(audio->bits / 8);	//Seek past the left channel sample

//Process audio samples for this channel
		for(cursamp=0;cursamp < (unsigned long)spectrogram->windowsize;cursamp++)
		{
			if(startsample + cursamp >= audio->len)	//If there are no more samples to read
			{
				//Zero-pad the remaining analysis window.  With overlap, a later frame may still begin before EOF.
				memset(spectrogram->buffin+cursamp,0,sizeof(double) * ((unsigned long)spectrogram->windowsize - cursamp));
				break;
			}

			sample=((unsigned char *)audio->data)[sampleindex];	//Store first sample byte (Allegro documentation states the sample data is stored in unsigned format)
			if(audio->bits > 8)	//If this sample is more than one byte long (16 bit)
				sample+=((unsigned char *)audio->data)[sampleindex+1]<<8;	//Assume little endian byte order, read the next (high byte) of data

			sample -= spectrogram->zeroamp;
			if(spectrogram->window_function && (spectrogram->windowsize > 1))
			{
				double hann = 0.5 - 0.5 * cos((2.0 * 3.14159265358979323846 * (double)cursamp) / (double)(spectrogram->windowsize - 1));
				spectrogram->buffin[cursamp] = (double)sample * hann;
			}
			else
			{
				spectrogram->buffin[cursamp] = (double)sample;
			}

			sampleindex+=samplesize;		//Adjust index to point to next sample for this channel
		}
		fftw_execute(fftplan);
		if(channel == 0)
		{
			dest=&spectrogram->left.slices[slicenum];
		}
		else
		{
			dest=&(spectrogram->right.slices[slicenum]);	//Store results to right channel array
		}
		halfsize = (unsigned long)spectrogram->windowsize / 2UL;
		if(halfsize == 0)
		{	//Logic error
			return -1;	//Return error
		}
		dest->amplist=(double*)malloc(sizeof(double) * (halfsize + 1));
		if(dest->amplist == NULL)
		{
			eof_log("Spectrogram: Unable to allocate memory for the amplitude data", 1);
			return -1;	//Return error
		}
		dest->amplist[0] = spectrogram->buffout[0];			//The first one is all real
		for(cursamp=halfsize - 1; cursamp > 0; cursamp--)
		{
			dest->amplist[cursamp] = sqrt(
				spectrogram->buffout[cursamp] * spectrogram->buffout[cursamp] +
				spectrogram->buffout[spectrogram->windowsize - cursamp] *
				spectrogram->buffout[spectrogram->windowsize - cursamp]
			);
			if(dest->amplist[cursamp] > spectrogram->destmax)
			{
				spectrogram->destmax = dest->amplist[cursamp];
			}
		}
		dest->amplist[halfsize] = spectrogram->buffout[halfsize];
	}

	if(startsample + (unsigned long)spectrogram->hopsize >= (unsigned long)audio->len)
		outofsamples = 1;
	return outofsamples;	//Return success/completed status
}

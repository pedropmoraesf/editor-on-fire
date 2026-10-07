#ifndef EOF_TONE_ANALYSIS_HQ_H
#define EOF_TONE_ANALYSIS_HQ_H

#include <stddef.h>
#include "song.h"

/* High-quality mixed-audio estimator used by the Rocksmith tone workflow. */
int eof_tone_analysis_hq_menu(void);

/* Estimates the two dominant broad guitar-effect families for one arrangement
 * without changing the EOF chart.  This is used by the PSARC exporter. */
int eof_tone_analysis_hq_estimate(EOF_SONG *sp, unsigned long track,
	char *base, size_t basesz, char *other, size_t othersz);

/* True after a successful tone-change analysis in the current session, or when
 * the arrangement already contains an AI-generated tone label from a saved
 * project. */
int eof_tone_analysis_hq_track_has_results(EOF_SONG *sp, unsigned long track);

#endif

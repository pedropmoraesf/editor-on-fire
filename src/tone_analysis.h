#ifndef EOF_TONE_ANALYSIS_H
#define EOF_TONE_ANALYSIS_H

/* Installs Track > Rocksmith > Tone change > Detect from audio. */
void eof_tone_analysis_install_menu(void);

/* Analyzes the loaded chart audio and generates estimated Rocksmith tone
 * changes for the currently selected pro guitar/bass arrangement. */
int eof_tone_analysis_menu(void);

#endif

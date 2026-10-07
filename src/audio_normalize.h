#ifndef EOF_AUDIO_NORMALIZE_H
#define EOF_AUDIO_NORMALIZE_H

#include <allegro.h>
#include <stddef.h>

extern char eof_audio_normalize_target_text[32];
extern int eof_audio_normalize_new_project_checked;
extern int eof_audio_normalize_psarc_checked;
extern int eof_audio_normalize_new_dialog_context;
extern int eof_audio_normalize_skip_next_ogg_dialog;
extern int eof_audio_normalize_pending_new_project;

void eof_audio_normalize_install(void);
int eof_audio_normalize_new_wizard(void);
int eof_audio_normalize_ogg_settings_popup(DIALOG *dialog, int focus);
int eof_audio_normalize_intercept_load_ogg(char *filename, char function);
int eof_audio_normalize_resolve_ffmpeg(int allow_prompt);
int eof_audio_normalize_parse_target(const char *text, double *value);
double eof_audio_normalize_target_value(void);
int eof_audio_normalize_wav_in_place(const char *path, double target);
int eof_audio_normalize_ogg_path_in_place(const char *path, double target, int bitrate_kbps);
int eof_menu_song_normalize_audio(void);

#endif

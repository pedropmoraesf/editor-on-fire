#include <allegro.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agup/agup.h"
#include "main.h"
#include "dialog.h"
#include "utility.h"
#include "beat.h"
#include "rs.h"
#include "undo.h"
#include "menu/file.h"
#include "menu/track.h"
#include "tone_analysis_hq.h"
#include "tone_workflow.h"
#include "psarc_export.h"

#define EOF_PSARC_MENU_MAX 32
#define EOF_PSARC_MAX_TONE_ROWS 32
#define EOF_PSARC_SUGGESTIONS 5

#define PSARC_DLG_TITLE_E      2
#define PSARC_DLG_ARTIST_E     4
#define PSARC_DLG_TONES       12
#define PSARC_DLG_LOAD        13
#define PSARC_DLG_ANALYZE     15
#define PSARC_DLG_SUGGEST     16
#define PSARC_DLG_USE         17
#define PSARC_DLG_DD          18
#define PSARC_DLG_SILENCE     19
#define PSARC_DLG_STICKS      22
#define PSARC_DLG_OK          23
#define PSARC_DLG_CANCEL      24

static MENU eof_psarc_export_menu[EOF_PSARC_MENU_MAX];
static int eof_psarc_menu_installed = 0;
static int eof_psarc_helper_built_this_session = 0;

static char eof_psarc_title[256] = {0};
static char eof_psarc_artist[256] = {0};
static char eof_psarc_album[256] = {0};
static char eof_psarc_year[16] = "2026";
static char eof_psarc_version[32] = "1";
static char eof_psarc_intro[16] = "3000";
static char eof_psarc_source_label[96] = "No imported effect";

typedef struct
{
	unsigned long track;
	unsigned char slot;
	char display[192];
	char tone_name[EOF_SECTION_NAME_LENGTH + 1];
	char source[1024];
	char suggestions[EOF_PSARC_SUGGESTIONS][48];
	int suggestion_choice;
	int suggestions_ready;
	int built_in_bass;
} EOF_PSARC_TONE_ROW;

static EOF_PSARC_TONE_ROW eof_psarc_tone_rows[EOF_PSARC_MAX_TONE_ROWS];
static int eof_psarc_tone_row_count = 0;
static DIALOG eof_psarc_dialog[];

static int eof_psarc_is_arrangement_track(EOF_SONG *sp, unsigned long track)
{
	if(!sp || !track || track >= sp->tracks)
		return 0;
	if(track == EOF_TRACK_DRUM_DTX)
		return 0;
	if(!eof_track_is_pro_guitar_track(sp, track) || !eof_get_track_size(sp, track))
		return 0;
	return 1;
}

static int eof_psarc_track_is_bass(EOF_SONG *sp, unsigned long track)
{
	unsigned long ptrack;
	EOF_PRO_GUITAR_TRACK *tp;
	if(!eof_psarc_is_arrangement_track(sp, track))
		return 0;
	ptrack = sp->track[track]->tracknum;
	if(ptrack >= sp->pro_guitar_tracks || !sp->pro_guitar_track[ptrack])
		return 0;
	tp = sp->pro_guitar_track[ptrack];
	if(tp->arrangement == EOF_BASS_ARRANGEMENT)
		return 1;
	return (track == EOF_TRACK_PRO_BASS || track == EOF_TRACK_PRO_BASS_22) ? 1 : 0;
}

static int eof_psarc_find_support_file(const char *name, char *out, size_t outsz)
{
	char exe[1024] = {0}, dir[1024] = {0}, candidate[1024] = {0};
	if(!name || !out || !outsz)
		return 0;
	out[0] = '\0';
	get_executable_name(exe, sizeof(exe));
	ustrzcpy(dir, sizeof(dir), exe);
	*get_filename(dir) = '\0';
	(void)snprintf(candidate, sizeof(candidate) - 1, "%stools%cpsarc%c%s",
		dir, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR, name);
	fix_filename_slashes(candidate);
	if(exists(candidate))
	{
		ustrzcpy(out, (int)outsz, candidate);
		return 1;
	}
	(void)snprintf(candidate, sizeof(candidate) - 1, "%s..%ctools%cpsarc%c%s",
		dir, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR, name);
	fix_filename_slashes(candidate);
	if(exists(candidate))
	{
		ustrzcpy(out, (int)outsz, candidate);
		return 1;
	}
	(void)snprintf(candidate, sizeof(candidate) - 1, "tools%cpsarc%c%s",
		OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR, name);
	fix_filename_slashes(candidate);
	if(exists(candidate))
	{
		ustrzcpy(out, (int)outsz, candidate);
		return 1;
	}
	return 0;
}

static int eof_psarc_track_needs_two_layout(EOF_PRO_GUITAR_TRACK *tp)
{
	unsigned long intro = 1UL;
	if(!tp || !tp->pgnotes)
		return 0;
	if(tp->note[0] && tp->note[0]->pos)
		intro = tp->note[0]->pos;
	if(tp->tonechanges != 2UL || !tp->defaulttone[0])
		return 1;
	if(tp->tonechange[0].start_pos != 0UL || tp->tonechange[0].end_pos != 1UL)
		return 1;
	if(ustricmp(tp->tonechange[0].name, tp->defaulttone))
		return 1;
	if(tp->tonechange[1].start_pos != intro)
		return 1;
	return 0;
}

static int eof_psarc_generic_tone_name(const char *name)
{
	if(!name || !name[0]) return 1;
	if(!ustricmp(name, "Default") || !ustricmp(name, "Tone 2")) return 1;
	if(!strncmp(name, "AI ", 3)) return 1;
	return 0;
}

static void eof_psarc_prepare_bass_names(EOF_PRO_GUITAR_TRACK *tp)
{
	if(!tp || tp->tonechanges < 2UL)
		return;
	if(eof_psarc_generic_tone_name(tp->defaulttone) || eof_psarc_generic_tone_name(tp->tonechange[0].name))
	{
		ustrzcpy(tp->defaulttone, sizeof(tp->defaulttone), "EOF Bass Default");
		ustrzcpy(tp->tonechange[0].name, sizeof(tp->tonechange[0].name), "EOF Bass Default");
	}
	if(eof_psarc_generic_tone_name(tp->tonechange[1].name))
		ustrzcpy(tp->tonechange[1].name, sizeof(tp->tonechange[1].name), "EOF Bass Alternate");
}

static unsigned eof_psarc_ensure_two_arrangements(void)
{
	unsigned long track;
	unsigned count = 0;
	int undo_made = 0;
	if(!eof_song) return 0;
	for(track = 1; track < eof_song->tracks; track++)
	{
		unsigned long ptrack;
		EOF_PRO_GUITAR_TRACK *tp;
		int needs, bass;
		if(!eof_psarc_is_arrangement_track(eof_song, track))
			continue;
		ptrack = eof_song->track[track]->tracknum;
		if(ptrack >= eof_song->pro_guitar_tracks || !eof_song->pro_guitar_track[ptrack])
			continue;
		tp = eof_song->pro_guitar_track[ptrack];
		eof_track_pro_guitar_sort_tone_changes(tp);
		needs = eof_psarc_track_needs_two_layout(tp);
		bass = eof_psarc_track_is_bass(eof_song, track);
		if((needs || bass) && !undo_made)
		{
			eof_undo_add(EOF_UNDO_TYPE_NONE);
			undo_made = 1;
		}
		if(needs)
			eof_tone_reduce_track_to_two(eof_song, track, 0);
		if(bass)
			eof_psarc_prepare_bass_names(tp);
		count++;
	}
	if(undo_made)
		eof_project_unsaved = 1;
	return count;
}

static char *eof_psarc_tone_list(int index, int *size)
{
	if(index < 0)
	{
		if(size) *size = eof_psarc_tone_row_count;
		return NULL;
	}
	if(index >= eof_psarc_tone_row_count)
		return NULL;
	return eof_psarc_tone_rows[index].display;
}

static int eof_psarc_tone_list_proc(int msg, DIALOG *d, int c)
{
	int before, ret;
	if(!d) return D_O_K;
	before = d->d1;
	ret = d_agup_list_proc(msg, d, c);
	if(msg == MSG_CLICK && d->d1 != before)
		return D_CLOSE;
	return ret;
}

static char *eof_psarc_suggestion_list(int index, int *size)
{
	int row = 0;
	if(eof_psarc_tone_row_count > 0)
	{
		row = eof_psarc_dialog[PSARC_DLG_TONES].d1;
		if(row < 0 || row >= eof_psarc_tone_row_count)
			row = 0;
	}
	if(index < 0)
	{
		if(size) *size = EOF_PSARC_SUGGESTIONS;
		return NULL;
	}
	if(index >= EOF_PSARC_SUGGESTIONS || !eof_psarc_tone_row_count)
		return NULL;
	return eof_psarc_tone_rows[row].suggestions[index];
}

static DIALOG eof_psarc_dialog[] =
{
	{ d_agup_window_proc, 0, 0, 610, 430, 0, 0, 0, 0, 0, 0, (void *)"Export Rocksmith 2014 PSARC", NULL, NULL },
	{ d_agup_text_proc, 18, 32, 80, 16, 0, 0, 0, 0, 0, 0, (void *)"Song name:", NULL, NULL },
	{ d_agup_edit_proc, 102, 28, 200, 20, 0, 0, 0, 0, 255, 0, eof_psarc_title, NULL, NULL },
	{ d_agup_text_proc, 318, 32, 50, 16, 0, 0, 0, 0, 0, 0, (void *)"Artist:", NULL, NULL },
	{ d_agup_edit_proc, 370, 28, 220, 20, 0, 0, 0, 0, 255, 0, eof_psarc_artist, NULL, NULL },
	{ d_agup_text_proc, 18, 62, 80, 16, 0, 0, 0, 0, 0, 0, (void *)"Album:", NULL, NULL },
	{ d_agup_edit_proc, 102, 58, 200, 20, 0, 0, 0, 0, 255, 0, eof_psarc_album, NULL, NULL },
	{ d_agup_text_proc, 318, 62, 45, 16, 0, 0, 0, 0, 0, 0, (void *)"Year:", NULL, NULL },
	{ d_agup_edit_proc, 370, 58, 72, 20, 0, 0, 0, 0, 4, 0, eof_psarc_year, NULL, NULL },
	{ d_agup_text_proc, 460, 62, 58, 16, 0, 0, 0, 0, 0, 0, (void *)"Version:", NULL, NULL },
	{ d_agup_edit_proc, 520, 58, 70, 20, 0, 0, 0, 0, 12, 0, eof_psarc_version, NULL, NULL },
	{ d_agup_text_proc, 18, 96, 260, 16, 0, 0, 0, 0, 0, 0, (void *)"Populated arrangements / tones", NULL, NULL },
	{ eof_psarc_tone_list_proc, 18, 116, 355, 174, 0, 0, 0, 0, 0, 0, eof_psarc_tone_list, NULL, NULL },
	{ d_agup_button_proc, 390, 116, 198, 24, 0, 0, 0, D_EXIT, 0, 0, (void *)"Load effect (PSARC/JSON)", NULL, NULL },
	{ d_agup_text_proc, 390, 146, 198, 16, 0, 0, 0, 0, 0, 0, eof_psarc_source_label, NULL, NULL },
	{ d_agup_button_proc, 390, 172, 198, 28, 0, 0, 0, D_EXIT, 0, 0, (void *)"Analyze guitar effects", NULL, NULL },
	{ d_agup_list_proc, 390, 172, 198, 90, 0, 0, 0, D_HIDDEN, 0, 0, eof_psarc_suggestion_list, NULL, NULL },
	{ d_agup_button_proc, 390, 266, 198, 24, 0, 0, 0, D_EXIT|D_HIDDEN, 0, 0, (void *)"Use selected suggestion", NULL, NULL },
	{ d_agup_check_proc, 18, 306, 290, 18, 0, 0, 0, D_SELECTED, 0, 0, (void *)"Generate dynamic difficulty", NULL, NULL },
	{ d_agup_radio_proc, 18, 334, 154, 18, 0, 0, 0, D_SELECTED, 1, 0, (void *)"Add silence before song", NULL, NULL },
	{ d_agup_text_proc, 183, 336, 70, 16, 0, 0, 0, 0, 0, 0, (void *)"Target ms:", NULL, NULL },
	{ d_agup_edit_proc, 254, 331, 78, 20, 0, 0, 0, 0, 8, 0, eof_psarc_intro, NULL, NULL },
	{ d_agup_radio_proc, 355, 334, 232, 18, 0, 0, 0, 0, 1, 0, (void *)"Stick count-in at predominant BPM", NULL, NULL },
	{ d_agup_button_proc, 380, 386, 100, 28, 0, 0, KEY_ENTER, D_EXIT, 0, 0, (void *)"OK", NULL, NULL },
	{ d_agup_button_proc, 488, 386, 100, 28, 0, 0, KEY_ESC, D_EXIT, 0, 0, (void *)"Cancel", NULL, NULL },
	{ NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, NULL }
};

static void eof_psarc_seed_suggestions(EOF_PSARC_TONE_ROW *row, const char *hint)
{
	const char *family = hint ? hint : "";
	if(!row) return;
	if(strstr(family, "Drive") || strstr(family, "drive") || strstr(family, "Distortion") || strstr(family, "distortion"))
	{
		ustrzcpy(row->suggestions[0], 48, "drive"); ustrzcpy(row->suggestions[1], 48, "overdrive");
		ustrzcpy(row->suggestions[2], 48, "distortion+delay"); ustrzcpy(row->suggestions[3], 48, "distortion+chorus"); ustrzcpy(row->suggestions[4], 48, "fuzz");
	}
	else if(strstr(family, "Modulation") || strstr(family, "modulation") || strstr(family, "Chorus") || strstr(family, "chorus"))
	{
		ustrzcpy(row->suggestions[0], 48, "chorus"); ustrzcpy(row->suggestions[1], 48, "flanger"); ustrzcpy(row->suggestions[2], 48, "phaser");
		ustrzcpy(row->suggestions[3], 48, "tremolo"); ustrzcpy(row->suggestions[4], 48, "clean+chorus");
	}
	else if(strstr(family, "Delay") || strstr(family, "delay"))
	{
		ustrzcpy(row->suggestions[0], 48, "delay"); ustrzcpy(row->suggestions[1], 48, "delay+reverb"); ustrzcpy(row->suggestions[2], 48, "drive+delay");
		ustrzcpy(row->suggestions[3], 48, "clean+delay"); ustrzcpy(row->suggestions[4], 48, "reverb");
	}
	else if(strstr(family, "Reverb") || strstr(family, "reverb"))
	{
		ustrzcpy(row->suggestions[0], 48, "reverb"); ustrzcpy(row->suggestions[1], 48, "delay+reverb"); ustrzcpy(row->suggestions[2], 48, "clean+reverb");
		ustrzcpy(row->suggestions[3], 48, "drive+reverb"); ustrzcpy(row->suggestions[4], 48, "delay");
	}
	else
	{
		ustrzcpy(row->suggestions[0], 48, "clean"); ustrzcpy(row->suggestions[1], 48, "clean+reverb"); ustrzcpy(row->suggestions[2], 48, "chorus");
		ustrzcpy(row->suggestions[3], 48, "acoustic"); ustrzcpy(row->suggestions[4], 48, "delay+reverb");
	}
	if(row->suggestion_choice < 0 || row->suggestion_choice >= EOF_PSARC_SUGGESTIONS) row->suggestion_choice = 0;
}

static void eof_psarc_build_tone_rows(void)
{
	EOF_PSARC_TONE_ROW old[EOF_PSARC_MAX_TONE_ROWS];
	int oldcount = eof_psarc_tone_row_count;
	unsigned long track;
	int i;
	char basspreset[1024] = {0};
	int have_basspreset = eof_psarc_find_support_file("default_bass_tone.json", basspreset, sizeof(basspreset));
	memcpy(old, eof_psarc_tone_rows, sizeof(old));
	memset(eof_psarc_tone_rows, 0, sizeof(eof_psarc_tone_rows));
	eof_psarc_tone_row_count = 0;
	if(!eof_song) return;
	for(track = 1; track < eof_song->tracks && eof_psarc_tone_row_count + 1 < EOF_PSARC_MAX_TONE_ROWS; track++)
	{
		unsigned long ptrack;
		EOF_PRO_GUITAR_TRACK *tp;
		const char *name0, *name1;
		int bass;
		if(!eof_psarc_is_arrangement_track(eof_song, track)) continue;
		ptrack = eof_song->track[track]->tracknum;
		if(ptrack >= eof_song->pro_guitar_tracks || !eof_song->pro_guitar_track[ptrack]) continue;
		tp = eof_song->pro_guitar_track[ptrack];
		bass = eof_psarc_track_is_bass(eof_song, track);
		eof_track_pro_guitar_sort_tone_changes(tp);
		name0 = tp->defaulttone[0] ? tp->defaulttone : (tp->tonechanges ? tp->tonechange[0].name : "Default");
		name1 = (tp->tonechanges > 1UL) ? tp->tonechange[1].name : "Tone 2";
		for(i = 0; i < 2; i++)
		{
			EOF_PSARC_TONE_ROW *row = &eof_psarc_tone_rows[eof_psarc_tone_row_count];
			int j;
			row->track = track; row->slot = (unsigned char)i;
			ustrzcpy(row->tone_name, sizeof(row->tone_name), i ? name1 : name0);
			(void)snprintf(row->display, sizeof(row->display) - 1, "%s | %s: %s", eof_song->track[track]->name, i ? "Tone 2" : "Default", row->tone_name);
			row->suggestion_choice = 0; eof_psarc_seed_suggestions(row, row->tone_name);
			row->suggestions_ready = !strncmp(row->tone_name, "AI ", 3) ? 1 : 0;
			for(j = 0; j < oldcount; j++) if(old[j].track == track && old[j].slot == (unsigned char)i)
			{
				ustrzcpy(row->source, sizeof(row->source), old[j].source); row->suggestion_choice = old[j].suggestion_choice;
				if(old[j].suggestions_ready) row->suggestions_ready = 1; break;
			}
			if(bass && !row->source[0] && have_basspreset) { ustrzcpy(row->source, sizeof(row->source), basspreset); row->built_in_bass = 1; }
			eof_psarc_tone_row_count++;
		}
	}
}

static void eof_psarc_update_dialog_mode(void)
{
	int row = eof_psarc_dialog[PSARC_DLG_TONES].d1;
	unsigned long track = 0;
	int bass = 0, analyzed = 0, ready = 0;
	if(row < 0 || row >= eof_psarc_tone_row_count) row = 0;
	if(eof_psarc_tone_row_count)
	{
		EOF_PSARC_TONE_ROW *tr = &eof_psarc_tone_rows[row];
		track = tr->track; bass = eof_psarc_track_is_bass(eof_song, track); analyzed = eof_tone_analysis_hq_track_has_results(eof_song, track); ready = tr->suggestions_ready;
		if(tr->source[0])
		{
			if(tr->built_in_bass) ustrzcpy(eof_psarc_source_label, sizeof(eof_psarc_source_label), "Built-in bass preset");
			else (void)snprintf(eof_psarc_source_label, sizeof(eof_psarc_source_label) - 1, "Loaded: %.72s", get_filename(tr->source));
		}
		else ustrzcpy(eof_psarc_source_label, sizeof(eof_psarc_source_label), "No imported effect");
	}
	else ustrzcpy(eof_psarc_source_label, sizeof(eof_psarc_source_label), "No imported effect");
	if(bass)
	{
		eof_psarc_dialog[PSARC_DLG_ANALYZE].flags |= D_HIDDEN; eof_psarc_dialog[PSARC_DLG_SUGGEST].flags |= D_HIDDEN; eof_psarc_dialog[PSARC_DLG_USE].flags |= D_HIDDEN;
	}
	else if(ready || analyzed)
	{
		eof_psarc_dialog[PSARC_DLG_ANALYZE].flags |= D_HIDDEN; eof_psarc_dialog[PSARC_DLG_SUGGEST].flags &= ~D_HIDDEN; eof_psarc_dialog[PSARC_DLG_USE].flags &= ~D_HIDDEN;
		if(eof_psarc_tone_row_count) eof_psarc_dialog[PSARC_DLG_SUGGEST].d1 = eof_psarc_tone_rows[row].suggestion_choice;
	}
	else
	{
		eof_psarc_dialog[PSARC_DLG_ANALYZE].flags &= ~D_HIDDEN; eof_psarc_dialog[PSARC_DLG_SUGGEST].flags |= D_HIDDEN; eof_psarc_dialog[PSARC_DLG_USE].flags |= D_HIDDEN;
	}
}

static int eof_psarc_analyze_effects(void)
{
	int row = eof_psarc_dialog[PSARC_DLG_TONES].d1, i;
	unsigned long track;
	char basehint[EOF_SECTION_NAME_LENGTH + 1] = {0}, otherhint[EOF_SECTION_NAME_LENGTH + 1] = {0};
	if(row < 0 || row >= eof_psarc_tone_row_count) return 0;
	track = eof_psarc_tone_rows[row].track;
	if(eof_psarc_track_is_bass(eof_song, track)) { allegro_message("Analyze guitar effects is only used for guitar arrangements.\nBass uses the bundled bass preset unless you load another tone."); return 0; }
	if(eof_tone_analysis_hq_track_has_results(eof_song, track)) { allegro_message("This arrangement already has a tone-change analysis from EOF.\nThe PSARC exporter uses those existing estimates instead of running another analysis."); return 0; }
	eof_cursor_visible = 0; eof_pen_visible = 0; eof_render();
	if(!eof_tone_analysis_hq_estimate(eof_song, track, basehint, sizeof(basehint), otherhint, sizeof(otherhint)))
	{
		eof_cursor_visible = 1; eof_pen_visible = 1; allegro_message("High-quality whole-song guitar effect analysis failed.\nSee eof_log.txt for details."); return 0;
	}
	eof_cursor_visible = 1; eof_pen_visible = 1;
	for(i = 0; i < eof_psarc_tone_row_count; i++) if(eof_psarc_tone_rows[i].track == track)
	{
		eof_psarc_seed_suggestions(&eof_psarc_tone_rows[i], eof_psarc_tone_rows[i].slot ? otherhint : basehint); eof_psarc_tone_rows[i].suggestions_ready = 1;
	}
	return 1;
}

static int eof_psarc_predominant_bpm(EOF_SONG *sp)
{
	unsigned long i;
	int histogram[301] = {0}, best = 120, bestcount = 0;
	if(!sp || sp->beats < 2) return 120;
	for(i = 0; i + 1UL < sp->beats; i++)
	{
		double len = sp->beat[i + 1UL]->fpos - sp->beat[i]->fpos; int bpm;
		if(len <= 0.0) continue;
		bpm = (int)(60000.0 / len + 0.5); while(bpm < 60) bpm *= 2; while(bpm > 240) bpm /= 2;
		if(bpm >= 30 && bpm <= 300 && ++histogram[bpm] > bestcount) { bestcount = histogram[bpm]; best = bpm; }
	}
	return best;
}

static void eof_psarc_put_be32(FILE *fp, unsigned long value)
{
	fputc((int)((value >> 24) & 0xFF), fp); fputc((int)((value >> 16) & 0xFF), fp); fputc((int)((value >> 8) & 0xFF), fp); fputc((int)(value & 0xFF), fp);
}

static int eof_psarc_append_byte(unsigned char *buffer, size_t capacity, unsigned long *pos, unsigned char value)
{
	if(!buffer || !pos || *pos >= capacity) return 0; buffer[(*pos)++] = value; return 1;
}

static int eof_psarc_append_vlq(unsigned char *buffer, size_t capacity, unsigned long *pos, unsigned long value)
{
	unsigned char tmp[5]; int n = 0, i; tmp[n++] = (unsigned char)(value & 0x7F); while((value >>= 7) != 0) tmp[n++] = (unsigned char)((value & 0x7F) | 0x80);
	for(i = n - 1; i >= 0; i--) if(!eof_psarc_append_byte(buffer, capacity, pos, tmp[i])) return 0; return 1;
}

static int eof_psarc_write_click_midi(const char *path, int bpm, unsigned long duration_ms)
{
	unsigned char trackdata[65536]; unsigned long pos = 0, beats, i, mpqn; double beatms; FILE *fp;
	if(!path || bpm <= 0) return 0; beatms = 60000.0 / (double)bpm; beats = (unsigned long)((double)duration_ms / beatms + 0.999); if(beats < 1UL) beats = 1UL; mpqn = (unsigned long)(60000000.0 / (double)bpm);
#define MIDI_BYTE(v) do { if(!eof_psarc_append_byte(trackdata, sizeof(trackdata), &pos, (unsigned char)(v))) return 0; } while(0)
	MIDI_BYTE(0); MIDI_BYTE(0xFF); MIDI_BYTE(0x51); MIDI_BYTE(3); MIDI_BYTE((mpqn >> 16) & 0xFF); MIDI_BYTE((mpqn >> 8) & 0xFF); MIDI_BYTE(mpqn & 0xFF);
	for(i = 0; i < beats; i++) { if(!eof_psarc_append_vlq(trackdata, sizeof(trackdata), &pos, (i == 0UL) ? 0UL : 420UL)) return 0; MIDI_BYTE(0x99); MIDI_BYTE(37); MIDI_BYTE(112); if(!eof_psarc_append_vlq(trackdata, sizeof(trackdata), &pos, 60UL)) return 0; MIDI_BYTE(0x89); MIDI_BYTE(37); MIDI_BYTE(0); }
	if(!eof_psarc_append_vlq(trackdata, sizeof(trackdata), &pos, 420UL)) return 0; MIDI_BYTE(0xFF); MIDI_BYTE(0x2F); MIDI_BYTE(0);
#undef MIDI_BYTE
	fp = fopen(path, "wb"); if(!fp) return 0; fwrite("MThd", 1, 4, fp); eof_psarc_put_be32(fp, 6); fputc(0, fp); fputc(0, fp); fputc(0, fp); fputc(1, fp); fputc(1, fp); fputc(0xE0, fp); fwrite("MTrk", 1, 4, fp); eof_psarc_put_be32(fp, pos); fwrite(trackdata, 1, pos, fp); fclose(fp); return 1;
}

static int eof_psarc_prompt_path(const char *section, const char *key, const char *title, const char *ext, char *out, size_t outsz)
{
	const char *saved = get_config_string(section, key, ""); char path[1024] = {0};
	if(saved && saved[0] && exists(saved)) { ustrzcpy(out, (int)outsz, saved); return 1; }
	if(!file_select_ex(title, path, ext, sizeof(path), 560, 420)) return 0; if(!exists(path)) return 0; ustrzcpy(out, (int)outsz, path); set_config_string(section, key, out); flush_config_file(); return 1;
}

static int eof_psarc_get_toolkit_root(char *out, size_t outsz)
{
	const char *saved = get_config_string("psarc", "rstoolkit_root", ""); char dll[1024] = {0}, candidate[1024] = {0};
	if(saved && saved[0]) { (void)snprintf(candidate, sizeof(candidate) - 1, "%s%cRocksmithToolkitLib.dll", saved, OTHER_PATH_SEPARATOR); if(eof_folder_exists(saved) && exists(candidate)) { ustrzcpy(out, (int)outsz, saved); return 1; } }
	if(!file_select_ex("Locate RocksmithToolkitLib.dll (RSToolkit)", dll, "dll", sizeof(dll), 560, 420)) return 0; if(!exists(dll)) return 0; ustrzcpy(out, (int)outsz, dll); *get_filename(out) = '\0'; set_config_string("psarc", "rstoolkit_root", out); flush_config_file(); return 1;
}

static int eof_psarc_build_helper(const char *toolkit_root, char *helper, size_t helpersz)
{
	char exe[1024] = {0}, programdir[1024] = {0}, script[1024] = {0}, source[1024] = {0}, command[4096] = {0};
	if(!toolkit_root || !helper || !helpersz) return 0; (void)snprintf(helper, helpersz - 1, "%s%ceof_psarc_helper.exe", toolkit_root, OTHER_PATH_SEPARATOR); if(eof_psarc_helper_built_this_session && exists(helper)) return 1;
	get_executable_name(exe, sizeof(exe)); ustrzcpy(programdir, sizeof(programdir), exe); *get_filename(programdir) = '\0';
	(void)snprintf(script, sizeof(script) - 1, "%stools%cpsarc%cbuild_helper.ps1", programdir, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR); (void)snprintf(source, sizeof(source) - 1, "%stools%cpsarc%ceof_psarc_helper.cs", programdir, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR);
	if(!exists(script) || !exists(source)) { (void)snprintf(script, sizeof(script) - 1, "%s..%ctools%cpsarc%cbuild_helper.ps1", programdir, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR); (void)snprintf(source, sizeof(source) - 1, "%s..%ctools%cpsarc%ceof_psarc_helper.cs", programdir, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR, OTHER_PATH_SEPARATOR); }
	if(!exists(script) || !exists(source)) return 0; (void)snprintf(command, sizeof(command) - 1, "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" -ToolkitRoot \"%s\" -Source \"%s\"", script, toolkit_root, source);
	if(system(command) != 0 || !exists(helper)) return 0; eof_psarc_helper_built_this_session = 1; return 1;
}

static int eof_psarc_build_audio(const char *staging, unsigned long prefix_ms, int sticks, int bpm, char *wav, size_t wavsz)
{
	char ffmpeg[1024] = {0}, command[4096] = {0}, preview[1024] = {0};
	char midi[1024] = {0}, clickwav[1024] = {0};
	char fluidsynth[1024] = {0}, sf2[1024] = {0};
	const char *saved;
	(void)bpm;
	(void)snprintf(wav, wavsz - 1, "%s%cpsarc_audio.wav", staging, OTHER_PATH_SEPARATOR);

	/* Direct sampled-count-in path.  This deliberately bypasses the legacy
	 * system()/FFmpeg/FluidSynth compatibility chain.  The force-included PSARC
	 * support headers provide these two static helpers in this translation unit. */
	if(sticks)
	{
		if(!eof_psarc_internal_ogg_to_wav_compat(eof_loaded_ogg_name, wav, prefix_ms, 0, 120))
		{
			eof_log("PSARC direct stick V5: internal OGG-to-WAV preparation failed.", 1);
			return 0;
		}
		if(!eof_psarc_stick_v2_mix_to_downbeat(wav, prefix_ms))
		{
			delete_file(wav);
			eof_log("PSARC direct stick V5: drumstick.wav mixing failed.", 1);
			return 0;
		}
		if(!exists(wav))
		{
			eof_log("PSARC direct stick V5: staged WAV missing after sample mix.", 1);
			return 0;
		}
		eof_log("PSARC direct stick V5: staged PCM16 WAV ready; FFmpeg/loudnorm/FluidSynth bypassed.", 1);
		(void)snprintf(preview, sizeof(preview) - 1, "%s%cpsarc_audio_preview.wav", staging, OTHER_PATH_SEPARATOR);
		return eof_copy_file(wav, preview);
	}

	saved = get_config_string("paths", "eof_ffmpeg_executable_path", "");
	if(saved && saved[0] && exists(saved)) ustrzcpy(ffmpeg, sizeof(ffmpeg), saved);
	else if(!eof_psarc_prompt_path("paths", "eof_ffmpeg_executable_path", "Locate ffmpeg.exe", "exe", ffmpeg, sizeof(ffmpeg))) return 0;
	if(!prefix_ms)
		(void)snprintf(command, sizeof(command) - 1, "\"%s\" -y -hide_banner -loglevel error -i \"%s\" -ar 44100 -ac 2 \"%s\"", ffmpeg, eof_loaded_ogg_name, wav);
	else
		(void)snprintf(command, sizeof(command) - 1, "\"%s\" -y -hide_banner -loglevel error -f lavfi -t %.3f -i anullsrc=r=44100:cl=stereo -i \"%s\" -filter_complex \"[0:a][1:a]concat=n=2:v=0:a=1[out]\" -map \"[out]\" -ar 44100 -ac 2 \"%s\"", ffmpeg, (double)prefix_ms / 1000.0, eof_loaded_ogg_name, wav);
	if(system(command) != 0 || !exists(wav)) return 0;
	(void)midi; (void)clickwav; (void)fluidsynth; (void)sf2;
	(void)snprintf(preview, sizeof(preview) - 1, "%s%cpsarc_audio_preview.wav", staging, OTHER_PATH_SEPARATOR);
	return eof_copy_file(wav, preview);
}

static void eof_psarc_json_string(FILE *fp, const char *text)
{
	const unsigned char *p = (const unsigned char *)(text ? text : ""); fputc('"', fp);
	while(*p) { if(*p == '"' || *p == '\\') { fputc('\\', fp); fputc(*p, fp); } else if(*p == '\n') fputs("\\n", fp); else if(*p == '\r') fputs("\\r", fp); else if(*p == '\t') fputs("\\t", fp); else if(*p < 32) fprintf(fp, "\\u%04x", (unsigned)*p); else fputc(*p, fp); p++; }
	fputc('"', fp);
}

static void eof_psarc_dlc_key(char *out, size_t outsz)
{
	char combined[520]; size_t i, o = 0; (void)snprintf(combined, sizeof(combined) - 1, "%s%s", eof_psarc_artist, eof_psarc_title);
	for(i = 0; combined[i] && o + 1 < outsz; i++) if(isalnum((unsigned char)combined[i])) out[o++] = combined[i];
	if(!o) ustrzcpy(out, (int)outsz, "EOFCustomSong"); else out[o] = '\0';
}

static const char *eof_psarc_track_role(unsigned long track)
{
	switch(track) { case EOF_TRACK_PRO_GUITAR: return "rhythm"; case EOF_TRACK_PRO_GUITAR_22: return "lead"; case EOF_TRACK_PRO_GUITAR_B: return "alt_rhythm"; case EOF_TRACK_PRO_GUITAR_22_BONUS: return "alt_lead"; case EOF_TRACK_PRO_BASS: case EOF_TRACK_PRO_BASS_22: return "bass"; default: return "auto"; }
}

static int eof_psarc_write_spec(const char *path, const char *wav, const char *wwise, const char *output, char xmlpaths[EOF_TRACKS_MAX][1024], int bpm, int dd, unsigned long prefix_ms)
{
	FILE *fp = fopen(path, "wb"); unsigned long track; char key[256] = {0}; int first = 1; if(!fp) return 0; eof_psarc_dlc_key(key, sizeof(key));
	fputs("{\n  \"artist\": ", fp); eof_psarc_json_string(fp, eof_psarc_artist); fputs(",\n  \"title\": ", fp); eof_psarc_json_string(fp, eof_psarc_title); fputs(",\n  \"album\": ", fp); eof_psarc_json_string(fp, eof_psarc_album);
	fprintf(fp, ",\n  \"year\": %d,\n  \"version\": ", atoi(eof_psarc_year)); eof_psarc_json_string(fp, eof_psarc_version); fputs(",\n  \"dlcKey\": ", fp); eof_psarc_json_string(fp, key);
	fprintf(fp, ",\n  \"averageTempo\": %d,\n  \"dynamicDifficulty\": %s,\n  \"prefixMs\": %lu,\n  \"musicLengthMs\": %lu,\n  \"wwise\": ", bpm, dd ? "true" : "false", prefix_ms, eof_music_length); eof_psarc_json_string(fp, wwise);
	fputs(",\n  \"wav\": ", fp); eof_psarc_json_string(fp, wav); fputs(",\n  \"output\": ", fp); eof_psarc_json_string(fp, output); fputs(",\n  \"arrangements\": [\n", fp);
	for(track = 1; track < eof_song->tracks; track++)
	{
		int r0 = -1, r1 = -1, i; if(!xmlpaths[track][0]) continue;
		for(i = 0; i < eof_psarc_tone_row_count; i++) { if(eof_psarc_tone_rows[i].track == track && eof_psarc_tone_rows[i].slot == 0) r0 = i; if(eof_psarc_tone_rows[i].track == track && eof_psarc_tone_rows[i].slot == 1) r1 = i; }
		if(r0 < 0 || r1 < 0) continue; if(!first) fputs(",\n", fp); first = 0;
		fprintf(fp, "    {\"track\": %lu, \"role\": ", track); eof_psarc_json_string(fp, eof_psarc_track_role(track)); fputs(", \"xml\": ", fp); eof_psarc_json_string(fp, xmlpaths[track]); fputs(", \"tones\": [", fp);
		for(i = 0; i < 2; i++) { EOF_PSARC_TONE_ROW *row = &eof_psarc_tone_rows[i ? r1 : r0]; if(i) fputs(", ", fp); fputs("{\"name\": ", fp); eof_psarc_json_string(fp, row->tone_name); fputs(", \"source\": ", fp); eof_psarc_json_string(fp, row->source); fputs(", \"suggestion\": ", fp); eof_psarc_json_string(fp, row->suggestions[row->suggestion_choice]); fputc('}', fp); }
		fputs("]}", fp);
	}
	fputs("\n  ]\n}\n", fp); fclose(fp); return 1;
}

static int eof_psarc_export_now(void)
{
	char wwise[1024] = {0}, toolkit[1024] = {0}, helper[1024] = {0}, staging[1024] = {0}, wav[1024] = {0}, spec[1024] = {0};
	char output[1024] = {0}, filename[512] = {0}, generated[1024] = {0}, xmlpaths[EOF_TRACKS_MAX][1024] = {{0}}, command[4096] = {0}, logpath[1024] = {0};
	unsigned long track, requested, existing, prefix; unsigned short warned = 0; int bpm, dd, sticks; const char *saved_wwise;
#ifndef ALLEGRO_WINDOWS
	allegro_message("PSARC generation through RSToolkit/Wwise is currently available on Windows only."); return 0;
#endif
	saved_wwise = get_config_string("psarc", "wwise_cli", ""); if(!saved_wwise || !saved_wwise[0] || !exists(saved_wwise)) { allegro_message("Wwise path is not set.\nUse the Wwise path... button in the PSARC exporter and select WwiseCLI.exe."); return 0; }
	ustrzcpy(wwise, sizeof(wwise), saved_wwise); if(!eof_psarc_get_toolkit_root(toolkit, sizeof(toolkit))) return 0;
	if(!eof_psarc_build_helper(toolkit, helper, sizeof(helper))) { allegro_message("Could not build the EOF/RSToolkit helper.\nCheck the RSToolkit folder and .NET Framework 4 installation."); return 0; }
	(void)snprintf(filename, sizeof(filename) - 1, "%s - %s.psarc", eof_psarc_artist, eof_psarc_title); eof_build_sanitized_filename_string(filename, generated); replace_filename(output, eof_loaded_ogg_name, generated, sizeof(output)); if(!file_select_ex("Save Rocksmith 2014 PSARC", output, "psarc", sizeof(output), 560, 420)) return 0;
	replace_filename(staging, eof_loaded_ogg_name, "eof_psarc_tmp", sizeof(staging)); if(!eof_folder_exists(staging) && eof_mkdir(staging)) { allegro_message("Could not create the temporary PSARC export folder."); return 0; }
	for(track = 1; track < eof_song->tracks; track++)
	{
		char xml[1024] = {0}; if(!eof_psarc_is_arrangement_track(eof_song, track)) continue; (void)snprintf(xml, sizeof(xml) - 1, "%s%carrangement_%lu.xml", staging, OTHER_PATH_SEPARATOR, track);
		if(!eof_export_rocksmith_2_track(eof_song, xml, track, &warned)) { allegro_message("Rocksmith XML export failed for one of the arrangements."); return 0; } ustrzcpy(xmlpaths[track], sizeof(xmlpaths[track]), xml);
	}
	requested = (unsigned long)strtoul(eof_psarc_intro, NULL, 10); existing = (eof_song->beats && eof_song->beat[0]) ? eof_song->beat[0]->pos : 0UL; prefix = (requested > existing) ? requested - existing : 0UL; bpm = eof_psarc_predominant_bpm(eof_song); sticks = (eof_psarc_dialog[PSARC_DLG_STICKS].flags & D_SELECTED) ? 1 : 0;
	if(!eof_psarc_build_audio(staging, prefix, sticks, bpm, wav, sizeof(wav))) { allegro_message("Could not prepare the staged WAV for Wwise.\nSee eof_log.txt for the audio preparation error."); return 0; }
	(void)snprintf(spec, sizeof(spec) - 1, "%s%cpsarc_spec.json", staging, OTHER_PATH_SEPARATOR); dd = (eof_psarc_dialog[PSARC_DLG_DD].flags & D_SELECTED) ? 1 : 0; if(!eof_psarc_write_spec(spec, wav, wwise, output, xmlpaths, bpm, dd, prefix)) return 0;
	(void)snprintf(logpath, sizeof(logpath) - 1, "%s%cpsarc_export.log", staging, OTHER_PATH_SEPARATOR); (void)snprintf(command, sizeof(command) - 1, "\"%s\" \"%s\" > \"%s\" 2>&1", helper, spec, logpath);
	if(system(command) != 0) { (void)snprintf(eof_log_string, sizeof(eof_log_string) - 1, "PSARC helper failed. See %s", logpath); eof_log(eof_log_string, 1); allegro_message("RSToolkit PSARC generation failed.\nSee eof_psarc_tmp\\psarc_export.log for details."); return 0; }
	ustrzcpy(generated, sizeof(generated), output); replace_extension(generated, generated, "", sizeof(generated)); if(generated[0] && generated[ustrlen(generated) - 1] == '.') generated[ustrlen(generated) - 1] = '\0'; eof_strncat(generated, "_p.psarc", sizeof(generated));
	if(!exists(generated) && !exists(output)) { allegro_message("The helper completed, but the generated PSARC could not be found.\nCheck the export log."); return 0; } allegro_message("Rocksmith 2014 PSARC generated successfully."); return 1;
}

int eof_menu_file_export_psarc(void)
{
	int ret, row; if(!eof_song || !eof_song_loaded) { allegro_message("Load a project first."); return D_O_K; } if(!eof_loaded_ogg_name[0] || !exists(eof_loaded_ogg_name)) { allegro_message("The project audio is not available."); return D_O_K; }
	if(!eof_psarc_ensure_two_arrangements()) { allegro_message("No populated Rocksmith pro guitar/bass arrangements were found."); return D_O_K; }
	ustrzcpy(eof_psarc_title, sizeof(eof_psarc_title), eof_song->tags ? eof_song->tags->title : ""); ustrzcpy(eof_psarc_artist, sizeof(eof_psarc_artist), eof_song->tags ? eof_song->tags->artist : ""); ustrzcpy(eof_psarc_album, sizeof(eof_psarc_album), "Live"); ustrzcpy(eof_psarc_year, sizeof(eof_psarc_year), "2026"); ustrzcpy(eof_psarc_version, sizeof(eof_psarc_version), "1"); ustrzcpy(eof_psarc_intro, sizeof(eof_psarc_intro), "3000");
	eof_psarc_dialog[PSARC_DLG_DD].flags |= D_SELECTED; eof_psarc_dialog[PSARC_DLG_SILENCE].flags |= D_SELECTED; eof_psarc_dialog[PSARC_DLG_STICKS].flags &= ~D_SELECTED; eof_psarc_dialog[PSARC_DLG_TONES].d1 = 0; eof_psarc_build_tone_rows();
	while(1)
	{
		eof_psarc_update_dialog_mode(); eof_color_dialog(eof_psarc_dialog, gui_fg_color, gui_bg_color); eof_conditionally_center_dialog(eof_psarc_dialog); ret = eof_popup_dialog(eof_psarc_dialog, PSARC_DLG_TITLE_E); row = eof_psarc_dialog[PSARC_DLG_TONES].d1; if(row < 0 || row >= eof_psarc_tone_row_count) row = 0;
		if(eof_psarc_tone_row_count && eof_psarc_tone_rows[row].suggestions_ready) { int choice = eof_psarc_dialog[PSARC_DLG_SUGGEST].d1; if(choice >= 0 && choice < EOF_PSARC_SUGGESTIONS) eof_psarc_tone_rows[row].suggestion_choice = choice; }
		if(ret == PSARC_DLG_CANCEL || ret < 0) break; if(ret == PSARC_DLG_TONES) continue;
		if(ret == PSARC_DLG_LOAD)
		{
			char path[1024] = {0}; if(eof_psarc_tone_row_count && file_select_ex("Load Rocksmith tone from PSARC, JSON or profile", path, "psarc;json;_prfldb", sizeof(path), 560, 420) && exists(path)) { ustrzcpy(eof_psarc_tone_rows[row].source, sizeof(eof_psarc_tone_rows[row].source), path); eof_psarc_tone_rows[row].built_in_bass = 0; } continue;
		}
		if(ret == PSARC_DLG_ANALYZE) { eof_psarc_analyze_effects(); continue; } if(ret == PSARC_DLG_USE) continue;
		if(ret == PSARC_DLG_OK)
		{
			if(!eof_psarc_title[0]) { allegro_message("Song name cannot be empty."); continue; } if(!eof_psarc_artist[0]) ustrzcpy(eof_psarc_artist, sizeof(eof_psarc_artist), "Unknown Artist"); if(atoi(eof_psarc_year) < 1000) ustrzcpy(eof_psarc_year, sizeof(eof_psarc_year), "2026"); if(!eof_psarc_version[0]) ustrzcpy(eof_psarc_version, sizeof(eof_psarc_version), "1");
			eof_cursor_visible = 0; eof_pen_visible = 0; eof_render(); eof_psarc_export_now(); eof_cursor_visible = 1; eof_pen_visible = 1; break;
		}
	}
	return D_O_K;
}

void eof_psarc_export_install_menu(void)
{
	unsigned i, count = 0; MENU *source = NULL; if(eof_psarc_menu_installed) return;
	for(i = 0; eof_file_menu[i].text; i++) if(eof_file_menu[i].child && strstr(eof_file_menu[i].text, "Export")) { source = eof_file_menu[i].child; break; }
	if(!source) return; while(source[count].text && count + 2U < EOF_PSARC_MENU_MAX) { eof_psarc_export_menu[count] = source[count]; count++; }
	eof_psarc_export_menu[count].text = "Rocksmith 2014 &PSARC"; eof_psarc_export_menu[count].proc = eof_menu_file_export_psarc; eof_psarc_export_menu[count].child = NULL; eof_psarc_export_menu[count].flags = 0; eof_psarc_export_menu[count].dp = NULL; count++; memset(&eof_psarc_export_menu[count], 0, sizeof(MENU)); eof_file_menu[i].child = eof_psarc_export_menu; eof_psarc_menu_installed = 1;
}

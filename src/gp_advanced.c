/*
 * gp_advanced.c
 *
 * File > Import > Guitar Pro (Rocksmith Advanced)
 *
 * Multi-track Guitar Pro/GoPlayAlong import front-end.  It deliberately uses
 * EOF's existing GP parser/importer and DTX carrier importer instead of
 * maintaining a second parser.  The code between parsing and importing is the
 * advanced layer: multilingual track classification, destination assignment,
 * empty-measure merging, per-arrangement fingering optimization and optional
 * Songsterr/Sonic Visualiser timing.
 */

#include <allegro.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agup/agup.h"
#include "main.h"
#include "config.h"
#include "song.h"
#include "event.h"
#include "undo.h"
#include "gp_import.h"
#include "dialog.h"
#include "dialog/proc.h"
#include "utility.h"
#include "chart_import.h"
#include "foflc/RS_parse.h"
#include "midi.h"
#include "menu/main.h"
#include "menu/file.h"
#include "menu/edit.h"
#include "menu/song.h"
#include "menu/track.h"
#include "dtx_integration.h"
#include "gp_advanced.h"
#include "gp_synth.h"

#ifdef USEMEMWATCH
#include "memwatch.h"
#endif

/* This is the exact Viterbi worker already implemented in fingering.c.  The
 * fingering_public_hook.h build hook only changes its linkage, not its code. */
extern unsigned long eof_fingering_optimize_track(EOF_SONG *sp, unsigned long track,
	unsigned char diff, int avoid_open, int use_selection, unsigned long *examined);

extern MENU eof_file_import_menu[];

#define GP_ADV_ROWS 8
#define GP_ADV_MAX_IMPORT_MENU 64
#define GP_ADV_MAX_FILE_MENU 64
#define GP_ADV_STATUS_UNKNOWN 0
#define GP_ADV_STATUS_VALID 1
#define GP_ADV_STATUS_INVALID 2

enum
{
	GP_ADV_DEST_NONE = 0,
	GP_ADV_DEST_BASS,
	GP_ADV_DEST_BASS22,
	GP_ADV_DEST_GUITAR,
	GP_ADV_DEST_GUITAR22,
	GP_ADV_DEST_GUITAR_BONUS,
	GP_ADV_DEST_DTX_ULT,
	GP_ADV_DEST_DTX_BSC,
	GP_ADV_DEST_DTX_ADV,
	GP_ADV_DEST_DTX_EXT,
	GP_ADV_DEST_DTX_MAS,
	GP_ADV_DEST_COUNT
};

enum
{
	GP_ADV_KIND_OTHER = 0,
	GP_ADV_KIND_GUITAR,
	GP_ADV_KIND_BASS,
	GP_ADV_KIND_DRUM,
	GP_ADV_KIND_VOCAL
};

static const char *gp_adv_dest_labels[GP_ADV_DEST_COUNT] =
{
	"",
	"PART_REAL_BASS",
	"PART_REAL_BASS_22",
	"PART_REAL_GUITAR",
	"PART_REAL_GUITAR_22",
	"PART_REAL_GUITAR_BONUS",
	"PART_REAL_DRUM_DTX",
	"PART_REAL_DRUM_DTX|BSC",
	"PART_REAL_DRUM_DTX|ADV",
	"PART_REAL_DRUM_DTX|EXT",
	"PART_REAL_DRUM_DTX|MAS"
};

static MENU gp_adv_import_menu[GP_ADV_MAX_IMPORT_MENU];
static MENU gp_adv_file_menu[GP_ADV_MAX_FILE_MENU];
static int gp_adv_menu_installed = 0;

static struct eof_guitar_pro_struct *gp_adv_gp = NULL;
static int *gp_adv_assign = NULL;
static unsigned char *gp_adv_kind = NULL;
static unsigned char *gp_adv_finger = NULL;
static unsigned char *gp_adv_avoid_open = NULL;
static unsigned long gp_adv_page = 0;
static DIALOG *gp_adv_active_dialog = NULL;
static int gp_adv_combo_index[GP_ADV_ROWS];
static int gp_adv_finger_index[GP_ADV_ROWS];
static int gp_adv_avoid_index[GP_ADV_ROWS];
static char gp_adv_merge_threshold[4] = "25";
static int gp_adv_merge_enabled = 1;
static int gp_adv_detect_crash_as_ride = 0;
static char gp_adv_songsterr_code[256] = {0};
static char gp_adv_songsterr_last_checked[256] = {0};
static char gp_adv_songsterr_path[1024] = {0};
static char gp_adv_songsterr_status_text[96] = "Songsterr/SVL: not loaded";
static int gp_adv_songsterr_status = GP_ADV_STATUS_UNKNOWN;
static int gp_adv_source_is_gpa_xml = 0;
static int gp_adv_suppress_timing = 0;
static int gp_adv_song_status_dialog_index = -1;
static int gp_adv_song_edit_dialog_index = -1;

static int gp_adv_combo_proc(int msg, DIALOG *d, int c);
static int gp_adv_finger_proc(int msg, DIALOG *d, int c);
static int gp_adv_avoid_proc(int msg, DIALOG *d, int c);
static int gp_adv_songsterr_edit_proc(int msg, DIALOG *d, int c);

static const char *gp_adv_strcasestr(const char *haystack, const char *needle)
{
	const char *h, *n, *start;

	if(!haystack || !needle)
		return NULL;
	if(!needle[0])
		return haystack;

	for(start = haystack; *start; start++)
	{
		h = start;
		n = needle;
		while(*h && *n &&
		      (tolower((unsigned char)*h) == tolower((unsigned char)*n)))
		{
			h++;
			n++;
		}
		if(!*n)
			return start;
	}
	return NULL;
}

/* menu/file.c's original Songsterr parser asks strcasestr_spec() for
 * "-tab-s" and then immediately expects the returned pointer to contain the
 * first digit of the song ID.  Preserve all other searches exactly and only
 * advance this one token past its prefix.  Use the Advanced importer's local
 * case-insensitive search implementation so this bridge has no dependency on
 * FoFLC's strcasestr_spec() declaration/linkage. */
char *eof_gp_advanced_songsterr_strcasestr(const char *haystack, const char *needle)
{
	char *match = (char *)gp_adv_strcasestr(haystack, needle);
	if(match && needle && !ustricmp(needle, "-tab-s"))
		match += 6;
	return match;
}

static void gp_adv_set_dialog_item(DIALOG *d, int (*proc)(int, DIALOG *, int),
	int x, int y, int w, int h, int key, int flags, int d1, int d2, void *dp)
{
	memset(d, 0, sizeof(DIALOG));
	d->proc = proc;
	d->x = x;
	d->y = y;
	d->w = w;
	d->h = h;
	d->fg = 2;
	d->bg = 23;
	d->key = key;
	d->flags = flags;
	d->d1 = d1;
	d->d2 = d2;
	d->dp = dp;
}

static int gp_adv_new_from_gp_audio(void)
{
	if(eof_menu_prompt_save_changes() == 3)
		return D_O_K;
	return eof_menu_file_new_from_gp_audio();
}

/* Rebuild the two displayed menu copies from EOF's stock arrays.  The stock
 * arrays keep their original indexes, so all existing index-based enable/
 * disable code remains valid.  The displayed File menu is only a view that
 * inserts New from GP Audio immediately after New. */
static void gp_adv_build_import_menu(void)
{
	unsigned in = 0, out = 0;
	int inserted = 0;

	while(eof_file_import_menu[in].text && (out + 2 < GP_ADV_MAX_IMPORT_MENU))
	{
		gp_adv_import_menu[out++] = eof_file_import_menu[in];
		if(!inserted && eof_file_import_menu[in].proc == eof_menu_file_gp_import)
		{
			MENU item = {"Guitar Pro (Rocksmith Advanced)", eof_menu_file_gp_advanced_import, NULL, 0, NULL};
			gp_adv_import_menu[out++] = item;
			inserted = 1;
		}
		in++;
	}
	if(!inserted && (out + 1 < GP_ADV_MAX_IMPORT_MENU))
	{
		MENU item = {"Guitar Pro (Rocksmith Advanced)", eof_menu_file_gp_advanced_import, NULL, 0, NULL};
		gp_adv_import_menu[out++] = item;
	}
	memset(&gp_adv_import_menu[out], 0, sizeof(MENU));
}

static void gp_adv_build_file_menu(void)
{
	unsigned in = 0, out = 0;

	while(eof_file_menu[in].text && (out + 2 < GP_ADV_MAX_FILE_MENU))
	{
		MENU item = eof_file_menu[in];
		if((item.child == eof_file_import_menu) || (item.text && !ustricmp(item.text, "&Import")))
			item.child = gp_adv_import_menu;
		gp_adv_file_menu[out++] = item;

		if(in == 0)
		{
			MENU synth = {"New from GP &Audio", gp_adv_new_from_gp_audio, NULL, 0, NULL};
			gp_adv_file_menu[out++] = synth;
		}
		in++;
	}
	memset(&gp_adv_file_menu[out], 0, sizeof(MENU));

	/* File is the first entry in EOF's top-level menu. */
	eof_main_menu[0].child = gp_adv_file_menu;
}

void eof_gp_advanced_prepare_file_menu(void)
{
	/* Run EOF's stock logic first so its original array has the current flags,
	 * then mirror that state into the displayed augmented menu. */
	eof_prepare_file_menu();
	if(gp_adv_menu_installed)
	{
		gp_adv_build_import_menu();
		gp_adv_build_file_menu();
	}
}

void eof_gp_advanced_install_menu(void)
{
	if(gp_adv_menu_installed)
		return;

	gp_adv_menu_installed = 1;
	gp_adv_build_import_menu();
	gp_adv_build_file_menu();
	eof_log("GP Advanced: installed Import entry and File > New from GP Audio directly below New", 1);
}

/* Initial Advanced parsing must always establish the GP's measure signatures,
 * regardless of the global import-time-signature preference.  A second parse
 * performed after Songsterr/SVL passes NULL for undo_made and deliberately
 * keeps eof_use_ts disabled so the final external grid is not overwritten. */
static struct eof_guitar_pro_struct *gp_adv_load_gp_timing_first(const char *fn, char *undo_made)
{
	struct eof_guitar_pro_struct *ret;
	int saved_use_ts = eof_use_ts;

	if(undo_made)
		eof_use_ts = 1;
	ret = eof_load_gp(fn, undo_made);
	eof_use_ts = saved_use_ts;
	return ret;
}

#include "gp_advanced_classify.inc"
#include "gp_advanced_controls.inc"
#include "gp_advanced_merge.inc"
#include "gp_advanced_timing.inc"
#include "gp_advanced_ui.inc"
#define eof_load_gp(fn, undo_made) gp_adv_load_gp_timing_first((fn), (undo_made))
#include "gp_advanced_commit.inc"
#undef eof_load_gp

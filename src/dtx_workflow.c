#include <allegro.h>
#include <limits.h>
#include <string.h>

#include "main.h"
#include "editor.h"
#include "song.h"
#include "config.h"
#include "menu/file.h"
#include "menu/song.h"
#include "menu/track.h"
#include "dtx_export.h"
#include "dtx_workflow.h"
#include "dtx_import.h"

static char eof_dtx_difficulty_names[EOF_MAX_DIFFICULTIES][10] =
{
	" BSC", " ADV", " EXT", " MAS", " ULT"
};

static unsigned long eof_dtx_previous_track = ULONG_MAX;
static unsigned eof_dtx_saved_instrument_difficulty = EOF_NOTE_AMAZING;
static int eof_dtx_exit_menu_patched = 0;
static int eof_dtx_seek_menu_patched = 0;
static int eof_dtx_sync_in_progress = 0;
static EOF_SONG *eof_dtx_baseline_song = NULL;
static unsigned long eof_dtx_baseline_signature = 0;
static int eof_dtx_baseline_valid = 0;
static int eof_dtx_legacy_migration_checked = 0;

static void eof_dtx_prepare_carrier_track(EOF_SONG *sp)
{
	unsigned long tracknum;
	EOF_PRO_GUITAR_TRACK *tp;

	if(!sp || (EOF_TRACK_DRUM_DTX >= sp->tracks) || !sp->track[EOF_TRACK_DRUM_DTX])
		return;
	if(sp->track[EOF_TRACK_DRUM_DTX]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT)
		return;

	tracknum = sp->track[EOF_TRACK_DRUM_DTX]->tracknum;
	if((tracknum >= sp->pro_guitar_tracks) || !sp->pro_guitar_track[tracknum])
		return;

	tp = sp->pro_guitar_track[tracknum];
	/* PART_REAL_DRUM_DTX deliberately reuses the pro-guitar note structure as
	 * a six-slot carrier for General MIDI percussion numbers.  Keep the carrier
	 * in a deterministic regular-note state even when it is completely empty. */
	eof_menu_track_set_tech_view_state(sp, EOF_TRACK_DRUM_DTX, 0);
	tp->numstrings = 6;
	tp->numfrets = 127;
	tp->capo = 0;
	tp->ignore_tuning = 1;
}

static unsigned long eof_dtx_track_signature(void)
{
	unsigned long i, s, tracknum, hash = 2166136261UL;
	EOF_PRO_GUITAR_TRACK *tp;

	if(!eof_song || eof_song->tracks <= EOF_TRACK_DRUM_DTX || !eof_song->track[EOF_TRACK_DRUM_DTX])
		return 0;
	tracknum = eof_song->track[EOF_TRACK_DRUM_DTX]->tracknum;
	if(tracknum >= eof_song->pro_guitar_tracks || !eof_song->pro_guitar_track[tracknum])
		return 0;

	tp = eof_song->pro_guitar_track[tracknum];
	for(i = 0; i < tp->pgnotes; i++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[i];
		if(!np)
			continue;
#define DTX_HASH(v) do { hash ^= (unsigned long)(v); hash *= 16777619UL; } while(0)
		DTX_HASH(np->type);
		DTX_HASH(np->note);
		DTX_HASH(np->pos);
		DTX_HASH(np->length);
		DTX_HASH(np->flags);
		DTX_HASH(np->eflags);
		DTX_HASH(np->ghost);
		for(s = 0; s < 6; s++)
			DTX_HASH(np->frets[s]);
#undef DTX_HASH
	}
	return hash;
}

static void eof_dtx_reset_baseline_if_needed(void)
{
	if(eof_dtx_baseline_song != eof_song)
	{
		eof_dtx_previous_track = ULONG_MAX;
		eof_dtx_baseline_song = eof_song;
		eof_dtx_baseline_signature = 0;
		eof_dtx_baseline_valid = 0;
		eof_dtx_legacy_migration_checked = 0;
	}
}

/*
 * While the DTX integration is being stabilized, always turn on EOF's most
 * detailed log after the user's config is read.  eof_log() flushes every line,
 * so the last successful operation is preserved even on a hard crash.
 */
void eof_dtx_load_config(char *fn)
{
	eof_load_config(fn);
	eof_log_level = 3;
	eof_log("DTX diagnostics: forcing exhaustive logging (level 3)", 0);
}

/*
 * main.c calls eof_sort_notes() as part of project initialization.  Old EOF
 * projects receive PART_REAL_DRUM_DTX as a newly-added empty carrier during
 * loading.  Validate each track explicitly and do not send that empty carrier
 * through generic pro-guitar sorting.  This also leaves a per-track breadcrumb
 * in eof_log.txt so a future crash can be located precisely.
 */
void eof_dtx_safe_sort_notes(EOF_SONG *sp)
{
	unsigned long j;

	eof_log("eof_dtx_safe_sort_notes() entered", 0);
	if(!sp)
	{
		eof_log("DTX sort: song pointer is NULL", 0);
		return;
	}

	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"DTX sort: tracks=%lu legacy=%lu vocal=%lu pro_guitar=%lu",
		sp->tracks, sp->legacy_tracks, sp->vocal_tracks, sp->pro_guitar_tracks);
	eof_log(eof_log_string, 0);

	for(j = 1; j < sp->tracks; j++)
	{
		unsigned long tracknum;

		if(!sp->track[j])
		{
			(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
				"DTX sort: track[%lu] is NULL -- skipped", j);
			eof_log(eof_log_string, 0);
			continue;
		}

		tracknum = sp->track[j]->tracknum;
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"DTX sort: before track=%lu name=\"%s\" format=%u behavior=%u tracknum=%lu",
			j, sp->track[j]->name, (unsigned)sp->track[j]->track_format,
			(unsigned)sp->track[j]->track_behavior, tracknum);
		eof_log(eof_log_string, 0);

		if(j == EOF_TRACK_DRUM_DTX)
		{
			EOF_PRO_GUITAR_TRACK *tp;

			eof_dtx_prepare_carrier_track(sp);
			if(sp->track[j]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT)
			{
				eof_log("DTX sort: DTX carrier has unexpected format -- skipped", 0);
				continue;
			}
			if((tracknum >= sp->pro_guitar_tracks) || !sp->pro_guitar_track[tracknum])
			{
				eof_log("DTX sort: DTX carrier has invalid pro-guitar backing pointer -- skipped", 0);
				continue;
			}

			tp = sp->pro_guitar_track[tracknum];
			(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
				"DTX sort: carrier pgnotes=%lu technotes=%lu notes=%lu numstrings=%u numfrets=%u",
				tp->pgnotes, tp->technotes, tp->notes, (unsigned)tp->numstrings, (unsigned)tp->numfrets);
			eof_log(eof_log_string, 0);

			if(!tp->pgnotes && !tp->technotes)
			{
				eof_log("DTX sort: empty carrier -- deliberately skipped", 0);
				continue;
			}
		}

		eof_track_sort_notes(sp, j);
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"DTX sort: after track=%lu", j);
		eof_log(eof_log_string, 0);
	}

	eof_log("eof_dtx_safe_sort_notes() completed", 0);
}

/*
 * The stock rewind-to-start path rebuilds/retargets several audio and editor
 * structures using the globally selected track.  PART_REAL_DRUM_DTX is stored
 * in a pro-guitar carrier even though its frets[] are MIDI percussion values.
 * Run that legacy transport operation with an ordinary track temporarily
 * selected, then restore DTX immediately.  No chart data or difficulty is
 * changed by this temporary substitution.
 */
int eof_dtx_menu_song_seek_start(void)
{
	unsigned long saved_track, surrogate = 0, ctr;
	int result;

	if(!eof_song || (eof_selected_track != EOF_TRACK_DRUM_DTX))
		return eof_menu_song_seek_start();

	eof_log("DTX transport: safe seek-to-start entered", 0);
	saved_track = eof_selected_track;

	/* PART GUITAR is present in standard projects.  Fall back to the first
	 * valid non-DTX track for unusual/custom projects. */
	if((EOF_TRACK_GUITAR < eof_song->tracks) && eof_song->track[EOF_TRACK_GUITAR])
		surrogate = EOF_TRACK_GUITAR;
	else
	{
		for(ctr = 1; ctr < eof_song->tracks; ctr++)
		{
			if((ctr != EOF_TRACK_DRUM_DTX) && eof_song->track[ctr])
			{
				surrogate = ctr;
				break;
			}
		}
	}

	if(!surrogate)
	{
		eof_log("DTX transport: no safe surrogate track; using stock seek path", 0);
		return eof_menu_song_seek_start();
	}

	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"DTX transport: temporarily selecting track %lu (\"%s\") for rewind",
		surrogate, eof_song->track[surrogate]->name);
	eof_log(eof_log_string, 0);

	eof_selected_track = surrogate;
	result = eof_menu_song_seek_start();
	eof_selected_track = saved_track;
	eof_dtx_prepare_carrier_track(eof_song);
	eof_window_title_dirty = 1;

	eof_log("DTX transport: safe seek-to-start completed and DTX restored", 0);
	return result;
}

static void eof_dtx_patch_menus(void)
{
	unsigned i;

	if(!eof_dtx_exit_menu_patched)
	{
		for(i = 0; eof_file_menu[i].text; i++)
		{
			if(eof_file_menu[i].proc == eof_menu_file_exit)
			{
				eof_file_menu[i].proc = eof_dtx_menu_file_exit;
				eof_dtx_exit_menu_patched = 1;
				break;
			}
		}
	}

	if(!eof_dtx_seek_menu_patched)
	{
		for(i = 0; eof_song_seek_menu[i].text; i++)
		{
			if(eof_song_seek_menu[i].proc == eof_menu_song_seek_start)
			{
				eof_song_seek_menu[i].proc = eof_dtx_menu_song_seek_start;
				eof_dtx_seek_menu_patched = 1;
				break;
			}
		}
	}
}

void eof_dtx_sync_editor_state(void)
{
	eof_dtx_patch_menus();
	eof_dtx_reset_baseline_if_needed();

	if(!eof_song)
	{
		eof_dtx_previous_track = ULONG_MAX;
		eof_dtx_sync_in_progress = 0;
		return;
	}
	if(eof_dtx_sync_in_progress)
		return;

	eof_dtx_sync_in_progress = 1;
	if(eof_selected_track == EOF_TRACK_DRUM_DTX)
	{
		eof_log("DTX workflow: entering PART_REAL_DRUM_DTX", 0);

		/* Keep the DTX carrier valid even in a project where this track has never
		 * received a drum note. */
		eof_dtx_prepare_carrier_track(eof_song);

		/* Very early DTX builds placed the imported GP drum chart in EOF Expert
		 * (difficulty 3).  Migrate that legacy state only once when a project is
		 * first opened. */
		if(!eof_dtx_legacy_migration_checked)
		{
			eof_dtx_migrate_import_to_ultimate(eof_song);
			eof_dtx_legacy_migration_checked = 1;
		}

		if(eof_dtx_previous_track != EOF_TRACK_DRUM_DTX)
		{
			if(eof_dtx_previous_track != ULONG_MAX && eof_note_type_i < EOF_MAX_DIFFICULTIES)
				eof_dtx_saved_instrument_difficulty = eof_note_type_i;

			eof_note_type = EOF_NOTE_SPECIAL;
			eof_note_type_i = EOF_NOTE_SPECIAL;
		}

		eof_note_type_name = eof_dtx_difficulty_names;
		(void)eof_detect_difficulties(eof_song, EOF_TRACK_DRUM_DTX);
		eof_window_title_dirty = 1;

		if(!eof_dtx_baseline_valid)
		{
			eof_dtx_baseline_signature = eof_dtx_track_signature();
			eof_dtx_baseline_valid = 1;
		}

		/* Imported DTX sets can assign a separate OGG/BPM to each difficulty.
		 * Synchronize linked BPM-only exercises after EOF has accepted the
		 * active difficulty change. */
		eof_dtx_import_sync_editor_state();
	}
	else
	{
		eof_note_type_name = eof_use_fof_difficulty_naming ? eof_note_type_name_fof : eof_note_type_name_rb;
		if(eof_dtx_previous_track == EOF_TRACK_DRUM_DTX)
		{
			eof_note_type_i = (unsigned char)eof_dtx_saved_instrument_difficulty;
			eof_note_type = (unsigned char)eof_dtx_saved_instrument_difficulty;
			eof_window_title_dirty = 1;
		}
		if((eof_selected_track > 0) && (eof_selected_track < eof_song->tracks))
			(void)eof_detect_difficulties(eof_song, eof_selected_track);
	}

	eof_dtx_previous_track = eof_selected_track;
	eof_dtx_sync_in_progress = 0;
}

unsigned char eof_dtx_detect_difficulties_hook(EOF_SONG *sp, unsigned long track)
{
	unsigned char result;

	if(sp && (track == EOF_TRACK_DRUM_DTX))
	{
		eof_log("DTX workflow: preparing carrier before difficulty detection", 0);
		eof_dtx_prepare_carrier_track(sp);
	}

	result = eof_detect_difficulties(sp, track);

	if(sp && (sp == eof_song) && (track == eof_selected_track))
		eof_dtx_sync_editor_state();

	return result;
}

static int eof_dtx_project_only_save(void)
{
	int result;
	int write_fof = eof_write_fof_files;
	int write_gh = eof_write_gh_files;
	int write_rb = eof_write_rb_files;
	int write_music_midi = eof_write_music_midi;
	int write_rs = eof_write_rs_files;
	int write_rs2 = eof_write_rs2_files;
	int write_immerrock = eof_write_immerrock_files;
	int write_bf = eof_write_bf_files;
	int write_lrc = eof_write_lrc_files;

	eof_write_fof_files = 0;
	eof_write_gh_files = 0;
	eof_write_rb_files = 0;
	eof_write_music_midi = 0;
	eof_write_rs_files = 0;
	eof_write_rs2_files = 0;
	eof_write_immerrock_files = 0;
	eof_write_bf_files = 0;
	eof_write_lrc_files = 0;

	result = eof_menu_file_save();

	eof_write_fof_files = write_fof;
	eof_write_gh_files = write_gh;
	eof_write_rb_files = write_rb;
	eof_write_music_midi = write_music_midi;
	eof_write_rs_files = write_rs;
	eof_write_rs2_files = write_rs2;
	eof_write_immerrock_files = write_immerrock;
	eof_write_bf_files = write_bf;
	eof_write_lrc_files = write_lrc;

	if(result == 0)
	{
		eof_dtx_baseline_song = eof_song;
		eof_dtx_baseline_signature = eof_dtx_track_signature();
		eof_dtx_baseline_valid = 1;
	}
	return result;
}

int eof_dtx_menu_file_exit(void)
{
	int answer;

	eof_dtx_reset_baseline_if_needed();
	if(eof_song_loaded && eof_dtx_track_has_notes(eof_song))
	{
		if(eof_dtx_baseline_valid && (eof_dtx_track_signature() != eof_dtx_baseline_signature))
			eof_changes = 1;

		answer = alert3("PART_REAL_DRUM_DTX contains DTXMania chart data.",
			"You can export the current BSC/ADV/EXT/MAS/ULT edits before closing.",
			"Export to DTXMania before exiting?",
			"&Export", "&Do not export", "Cancel", 'e', 'd', 0);
		if(answer == 3)
			return 1;
		if(answer == 1)
		{
			(void)eof_menu_file_export_dtxmania();
			if(!eof_dtx_last_export_was_successful())
				return 1;
		}

		if(eof_changes)
		{
			answer = alert(NULL, "Save EOF project changes before closing?", NULL,
				"&Yes", "&No", 'y', 'n');
			if(answer == 1)
			{
				if(eof_dtx_project_only_save() != 0)
					return 1;
				eof_changes = 0;
			}
			else
			{
				eof_changes = 0;
			}
		}
	}

	return eof_menu_file_exit();
}

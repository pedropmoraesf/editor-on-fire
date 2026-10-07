/* ------------------------------------------------------------------------- */
/* Commit parsed tracks into EOF                                              */
/* ------------------------------------------------------------------------- */

#include "beat.h"

/* Legacy GP3/4/5 stores an initial BPM in the fixed file header.  EOF's stock
 * GP loader parses that value but does not seed the active project's tempo map
 * from it.  Advanced import must establish GP timing before note placement when
 * no external Songsterr/SVL timing is used. */
static int gp_adv_legacy_gp_version(const char *text)
{
	if(!strcmp(text, "FICHIER GUITARE PRO v3.00")) return 300;
	if(!strcmp(text, "FICHIER GUITARE PRO v4.00")) return 400;
	if(!strcmp(text, "FICHIER GUITARE PRO v4.06")) return 406;
	if(!strcmp(text, "FICHIER GUITAR PRO L4.06")) return 406;
	if(!strcmp(text, "FICHIER GUITAR PRO v5.00")) return 500;
	if(!strcmp(text, "FICHIER GUITAR PRO v5.10")) return 510;
	return 0;
}

static int gp_adv_read_legacy_tempo(const char *path, unsigned long *tempo)
{
	PACKFILE *fp;
	char buffer[1025];
	unsigned word = 0;
	unsigned long dword = 0, count, i;
	int version;

	if(!path || !tempo)
		return 0;
	fp = pack_fopen(path, "rb");
	if(!fp)
		return 0;
	buffer[0] = '\0';
	if(!eof_read_gp_string(fp, &word, buffer, 0) || (word > 30U))
		goto fail;
	(void)pack_fseek(fp, 30L - (long)word);
	version = gp_adv_legacy_gp_version(buffer);
	if(!version)
		goto fail;

	/* title, subtitle, artist, album */
	for(i = 0; i < 4; i++) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail;
	if(version >= 500) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail; /* words */
	/* composer/music, copyright, transcriber, instructions */
	for(i = 0; i < 4; i++) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail;
	pack_ReadDWORDLE(fp, &count);
	if(count > 1000UL) goto fail;
	for(i = 0; i < count; i++) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail;
	if(version < 500)
		(void)pack_getc(fp); /* shuffle feel */

	if(version >= 400)
	{
		pack_ReadDWORDLE(fp, NULL); /* lyrics track */
		for(i = 0; i < 5; i++)
		{
			pack_ReadDWORDLE(fp, NULL); /* start bar */
			pack_ReadDWORDLE(fp, &dword);
			if(dword > 16UL * 1024UL * 1024UL) goto fail;
			if(dword && pack_fseek(fp, (long)dword)) goto fail;
		}
	}

	if(version > 500)
	{
		pack_ReadDWORDLE(fp, NULL); /* master volume */
		if(pack_fseek(fp, 4)) goto fail;
		for(i = 0; i < 11; i++) (void)pack_getc(fp); /* EQ + gain */
	}

	if(version >= 500)
	{
		for(i = 0; i < 7; i++) pack_ReadDWORDLE(fp, NULL); /* page setup */
		pack_ReadWORDLE(fp, NULL);
		for(i = 0; i < 10; i++) if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail;
		if(!eof_read_gp_string(fp, NULL, buffer, 1)) goto fail; /* tempo text */
	}
	pack_ReadDWORDLE(fp, &dword);
	pack_fclose(fp);
	if((dword < 20UL) || (dword > 500UL))
		return 0;
	*tempo = dword;
	return 1;

fail:
	pack_fclose(fp);
	return 0;
}

static void gp_adv_seed_legacy_tempo(unsigned long bpm)
{
	unsigned long i, ppqn;

	if(!eof_song || !eof_song->beats || (bpm < 20UL) || (bpm > 500UL))
		return;
	if(!gp_import_undo_made)
	{
		eof_prepare_undo(EOF_UNDO_TYPE_NONE);
		gp_import_undo_made = 1;
	}
	ppqn = (60000000UL + bpm / 2UL) / bpm;
	for(i = 0; i < eof_song->beats; i++)
		eof_song->beat[i]->ppqn = ppqn;
	eof_song->beat[0]->flags |= EOF_BEAT_FLAG_ANCHOR;
	eof_calculate_beats(eof_song);
	eof_chart_length = eof_song->beat[eof_song->beats - 1]->pos;
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"GP Advanced: seeded Guitar Pro initial tempo at %lu BPM before track import", bpm);
	eof_log(eof_log_string, 1);
}

static unsigned long gp_adv_dest_track(int dest)
{
	switch(dest)
	{
		case GP_ADV_DEST_BASS: return EOF_TRACK_PRO_BASS;
		case GP_ADV_DEST_BASS22: return EOF_TRACK_PRO_BASS_22;
		case GP_ADV_DEST_GUITAR: return EOF_TRACK_PRO_GUITAR;
		case GP_ADV_DEST_GUITAR22: return EOF_TRACK_PRO_GUITAR_22;
		case GP_ADV_DEST_GUITAR_BONUS: return EOF_TRACK_PRO_GUITAR_B;
	}
	return 0;
}

static unsigned char gp_adv_dtx_diff(int dest)
{
	switch(dest)
	{
		case GP_ADV_DEST_DTX_BSC: return EOF_NOTE_SUPAEASY;
		case GP_ADV_DEST_DTX_ADV: return EOF_NOTE_EASY;
		case GP_ADV_DEST_DTX_EXT: return EOF_NOTE_MEDIUM;
		case GP_ADV_DEST_DTX_MAS: return EOF_NOTE_AMAZING;
		case GP_ADV_DEST_DTX_ULT: default: return EOF_NOTE_SPECIAL;
	}
}

static int gp_adv_is_string_dest(int dest)
{
	return (dest >= GP_ADV_DEST_BASS) && (dest <= GP_ADV_DEST_GUITAR_BONUS);
}

static int gp_adv_is_dtx_dest(int dest)
{
	return (dest >= GP_ADV_DEST_DTX_ULT) && (dest <= GP_ADV_DEST_DTX_MAS);
}

static void gp_adv_import_text_events(void)
{
	unsigned long i, track;
	if(!gp_adv_gp || !gp_adv_gp->text_events)
		return;
	if(!gp_import_undo_made)
	{
		eof_prepare_undo(EOF_UNDO_TYPE_NONE);
		gp_import_undo_made = 1;
	}
	track = eof_gp_import_track_specific_events ? eof_selected_track : 0;
	for(i = 0; i < gp_adv_gp->text_events; i++)
	{
		if(!gp_adv_gp->text_event[i]) continue;
		if(!eof_event_exists(eof_song, gp_adv_gp->text_event[i]->pos, gp_adv_gp->text_event[i]->text,
		                     track, gp_adv_gp->text_event[i]->flags))
		{
			(void)eof_song_add_text_event(eof_song, gp_adv_gp->text_event[i]->pos,
				gp_adv_gp->text_event[i]->text, track, gp_adv_gp->text_event[i]->flags, 0);
		}
	}
	eof_sort_events(eof_song);
}

static int gp_adv_apply_assignments(void)
{
	unsigned long i;
	unsigned long saved_track = eof_selected_track;
	unsigned saved_note_type = eof_note_type, saved_note_type_i = eof_note_type_i;
	int saved_replace = eof_gp_import_replaces_track;
	unsigned char string_diff = (saved_note_type <= EOF_NOTE_AMAZING) ? (unsigned char)saved_note_type : EOF_NOTE_AMAZING;
	int imported = 0;

	if(!gp_adv_gp || !eof_song)
		return 0;

	/* Both stock GP import and the existing DTX GP importer consume this same
	 * parsed structure.  Keep one canonical parser/import path instead of
	 * duplicating either implementation in the advanced front-end. */
	eof_parsed_gp_file = gp_adv_gp;
	eof_gp_import_replaces_track = 1;
	gp_adv_import_text_events();

	for(i = 0; i < gp_adv_gp->numtracks; i++)
	{
		int dest = gp_adv_assign[i];
		if(gp_adv_is_string_dest(dest))
		{
			unsigned long eoftrack = gp_adv_dest_track(dest), examined = 0, changed = 0;
			if(!eoftrack || (eoftrack >= eof_song->tracks) || !eof_song->track[eoftrack])
				continue;
			(void)eof_menu_track_selected_track_number(eoftrack, 1);
			eof_note_type = eof_note_type_i = string_diff;
			eof_gp_import_dialog[1].d1 = (int)i;
			if(eof_gp_import_guitar_track(3) == D_CLOSE)
			{
				eof_track_find_crazy_notes(eof_song, eoftrack, 1);
				eof_track_fixup_notes(eof_song, eoftrack, 1);
				eof_track_fixup_notes(eof_song, eoftrack, 1);
				if(gp_adv_finger[i])
				{
					/* This is the exact Viterbi worker used by Note > Optimize fingering.
					 * fingering_public_hook.h changes only its linkage. */
					changed = eof_fingering_optimize_track(eof_song, eoftrack, string_diff,
						gp_adv_avoid_open[i] ? 1 : 0, 0, &examined);
					(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
						"GP Advanced: fingering optimized track %lu, examined=%lu changed=%lu avoid_open=%u",
						eoftrack, examined, changed, (unsigned)gp_adv_avoid_open[i]);
					eof_log(eof_log_string, 1);
					eof_track_sort_notes(eof_song, eoftrack);
				}
				imported++;
			}
		}
		else if(gp_adv_is_dtx_dest(dest))
		{
			unsigned char diff = gp_adv_dtx_diff(dest);
			if((EOF_TRACK_DRUM_DTX >= eof_song->tracks) || !eof_song->track[EOF_TRACK_DRUM_DTX])
				continue;
			(void)eof_menu_track_selected_track_number(EOF_TRACK_DRUM_DTX, 1);
			eof_note_type = eof_note_type_i = diff;
			eof_gp_import_dialog[1].d1 = (int)i;
			/* Reuse the DTX GP importer already used by the normal GP workflow.  It
			 * handles MIDI-percussion mapping, unsupported pieces, difficulty
			 * replacement and the six-slot DTX carrier consistently. */
			if(eof_dtx_import_selected_gp_drum_track())
				imported++;
		}
	}

	eof_gp_import_replaces_track = saved_replace;
	if(saved_track && (saved_track < eof_song->tracks) && eof_song->track[saved_track])
		(void)eof_menu_track_selected_track_number(saved_track, 1);
	eof_note_type = saved_note_type;
	eof_note_type_i = saved_note_type_i;
	if(imported)
	{
		eof_fixup_notes(eof_song);
		eof_changes = 1;
	}
	return imported;
}

static void gp_adv_release_gp(void)
{
	unsigned long i, j;
	if(!gp_adv_gp)
		return;
	for(i = 0; i < gp_adv_gp->text_events; i++)
	{
		free(gp_adv_gp->text_event[i]);
		gp_adv_gp->text_event[i] = NULL;
	}
	gp_adv_gp->text_events = 0;
	for(i = 0; i < gp_adv_gp->numtracks; i++)
	{
		if(gp_adv_gp->names)
			free(gp_adv_gp->names[i]);
		if(gp_adv_gp->track && gp_adv_gp->track[i])
		{
			for(j = 0; j < gp_adv_gp->track[i]->notes; j++)
				free(gp_adv_gp->track[i]->note[j]);
			for(j = 0; j < gp_adv_gp->track[i]->technotes; j++)
				free(gp_adv_gp->track[i]->technote[j]);
			free(gp_adv_gp->track[i]);
		}
	}
	free(gp_adv_gp->names);
	free(gp_adv_gp->track);
	free(gp_adv_gp->instrument_types);
	free(gp_adv_gp->measure);
	free(gp_adv_gp);
	gp_adv_gp = NULL;
	eof_parsed_gp_file = NULL;
}

static void gp_adv_release_ui_state(void)
{
	free(gp_adv_assign); gp_adv_assign = NULL;
	free(gp_adv_kind); gp_adv_kind = NULL;
	free(gp_adv_finger); gp_adv_finger = NULL;
	free(gp_adv_avoid_open); gp_adv_avoid_open = NULL;
	gp_adv_active_dialog = NULL;
	gp_adv_song_status_dialog_index = -1;
	gp_adv_song_edit_dialog_index = -1;
}

/* After Songsterr/SVL changes the grid, parse the GP again with time-signature
 * importing disabled.  This makes the parsed note timestamps derive from the
 * FINAL grid instead of the provisional GP grid, while retaining the GP time
 * signatures already present in the project for measure structure. */
static int gp_adv_reparse_after_external_timing(const char *selected_path, unsigned long expected_tracks)
{
	int saved_use_ts;

	if(!selected_path || !gp_adv_gp)
		return 0;
	gp_adv_release_gp();
	saved_use_ts = eof_use_ts;
	eof_use_ts = 0;
	gp_adv_gp = eof_load_gp(selected_path, NULL);
	eof_use_ts = saved_use_ts;
	if(!gp_adv_gp)
	{
		eof_log("GP Advanced: reparse after external timing failed", 1);
		return 0;
	}
	if(gp_adv_gp->numtracks != expected_tracks)
	{
		eof_log("GP Advanced: track count changed during timing reparse", 1);
		gp_adv_release_gp();
		return 0;
	}
	eof_log("GP Advanced: reparsed GP against final Songsterr/SVL beat grid before note import", 1);
	return 1;
}

static void gp_adv_expand_chart_for_synth_source(void)
{
	unsigned long track, note, maxend = 0;

	if(!gp_adv_gp || !eof_song)
		return;
	for(track = 0; track < gp_adv_gp->numtracks; track++)
	{
		EOF_PRO_GUITAR_TRACK *tp = gp_adv_gp->track ? gp_adv_gp->track[track] : NULL;
		if(!tp)
			continue;
		for(note = 0; note < tp->notes; note++)
		{
			EOF_PRO_GUITAR_NOTE *np = tp->note[note];
			unsigned long end;
			if(!np)
				continue;
			end = np->pos + (unsigned long)((np->length > 0) ? np->length : 1);
			if(end > maxend)
				maxend = end;
		}
	}
	if(maxend && (maxend + 2000UL > eof_chart_length))
	{
		eof_chart_length = maxend + 2000UL;
		/* eof_load_gp() has already created/applied its measure and TS layout.
		 * Ensure the beat array extends far enough that import fixup and musical
		 * MIDI export cannot regard notes as being beyond the 1-second seed WAV. */
		(void)eof_calculate_beats(eof_song);
	}
}

int eof_gp_advanced_import_path(const char *selected_path, int suppress_timing)
{
	int imported = 0;
	int synth_mode;
	int external_timing = 0;
	int timing_ready = 1;
	unsigned long expected_tracks = 0, legacy_bpm = 0;

	if(!selected_path || !selected_path[0] || !eof_song || !eof_song_loaded)
		return 0;

	gp_import_undo_made = 0;
	gp_adv_suppress_timing = suppress_timing ? 1 : 0;
	synth_mode = gp_adv_suppress_timing;
	gp_adv_source_is_gpa_xml = !ustricmp(get_extension(selected_path), "xml");
	gp_adv_songsterr_code[0] = '\0';
	gp_adv_songsterr_last_checked[0] = '\0';
	gp_adv_songsterr_path[0] = '\0';
	gp_adv_set_song_status(GP_ADV_STATUS_UNKNOWN, "Songsterr/SVL: not loaded");
	ustrzcpy(gp_adv_merge_threshold, sizeof(gp_adv_merge_threshold), "25");
	gp_adv_merge_enabled = 1;
	gp_adv_detect_crash_as_ride = 0;
	eof_gp_detect_crash_as_ride = 0;

	/* For ordinary GP Advanced imports, establish the GP's own initial BPM
	 * before parsing its measure/time-signature layout.  New from GP Audio has
	 * already done this before entering the shared importer. */
	if(!synth_mode && gp_adv_read_legacy_tempo(selected_path, &legacy_bpm))
		gp_adv_seed_legacy_tempo(legacy_bpm);

	/* eof_load_gp() is EOF's existing parser/router: GP3/GP4/GP5, modern
	 * .gp/.gpx GPIF containers and Go PlayAlong XML all enter through here.
	 * At this stage GP time signatures are allowed to populate the grid. */
	gp_adv_gp = eof_load_gp(selected_path, &gp_import_undo_made);
	if(!gp_adv_gp)
	{
		if(gp_import_undo_made)
		{
			eof_menu_edit_undo();
			gp_import_undo_made = 0;
		}
		allegro_message("Guitar Pro/GoPlayAlong import failed. Check eof_log.txt for details.");
		gp_adv_suppress_timing = 0;
		return 0;
	}
	if(!gp_adv_gp->numtracks)
	{
		gp_adv_release_gp();
		gp_adv_suppress_timing = 0;
		return 0;
	}
	if(synth_mode)
		gp_adv_expand_chart_for_synth_source();

	gp_adv_assign = calloc(gp_adv_gp->numtracks, sizeof(*gp_adv_assign));
	gp_adv_kind = calloc(gp_adv_gp->numtracks, sizeof(*gp_adv_kind));
	gp_adv_finger = calloc(gp_adv_gp->numtracks, sizeof(*gp_adv_finger));
	gp_adv_avoid_open = calloc(gp_adv_gp->numtracks, sizeof(*gp_adv_avoid_open));
	if(!gp_adv_assign || !gp_adv_kind || !gp_adv_finger || !gp_adv_avoid_open)
	{
		allegro_message("Not enough memory for the advanced GP mapping dialog.");
		gp_adv_release_ui_state();
		gp_adv_release_gp();
		gp_adv_suppress_timing = 0;
		return 0;
	}

	gp_adv_autofill_assignments();
	eof_cursor_visible = 0;
	eof_pen_visible = 0;
	eof_render();
	if(gp_adv_run_mapping_dialog())
	{
		external_timing = !gp_adv_suppress_timing && !gp_adv_source_is_gpa_xml &&
			(gp_adv_songsterr_status == GP_ADV_STATUS_VALID) && gp_adv_songsterr_path[0];

		if(external_timing)
		{
			expected_tracks = gp_adv_gp->numtracks;
			eof_log("GP Advanced: applying Songsterr/SVL timing before importing any GP notes", 1);
			if(!gp_adv_apply_timing_file(gp_adv_songsterr_path))
			{
				allegro_message("Songsterr/SVL timing could not be applied. GP tracks were not imported.");
				timing_ready = 0;
			}
			else if(!gp_adv_reparse_after_external_timing(selected_path, expected_tracks))
			{
				allegro_message("The timing was applied, but Guitar Pro could not be reparsed against the new grid. GP tracks were not imported.");
				timing_ready = 0;
			}
			else
			{
				/* Merging must inspect the final, externally-timed GP positions. */
				if(gp_adv_merge_enabled)
					gp_adv_merge_guitars();
			}
		}
		else
		{
			eof_log("GP Advanced: using Guitar Pro BPM/time signatures before importing GP notes", 1);
		}

		if(timing_ready)
			imported = gp_adv_apply_assignments();
		if(timing_ready && !imported)
			allegro_message("No GP tracks were imported. Check the destination assignments.");
	}
	else if(gp_import_undo_made)
	{
		eof_menu_edit_undo();
		gp_import_undo_made = 0;
	}

	gp_adv_release_ui_state();
	gp_adv_release_gp();
	/* A synthetic-audio project begins with a 1-second seed WAV.  Truncating to
	 * that audio here would destroy the just-imported GP notes before FluidSynth
	 * gets a chance to render them.  Normal Advanced imports keep stock behavior. */
	if(!synth_mode)
		eof_truncate_chart(eof_song);
	gp_adv_suppress_timing = 0;
	eof_gp_detect_crash_as_ride = 0;
	eof_beat_stats_cached = 0;
	eof_cursor_visible = 1;
	eof_pen_visible = 1;
	eof_show_mouse(NULL);
	(void)eof_detect_difficulties(eof_song, eof_selected_track);
	eof_fix_window_title();
	eof_render();
	return imported;
}

int eof_menu_file_gp_advanced_import(void)
{
	char *selected;
	char selected_path[1024] = {0};
	char *initial;

	eof_log("eof_menu_file_gp_advanced_import() entered", 1);
	if(!eof_song || !eof_song_loaded)
	{
		allegro_message("Open or create an EOF project before using Guitar Pro (Rocksmith Advanced).");
		return D_O_K;
	}

	initial = (eof_imports_recall_last_path && eof_folder_exists(eof_last_gp_path)) ? eof_last_gp_path : eof_last_eof_path;
	selected = ncd_file_select(0, initial, "Import Guitar Pro / GoPlayAlong XML (Rocksmith Advanced)", eof_filter_gp_files);
	eof_clear_input();
	if(!selected)
		return D_O_K;
	ustrzcpy(selected_path, sizeof(selected_path), selected);
	(void)replace_filename(eof_last_gp_path, selected_path, "", sizeof(eof_last_gp_path));
	if(eof_gp_advanced_import_path(selected_path, 0))
	{
		/* The first save after an Advanced GP import is automatic and silent:
		 * persist the completed import without displaying EOF's save-time
		 * validation/warning prompts.  This flag is local to this call, so any
		 * later normal File > Save uses the usual checks and warnings again. */
		eof_log("GP Advanced: import complete; performing automatic silent save", 1);
		if(eof_menu_file_save_logic(1))
			eof_log("GP Advanced: automatic silent save did not complete successfully", 1);
	}
	return D_O_K;
}

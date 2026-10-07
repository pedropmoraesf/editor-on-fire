/* ------------------------------------------------------------------------- */
/* Main mapping dialog                                                        */
/* ------------------------------------------------------------------------- */

static char gp_adv_row_name[GP_ADV_ROWS][96];
static char gp_adv_page_text[64];

static int gp_adv_build_dialog(DIALOG *dlg, int *prev_idx, int *next_idx,
	int *merge_idx, int *ride_idx, int *check_idx, int *browse_idx, int *ok_idx, int *cancel_idx)
{
	int n = 0, r;
	unsigned long start = gp_adv_page * GP_ADV_ROWS;
	unsigned long pages = (gp_adv_gp->numtracks + GP_ADV_ROWS - 1) / GP_ADV_ROWS;
	int song_flags = gp_adv_source_is_gpa_xml ? D_DISABLED : 0;
	int show_timing = !gp_adv_suppress_timing;
	int button_y = show_timing ? 429 : 339;
	int dialog_h = show_timing ? 479 : 389;

	gp_adv_set_dialog_item(&dlg[n++], eof_window_proc, 0, 0, 630, dialog_h, 0, 0, 0, 0, "Guitar Pro (Rocksmith Advanced)");
	gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 14, 28, 180, 8, 0, 0, 0, 0, "GP track");
	gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 205, 28, 190, 8, 0, 0, 0, 0, "EOF destination");
	gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 407, 28, 84, 8, 0, 0, 0, 0, "Fingering");
	gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 500, 28, 120, 8, 0, 0, 0, 0, "Open strings");

	for(r = 0; r < GP_ADV_ROWS; r++)
	{
		unsigned long idx = start + (unsigned long)r;
		int y = 48 + r * 24;
		int inactive = (idx >= gp_adv_gp->numtracks);
		int string_track = !inactive && ((gp_adv_kind[idx] == GP_ADV_KIND_GUITAR) || (gp_adv_kind[idx] == GP_ADV_KIND_BASS));
		int fidx, aidx;

		if(!inactive)
			snprintf(gp_adv_row_name[r], sizeof(gp_adv_row_name[r]), "%lu. %.21s", idx + 1,
			         gp_adv_gp->names[idx] ? gp_adv_gp->names[idx] : "(unnamed)");
		else
			gp_adv_row_name[r][0] = '\0';
		gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 14, y + 5, 185, 8, 0,
		                    inactive ? D_DISABLED : 0, 0, 0, gp_adv_row_name[r]);
		gp_adv_combo_index[r] = n;
		gp_adv_set_dialog_item(&dlg[n++], gp_adv_combo_proc, 203, y, 193, 20, 0,
		                    inactive ? D_DISABLED : 0, r, 0,
		                    (void *)(inactive ? "" : gp_adv_dest_labels[gp_adv_assign[idx]]));
		fidx = n;
		gp_adv_finger_index[r] = fidx;
		gp_adv_set_dialog_item(&dlg[n++], gp_adv_finger_proc, 408, y, 82, 20, 0,
		                    (!string_track ? D_DISABLED : 0) | ((!inactive && gp_adv_finger[idx]) ? D_SELECTED : 0),
		                    r, 0, "Optimize");
		aidx = n;
		gp_adv_avoid_index[r] = aidx;
		gp_adv_set_dialog_item(&dlg[n++], gp_adv_avoid_proc, 500, y, 116, 20, 0,
		                    (!string_track || !gp_adv_finger[idx] ? D_DISABLED : 0) |
		                    ((!inactive && gp_adv_avoid_open[idx]) ? D_SELECTED : 0),
		                    r, 0, "Avoid open");
		dlg[fidx].d2 = aidx;
	}

	gp_adv_set_dialog_item(&dlg[n++], d_agup_box_proc, 10, 244, 610, 54, 0, 0, 0, 0, NULL);
	*merge_idx = n;
	gp_adv_set_dialog_item(&dlg[n++], d_agup_check_proc, 18, 248, 170, 20, 0,
	                    gp_adv_merge_enabled ? D_SELECTED : 0, 0, 0, "Merge lead on OK");
	gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 193, 254, 58, 8, 0, 0, 0, 0, "when over");
	gp_adv_set_dialog_item(&dlg[n++], eof_verified_edit_proc, 254, 249, 38, 20, 0, 0, 3, 0, gp_adv_merge_threshold);
	dlg[n - 1].dp2 = "0123456789";
	gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 297, 254, 170, 8, 0, 0, 0, 0, "% of measures are empty");
	gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 18, 278, 560, 8, 0, 0, 0, 0,
	                    "Only empty measures are filled; existing notes are never overwritten.");

	*ride_idx = n;
	gp_adv_set_dialog_item(&dlg[n++], d_agup_check_proc, 18, 303, 300, 20, 0,
	                    gp_adv_detect_crash_as_ride ? D_SELECTED : 0, 0, 0,
	                    "Detect Crash Cymbals Used as Ride");

	*check_idx = *browse_idx = -1;
	gp_adv_song_status_dialog_index = -1;
	gp_adv_song_edit_dialog_index = -1;
	if(show_timing)
	{
		gp_adv_set_dialog_item(&dlg[n++], d_agup_box_proc, 10, 328, 610, 82, 0, song_flags, 0, 0, NULL);
		gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 18, 335, 300, 8, 0, song_flags, 0, 0,
		                    gp_adv_source_is_gpa_xml ? "Songsterr/SVL timing (disabled for GoPlayAlong XML)" : "Optional Songsterr/SVL timing");
		gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 18, 354, 132, 8, 0, song_flags, 0, 0, "Revision URL / IDs:");
		gp_adv_song_edit_dialog_index = n;
		/* Use EOF's normal AGUP edit path.  gp_adv_songsterr_edit_proc only adds
		 * validation-on-focus-loss and a thin red/green border after stock draw. */
		gp_adv_set_dialog_item(&dlg[n++], gp_adv_songsterr_edit_proc, 151, 349, 270, 20, 0, song_flags, 255, 0, gp_adv_songsterr_code);
		*check_idx = n;
		gp_adv_set_dialog_item(&dlg[n++], d_agup_button_proc, 428, 349, 82, 20, 0, D_EXIT | song_flags, 0, 0, "Check");
		*browse_idx = n;
		gp_adv_set_dialog_item(&dlg[n++], d_agup_button_proc, 517, 349, 91, 20, 0, D_EXIT | song_flags, 0, 0, "Load timing");
		gp_adv_song_status_dialog_index = n;
		gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 18, 382, 570, 8, 0, song_flags, 0, 0, gp_adv_songsterr_status_text);
	}

	snprintf(gp_adv_page_text, sizeof(gp_adv_page_text), "Page %lu/%lu", gp_adv_page + 1, pages ? pages : 1);
	*prev_idx = n;
	gp_adv_set_dialog_item(&dlg[n++], d_agup_button_proc, 12, button_y, 64, 24, 0, D_EXIT | (gp_adv_page ? 0 : D_DISABLED), 0, 0, "< Prev");
	*next_idx = n;
	gp_adv_set_dialog_item(&dlg[n++], d_agup_button_proc, 82, button_y, 64, 24, 0,
	                    D_EXIT | ((gp_adv_page + 1 < pages) ? 0 : D_DISABLED), 0, 0, "Next >");
	gp_adv_set_dialog_item(&dlg[n++], d_agup_text_proc, 158, button_y + 6, 80, 8, 0, 0, 0, 0, gp_adv_page_text);
	*ok_idx = n;
	gp_adv_set_dialog_item(&dlg[n++], d_agup_button_proc, 438, button_y - 3, 80, 28, '\r', D_EXIT, 0, 0, "OK");
	*cancel_idx = n;
	gp_adv_set_dialog_item(&dlg[n++], d_agup_button_proc, 528, button_y - 3, 80, 28, 0, D_EXIT, 0, 0, "Cancel");
	memset(&dlg[n], 0, sizeof(DIALOG));
	return n;
}

static int gp_adv_run_mapping_dialog(void)
{
	DIALOG dlg[64];
	int prev_idx, next_idx, merge_idx, ride_idx, check_idx, browse_idx, ok_idx, cancel_idx;
	int ret;

	gp_adv_page = 0;
	for(;;)
	{
		memset(dlg, 0, sizeof(dlg));
		(void)gp_adv_build_dialog(dlg, &prev_idx, &next_idx, &merge_idx, &ride_idx, &check_idx, &browse_idx, &ok_idx, &cancel_idx);
		gp_adv_active_dialog = dlg;
		eof_color_dialog(dlg, gui_fg_color, gui_bg_color);
		if(!gp_adv_suppress_timing)
		{
			if(gp_adv_source_is_gpa_xml)
				gp_adv_set_song_status(GP_ADV_STATUS_UNKNOWN, "Songsterr/SVL disabled: GoPlayAlong XML already supplies synchronization.");
			else
				gp_adv_set_song_status(gp_adv_songsterr_status, gp_adv_songsterr_status_text);
		}
		eof_conditionally_center_dialog(dlg);
		ret = eof_popup_dialog(dlg, 0);
		gp_adv_merge_enabled = (dlg[merge_idx].flags & D_SELECTED) ? 1 : 0;
		gp_adv_detect_crash_as_ride = (dlg[ride_idx].flags & D_SELECTED) ? 1 : 0;
		gp_adv_active_dialog = NULL;

		if(ret == cancel_idx || ret < 0)
		{
			eof_gp_detect_crash_as_ride = 0;
			return 0;
		}
		if(ret == prev_idx)
		{
			if(gp_adv_page) gp_adv_page--;
			continue;
		}
		if(ret == next_idx)
		{
			if((gp_adv_page + 1) * GP_ADV_ROWS < gp_adv_gp->numtracks) gp_adv_page++;
			continue;
		}
		if((check_idx >= 0) && (ret == check_idx))
		{
			(void)gp_adv_download_songsterr();
			ustrzcpy(gp_adv_songsterr_last_checked, sizeof(gp_adv_songsterr_last_checked), gp_adv_songsterr_code);
			continue;
		}
		if((browse_idx >= 0) && (ret == browse_idx))
		{
			char *selected = ncd_file_select(0, eof_last_eof_path,
				"Load Songsterr JSON or Sonic Visualiser SVL", gp_adv_get_timing_filter());
			eof_clear_input();
			if(selected)
			{
				ustrzcpy(gp_adv_songsterr_path, sizeof(gp_adv_songsterr_path), selected);
				if(gp_adv_validate_timing_file(gp_adv_songsterr_path))
					gp_adv_set_song_status(GP_ADV_STATUS_VALID, "Songsterr/SVL: valid local timing file loaded");
				else
				{
					gp_adv_songsterr_path[0] = '\0';
					gp_adv_set_song_status(GP_ADV_STATUS_INVALID, "Songsterr/SVL: selected file is invalid");
				}
			}
			continue;
		}
		if(ret == ok_idx)
		{
			if(!gp_adv_suppress_timing && !gp_adv_source_is_gpa_xml && (gp_adv_songsterr_status != GP_ADV_STATUS_VALID))
			{
				const char *why = (gp_adv_songsterr_status == GP_ADV_STATUS_INVALID) ?
					"The Songsterr URL/file is invalid (red status)." : "No Songsterr JSON or SVL timing file was loaded.";
				if(alert(why, "Continue the Guitar Pro import without optional Songsterr/SVL timing?", NULL,
				         "&Yes", "&No", 'y', 'n') != 1)
					continue;
			}
			eof_gp_detect_crash_as_ride = gp_adv_detect_crash_as_ride;
			if(eof_gp_detect_crash_as_ride)
				eof_dtx_warn_crash_as_ride_detection();

			/* If external timing is selected, defer merging until after that timing
			 * is applied and the GP is reparsed against the final beat grid. */
			if(gp_adv_merge_enabled &&
			   (gp_adv_suppress_timing || gp_adv_source_is_gpa_xml ||
			    (gp_adv_songsterr_status != GP_ADV_STATUS_VALID) || !gp_adv_songsterr_path[0]))
				gp_adv_merge_guitars();
			return 1;
		}
	}
}

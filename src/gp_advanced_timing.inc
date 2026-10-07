/* ------------------------------------------------------------------------- */
/* Songsterr JSON / Sonic Visualiser timing                                   */
/* ------------------------------------------------------------------------- */

/* The advanced dialog selects/downloads the timing file itself.  SVL timing
 * continues to use EOF's stock importer.  Songsterr JSON deliberately uses the
 * tolerant parser from the first Advanced importer implementation because the
 * stock EOF parser assumes a very specific compact numeric-list formatting and
 * rejects valid API JSON containing whitespace between values. */
static int gp_adv_timing_override_mode = 0; /* 2=SVL; Songsterr uses local parser */
static char gp_adv_timing_override_path[1024] = {0};
static NCDFS_FILTER_LIST *gp_adv_timing_filter = NULL;
extern DIALOG eof_menu_file_songsterr_import_dialog[];

static NCDFS_FILTER_LIST *gp_adv_get_timing_filter(void)
{
	if(!gp_adv_timing_filter)
	{
		gp_adv_timing_filter = ncdfs_filter_list_create();
		if(gp_adv_timing_filter)
			(void)ncdfs_filter_list_add(gp_adv_timing_filter, "json;svl", "Timing files (*.json, *.svl)", 1);
	}
	return gp_adv_timing_filter;
}

static void gp_adv_set_song_status(int status, const char *text)
{
	gp_adv_songsterr_status = status;
	ustrzcpy(gp_adv_songsterr_status_text, sizeof(gp_adv_songsterr_status_text), text ? text : "");
	if(gp_adv_active_dialog)
	{
		if(gp_adv_song_status_dialog_index >= 0)
		{
			DIALOG *d = &gp_adv_active_dialog[gp_adv_song_status_dialog_index];
			d->dp = gp_adv_songsterr_status_text;
			if(status == GP_ADV_STATUS_VALID)
				d->fg = makecol(0, 180, 0);
			else if(status == GP_ADV_STATUS_INVALID)
				d->fg = makecol(220, 0, 0);
			else
				d->fg = gui_fg_color;
		}
		if(gp_adv_song_edit_dialog_index >= 0)
		{
			DIALOG *d = &gp_adv_active_dialog[gp_adv_song_edit_dialog_index];
			if(status == GP_ADV_STATUS_VALID)
				d->fg = makecol(0, 180, 0);
			else if(status == GP_ADV_STATUS_INVALID)
				d->fg = makecol(220, 0, 0);
			else
				d->fg = gui_fg_color;
		}
	}
}

static int gp_adv_validate_timing_file(const char *path)
{
	char *buffer;
	int valid = 0;
	const char *ext;
	if(!path || !path[0] || !exists(path))
		return 0;
	buffer = (char *)eof_buffer_file(path, 1, 0);
	if(!buffer)
		return 0;
	ext = get_extension(path);
	if(ext && !ustricmp(ext, "svl"))
	{
		if(gp_adv_strcasestr(buffer, "<model") && gp_adv_strcasestr(buffer, "<point"))
			valid = 1;
	}
	else if(ext && !ustricmp(ext, "json") && gp_adv_strcasestr(buffer, "\"points\"") && gp_adv_strcasestr(buffer, "["))
	{
		valid = 1;
	}
	free(buffer);
	return valid;
}

static int gp_adv_parse_positive_long(const char *p, long *value, const char **after)
{
	char *endptr;
	long parsed;
	if(!p || !value || !isdigit((unsigned char)*p))
		return 0;
	parsed = strtol(p, &endptr, 10);
	if((endptr == p) || (parsed <= 0))
		return 0;
	*value = parsed;
	if(after)
		*after = endptr;
	return 1;
}

static int gp_adv_parse_songsterr_ids(const char *text, long *song_id, long *revision_id)
{
	const char *p, *q, *after;
	long s = 0, r = 0;
	if(!text || !song_id || !revision_id)
		return 0;

	/* Accept the API URL itself:
	 * https://www.songsterr.com/api/video-points/6858126/8952941/list */
	p = gp_adv_strcasestr(text, "/api/video-points/");
	if(p)
	{
		p += strlen("/api/video-points/");
		if(!gp_adv_parse_positive_long(p, &s, &after) || (*after != '/'))
			return 0;
		if(!gp_adv_parse_positive_long(after + 1, &r, NULL))
			return 0;
	}
	else
	{
		/* Songsterr revision page.  Use the LAST -tab-s token in case the title
		 * itself happens to contain similar text.  The query string after the
		 * revision is intentionally irrelevant: strtol() stops before '?'. */
		p = gp_adv_strcasestr(text, "-tab-s");
		if(p)
		{
			for(;;)
			{
				q = gp_adv_strcasestr(p + 6, "-tab-s");
				if(!q)
					break;
				p = q;
			}
			p += 6;
			if(!gp_adv_parse_positive_long(p, &s, &after))
				return 0;
			q = gp_adv_strcasestr(after, "/r");
			if(!q)
				return 0;
			q += 2;
			if(!gp_adv_parse_positive_long(q, &r, NULL))
				return 0;
		}
		else
		{
			/* Also accept compact forms such as "6858126 8952941". */
			p = text;
			while(*p && !isdigit((unsigned char)*p))
				p++;
			if(!gp_adv_parse_positive_long(p, &s, &after))
				return 0;
			p = after;
			while(*p && !isdigit((unsigned char)*p))
				p++;
			if(!gp_adv_parse_positive_long(p, &r, NULL))
				return 0;
		}
	}

	*song_id = s;
	*revision_id = r;
	return 1;
}

static int gp_adv_download_songsterr(void)
{
	long song_id, revision_id;
	char url[256], command[1536];
	char folder[1024] = {0};

	if(!gp_adv_parse_songsterr_ids(gp_adv_songsterr_code, &song_id, &revision_id))
	{
		gp_adv_songsterr_path[0] = '\0';
		gp_adv_set_song_status(GP_ADV_STATUS_INVALID, "Songsterr: invalid revision URL / IDs");
		return 0;
	}

	if(eof_song_path[0])
		ustrzcpy(folder, sizeof(folder), eof_song_path);
	else
		ustrzcpy(folder, sizeof(folder), eof_last_eof_path);
	append_filename(gp_adv_songsterr_path, folder, "songsterr_advanced.json", sizeof(gp_adv_songsterr_path));
	delete_file(gp_adv_songsterr_path);

	snprintf(url, sizeof(url), "https://www.songsterr.com/api/video-points/%ld/%ld/list", song_id, revision_id);
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
		"GP Advanced: Songsterr IDs song=%ld revision=%ld URL=%s", song_id, revision_id, url);
	eof_log(eof_log_string, 1);
#ifdef ALLEGRO_WINDOWS
	snprintf(command, sizeof(command), "curl.exe -L -f -sS --connect-timeout 5 --max-time 20 \"%s\" -o \"%s\"", url, gp_adv_songsterr_path);
#else
	snprintf(command, sizeof(command), "curl -L -f -sS --connect-timeout 5 --max-time 20 \"%s\" -o \"%s\"", url, gp_adv_songsterr_path);
#endif
	(void)eof_system(command);
	if(gp_adv_validate_timing_file(gp_adv_songsterr_path))
	{
		gp_adv_set_song_status(GP_ADV_STATUS_VALID, "Songsterr: valid JSON downloaded");
		return 1;
	}
	delete_file(gp_adv_songsterr_path);
	gp_adv_songsterr_path[0] = '\0';
	gp_adv_set_song_status(GP_ADV_STATUS_INVALID, "Songsterr: endpoint did not return valid JSON");
	return 0;
}

static int gp_adv_songsterr_edit_proc(int msg, DIALOG *d, int c)
{
	int ret;

	/* Keep EOF/AGUP's normal edit control for all painting and keyboard input.
	 * Only overlay a thin validation border after the normal control is drawn. */
	if(msg == MSG_DRAW)
	{
		ret = eof_edit_proc(msg, d, c);
		if(gp_adv_songsterr_status == GP_ADV_STATUS_VALID)
			rect(screen, d->x, d->y, d->x + d->w - 1, d->y + d->h - 1, makecol(0, 180, 0));
		else if(gp_adv_songsterr_status == GP_ADV_STATUS_INVALID)
			rect(screen, d->x, d->y, d->x + d->w - 1, d->y + d->h - 1, makecol(220, 0, 0));
		return ret;
	}

	/* As soon as the URL is edited, an old green result must no longer remain
	 * authoritative.  Validation/download happens when focus leaves the field. */
	if((msg == MSG_CHAR) && !gp_adv_suppress_timing && !gp_adv_source_is_gpa_xml)
		gp_adv_set_song_status(GP_ADV_STATUS_UNKNOWN, "Songsterr: URL changed; validating on exit");

	ret = eof_edit_proc(msg, d, c);
	if((msg == MSG_LOSTFOCUS) && !gp_adv_suppress_timing && !gp_adv_source_is_gpa_xml)
	{
		if(strcmp(gp_adv_songsterr_last_checked, gp_adv_songsterr_code))
		{
			ustrzcpy(gp_adv_songsterr_last_checked, sizeof(gp_adv_songsterr_last_checked), gp_adv_songsterr_code);
			if(gp_adv_songsterr_code[0])
				(void)gp_adv_download_songsterr();
			else
			{
				gp_adv_songsterr_path[0] = '\0';
				gp_adv_set_song_status(GP_ADV_STATUS_UNKNOWN, "Songsterr/SVL: not loaded");
			}
		}
		return D_REDRAW;
	}
	return ret;
}

/* Return a pointer just after the opening '[' of the first Songsterr points
 * array.  Be permissive about JSON whitespace around ':' and '['. */
static char *gp_adv_find_songsterr_points(char *text)
{
	char *p;
	if(!text)
		return NULL;
	p = (char *)gp_adv_strcasestr(text, "\"points\"");
	if(!p)
		return NULL;
	p += strlen("\"points\"");
	while(*p && isspace((unsigned char)*p)) p++;
	if(*p != ':')
		return NULL;
	p++;
	while(*p && isspace((unsigned char)*p)) p++;
	if(*p != '[')
		return NULL;
	return p + 1;
}

/* Tolerant Songsterr timing import.  This is based on the parser used by the
 * first GP Advanced implementation, which successfully handled Songsterr's
 * normal JSON formatting.  Unlike EOF's legacy parser it explicitly skips
 * whitespace between numeric values and accepts ordinary strtod() syntax. */
static int gp_adv_apply_songsterr_json(const char *path)
{
	char *textbuffer, *syncpoints, *endptr;
	unsigned num = 0, den = 0;
	unsigned long beatctr = 0, needed, measurectr = 1;
	double timestamp, lastbeatlen;
	int error = 0, another = 0;

	if(!eof_song || !path)
		return 0;
	if(!eof_get_effective_ts(eof_song, &num, &den, 0, 1) || !num || !den)
	{
		allegro_message("Songsterr timing requires a time signature starting on the first beat.");
		return 0;
	}

	textbuffer = (char *)eof_buffer_file(path, 1, 0);
	if(!textbuffer)
		return 0;
	syncpoints = gp_adv_find_songsterr_points(textbuffer);
	if(!syncpoints)
	{
		free(textbuffer);
		allegro_message("Malformed Songsterr JSON file: no points array was found.");
		return 0;
	}

	eof_prepare_undo(EOF_UNDO_TYPE_NONE);
	(void)eof_menu_edit_cut(0, 1);
	for(beatctr = 0; beatctr < eof_song->beats; beatctr++)
	{
		eof_song->beat[beatctr]->ppqn = 500000;
		if(beatctr && (eof_get_ts(eof_song, NULL, NULL, beatctr) != 1))
			eof_song->beat[beatctr]->flags &= ~EOF_BEAT_FLAG_ANCHOR;
	}
	eof_calculate_beats(eof_song);
	eof_process_beat_statistics(eof_song, eof_selected_track);

	beatctr = 0;
	while(*syncpoints && (*syncpoints != ']') && !error)
	{
		while(*syncpoints && isspace((unsigned char)*syncpoints)) syncpoints++;
		if(*syncpoints == ']')
			break;

		errno = 0;
		timestamp = strtod(syncpoints, &endptr);
		if((endptr == syncpoints) || errno || (timestamp < 0.0))
		{
			error = 1;
			break;
		}
		syncpoints = endptr;
		while(*syncpoints && isspace((unsigned char)*syncpoints)) syncpoints++;
		if(*syncpoints == ',')
		{
			another = 1;
			syncpoints++;
		}
		else if(*syncpoints == ']')
		{
			another = 0;
		}
		else
		{
			error = 1;
			break;
		}

		if(beatctr >= eof_song->beats)
		{
			error = 1;
			break;
		}
		if(eof_song->beat[beatctr]->measurenum != measurectr)
		{
			error = 1;
			break;
		}

		timestamp *= 1000.0;
		eof_song->beat[beatctr]->fpos = timestamp;
		eof_song->beat[beatctr]->pos = timestamp + 0.5;
		eof_recalculate_beats(eof_song, beatctr, 0);
		eof_song->beat[beatctr]->flags |= EOF_BEAT_FLAG_ANCHOR;

		if(another)
		{
			needed = beatctr + eof_song->beat[beatctr]->num_beats_in_measure;
			if(needed >= eof_song->beats)
			{
				unsigned long add = needed - eof_song->beats + 1;
				if(!eof_song_append_beats(eof_song, add))
				{
					error = 1;
					break;
				}
				eof_process_beat_statistics(eof_song, eof_selected_track);
			}
			beatctr = needed;
			measurectr++;
		}
	}

	if(error)
	{
		eof_menu_edit_undo();
		free(textbuffer);
		allegro_message("Songsterr timing import failed; the timing change was undone.");
		return 0;
	}
	if(!measurectr || !eof_song->beats)
	{
		eof_menu_edit_undo();
		free(textbuffer);
		return 0;
	}

	if(beatctr > 0)
		lastbeatlen = eof_get_beat_length(eof_song, beatctr - 1);
	else
		lastbeatlen = eof_get_beat_length(eof_song, beatctr);
	while(beatctr + 1 < eof_song->beats)
	{
		eof_song->beat[beatctr + 1]->fpos = eof_song->beat[beatctr]->fpos + lastbeatlen;
		eof_song->beat[beatctr + 1]->pos = eof_song->beat[beatctr + 1]->fpos + 0.5;
		beatctr++;
	}
	eof_calculate_tempo_map(eof_song);
	(void)eof_menu_edit_cut_paste(0, 1);
	eof_song->tags->ogg[0].midi_offset = eof_song->beat[0]->pos;
	eof_truncate_chart(eof_song);
	eof_process_beat_statistics(eof_song, eof_selected_track);
	free(textbuffer);
	eof_log("GP Advanced: Songsterr timing imported with tolerant points parser", 1);
	return 1;
}

/* These two functions are called from menu/file.c through the force-included
 * bridge.  They are normal pass-throughs unless the advanced importer is
 * actively invoking EOF's original SVL timing import command. */
int eof_gp_advanced_file_popup_dialog(DIALOG *dialog, int focus)
{
	if((gp_adv_timing_override_mode == 1) && (dialog == eof_menu_file_songsterr_import_dialog))
		return 6;
	return eof_popup_dialog(dialog, focus);
}

char *eof_gp_advanced_file_select(int type, char *initial, const char *title, NCDFS_FILTER_LIST *filters)
{
	if(gp_adv_timing_override_mode && gp_adv_timing_override_path[0])
		return gp_adv_timing_override_path;
	return ncd_file_select(type, initial, title, filters);
}

static int gp_adv_apply_timing_file(const char *path)
{
	const char *ext;
	int mode;
	if(!path || !path[0] || !gp_adv_validate_timing_file(path))
		return 0;

	ext = get_extension(path);
	mode = (ext && !ustricmp(ext, "svl")) ? 2 : 1;

	/* Songsterr: use the first Advanced importer's whitespace-tolerant parser.
	 * This intentionally avoids EOF's legacy decimal-list parser, which rejects
	 * valid JSON formatting with spaces between point values. */
	if(mode == 1)
		return gp_adv_apply_songsterr_json(path);

	/* Sonic Visualiser remains delegated to EOF's established importer. */
	ustrzcpy(gp_adv_timing_override_path, sizeof(gp_adv_timing_override_path), path);
	gp_adv_timing_override_mode = 2;
	(void)eof_menu_file_sonic_visualiser_import();
	gp_adv_timing_override_mode = 0;
	gp_adv_timing_override_path[0] = '\0';
	return 1;
}

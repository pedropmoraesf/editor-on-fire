/* ------------------------------------------------------------------------- */
/* Empty-measure merge                                                        */
/* ------------------------------------------------------------------------- */

static int gp_adv_measure_bounds(unsigned long measure, unsigned long *start, unsigned long *end)
{
	unsigned long b;
	int found = 0;
	if(!eof_song || !start || !end || (measure < 1))
		return 0;
	for(b = 0; b < eof_song->beats; b++)
	{
		if(eof_song->beat[b]->measurenum == measure)
		{
			*start = eof_song->beat[b]->pos;
			found = 1;
			break;
		}
	}
	if(!found)
		return 0;
	for(; b < eof_song->beats; b++)
	{
		if(eof_song->beat[b]->measurenum > measure)
		{
			*end = eof_song->beat[b]->pos;
			return (*end > *start);
		}
	}
	*end = eof_chart_length + 1;
	if(*end <= *start)
		*end = *start + 1000;
	return 1;
}

static int gp_adv_track_measure_has_notes(EOF_PRO_GUITAR_TRACK *tp, unsigned long measure)
{
	unsigned long start, end, n;
	if(!tp || !gp_adv_measure_bounds(measure, &start, &end))
		return 0;
	for(n = 0; n < tp->notes; n++)
	{
		if(tp->note[n] && (tp->note[n]->pos >= start) && (tp->note[n]->pos < end))
			return 1;
	}
	return 0;
}

static unsigned long gp_adv_effective_measure_count(void)
{
	unsigned long m, last = 0;
	if(!eof_song)
		return 0;
	for(m = 0; m < eof_song->beats; m++)
	{
		if(eof_song->beat[m]->measurenum > last)
			last = eof_song->beat[m]->measurenum;
	}
	if(gp_adv_gp && gp_adv_gp->measures && (!last || (gp_adv_gp->measures < last)))
		last = gp_adv_gp->measures;
	return last;
}

static double gp_adv_track_empty_percent(unsigned long track)
{
	unsigned long m, measures, empty = 0;
	EOF_PRO_GUITAR_TRACK *tp;
	if(!gp_adv_gp || (track >= gp_adv_gp->numtracks) || !gp_adv_gp->track[track])
		return 100.0;
	tp = gp_adv_gp->track[track];
	measures = gp_adv_effective_measure_count();
	if(!measures)
		return 100.0;
	for(m = 1; m <= measures; m++)
	{
		if(!gp_adv_track_measure_has_notes(tp, m))
			empty++;
	}
	return 100.0 * (double)empty / (double)measures;
}

static unsigned long gp_adv_copy_measure_notes(unsigned long target, unsigned long donor, unsigned long measure)
{
	EOF_PRO_GUITAR_TRACK *dst, *src;
	unsigned long start, end, n, copied = 0;
	if(!gp_adv_gp || (target >= gp_adv_gp->numtracks) || (donor >= gp_adv_gp->numtracks) || (target == donor))
		return 0;
	dst = gp_adv_gp->track[target];
	src = gp_adv_gp->track[donor];
	if(!dst || !src || !gp_adv_measure_bounds(measure, &start, &end))
		return 0;
	if(gp_adv_track_measure_has_notes(dst, measure) || !gp_adv_track_measure_has_notes(src, measure))
		return 0;

	for(n = 0; n < src->notes; n++)
	{
		EOF_PRO_GUITAR_NOTE *np;
		if(!src->note[n] || (src->note[n]->pos < start) || (src->note[n]->pos >= end))
			continue;
		np = eof_pro_guitar_track_add_note(dst);
		if(!np)
			break;
		memcpy(np, src->note[n], sizeof(EOF_PRO_GUITAR_NOTE));
		copied++;
	}
	if(copied)
		eof_pro_guitar_track_sort_notes(dst);
	return copied;
}

static int gp_adv_donor_matches_group(unsigned long donor, int group)
{
	if(!gp_adv_kind || !gp_adv_gp || (donor >= gp_adv_gp->numtracks))
		return 0;
	switch(group)
	{
		case 1: return gp_adv_kind[donor] == GP_ADV_KIND_GUITAR;
		case 2: return gp_adv_kind[donor] == GP_ADV_KIND_BASS;
		case 3: return gp_adv_kind[donor] == GP_ADV_KIND_OTHER;
		case 4: return gp_adv_kind[donor] == GP_ADV_KIND_VOCAL;
	}
	return 0;
}

static unsigned long gp_adv_fill_empty_from_group(unsigned long target, int group)
{
	unsigned long m, measures, copied = 0;
	if(!gp_adv_gp || (target >= gp_adv_gp->numtracks))
		return 0;
	measures = gp_adv_effective_measure_count();
	for(m = 1; m <= measures; m++)
	{
		unsigned long d, best = ULONG_MAX;
		double best_empty = 101.0;
		if(gp_adv_track_measure_has_notes(gp_adv_gp->track[target], m))
			continue;
		for(d = 0; d < gp_adv_gp->numtracks; d++)
		{
			double emptiness;
			if((d == target) || !gp_adv_donor_matches_group(d, group) || !gp_adv_gp->track[d])
				continue;
			if(!gp_adv_track_measure_has_notes(gp_adv_gp->track[d], m))
				continue;
			emptiness = gp_adv_track_empty_percent(d);
			if(emptiness < best_empty)
			{
				best_empty = emptiness;
				best = d;
			}
		}
		if(best != ULONG_MAX)
			copied += gp_adv_copy_measure_notes(target, best, m);
	}
	return copied;
}

static void gp_adv_merge_guitars(void)
{
	unsigned long i;
	int threshold = atoi(gp_adv_merge_threshold);
	unsigned long changed = 0, over50 = 0, guitars = 0;
	char msg[256];

	if(!gp_adv_gp || !eof_song)
		return;
	if(threshold < 0) threshold = 0;
	if(threshold > 100) threshold = 100;
	snprintf(gp_adv_merge_threshold, sizeof(gp_adv_merge_threshold), "%d", threshold);

	eof_process_beat_statistics(eof_song, eof_selected_track);
	if(!gp_adv_effective_measure_count())
	{
		allegro_message("Unable to identify measures in the active EOF tempo map.  The merge was not performed.");
		return;
	}

	for(i = 0; i < gp_adv_gp->numtracks; i++)
	{
		if(gp_adv_kind[i] != GP_ADV_KIND_GUITAR)
			continue;
		guitars++;
		if(gp_adv_track_empty_percent(i) > (double)threshold)
			changed += gp_adv_fill_empty_from_group(i, 1);
	}

	for(i = 0; i < gp_adv_gp->numtracks; i++)
	{
		if((gp_adv_kind[i] == GP_ADV_KIND_GUITAR) && (gp_adv_track_empty_percent(i) > 50.0))
			over50++;
	}

	if((guitars >= 2) && (over50 >= 2))
	{
		if(alert("Two or more guitar tracks still have over 50% empty measures.",
		         "Complete remaining empty measures using other instruments?",
		         "Preference: other guitars -> bass -> other GP instruments -> vocals.",
		         "&Yes", "&No", 'y', 'n') == 1)
		{
			for(i = 0; i < gp_adv_gp->numtracks; i++)
			{
				if((gp_adv_kind[i] != GP_ADV_KIND_GUITAR) || (gp_adv_track_empty_percent(i) <= 50.0))
					continue;
				if(gp_adv_track_empty_percent(i) > 50.0) changed += gp_adv_fill_empty_from_group(i, 2);
				if(gp_adv_track_empty_percent(i) > 50.0) changed += gp_adv_fill_empty_from_group(i, 3);
				if(gp_adv_track_empty_percent(i) > 50.0) changed += gp_adv_fill_empty_from_group(i, 4);
			}
		}
	}

	snprintf(msg, sizeof(msg), "Merge complete: %lu note(s) copied into previously empty measures.", changed);
	allegro_message("%s", msg);
}

/* ------------------------------------------------------------------------- */
/* Assignment combo and per-row controls                                      */
/* ------------------------------------------------------------------------- */

static char *gp_adv_assignment_list(int index, int *size)
{
	if(index < 0)
	{
		if(size) *size = GP_ADV_DEST_COUNT;
		return NULL;
	}
	if(index >= GP_ADV_DEST_COUNT)
		return NULL;
	return (char *)gp_adv_dest_labels[index];
}

static int gp_adv_choose_assignment(int current)
{
	DIALOG dlg[5];
	int ret;
	memset(dlg, 0, sizeof(dlg));
	gp_adv_set_dialog_item(&dlg[0], eof_window_proc, 0, 0, 330, 250, 0, 0, 0, 0, "Destination");
	gp_adv_set_dialog_item(&dlg[1], d_agup_list_proc, 12, 28, 306, 164, 0, 0, current, 0, (void *)gp_adv_assignment_list);
	gp_adv_set_dialog_item(&dlg[2], d_agup_button_proc, 75, 206, 75, 28, '\r', D_EXIT, 0, 0, "OK");
	gp_adv_set_dialog_item(&dlg[3], d_agup_button_proc, 180, 206, 75, 28, 0, D_EXIT, 0, 0, "Cancel");
	eof_color_dialog(dlg, gui_fg_color, gui_bg_color);
	eof_conditionally_center_dialog(dlg);
	ret = eof_popup_dialog(dlg, 1);
	if(ret == 2)
		return dlg[1].d1;
	return current;
}

static void gp_adv_refresh_visible_assignment(unsigned long track)
{
	unsigned r;
	if(!gp_adv_active_dialog)
		return;
	for(r = 0; r < GP_ADV_ROWS; r++)
	{
		unsigned long idx = gp_adv_page * GP_ADV_ROWS + r;
		if(idx == track)
		{
			gp_adv_active_dialog[gp_adv_combo_index[r]].dp = (void *)gp_adv_dest_labels[gp_adv_assign[idx]];
			break;
		}
	}
}

static int gp_adv_combo_proc(int msg, DIALOG *d, int c)
{
	if(msg == MSG_CLICK)
	{
		unsigned long row = gp_adv_page * GP_ADV_ROWS + (unsigned long)d->d1;
		int chosen;
		unsigned long i;
		if(!gp_adv_gp || (row >= gp_adv_gp->numtracks))
			return D_O_K;
		chosen = gp_adv_choose_assignment(gp_adv_assign[row]);
		if(chosen != gp_adv_assign[row])
		{
			if(chosen != GP_ADV_DEST_NONE)
			{
				for(i = 0; i < gp_adv_gp->numtracks; i++)
				{
					if((i != row) && (gp_adv_assign[i] == chosen))
					{
						gp_adv_assign[i] = GP_ADV_DEST_NONE;
						gp_adv_refresh_visible_assignment(i);
					}
				}
			}
			gp_adv_assign[row] = chosen;
			d->dp = (void *)gp_adv_dest_labels[chosen];
		}
		return D_REDRAW;
	}
	return d_agup_button_proc(msg, d, c);
}

static int gp_adv_finger_proc(int msg, DIALOG *d, int c)
{
	int ret = d_agup_check_proc(msg, d, c);
	if(msg == MSG_CLICK)
	{
		unsigned long row = gp_adv_page * GP_ADV_ROWS + (unsigned long)d->d1;
		if(gp_adv_gp && (row < gp_adv_gp->numtracks))
		{
			gp_adv_finger[row] = (d->flags & D_SELECTED) ? 1 : 0;
			if(gp_adv_active_dialog && (d->d2 >= 0))
			{
				if(gp_adv_finger[row])
					gp_adv_active_dialog[d->d2].flags &= ~D_DISABLED;
				else
					gp_adv_active_dialog[d->d2].flags |= D_DISABLED;
			}
		}
	}
	return ret;
}

static int gp_adv_avoid_proc(int msg, DIALOG *d, int c)
{
	int ret = d_agup_check_proc(msg, d, c);
	if(msg == MSG_CLICK)
	{
		unsigned long row = gp_adv_page * GP_ADV_ROWS + (unsigned long)d->d1;
		if(gp_adv_gp && (row < gp_adv_gp->numtracks))
			gp_adv_avoid_open[row] = (d->flags & D_SELECTED) ? 1 : 0;
	}
	return ret;
}

#include <allegro.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "song.h"
#include "utility.h"
#include "dtx_integration.h"
#include "dtx_xml.h"

static const char *eof_dtx_xml_diff_name(unsigned char diff)
{
	static const char *names[5] = {"BASIC", "ADVANCED", "EXTREME", "MASTER", "ULTIMATE"};
	return (diff < 5) ? names[diff] : "UNKNOWN";
}

static const char *eof_dtx_xml_channel_name(unsigned midi)
{
	switch(eof_dtx_integration_channel_from_midi(midi))
	{
		case EOF_DTX_INT_HH: return "hi_hat";
		case EOF_DTX_INT_HH_OPEN: return "hi_hat_open";
		case EOF_DTX_INT_RIDE: return "ride";
		case EOF_DTX_INT_KICK: return "kick";
		case EOF_DTX_INT_SNARE: return "snare";
		case EOF_DTX_INT_TOM1: return "tom1";
		case EOF_DTX_INT_TOM2: return "tom2";
		case EOF_DTX_INT_TOM3: return "tom3";
		case EOF_DTX_INT_CRASH: return "crash";
		default: return "unsupported";
	}
}

static void eof_dtx_xml_escape(const char *src, char *dst, size_t size)
{
	size_t used = 0;
	const char *rep;
	if(!dst || !size)
		return;
	dst[0] = '\0';
	if(!src)
		return;
	while(*src && used + 1 < size)
	{
		switch(*src)
		{
			case '&': rep = "&amp;"; break;
			case '<': rep = "&lt;"; break;
			case '>': rep = "&gt;"; break;
			case '"': rep = "&quot;"; break;
			case '\'': rep = "&apos;"; break;
			default:
				dst[used++] = *src++;
				dst[used] = '\0';
				continue;
		}
		while(*rep && used + 1 < size)
			dst[used++] = *rep++;
		dst[used] = '\0';
		src++;
	}
}

int eof_dtx_export_track_xml(EOF_SONG *sp, const char *project_filename)
{
	char path[1024] = {0}, line[1024] = {0};
	char title[512] = {0}, artist[512] = {0};
	PACKFILE *fp;
	EOF_PRO_GUITAR_TRACK *tp;
	unsigned long ptrack, ctr, stringnum, hit_count = 0;

	if(!sp || !project_filename || !project_filename[0])
		return 0;
	if((EOF_TRACK_DRUM_DTX >= sp->tracks) || !sp->track[EOF_TRACK_DRUM_DTX])
		return 0;
	if(sp->track[EOF_TRACK_DRUM_DTX]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT)
		return 0;
	ptrack = sp->track[EOF_TRACK_DRUM_DTX]->tracknum;
	if((ptrack >= sp->pro_guitar_tracks) || !sp->pro_guitar_track[ptrack])
		return 0;
	tp = sp->pro_guitar_track[ptrack];

	for(ctr = 0; ctr < tp->pgnotes; ctr++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[ctr];
		if(!np)
			continue;
		for(stringnum = 0; stringnum < 6; stringnum++)
			if(np->note & (1UL << stringnum))
				hit_count++;
	}

	replace_filename(path, project_filename, "PART_REAL_DRUM_DTX.xml", sizeof(path));
	fp = pack_fopen(path, "w");
	if(!fp)
		return 0;

	eof_dtx_xml_escape(sp->tags ? sp->tags->title : "", title, sizeof(title));
	eof_dtx_xml_escape(sp->tags ? sp->tags->artist : "", artist, sizeof(artist));
	pack_fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", fp);
	(void)snprintf(line, sizeof(line) - 1,
		"<partRealDrumDtx version=\"1\" title=\"%s\" artist=\"%s\" hitCount=\"%lu\">\n",
		title, artist, hit_count);
	pack_fputs(line, fp);
	pack_fputs("  <track name=\"PART_REAL_DRUM_DTX\">\n", fp);

	for(ctr = 0; ctr < tp->pgnotes; ctr++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[ctr];
		if(!np)
			continue;
		for(stringnum = 0; stringnum < 6; stringnum++)
		{
			unsigned midi;
			if(!(np->note & (1UL << stringnum)))
				continue;
			midi = np->frets[stringnum] & 0x7F;
			(void)snprintf(line, sizeof(line) - 1,
				"    <hit timeMs=\"%lu\" time=\"%.3f\" lengthMs=\"%lu\" difficulty=\"%u\" difficultyName=\"%s\" midi=\"%u\" string=\"%lu\" channel=\"%s\" />\n",
				np->pos, (double)np->pos / 1000.0, np->length, (unsigned)np->type,
				eof_dtx_xml_diff_name(np->type), midi, stringnum, eof_dtx_xml_channel_name(midi));
			pack_fputs(line, fp);
		}
	}

	pack_fputs("  </track>\n", fp);
	pack_fputs("</partRealDrumDtx>\n", fp);
	pack_fclose(fp);
	return exists(path) ? 1 : 0;
}

int eof_dtx_save_song_hook(EOF_SONG *sp, const char *filename)
{
	int result = eof_save_song(sp, filename);
	const char *name;

	if(!result || !sp || !filename || !filename[0])
		return result;

	/* This hook is injected only into menu/file.c.  Ignore its emergency lost
	 * OGG backup and clone saves; ordinary Save/Save As for the active project
	 * gets the sidecar XML. */
	name = get_filename(filename);
	if(sp != eof_song || (name && strstr(name, "lostoggbackup")))
		return result;
	if(ustricmp(get_extension(filename), "eof"))
		return result;

	if(!eof_dtx_export_track_xml(sp, filename))
		eof_log("Warning: PART_REAL_DRUM_DTX.xml could not be generated beside the saved EOF project.", 1);
	return result;
}

#ifndef EOF_DTX_DIALOG_HOOK_H
#define EOF_DTX_DIALOG_HOOK_H

/* Force-included only for dtx_export_v2.c.  Load EOF's normal declarations
 * first, then redirect only this exporter's popup and system-command call sites
 * to DTX-specific wrappers. */
#include <stdio.h>
#include <string.h>
#include "dialog.h"
#include "utility.h"
#include "dtx_dialog.h"
#include "dtx_preview.h"

/* dtx_export_v2.c historically fell back to eof_copy_file(audio, preview.ogg)
 * when FFmpeg was unavailable or preview generation failed.  Do not copy the
 * full song.  Feed the same source/destination paths back through the DTX
 * preview hook using its recognizable command signature.  eof_dtx_system_hook()
 * now replaces that operation entirely with the native ALOGG + RMS + fades +
 * OggEnc implementation, so this succeeds even when FFmpeg is unavailable. */
static int eof_dtx_preview_copy_file_hook(const char *src, const char *dest)
{
	const char *name;

	if(dest)
	{
		name = get_filename(dest);
		if(name && !ustricmp(name, "preview.ogg"))
		{
			char command[8192];
			int result;

			if(exists(dest))
				delete_file(dest);
			if(!src || !src[0])
				return 0;

			/* The executable token is intentionally only a marker.  The preview
			 * hook recognizes this signature and never executes FFmpeg for it. */
			(void)snprintf(command, sizeof(command),
				"\"ffmpeg\" -y -ss 0 -i \"%s\" -t 25 -af afade=t=in:st=0:d=2,afade=t=out:st=23:d=2 -c:a libvorbis -q:a 4 \"%s\"",
				src, dest);
			result = eof_dtx_system_hook(command);
			return ((result == 0) && exists(dest)) ? 1 : 0;
		}
	}
	return eof_copy_file(src, dest);
}

/* Preview generation is native now.  FFmpeg remains useful only for the
 * optional loudness-normalization pass of the exported full song. */
static int eof_dtx_preview_alert3(const char *s1, const char *s2, const char *s3,
	const char *b1, const char *b2, const char *b3, int c1, int c2, int c3)
{
	if(s1 && !ustricmp(s1, "FFmpeg was not found automatically."))
	{
		return alert3(s1,
			"The 25-second DTX preview is generated natively and does not require FFmpeg.",
			"FFmpeg is only needed here for optional loudness normalization of the full song.",
			b1, b2, b3, c1, c2, c3);
	}
	return alert3(s1, s2, s3, b1, b2, b3, c1, c2, c3);
}

#define eof_popup_dialog(dialog, focus) eof_dtx_popup_dialog((dialog), (focus))
#define eof_system(command) eof_dtx_system_hook((command))
#define eof_copy_file(src, dest) eof_dtx_preview_copy_file_hook((src), (dest))
#define alert3(s1, s2, s3, b1, b2, b3, c1, c2, c3) \
	eof_dtx_preview_alert3((s1), (s2), (s3), (b1), (b2), (b3), (c1), (c2), (c3))

#endif

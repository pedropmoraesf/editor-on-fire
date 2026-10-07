#ifndef EOF_DTX_INTEGRATION_COLOR_HOOK_H
#define EOF_DTX_INTEGRATION_COLOR_HOOK_H

/* dtx_integration.c used eof_color_gray only for the DTX crash cymbal.
 * Remap that reference to EOF's light gray palette entry without changing the
 * normal EOF theme colors used by other tracks. */
#define eof_color_gray eof_color_light_gray

#endif

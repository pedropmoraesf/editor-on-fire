#ifndef EOF_FINGERING_PUBLIC_HOOK_H
#define EOF_FINGERING_PUBLIC_HOOK_H

/*
 * fingering.c originally keeps the Viterbi implementation file-local.  The
 * advanced GP importer must call that exact implementation without duplicating
 * it.  Pre-load every header used by fingering.c, then make its file-local
 * helpers link-visible for this translation unit only.  The symbols all carry
 * the eof_fingering_ prefix and therefore don't collide with other modules.
 */
#include <allegro.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agup/agup.h"
#include "main.h"
#include "song.h"
#include "undo.h"
#include "dialog.h"
#include "tuning.h"
#include "menu/track.h"
#include "fingering.h"
#ifdef USEMEMWATCH
#include "memwatch.h"
#endif

#define static

#endif

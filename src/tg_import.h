#ifndef EOF_TG_IMPORT_H
#define EOF_TG_IMPORT_H

#include "gp_import.h"

/* Loads the native TuxGuitar 2.x .tg container (version.txt + content.xml)
 * into EOF's existing Guitar Pro import interchange structure. */
struct eof_guitar_pro_struct *eof_load_tg(const char *fn, char *undo_made);

#endif

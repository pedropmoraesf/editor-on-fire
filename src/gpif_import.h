#ifndef EOF_GPIF_IMPORT_H
#define EOF_GPIF_IMPORT_H

#ifdef EOF_BUILD
struct eof_guitar_pro_struct;

/* Returns nonzero when fn is a ZIP container.  The GPIF loader performs the
 * stricter check for Content/score.gpif. */
int eof_gpif_is_container(const char *fn);

/* Loads Guitar Pro GPIF based files (.gp from GP7/8 and ZIP based .gpx) into
 * the same intermediate structure used by EOF's GP3/4/5 importer. */
struct eof_guitar_pro_struct *eof_load_gpif(const char *fn, char *undo_made);
#endif

#endif

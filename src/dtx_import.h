#ifndef EOF_DTX_IMPORT_H
#define EOF_DTX_IMPORT_H

int eof_menu_file_dtx_import(void);
int eof_dtx_import_session_active(void);
int eof_dtx_import_is_bpm_linked(void);
int eof_dtx_import_active_difficulty(void);
void eof_dtx_import_sync_editor_state(void);
int eof_dtx_import_grid_start_click(unsigned long beat);
int eof_dtx_import_grid_line_mouse(int near_line, int pressed);

#endif

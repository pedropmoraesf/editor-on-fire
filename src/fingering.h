#ifndef EOF_FINGERING_H
#define EOF_FINGERING_H

extern DIALOG eof_menu_note_optimize_fingering_dialog[];

int eof_menu_note_optimize_fingering(void);
	//Note>Optimize fingering
	//Re-frets the single notes of the active pro guitar/bass track difficulty (or only the selected ones)
	//to minimize fretting hand movement, using dynamic programming (Viterbi) over the whole note sequence.
	//Chords and notes whose techniques depend on their exact position are left untouched.

#endif

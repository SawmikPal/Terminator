#ifndef GUI_H
#define GUI_H

/* Runs the X11 window until the last tab closes.
 * use_process = 0: each tab is a thread; 1: each tab is a forked process. */
int gui_run(int use_process);

#endif

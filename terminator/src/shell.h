/* shell.h - the shell engine.
 *
 * The engine does not care where its text comes from: the GUI (one
 * interactive engine per tab), a script file, -c, or stdin.
 */
#ifndef SHELL_H
#define SHELL_H

#include <limits.h>
#include <sys/types.h>

#include "proto.h"

/* Interactive engine for one tab. Runs until the tab closes.
 *   msg_fd: framed messages from the GUI
 *   out_fd: raw output to the GUI (also given to every command as 1 and 2)
 *   ctl_fd: framed control messages to the GUI
 *   is_thread: 1 if this engine shares its process with other tabs */
void shell_main_interactive(int msg_fd, int out_fd, int ctl_fd, int is_thread,
                            const char *cwd);

/* Non-GUI entry point: "-c cmd", a script file, or lines on stdin. */
int shell_run_headless(int argc, char **argv);

#endif

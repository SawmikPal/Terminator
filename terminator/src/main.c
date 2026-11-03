/* main.c - Terminator entry point.
 *
 *   ./terminator                    GUI, thread mode (each tab is a thread)
 *   ./terminator useProcess=True    GUI, process mode (each tab is a process)
 *   ./terminator -c 'cmd' [args]    run one command line, no window
 *   ./terminator script.sh [args]   interpret a script, no window
 *   ./terminator --headless         read commands from stdin, no window
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "gui.h"
#include "history.h"
#include "shell.h"

static int truthy(const char *v)
{
    return !strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "yes");
}

int main(int argc, char **argv)
{
    /* A dead reader must give EPIPE, not kill the whole terminal.
     * Children reset this to the default before exec. */
    signal(SIGPIPE, SIG_IGN);

    int process_mode = 0;
    int first_arg = 1;
    for (; first_arg < argc; first_arg++) {
        const char *a = argv[first_arg];
        if (!strncasecmp(a, "useProcess=", 11)) process_mode = truthy(a + 11);
        else if (!strcmp(a, "--process")) process_mode = 1;
        else if (!strcmp(a, "--thread")) process_mode = 0;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: terminator [useProcess=True|False]\n"
                   "       terminator -c 'command' [args]\n"
                   "       terminator script.sh [args]\n"
                   "       terminator --headless   (commands from stdin)\n");
            return 0;
        } else break;
    }

    /* Shared history is mapped before any tab exists, so every thread and
     * every forked tab process sees the same memory. */
    hist_init();

    if (first_arg < argc)
        return shell_run_headless(argc - first_arg, argv + first_arg);
    return gui_run(process_mode);
}

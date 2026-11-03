/* history.h - command history shared by every tab.
 *
 * The history lives in one anonymous MAP_SHARED mapping created before any
 * tab exists, so tab threads (thread mode) and forked tab processes
 * (process mode) all see the same bytes. A process-shared POSIX semaphore
 * inside the mapping serialises access in both modes.
 */
#ifndef HISTORY_H
#define HISTORY_H

#include <stddef.h>

#define HIST_MAX 10000
#define HIST_LEN 1024

void hist_init(void);
void hist_add(const char *cmd);
int  hist_count(void);
int  hist_get(int i, char *out, size_t cap);   /* 0 = oldest; returns 0 on success */
int  hist_snapshot(char ***out);               /* copies all entries, oldest first */
void hist_free_snapshot(char **v, int n);

#endif

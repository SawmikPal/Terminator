#define _GNU_SOURCE
#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "proto.h"

typedef struct {
    sem_t lock;               /* pshared = 1: works across threads and processes */
    int start, count;         /* ring buffer of HIST_MAX entries                 */
    char e[HIST_MAX][HIST_LEN];
} histmem;

static histmem *H;
static char hpath[PATH_MAX];

static void lock(void)
{
    while (sem_wait(&H->lock) < 0 && errno == EINTR)
        ;
}
static void unlock(void) { sem_post(&H->lock); }

/* On disk, one entry per line: '\' -> "\\", newline -> "\n". */
static void encode(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    for (; *in && j + 3 < cap; in++) {
        if (*in == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (*in == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else out[j++] = *in;
    }
    out[j] = 0;
}

static void decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1] == 'n') { *o++ = '\n'; s++; }
        else if (*s == '\\' && s[1] == '\\') { *o++ = '\\'; s++; }
        else *o++ = *s;
    }
    *o = 0;
}

static void push(const char *s)
{
    int idx;
    if (H->count == HIST_MAX) {
        idx = H->start;
        H->start = (H->start + 1) % HIST_MAX;
    } else {
        idx = (H->start + H->count) % HIST_MAX;
        H->count++;
    }
    strncpy(H->e[idx], s, HIST_LEN - 1);
    H->e[idx][HIST_LEN - 1] = 0;
}

static const char *at(int i) { return H->e[(H->start + i) % HIST_MAX]; }

void hist_init(void)
{
    H = mmap(NULL, sizeof(histmem), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (H == MAP_FAILED) {
        perror("terminator: mmap history");
        exit(1);
    }
    sem_init(&H->lock, 1, 1);

    const char *home = getenv("HOME");
    snprintf(hpath, sizeof hpath, "%s/.terminator_history", home ? home : ".");

    FILE *f = fopen(hpath, "re");
    if (!f)
        return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int total = 0;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[--n] = 0;
        if (!n)
            continue;
        decode(line);
        push(line);
        total++;
    }
    free(line);
    fclose(f);

    /* Keep the file from growing forever: rewrite it with the last HIST_MAX. */
    if (total > HIST_MAX + HIST_MAX / 2) {
        char tmp[PATH_MAX + 8];
        snprintf(tmp, sizeof tmp, "%s.tmp", hpath);
        FILE *o = fopen(tmp, "we");
        if (o) {
            char enc[HIST_LEN * 2 + 4];
            for (int i = 0; i < H->count; i++) {
                encode(at(i), enc, sizeof enc);
                fprintf(o, "%s\n", enc);
            }
            fclose(o);
            rename(tmp, hpath);
        }
    }
}

void hist_add(const char *cmd)
{
    const char *p = cmd;
    while (*p == ' ' || *p == '\t' || *p == '\n')
        p++;
    if (!*p)
        return;
    lock();
    if (H->count == 0 || strcmp(at(H->count - 1), cmd) != 0) {
        push(cmd);
        int fd = open(hpath, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) {
            char enc[HIST_LEN * 2 + 4];
            encode(cmd, enc, sizeof enc - 1);
            size_t l = strlen(enc);
            enc[l++] = '\n';
            write_all(fd, enc, l);
            close(fd);
        }
    }
    unlock();
}

int hist_count(void)
{
    lock();
    int n = H->count;
    unlock();
    return n;
}

int hist_get(int i, char *out, size_t cap)
{
    int r = -1;
    lock();
    if (i >= 0 && i < H->count) {
        snprintf(out, cap, "%s", at(i));
        r = 0;
    }
    unlock();
    return r;
}

int hist_snapshot(char ***out)
{
    lock();
    int n = H->count;
    char **v = malloc(sizeof(char *) * (size_t)(n + 1));
    for (int i = 0; i < n; i++)
        v[i] = strdup(at(i));
    v[n] = NULL;
    unlock();
    *out = v;
    return n;
}

void hist_free_snapshot(char **v, int n)
{
    for (int i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

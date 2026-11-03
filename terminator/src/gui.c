/* gui.c - Terminator's window: an X11 client living in one poll() loop.
 *
 * Only this file talks to Xlib, and only from the main thread.
 * The screen is always a drawing of the current tab's buffer: keys and
 * output change the buffer, then the window is redrawn from it.
 */
#define _GNU_SOURCE
#include "gui.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "history.h"
#include "keys.h"
#include "proto.h"
#include "shell.h"

#define MAX_TABS   16
#define SCROLLBACK 10000
#define INPUT_MAX  16384
#define PAD        6
#define NEWLINE_GLYPH ((char)0xB6)   /* shows a newline inside the input line */

/* ------------------------------------------------------------------ */
/* tab state                                                           */
/* ------------------------------------------------------------------ */

typedef struct { int line, col; } pos;

typedef struct tab {
    int id;
    pid_t pid;                 /* process mode */
    pthread_t th;              /* thread mode  */
    int in_w, out_r, ctl_r;    /* GUI ends of the three pipes */
    msgbuf ctl;
    int closing;

    char **lines; int nl;      /* committed output lines                    */
    char *cur; int curlen, curcap;  /* the current (unfinished) line          */
    int esc;                   /* ANSI escape parser state                   */
    int cr;                    /* saw '\r': next text overwrites the line    */

    char input[INPUT_MAX]; int ilen, icur;
    char prompt[256];
    char main_prompt[256];     /* the user@terminator> prompt, for colouring */
    char cwd[PATH_MAX];
    int scroll;                /* rows scrolled back from the bottom         */

    int searching;             /* Ctrl+R: typing the search term             */
    int hist_idx;              /* -1 = not browsing                          */
    char hist_saved[INPUT_MAX];

    char **choices; int nchoices; /* Tab completion menu                    */
    int choice_start;          /* index in input where the completed word starts */
    char choice_dir[PATH_MAX];
    char digits[8]; int ndigits;

    int has_sel, selecting;
    pos sel_a, sel_b;
} tab;

static Display *dpy;
static Window win;
static int scr;
static GC gc;
static XFontStruct *font;
static Pixmap pix;
static int W = 900, H = 560, cw, ch, asc;
static int tabbar_h;
static unsigned long c_bg, c_fg, c_bar, c_tab, c_tabact, c_sel, c_cursor, c_prompt, c_dim;
static Atom a_clipboard, a_utf8, a_targets, a_paste, a_wmdel, a_wmproto;

static tab *tabs[MAX_TABS];
static int ntabs, curt;
static int use_process;
static int next_id = 1;
static char *g_clip;           /* shared clipboard: one GUI, every tab */
static tab *paste_target;
static int dirty = 1;

typedef struct { int line, col0, len; } rowmap;
static rowmap rows_drawn[512];
static int nrows_drawn, text_top;

/* ------------------------------------------------------------------ */
/* buffer                                                              */
/* ------------------------------------------------------------------ */

static void cur_put(tab *t, char c)
{
    if (t->curlen + 2 > t->curcap) {
        t->curcap = t->curcap ? t->curcap * 2 : 256;
        t->cur = realloc(t->cur, (size_t)t->curcap);
    }
    t->cur[t->curlen++] = c;
    t->cur[t->curlen] = 0;
}

static void commit_line(tab *t)
{
    if (t->nl == SCROLLBACK + SCROLLBACK / 10) {
        /* trim in batches: one memmove per 1000 lines, not per line */
        int drop = SCROLLBACK / 10;
        for (int i = 0; i < drop; i++) free(t->lines[i]);
        memmove(t->lines, t->lines + drop, sizeof(char *) * (size_t)(t->nl - drop));
        t->nl -= drop;
        t->has_sel = 0;
    }
    if (!t->lines) t->lines = malloc(sizeof(char *) * (SCROLLBACK + SCROLLBACK / 10));
    t->lines[t->nl++] = strndup(t->cur ? t->cur : "", (size_t)t->curlen);
    t->curlen = 0;
    if (t->cur) t->cur[0] = 0;
}

/* Everything shown in a tab goes through here: command output, prompts
 * and the GUI's own messages. Handles \n, \r, \t, \b and strips ANSI
 * escape sequences (commands see a pipe, so few send them anyway). */
static void put_text(tab *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (t->esc == 1) { t->esc = (c == '[') ? 2 : (c == ']') ? 3 : 0; continue; }
        if (t->esc == 2) { if (c >= 0x40 && c <= 0x7e) t->esc = 0; continue; }
        if (t->esc == 3) { if (c == 7) t->esc = 0; else if (c == 27) t->esc = 1; continue; }
        if (c == 27) { t->esc = 1; continue; }
        if (c == '\n') { commit_line(t); t->cr = 0; continue; }
        if (c == '\r') { t->cr = 1; continue; }
        if (t->cr) { t->curlen = 0; if (t->cur) t->cur[0] = 0; t->cr = 0; }
        if (c == '\t') { do cur_put(t, ' '); while (t->curlen % 8); continue; }
        if (c == '\b') { if (t->curlen) t->cur[--t->curlen] = 0; continue; }
        if (c < 32 || c == 127) continue;
        if (c >= 0x80) {
            if (c >= 0xC0) cur_put(t, '?');   /* UTF-8 lead byte: one '?'   */
            continue;                         /* continuation bytes dropped */
        }
        cur_put(t, (char)c);
    }
    dirty = 1;
}

static void put_str(tab *t, const char *s) { put_text(t, s, strlen(s)); }

/* Turn "prompt + typed input" into a committed line (Enter, Ctrl+C...). */
static void echo_input(tab *t, const char *suffix)
{
    put_text(t, t->input, (size_t)t->ilen);
    if (suffix) put_str(t, suffix);
    put_str(t, "\n");
}

static void set_input(tab *t, const char *s)
{
    snprintf(t->input, sizeof t->input, "%s", s);
    t->ilen = (int)strlen(t->input);
    t->icur = t->ilen;
}

static void clear_input(tab *t) { t->ilen = t->icur = 0; t->input[0] = 0; }

static void insert_text(tab *t, const char *s, int n)
{
    if (t->ilen + n >= INPUT_MAX) n = INPUT_MAX - 1 - t->ilen;
    if (n <= 0) return;
    memmove(t->input + t->icur + n, t->input + t->icur, (size_t)(t->ilen - t->icur));
    memcpy(t->input + t->icur, s, (size_t)n);
    t->ilen += n;
    t->icur += n;
    t->input[t->ilen] = 0;
}

static void delete_range(tab *t, int a, int b)
{
    if (a < 0) a = 0;
    if (b > t->ilen) b = t->ilen;
    if (a >= b) return;
    memmove(t->input + a, t->input + b, (size_t)(t->ilen - b));
    t->ilen -= b - a;
    t->input[t->ilen] = 0;
    if (t->icur > b) t->icur -= b - a;
    else if (t->icur > a) t->icur = a;
}

/* ------------------------------------------------------------------ */
/* tabs                                                                */
/* ------------------------------------------------------------------ */

typedef struct { int msg, out, ctl; char cwd[PATH_MAX]; } thread_args;

static void *tab_thread(void *arg)
{
    thread_args *a = arg;
    /* The GUI thread handles nothing signal-related, but block job-control
     * signals here too so a stray SIGTSTP can never stop the window. */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTSTP);
    sigaddset(&s, SIGTTIN);
    sigaddset(&s, SIGTTOU);
    pthread_sigmask(SIG_BLOCK, &s, NULL);
    shell_main_interactive(a->msg, a->out, a->ctl, 1, a->cwd);
    free(a);
    return NULL;
}

static void close_fds_except(int keep1, int keep2, int keep3)
{
    int keep[3] = { keep1, keep2, keep3 };
    for (int i = 0; i < 3; i++)        /* sort */
        for (int j = i + 1; j < 3; j++)
            if (keep[j] < keep[i]) { int x = keep[i]; keep[i] = keep[j]; keep[j] = x; }
    int lo = 3;
    for (int i = 0; i < 3; i++) {
        if (keep[i] > lo) syscall(SYS_close_range, (unsigned)lo, (unsigned)keep[i] - 1, 0);
        lo = keep[i] + 1;
    }
    syscall(SYS_close_range, (unsigned)lo, ~0U, 0);
}

static tab *tab_new(const char *cwd)
{
    if (ntabs == MAX_TABS) return NULL;
    int m[2], o[2], c[2];
    if (pipe2(m, O_CLOEXEC) < 0) return NULL;
    if (pipe2(o, O_CLOEXEC) < 0) { close(m[0]); close(m[1]); return NULL; }
    if (pipe2(c, O_CLOEXEC) < 0) { close(m[0]); close(m[1]); close(o[0]); close(o[1]); return NULL; }

    tab *t = calloc(1, sizeof *t);
    t->id = next_id++;
    t->in_w = m[1];
    t->out_r = o[0];
    t->ctl_r = c[0];
    t->hist_idx = -1;
    snprintf(t->cwd, sizeof t->cwd, "%s", cwd);
    fcntl(t->out_r, F_SETFL, O_NONBLOCK);
    fcntl(t->ctl_r, F_SETFL, O_NONBLOCK);

    if (use_process) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); free(t); return NULL; }
        if (pid == 0) {
            /* The tab process never touches X: drop the connection and
             * every other tab's pipes, keep only our three ends. */
            close_fds_except(m[0], o[1], c[1]);
            shell_main_interactive(m[0], o[1], c[1], 0, cwd);
            _exit(0);
        }
        t->pid = pid;
        close(m[0]); close(o[1]); close(c[1]);
    } else {
        thread_args *a = malloc(sizeof *a);
        a->msg = m[0]; a->out = o[1]; a->ctl = c[1];
        snprintf(a->cwd, sizeof a->cwd, "%s", cwd);
        if (pthread_create(&t->th, NULL, tab_thread, a) != 0) {
            perror("pthread_create");
            free(a); free(t);
            return NULL;
        }
    }
    tabs[ntabs++] = t;
    curt = ntabs - 1;
    dirty = 1;
    return t;
}

static void drain_out(tab *t)
{
    char buf[16384];
    for (;;) {
        ssize_t r = read(t->out_r, buf, sizeof buf);
        if (r > 0) { put_text(t, buf, (size_t)r); continue; }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
}

static void tab_free(int k)
{
    tab *t = tabs[k];
    drain_out(t);
    close(t->in_w);
    close(t->out_r);
    close(t->ctl_r);
    if (use_process) waitpid(t->pid, NULL, 0);
    else pthread_join(t->th, NULL);
    for (int i = 0; i < t->nl; i++) free(t->lines[i]);
    free(t->lines);
    free(t->cur);
    for (int i = 0; i < t->nchoices; i++) free(t->choices[i]);
    free(t->choices);
    msgbuf_free(&t->ctl);
    free(t);
    memmove(tabs + k, tabs + k + 1, sizeof(tab *) * (size_t)(ntabs - k - 1));
    ntabs--;
    if (curt >= ntabs) curt = ntabs - 1;
    if (curt < 0) curt = 0;
    dirty = 1;
}

static void send(tab *t, int type, const char *data, int len)
{
    msg_send(t->in_w, type, data, len);
}

/* ------------------------------------------------------------------ */
/* history browsing and Ctrl+R                                          */
/* ------------------------------------------------------------------ */

static void hist_move(tab *t, int dir)
{
    int n = hist_count();
    if (!n) return;
    if (dir < 0) {
        if (t->hist_idx == -1) {
            snprintf(t->hist_saved, sizeof t->hist_saved, "%s", t->input);
            t->hist_idx = n - 1;
        } else if (t->hist_idx > 0) {
            t->hist_idx--;
        }
    } else {
        if (t->hist_idx == -1) return;
        t->hist_idx++;
        if (t->hist_idx >= n) {
            t->hist_idx = -1;
            set_input(t, t->hist_saved);
            return;
        }
    }
    char e[HIST_LEN];
    if (hist_get(t->hist_idx, e, sizeof e) == 0) set_input(t, e);
}

static int lcs_len(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b), best = 0;
    if (!la || !lb) return 0;
    int *prev = calloc((size_t)lb + 1, sizeof(int)), *row = calloc((size_t)lb + 1, sizeof(int));
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            row[j] = (a[i - 1] == b[j - 1]) ? prev[j - 1] + 1 : 0;
            if (row[j] > best) best = row[j];
        }
        int *x = prev; prev = row; row = x;
    }
    free(prev); free(row);
    return best;
}

static void begin_search(tab *t)
{
    echo_input(t, NULL);
    clear_input(t);
    t->searching = 1;
    put_str(t, "Enter search term: ");
}

static void finish_search(tab *t)
{
    char term[INPUT_MAX];
    snprintf(term, sizeof term, "%s", t->input);
    echo_input(t, NULL);
    clear_input(t);
    t->searching = 0;

    char **v;
    int n = hist_snapshot(&v);
    int exact = -1;
    for (int i = n - 1; i >= 0 && term[0]; i--)
        if (!strcmp(v[i], term)) { exact = i; break; }

    if (exact >= 0) {
        put_str(t, v[exact]);
        put_str(t, "\n");
        set_input(t, v[exact]);
    } else {
        int best = 0;
        int *len = calloc((size_t)n + 1, sizeof(int));
        for (int i = 0; i < n && term[0]; i++) {
            len[i] = lcs_len(term, v[i]);
            if (len[i] > best) best = len[i];
        }
        if (best > 2) {
            /* most recent first, each command once */
            for (int i = n - 1; i >= 0; i--) {
                if (len[i] != best) continue;
                int dup = 0;
                for (int j = i + 1; j < n; j++)
                    if (len[j] == best && !strcmp(v[i], v[j])) { dup = 1; break; }
                if (dup) continue;
                put_str(t, v[i]);
                put_str(t, "\n");
            }
        } else {
            put_str(t, "No match for search term in history\n");
        }
        free(len);
    }
    hist_free_snapshot(v, n);
    put_str(t, t->prompt);
}

static void cancel_search(tab *t)
{
    echo_input(t, "^C");
    clear_input(t);
    t->searching = 0;
    put_str(t, t->prompt);
}

/* ------------------------------------------------------------------ */
/* Tab completion (file names)                                         */
/* ------------------------------------------------------------------ */

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void free_choices(tab *t)
{
    for (int i = 0; i < t->nchoices; i++) free(t->choices[i]);
    free(t->choices);
    t->choices = NULL;
    t->nchoices = 0;
    t->ndigits = 0;
}

/* Replace input[start..icur) with s (+ "/" if it names a directory). */
static void complete_with(tab *t, int start, const char *dir_part, const char *name, const char *dirpath)
{
    char repl[PATH_MAX * 2];
    char full[PATH_MAX * 2];
    snprintf(full, sizeof full, "%s/%s", dirpath, name);
    struct stat st;
    int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    snprintf(repl, sizeof repl, "%s%s%s", dir_part, name, isdir ? "/" : "");
    delete_range(t, start, t->icur);
    t->icur = start;
    insert_text(t, repl, (int)strlen(repl));
}

static void complete(tab *t)
{
    int ws = t->icur;
    while (ws > 0 && t->input[ws - 1] != ' ' && t->input[ws - 1] != '\t' && t->input[ws - 1] != '\n')
        ws--;
    char word[PATH_MAX];
    snprintf(word, sizeof word, "%.*s", t->icur - ws, t->input + ws);

    /* split into the directory part (kept as typed) and the prefix */
    char *slash = strrchr(word, '/');
    char dir_part[PATH_MAX] = "", prefix[PATH_MAX], dirpath[PATH_MAX * 2];
    if (slash) {
        snprintf(dir_part, sizeof dir_part, "%.*s", (int)(slash - word + 1), word);
        snprintf(prefix, sizeof prefix, "%s", slash + 1);
    } else {
        snprintf(prefix, sizeof prefix, "%s", word);
    }
    const char *home = getenv("HOME");
    if (!dir_part[0]) snprintf(dirpath, sizeof dirpath, "%s", t->cwd);
    else if (dir_part[0] == '/') snprintf(dirpath, sizeof dirpath, "%s", dir_part);
    else if (dir_part[0] == '~' && dir_part[1] == '/') snprintf(dirpath, sizeof dirpath, "%s%s", home ? home : "", dir_part + 1);
    else snprintf(dirpath, sizeof dirpath, "%s/%s", t->cwd, dir_part);

    DIR *d = opendir(dirpath);
    if (!d) return;
    char **names = NULL;
    int n = 0, cap = 0;
    size_t pl = strlen(prefix);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.' && prefix[0] != '.') continue;
        if (strncmp(e->d_name, prefix, pl)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = realloc(names, sizeof(char *) * (size_t)cap); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n == 0) { free(names); return; }            /* no match: do nothing */
    qsort(names, (size_t)n, sizeof(char *), cmp_str);

    if (n == 1) {                                    /* one match: complete it */
        complete_with(t, ws, dir_part, names[0], dirpath);
        free(names[0]);
        free(names);
        return;
    }

    /* several: extend to the longest common prefix ... */
    size_t lcp = strlen(names[0]);
    for (int i = 1; i < n; i++) {
        size_t k = 0;
        while (k < lcp && names[i][k] == names[0][k]) k++;
        lcp = k;
    }
    if (lcp > pl) {
        char ext[PATH_MAX];
        snprintf(ext, sizeof ext, "%.*s", (int)(lcp - pl), names[0] + pl);
        insert_text(t, ext, (int)strlen(ext));
    }
    /* ... then show a numbered list and wait for a number */
    put_text(t, t->input, (size_t)t->ilen);
    put_str(t, "\n");
    char item[PATH_MAX + 16];
    for (int i = 0; i < n; i++) {
        snprintf(item, sizeof item, "%s%d. %s", i ? "   " : "", i + 1, names[i]);
        put_str(t, item);
    }
    put_str(t, "\n");
    put_str(t, t->prompt);
    free_choices(t);
    t->choices = names;
    t->nchoices = n;
    t->choice_start = ws;
    snprintf(t->choice_dir, sizeof t->choice_dir, "%s", dirpath);
}

static void choose(tab *t, int k)
{
    if (k >= 1 && k <= t->nchoices) {
        char word[PATH_MAX], dir_part[PATH_MAX] = "";
        snprintf(word, sizeof word, "%.*s", t->icur - t->choice_start, t->input + t->choice_start);
        char *slash = strrchr(word, '/');
        if (slash) snprintf(dir_part, sizeof dir_part, "%.*s", (int)(slash - word + 1), word);
        complete_with(t, t->choice_start, dir_part, t->choices[k - 1], t->choice_dir);
    }
    free_choices(t);
}

/* ------------------------------------------------------------------ */
/* clipboard and selection                                             */
/* ------------------------------------------------------------------ */

static char *line_text(tab *t, int l, int *len)
{
    if (l < t->nl) { *len = (int)strlen(t->lines[l]); return t->lines[l]; }
    *len = t->curlen;
    return t->cur ? t->cur : "";
}

static int pos_lt(pos a, pos b) { return a.line < b.line || (a.line == b.line && a.col < b.col); }

static char *selection_text(tab *t)
{
    pos a = t->sel_a, b = t->sel_b;
    if (pos_lt(b, a)) { pos x = a; a = b; b = x; }
    size_t cap = 256, n = 0;
    char *out = malloc(cap);
    for (int l = a.line; l <= b.line && l <= t->nl; l++) {
        int len;
        char *s = line_text(t, l, &len);
        int from = l == a.line ? a.col : 0;
        int to = l == b.line ? b.col : len;
        if (to > len) to = len;
        if (from > to) from = to;
        while (n + (size_t)(to - from) + 2 > cap) { cap *= 2; out = realloc(out, cap); }
        memcpy(out + n, s + from, (size_t)(to - from));
        n += (size_t)(to - from);
        if (l < b.line) out[n++] = '\n';
    }
    out[n] = 0;
    return out;
}

static void do_copy(tab *t)
{
    char *text = (t->has_sel) ? selection_text(t) : strdup(t->input);
    if (!*text) { free(text); return; }
    free(g_clip);
    g_clip = text;
    XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
}

static void paste_into(tab *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') continue;
        if ((unsigned char)c < 32 && c != '\n' && c != '\t') continue;
        if (c == '\t') c = ' ';
        insert_text(t, &c, 1);
    }
    dirty = 1;
}

static void do_paste(tab *t)
{
    Window owner = XGetSelectionOwner(dpy, a_clipboard);
    if (owner == win || owner == None) {
        if (g_clip) paste_into(t, g_clip, strlen(g_clip));
        return;
    }
    paste_target = t;
    XConvertSelection(dpy, a_clipboard, a_utf8, a_paste, win, CurrentTime);
}

static void on_selection_request(XSelectionRequestEvent *rq)
{
    XSelectionEvent ev = { .type = SelectionNotify, .display = rq->display, .requestor = rq->requestor,
                           .selection = rq->selection, .target = rq->target, .time = rq->time,
                           .property = None };
    if (rq->target == a_targets) {
        Atom list[3] = { a_targets, a_utf8, XA_STRING };
        XChangeProperty(dpy, rq->requestor, rq->property, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)list, 3);
        ev.property = rq->property;
    } else if ((rq->target == a_utf8 || rq->target == XA_STRING) && g_clip) {
        XChangeProperty(dpy, rq->requestor, rq->property, rq->target, 8, PropModeReplace,
                        (unsigned char *)g_clip, (int)strlen(g_clip));
        ev.property = rq->property;
    }
    XSendEvent(dpy, rq->requestor, False, 0, (XEvent *)&ev);
}

static void on_selection_notify(XSelectionEvent *ev)
{
    if (ev->property == None || !paste_target) return;
    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, ev->property, 0, 1 << 20, True, AnyPropertyType,
                           &type, &format, &nitems, &after, &data) == Success && data) {
        for (int k = 0; k < ntabs; k++)
            if (tabs[k] == paste_target) paste_into(paste_target, (char *)data, nitems);
        XFree(data);
    }
    paste_target = NULL;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static unsigned long rgb(const char *hex)
{
    XColor c;
    Colormap cm = DefaultColormap(dpy, scr);
    if (XParseColor(dpy, cm, hex, &c) && XAllocColor(dpy, cm, &c)) return c.pixel;
    return WhitePixel(dpy, scr);
}

static int text_cols(void) { int c = (W - 2 * PAD) / cw; return c < 10 ? 10 : c; }
static int text_rows(void) { int r = (H - tabbar_h - 2 * PAD) / ch; return r < 1 ? 1 : r; }

static void tab_label(tab *t, int idx, char *buf, size_t cap)
{
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (home && hl > 1 && !strncmp(t->cwd, home, hl) && (t->cwd[hl] == '/' || !t->cwd[hl]))
        snprintf(buf, cap, "%d: ~%s", idx + 1, t->cwd + hl);
    else
        snprintf(buf, cap, "%d: %s", idx + 1, t->cwd);
}

static int tab_w(void)
{
    int w = (W - 40) / (ntabs ? ntabs : 1);
    return w > 220 ? 220 : w;
}

static void draw_tabbar(void)
{
    XSetForeground(dpy, gc, c_bar);
    XFillRectangle(dpy, pix, gc, 0, 0, (unsigned)W, (unsigned)tabbar_h);
    int w = tab_w();
    for (int i = 0; i < ntabs; i++) {
        int x = i * w;
        XSetForeground(dpy, gc, i == curt ? c_tabact : c_tab);
        XFillRectangle(dpy, pix, gc, x + 2, 3, (unsigned)(w - 4), (unsigned)(tabbar_h - 3));
        char label[PATH_MAX + 16];
        tab_label(tabs[i], i, label, sizeof label);
        int maxc = (w - 12) / cw;
        int len = (int)strlen(label);
        const char *s = label;
        if (len > maxc && maxc > 3) { s = label + len - maxc; len = maxc; }
        XSetForeground(dpy, gc, i == curt ? c_fg : c_dim);
        XDrawString(dpy, pix, gc, x + 8, 3 + (tabbar_h - 3 - ch) / 2 + asc, s, len);
    }
    int px = ntabs * w;
    XSetForeground(dpy, gc, c_tab);
    XFillRectangle(dpy, pix, gc, px + 2, 3, 30, (unsigned)(tabbar_h - 3));
    XSetForeground(dpy, gc, c_fg);
    XDrawString(dpy, pix, gc, px + 2 + (30 - cw) / 2, 3 + (tabbar_h - 3 - ch) / 2 + asc, "+", 1);
}

static int rows_for(int len, int cols) { return len <= 0 ? 1 : (len + cols - 1) / cols; }

static int in_sel(tab *t, int line, int col)
{
    if (!t->has_sel) return 0;
    pos a = t->sel_a, b = t->sel_b, p = { line, col };
    if (pos_lt(b, a)) { pos x = a; a = b; b = x; }
    return !pos_lt(p, a) && pos_lt(p, b);
}

static void render(void)
{
    XSetForeground(dpy, gc, c_bg);
    XFillRectangle(dpy, pix, gc, 0, 0, (unsigned)W, (unsigned)H);
    draw_tabbar();
    nrows_drawn = 0;
    if (!ntabs) { XCopyArea(dpy, pix, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0); return; }

    tab *t = tabs[curt];
    int cols = text_cols(), rows = text_rows();
    text_top = tabbar_h + PAD;

    /* the last logical line = unfinished output (usually the prompt) + input */
    int lastlen = t->curlen + t->ilen;
    char *last = malloc((size_t)lastlen + 1);
    if (t->curlen) memcpy(last, t->cur, (size_t)t->curlen);
    for (int i = 0; i < t->ilen; i++)
        last[t->curlen + i] = t->input[i] == '\n' ? NEWLINE_GLYPH : t->input[i];
    last[lastlen] = 0;
    int cursor = t->curlen + t->icur;
    int last_rows = rows_for(lastlen > cursor ? lastlen : cursor + 1, cols);

    int total = last_rows;
    for (int i = 0; i < t->nl; i++) total += rows_for((int)strlen(t->lines[i]), cols);
    int maxscroll = total > rows ? total - rows : 0;
    if (t->scroll > maxscroll) t->scroll = maxscroll;
    int first = total - rows - t->scroll;
    if (first < 0) first = 0;

    int vr = 0, y = text_top;
    for (int l = 0; l <= t->nl && nrows_drawn < rows; l++) {
        const char *s = l < t->nl ? t->lines[l] : last;
        int len = l < t->nl ? (int)strlen(s) : lastlen;
        int nr = l < t->nl ? rows_for(len, cols) : last_rows;
        if (vr + nr <= first) { vr += nr; continue; }
        for (int r = 0; r < nr && nrows_drawn < rows; r++, vr++) {
            if (vr < first) continue;
            int col0 = r * cols;
            int n = len - col0;
            if (n > cols) n = cols;
            if (n < 0) n = 0;
            if (t->has_sel) {
                XSetForeground(dpy, gc, c_sel);
                for (int k = 0; k < cols; k++)
                    if (in_sel(t, l, col0 + k) && col0 + k < len)
                        XFillRectangle(dpy, pix, gc, PAD + k * cw, y, (unsigned)cw, (unsigned)ch);
            }
            if (n > 0) {
                /* prompt in its own colour */
                int pl = 0;
                size_t mpl = strlen(t->main_prompt);
                if (mpl && !strncmp(s, t->main_prompt, mpl)) pl = (int)mpl - col0;
                else if (l == t->nl && t->prompt[0] && !strncmp(s, t->prompt, strlen(t->prompt)))
                    pl = (int)strlen(t->prompt) - col0;
                if (pl > n) pl = n;
                if (pl > 0) {
                    XSetForeground(dpy, gc, c_prompt);
                    XDrawString(dpy, pix, gc, PAD, y + asc, s + col0, pl);
                } else pl = 0;
                XSetForeground(dpy, gc, c_fg);
                XDrawString(dpy, pix, gc, PAD + pl * cw, y + asc, s + col0 + pl, n - pl);
            }
            if (l == t->nl && cursor >= col0 && cursor < col0 + cols && t->scroll == 0) {
                int cx = PAD + (cursor - col0) * cw;
                XSetForeground(dpy, gc, t->nchoices ? c_prompt : c_cursor);
                XFillRectangle(dpy, pix, gc, cx, y, (unsigned)cw, (unsigned)ch);
                if (cursor < lastlen) {
                    XSetForeground(dpy, gc, c_bg);
                    XDrawString(dpy, pix, gc, cx, y + asc, last + cursor, 1);
                }
            }
            rows_drawn[nrows_drawn].line = l;
            rows_drawn[nrows_drawn].col0 = col0;
            rows_drawn[nrows_drawn].len = n;
            nrows_drawn++;
            y += ch;
        }
    }
    free(last);
    XCopyArea(dpy, pix, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
    dirty = 0;
}

/* ------------------------------------------------------------------ */
/* input handling                                                      */
/* ------------------------------------------------------------------ */

static void switch_tab(int k)
{
    if (k < 0 || k >= ntabs) return;
    curt = k;
    dirty = 1;
}

static void new_tab(void)
{
    const char *cwd = ntabs ? tabs[curt]->cwd : getenv("HOME");
    if (!tab_new(cwd ? cwd : "/") && ntabs) put_str(tabs[curt], "terminator: cannot open more tabs\n");
}

static void submit(tab *t)
{
    if (t->searching) { finish_search(t); return; }
    echo_input(t, NULL);
    send(t, M_LINE, t->input, t->ilen);
    clear_input(t);
    t->hist_idx = -1;
}

static void do_action(tab *t, int a)
{
    switch (a) {
    case A_LEFT:  if (t->icur > 0) t->icur--; break;
    case A_RIGHT: if (t->icur < t->ilen) t->icur++; break;
    case A_HOME:  t->icur = 0; break;
    case A_END:   t->icur = t->ilen; break;
    case A_HIST_PREV: if (!t->searching) hist_move(t, -1); break;
    case A_HIST_NEXT: if (!t->searching) hist_move(t, 1); break;
    case A_KILL_START: delete_range(t, 0, t->icur); break;
    case A_KILL_END:   delete_range(t, t->icur, t->ilen); break;
    case A_KILL_WORD: {
        int k = t->icur;
        while (k > 0 && t->input[k - 1] == ' ') k--;
        while (k > 0 && t->input[k - 1] != ' ') k--;
        delete_range(t, k, t->icur);
        break;
    }
    case A_BACKSPACE: if (t->icur > 0) delete_range(t, t->icur - 1, t->icur); break;
    case A_DELETE:    delete_range(t, t->icur, t->icur + 1); break;
    case A_CLEAR:
        for (int i = 0; i < t->nl; i++) free(t->lines[i]);
        t->nl = 0;
        t->scroll = 0;
        t->has_sel = 0;
        break;
    case A_EOF:
        if (t->ilen) delete_range(t, t->icur, t->icur + 1);
        else if (!t->searching) send(t, M_EOF, "", 0);
        break;
    case A_INTERRUPT:
        if (t->searching) { cancel_search(t); break; }
        echo_input(t, "^C");
        clear_input(t);
        t->hist_idx = -1;
        send(t, M_INT, "", 0);
        break;
    case A_SUSPEND:
        if (t->searching) break;
        echo_input(t, "^Z");
        clear_input(t);
        send(t, M_TSTP, "", 0);
        break;
    case A_SEARCH: if (!t->searching) begin_search(t); break;
    case A_COMPLETE: if (!t->searching) complete(t); break;
    case A_COPY:  do_copy(t); break;
    case A_PASTE: do_paste(t); break;
    case A_NEW_TAB: new_tab(); break;
    case A_CLOSE_TAB: send(t, M_CLOSE, "", 0); t->closing = 1; break;
    case A_NEXT_TAB: switch_tab((curt + 1) % ntabs); break;
    case A_PREV_TAB: switch_tab((curt + ntabs - 1) % ntabs); break;
    case A_SCROLL_UP:   t->scroll += text_rows() / 2; break;
    case A_SCROLL_DOWN: t->scroll -= text_rows() / 2; if (t->scroll < 0) t->scroll = 0; break;
    case A_ENTER: submit(t); break;
    }
    dirty = 1;
}

static void on_key(XKeyEvent *ev)
{
    if (!ntabs) return;
    tab *t = tabs[curt];
    char text[32];
    KeySym sym;
    int n = XLookupString(ev, text, sizeof text - 1, &sym, NULL);
    text[n > 0 ? n : 0] = 0;
    KeySym base = XLookupKeysym(ev, 0);
    unsigned mods = ev->state & (ControlMask | ShiftMask | Mod1Mask);

    if (base == XK_Shift_L || base == XK_Shift_R || base == XK_Control_L || base == XK_Control_R ||
        base == XK_Alt_L || base == XK_Alt_R || base == XK_Super_L || base == XK_Caps_Lock)
        return;

    /* Completion menu: a number picks, anything else closes the menu. */
    if (t->nchoices) {
        if (n == 1 && isdigit((unsigned char)text[0]) && !(mods & ControlMask)) {
            t->digits[t->ndigits++] = text[0];
            t->digits[t->ndigits] = 0;
            int k = atoi(t->digits);
            if (t->nchoices <= 9 || k * 10 > t->nchoices || t->ndigits >= 6) choose(t, k);
            dirty = 1;
            return;
        }
        if ((sym == XK_Return || sym == XK_KP_Enter) && t->ndigits) {
            choose(t, atoi(t->digits));
            dirty = 1;
            return;
        }
        free_choices(t);
        dirty = 1;
    }

    if (t->searching && sym == XK_Escape) { cancel_search(t); dirty = 1; return; }

    t->has_sel = 0;
    int a = keys_lookup(mods, base);
    if (a == A_NONE && base != sym) a = keys_lookup(mods & ~ShiftMask, sym);
    if (a != A_NONE) {
        if (a != A_SCROLL_UP && a != A_SCROLL_DOWN) t->scroll = 0;
        do_action(t, a);
        return;
    }
    if (n > 0 && !(mods & (ControlMask | Mod1Mask))) {
        for (int i = 0; i < n; i++)
            if ((unsigned char)text[i] >= 32 && text[i] != 127) insert_text(t, &text[i], 1);
        t->scroll = 0;
        dirty = 1;
    }
}

static pos pos_at(int x, int y)
{
    tab *t = tabs[curt];
    pos p = { t->nl, 0 };
    if (!nrows_drawn) return p;
    int r = (y - text_top) / ch;
    if (y < text_top) r = 0;
    if (r >= nrows_drawn) {
        rowmap *m = &rows_drawn[nrows_drawn - 1];
        p.line = m->line; p.col = m->col0 + m->len;
        return p;
    }
    rowmap *m = &rows_drawn[r];
    int c = (x - PAD + cw / 2) / cw;
    if (c < 0) c = 0;
    if (c > m->len) c = m->len;
    p.line = m->line;
    p.col = m->col0 + c;
    return p;
}

static void on_button(XButtonEvent *ev, int press)
{
    if (!ntabs) return;
    tab *t = tabs[curt];
    if (press && ev->y < tabbar_h) {
        int w = tab_w();
        int k = ev->x / w;
        if (k < ntabs) switch_tab(k);
        else if (ev->x >= ntabs * w && ev->x < ntabs * w + 34) new_tab();
        return;
    }
    if (press && ev->button == Button4) { t->scroll += 3; dirty = 1; return; }
    if (press && ev->button == Button5) { t->scroll -= 3; if (t->scroll < 0) t->scroll = 0; dirty = 1; return; }
    if (ev->button == Button2 && press) { do_paste(t); return; }
    if (ev->button != Button1) return;
    if (press) {
        t->sel_a = t->sel_b = pos_at(ev->x, ev->y);
        t->selecting = 1;
        t->has_sel = 0;
    } else if (t->selecting) {
        t->sel_b = pos_at(ev->x, ev->y);
        t->selecting = 0;
        t->has_sel = pos_lt(t->sel_a, t->sel_b) || pos_lt(t->sel_b, t->sel_a);
    }
    dirty = 1;
}

/* ------------------------------------------------------------------ */
/* messages from the shells                                            */
/* ------------------------------------------------------------------ */

static void on_ctl(tab *t, int type, char *data)
{
    char msg[8192];
    switch (type) {
    case C_PROMPT:
        snprintf(t->prompt, sizeof t->prompt, "%s", data);
        if (strcmp(data, "> ") != 0) snprintf(t->main_prompt, sizeof t->main_prompt, "%s", data);
        /* a prompt always starts on a fresh line */
        if (t->curlen) put_str(t, "\n");
        put_str(t, data);
        break;
    case C_CWD:
        snprintf(t->cwd, sizeof t->cwd, "%s", data);
        dirty = 1;
        break;
    case C_EXIT:
        t->closing = 2;
        break;
    case C_BIND: {
        char act[128] = "", key[128] = "";
        sscanf(data, "%127s %127s", act, key);
        keys_bind(act, key, 1, msg, sizeof msg);
        put_str(t, msg);
        put_str(t, "\n");
        break;
    }
    case C_BINDLIST:
        keys_describe(msg, sizeof msg);
        put_str(t, msg);
        break;
    case C_CLEAR:
        for (int i = 0; i < t->nl; i++) free(t->lines[i]);
        t->nl = 0;
        t->curlen = 0;
        if (t->cur) t->cur[0] = 0;
        t->scroll = 0;
        t->has_sel = 0;
        dirty = 1;
        break;
    }
}

static void service_tab(tab *t, short out_ev, short ctl_ev)
{
    /* Output first: anything a command printed was written before the
     * shell sent the next prompt, so this keeps them in order. */
    if (out_ev) drain_out(t);
    if (!ctl_ev) return;
    for (;;) {
        int r = msgbuf_read(&t->ctl, t->ctl_r);
        if (r == 0) { t->closing = 2; break; }   /* engine gone (or crashed) */
        if (r < 0) break;
    }
    int type, len;
    char *data;
    drain_out(t);
    while (msgbuf_next(&t->ctl, &type, &data, &len)) {
        on_ctl(t, type, data);
        free(data);
    }
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */
/* ------------------------------------------------------------------ */

static void make_pixmap(void)
{
    if (pix) XFreePixmap(dpy, pix);
    pix = XCreatePixmap(dpy, win, (unsigned)W, (unsigned)H, (unsigned)DefaultDepth(dpy, scr));
}

static XFontStruct *load_font(void)
{
    const char *names[] = {
        "-*-dejavu sans mono-medium-r-normal--15-*-*-*-m-*-iso8859-1",
        "-misc-fixed-medium-r-normal--15-*-*-*-*-*-iso8859-1",
        "9x15", "8x13", "fixed", NULL
    };
    for (int i = 0; names[i]; i++) {
        XFontStruct *f = XLoadQueryFont(dpy, names[i]);
        if (f) return f;
    }
    return NULL;
}

int gui_run(int process_mode)
{
    use_process = process_mode;
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "terminator: cannot open X display (is DISPLAY set?)\n");
        return 1;
    }
    fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);
    scr = DefaultScreen(dpy);
    font = load_font();
    if (!font) { fprintf(stderr, "terminator: no usable font\n"); return 1; }
    cw = font->max_bounds.width;
    asc = font->ascent;
    ch = font->ascent + font->descent + 2;
    tabbar_h = ch + 10;
    W = 100 * cw + 2 * PAD;
    H = 30 * ch + tabbar_h + 2 * PAD;

    c_bg = rgb("#1d1f21"); c_fg = rgb("#d0d2d0"); c_bar = rgb("#111214");
    c_tab = rgb("#25282c"); c_tabact = rgb("#3a3f46"); c_sel = rgb("#3b4f6b");
    c_cursor = rgb("#d0d2d0"); c_prompt = rgb("#8ccf7e"); c_dim = rgb("#8a8f96");

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)W, (unsigned)H, 0, c_fg, c_bg);
    XStoreName(dpy, win, use_process ? "Terminator (process mode)" : "Terminator");
    XClassHint *hint = XAllocClassHint();
    hint->res_name = "terminator";
    hint->res_class = "Terminator";
    XSetClassHint(dpy, win, hint);
    XFree(hint);
    XSelectInput(dpy, win, KeyPressMask | ExposureMask | StructureNotifyMask | ButtonPressMask |
                           ButtonReleaseMask | Button1MotionMask);
    a_wmproto = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_wmdel = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wmdel, 1);
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_paste = XInternAtom(dpy, "TERMINATOR_PASTE", False);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFont(dpy, gc, font->fid);
    make_pixmap();
    XMapWindow(dpy, win);

    char warnings[4096];
    keys_init(warnings, sizeof warnings);

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "%s", getenv("HOME") ? getenv("HOME") : "/");
    tab *first = tab_new(cwd);
    if (!first) { fprintf(stderr, "terminator: cannot start a tab\n"); return 1; }
    if (warnings[0]) put_str(first, warnings);

    int xfd = ConnectionNumber(dpy);
    int running = 1;
    while (running) {
        /* 1. every queued X event (Xlib keeps its own queue: drain it) */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose: if (ev.xexpose.count == 0) dirty = 1; break;
            case ConfigureNotify:
                if (ev.xconfigure.width != W || ev.xconfigure.height != H) {
                    W = ev.xconfigure.width;
                    H = ev.xconfigure.height;
                    make_pixmap();
                    dirty = 1;
                }
                break;
            case KeyPress: on_key(&ev.xkey); break;
            case ButtonPress: on_button(&ev.xbutton, 1); break;
            case ButtonRelease: on_button(&ev.xbutton, 0); break;
            case MotionNotify:
                if (ntabs && tabs[curt]->selecting) {
                    tabs[curt]->sel_b = pos_at(ev.xmotion.x, ev.xmotion.y);
                    tabs[curt]->has_sel = 1;
                    dirty = 1;
                }
                break;
            case SelectionRequest: on_selection_request(&ev.xselectionrequest); break;
            case SelectionNotify: on_selection_notify(&ev.xselection); break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == a_wmdel) {
                    for (int k = 0; k < ntabs; k++) { send(tabs[k], M_CLOSE, "", 0); tabs[k]->closing = 1; }
                }
                break;
            }
        }

        /* 2. close tabs whose shell has finished */
        for (int k = 0; k < ntabs; k++)
            if (tabs[k]->closing == 2) { tab_free(k); k--; }
        if (!ntabs) break;

        /* 3. redraw only when something changed */
        if (dirty) render();
        XFlush(dpy);

        /* 4. sleep until X or any tab has something for us */
        struct pollfd pf[1 + 2 * MAX_TABS];
        int np = 0;
        pf[np].fd = xfd; pf[np].events = POLLIN; np++;
        for (int k = 0; k < ntabs; k++) {
            pf[np].fd = tabs[k]->out_r; pf[np].events = POLLIN; np++;
            pf[np].fd = tabs[k]->ctl_r; pf[np].events = POLLIN; np++;
        }
        if (poll(pf, (nfds_t)np, -1) < 0 && errno != EINTR) break;
        int nt = ntabs;
        for (int k = 0; k < nt; k++)
            service_tab(tabs[k], pf[1 + 2 * k].revents, pf[2 + 2 * k].revents);
    }

    free(g_clip);
    XFreePixmap(dpy, pix);
    XFreeGC(dpy, gc);
    XFreeFont(dpy, font);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}

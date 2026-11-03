/* shell.c - Terminator's shell engine: expansion, execution, builtins,
 * job control, scripts and multiWatch.
 *
 * Key rules this file follows (see DESIGN.md):
 *  - fds are rewired with dup2() only in children, never in the engine.
 *  - In thread mode several engines share one process, so the engine never
 *    calls chdir(); each tab keeps its own cwd string and every child calls
 *    chdir(cwd) before exec.
 *  - Every pipe is created O_CLOEXEC so a fork() in one tab cannot hold
 *    another tab's pipe open after exec.
 *  - The engine waits only for its own pids (never waitpid(-1)), so tabs
 *    cannot steal each other's children.
 */
#define _GNU_SOURCE
#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "history.h"
#include "parse.h"
#include "proto.h"

extern char **environ;

/* ------------------------------------------------------------------ */
/* data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct { char *name, *val; int exp; } var;

enum { JOB_RUNNING, JOB_STOPPED };

typedef struct job {
    int id;
    pid_t pgid;
    int n;
    pid_t *pids;
    int *done, *stopped;
    int status;          /* exit status of the last process */
    int state;
    int bg;
    int jin;             /* write end of the job's stdin pipe, or -1 */
    char *text;
} job;

typedef struct shell {
    int interactive;     /* attached to a GUI tab: job control on */
    int is_thread;       /* shares the process with other tabs     */
    int msg_fd, ctl_fd;
    msgbuf mr;
    int in_fd, out_fd, err_fd, devnull;
    char cwd[PATH_MAX], oldpwd[PATH_MAX];
    var *vars; int nvars, capvars;
    char *argv0;
    char **pos; int npos;
    int last_status;
    job **jobs; int njobs, capjobs;
    char *pending; size_t plen, pcap;
    int exiting, exit_code;
    pid_t shell_pid;     /* $$: the shell's pid, even inside children */
} shell;

typedef struct { char *s; size_t n, cap; } str;
typedef struct { char **v; int n, cap; } vec;

static void str_addn(str *s, const char *p, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap : 64;
        while (nc < s->n + n + 1) nc *= 2;
        s->s = realloc(s->s, nc);
        s->cap = nc;
    }
    memcpy(s->s + s->n, p, n);
    s->n += n;
    s->s[s->n] = 0;
}
static void str_addc(str *s, char c) { str_addn(s, &c, 1); }
static void str_adds(str *s, const char *p) { str_addn(s, p, strlen(p)); }
static char *str_take(str *s) { char *r = s->s ? s->s : strdup(""); s->s = NULL; s->n = s->cap = 0; return r; }

static void vec_push(vec *v, const char *s)
{
    if (v->n + 2 > v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->v = realloc(v->v, sizeof(char *) * (size_t)v->cap);
    }
    v->v[v->n++] = strdup(s);
    v->v[v->n] = NULL;
}
static void vec_free(vec *v)
{
    for (int i = 0; i < v->n; i++) free(v->v[i]);
    free(v->v);
    v->v = NULL; v->n = v->cap = 0;
}

/* ------------------------------------------------------------------ */
/* output helpers                                                      */
/* ------------------------------------------------------------------ */

static void outf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(int fd, const char *fmt, ...)
{
    char small[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof small) { write_all(fd, small, (size_t)n); return; }
    char *big = malloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    write_all(fd, big, (size_t)n);
    free(big);
}

#define ERR(sh, ...) outf((sh)->err_fd, "terminator: " __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* variables                                                           */
/* ------------------------------------------------------------------ */

static var *var_find(shell *sh, const char *name)
{
    for (int i = 0; i < sh->nvars; i++)
        if (!strcmp(sh->vars[i].name, name)) return &sh->vars[i];
    return NULL;
}

static const char *var_get(shell *sh, const char *name)
{
    var *v = var_find(sh, name);
    return v ? v->val : NULL;
}

/* exp: 1 export, 0 keep current flag */
static void var_set(shell *sh, const char *name, const char *val, int exp)
{
    var *v = var_find(sh, name);
    if (v) {
        free(v->val);
        v->val = strdup(val);
        if (exp) v->exp = 1;
        return;
    }
    if (sh->nvars == sh->capvars) {
        sh->capvars = sh->capvars ? sh->capvars * 2 : 64;
        sh->vars = realloc(sh->vars, sizeof(var) * (size_t)sh->capvars);
    }
    sh->vars[sh->nvars].name = strdup(name);
    sh->vars[sh->nvars].val = strdup(val);
    sh->vars[sh->nvars].exp = exp;
    sh->nvars++;
}

static void var_unset(shell *sh, const char *name)
{
    var *v = var_find(sh, name);
    if (!v) return;
    free(v->name);
    free(v->val);
    *v = sh->vars[--sh->nvars];
}

static char **build_env(shell *sh)
{
    char **env = malloc(sizeof(char *) * (size_t)(sh->nvars + 1));
    int n = 0;
    for (int i = 0; i < sh->nvars; i++) {
        if (!sh->vars[i].exp) continue;
        size_t l = strlen(sh->vars[i].name) + strlen(sh->vars[i].val) + 2;
        env[n] = malloc(l);
        snprintf(env[n], l, "%s=%s", sh->vars[i].name, sh->vars[i].val);
        n++;
    }
    env[n] = NULL;
    return env;
}

/* ------------------------------------------------------------------ */
/* paths and fds                                                       */
/* ------------------------------------------------------------------ */

/* Resolve a path against the tab's own cwd (thread mode has no usable
 * process cwd). */
static void make_abs(shell *sh, const char *path, char *out, size_t cap)
{
    if (path[0] == '/') snprintf(out, cap, "%s", path);
    else if (!strcmp(sh->cwd, "/")) snprintf(out, cap, "/%s", path);
    else snprintf(out, cap, "%s/%s", sh->cwd, path);
}

static const char *home_dir(shell *sh)
{
    const char *h = var_get(sh, "HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/";
}

/* Make in/out/err the child's 0/1/2. Goes through fds >= 10 first so
 * the three sources can never clobber one another. */
static void setup_std(int in, int out, int err)
{
    int a = fcntl(in, F_DUPFD_CLOEXEC, 10);
    int b = fcntl(out, F_DUPFD_CLOEXEC, 10);
    int c = fcntl(err, F_DUPFD_CLOEXEC, 10);
    if (a >= 0) { dup2(a, 0); close(a); }
    if (b >= 0) { dup2(b, 1); close(b); }
    if (c >= 0) { dup2(c, 2); close(c); }
}

static void close_from(int lowfd)
{
#ifdef SYS_close_range
    if (syscall(SYS_close_range, (unsigned)lowfd, ~0U, 0) == 0) return;
#endif
    for (int fd = lowfd; fd < 4096; fd++) close(fd);
}

static void reset_signals(void)
{
    int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGCHLD, SIGHUP, SIGTERM };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) signal(sigs[i], SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

/* Called in a forked child that keeps running engine code instead of
 * exec'ing (subshells, $(...), builtins inside pipelines, scripts).
 * After setup_std(), 0/1/2 are this child's stdin/stdout/stderr. */
static void become_subshell(shell *sh)
{
    sh->in_fd = 0; sh->out_fd = 1; sh->err_fd = 2;
    sh->interactive = 0;
    sh->msg_fd = sh->ctl_fd = -1;
    if (sh->is_thread) {
        if (chdir(sh->cwd) < 0) { /* reported by the command itself */ }
        sh->is_thread = 0;
    }
    /* Forget the parent's jobs: they are not our children. */
    sh->njobs = 0;
    /* Drop every inherited fd: other tabs' pipes, the X connection,
     * a job's stdin write end... Otherwise readers would never see EOF. */
    close_from(3);
    sh->devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
}

/* ------------------------------------------------------------------ */
/* forward declarations                                                */
/* ------------------------------------------------------------------ */

static int  run_string(shell *sh, const char *text);
static void exec_list(shell *sh, list *l);
static int  exec_if(shell *sh, ifnode *f);
static int  run_builtin(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err);
static int  find_builtin(const char *name);

/* ------------------------------------------------------------------ */
/* expansion                                                           */
/* ------------------------------------------------------------------ */

static char *cmd_subst(shell *sh, const char *inner)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return strdup("");
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return strdup(""); }
    if (pid == 0) {
        reset_signals();
        setup_std(sh->in_fd, p[1], sh->err_fd);
        become_subshell(sh);
        run_string(sh, inner);
        _exit(sh->exiting ? sh->exit_code : sh->last_status);
    }
    close(p[1]);
    str s = {0};
    char buf[4096];
    for (;;) {
        ssize_t r = read(p[0], buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        str_addn(&s, buf, (size_t)r);
    }
    close(p[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    char *r = str_take(&s);
    size_t l = strlen(r);
    while (l && r[l - 1] == '\n') r[--l] = 0;
    return r;
}

/* s[*pi] == '$'. Appends the expansion to out and moves *pi past it. */
static void expand_dollar(shell *sh, const char *s, int *pi, str *out)
{
    int i = *pi + 1;
    char c = s[i];
    char tmp[64];
    if (c == '(') {
        int j = scan_subst(s, i + 1);
        if (j < 0) j = (int)strlen(s) - 1;
        char *inner = strndup(s + i + 1, (size_t)(j - i - 1));
        char *res = cmd_subst(sh, inner);
        str_adds(out, res);
        free(res);
        free(inner);
        *pi = j + 1;
        return;
    }
    if (c == '{') {
        int j = i + 1;
        while (s[j] && s[j] != '}') j++;
        char *name = strndup(s + i + 1, (size_t)(j - i - 1));
        const char *v = NULL;
        if (!strcmp(name, "?")) { snprintf(tmp, sizeof tmp, "%d", sh->last_status); v = tmp; }
        else if (!strcmp(name, "#")) { snprintf(tmp, sizeof tmp, "%d", sh->npos); v = tmp; }
        else if (isdigit((unsigned char)name[0])) {
            int k = atoi(name);
            v = k == 0 ? sh->argv0 : (k <= sh->npos ? sh->pos[k - 1] : "");
        } else v = var_get(sh, name);
        if (v) str_adds(out, v);
        free(name);
        *pi = s[j] ? j + 1 : j;
        return;
    }
    if (c == '?') { snprintf(tmp, sizeof tmp, "%d", sh->last_status); str_adds(out, tmp); *pi = i + 1; return; }
    if (c == '#') { snprintf(tmp, sizeof tmp, "%d", sh->npos); str_adds(out, tmp); *pi = i + 1; return; }
    if (c == '$') { snprintf(tmp, sizeof tmp, "%d", (int)sh->shell_pid); str_adds(out, tmp); *pi = i + 1; return; }
    if (c == '@' || c == '*') {
        for (int k = 0; k < sh->npos; k++) {
            if (k) str_addc(out, ' ');
            str_adds(out, sh->pos[k]);
        }
        *pi = i + 1;
        return;
    }
    if (isdigit((unsigned char)c)) {
        int k = c - '0';
        const char *v = k == 0 ? sh->argv0 : (k <= sh->npos ? sh->pos[k - 1] : "");
        str_adds(out, v ? v : "");
        *pi = i + 1;
        return;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        int j = i;
        while (isalnum((unsigned char)s[j]) || s[j] == '_') j++;
        char *name = strndup(s + i, (size_t)(j - i));
        const char *v = var_get(sh, name);
        if (v) str_adds(out, v);
        free(name);
        *pi = j;
        return;
    }
    str_addc(out, '$');
    *pi = i;
}

/* Pathname expansion relative to the tab's cwd. */
static void push_field(shell *sh, vec *out, const char *f, int globbing)
{
    if (globbing) {
        char pat[PATH_MAX * 2];
        size_t strip = 0;
        if (f[0] == '/') snprintf(pat, sizeof pat, "%s", f);
        else {
            snprintf(pat, sizeof pat, "%s/%s", sh->cwd, f);
            strip = strlen(sh->cwd) + 1;
        }
        glob_t g;
        if (glob(pat, 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++)
                vec_push(out, g.gl_pathv[k] + strip);
            globfree(&g);
            return;
        }
        globfree(&g);
    }
    vec_push(out, f);
}

/* Expand one raw word. split=1: field splitting + globbing (command
 * arguments). split=0: one field (assignments, redirection targets). */
static void expand_word(shell *sh, const char *w, vec *out, int split)
{
    str cur = {0};
    int quoted = 0, globbing = 0, i = 0;

    if (w[0] == '~' && (w[1] == '/' || w[1] == 0)) {
        str_adds(&cur, home_dir(sh));
        i = 1;
    }
    while (w[i]) {
        char c = w[i];
        if (c == '\\') {
            if (w[i + 1]) { str_addc(&cur, w[i + 1]); i += 2; }
            else i++;
            quoted = 1;
        } else if (c == '\'') {
            i++;
            while (w[i] && w[i] != '\'') str_addc(&cur, w[i++]);
            if (w[i]) i++;
            quoted = 1;
        } else if (c == '"') {
            i++;
            quoted = 1;
            while (w[i] && w[i] != '"') {
                if (w[i] == '\\' && w[i + 1] && strchr("$`\"\\\n", w[i + 1])) {
                    str_addc(&cur, w[i + 1]);
                    i += 2;
                } else if (w[i] == '$') {
                    expand_dollar(sh, w, &i, &cur);
                } else {
                    str_addc(&cur, w[i++]);
                }
            }
            if (w[i]) i++;
        } else if (c == '$') {
            str t = {0};
            expand_dollar(sh, w, &i, &t);
            if (!split) {
                if (t.n) str_addn(&cur, t.s, t.n);
            } else {
                for (size_t k = 0; k < t.n; k++) {
                    if (t.s[k] == ' ' || t.s[k] == '\t' || t.s[k] == '\n') {
                        if (cur.n || quoted) {
                            push_field(sh, out, cur.s ? cur.s : "", globbing);
                            cur.n = 0;
                            if (cur.s) cur.s[0] = 0;
                            quoted = globbing = 0;
                        }
                    } else {
                        str_addc(&cur, t.s[k]);
                    }
                }
            }
            free(t.s);
        } else {
            if (split && (c == '*' || c == '?' || c == '[')) globbing = 1;
            str_addc(&cur, c);
            i++;
        }
    }
    if (cur.n || quoted || !split)
        push_field(sh, out, cur.s ? cur.s : "", globbing);
    free(cur.s);
}

static char *expand_one(shell *sh, const char *w)
{
    vec v = {0};
    expand_word(sh, w, &v, 0);
    char *r = strdup(v.n ? v.v[0] : "");
    vec_free(&v);
    return r;
}

static void expand_words(shell *sh, cmd *c, vec *argv)
{
    for (int i = 0; i < c->nw; i++) expand_word(sh, c->words[i], argv, 1);
}

/* ------------------------------------------------------------------ */
/* redirection                                                         */
/* ------------------------------------------------------------------ */

static int open_flags(int type)
{
    if (type == R_IN) return O_RDONLY;
    if (type == R_APPEND) return O_WRONLY | O_CREAT | O_APPEND;
    return O_WRONLY | O_CREAT | O_TRUNC;
}

/* In a child, between fork and exec: rewire the real fds. */
static int apply_redirs_child(shell *sh, cmd *c)
{
    for (redir *r = c->redirs; r; r = r->next) {
        if (r->type == R_DUP) {
            if (dup2(r->dupfd, r->fd) < 0) { ERR(sh, "%d: %s\n", r->dupfd, strerror(errno)); return -1; }
            continue;
        }
        char *t = expand_one(sh, r->target);
        int fd = open(t, open_flags(r->type), 0644);
        if (fd < 0) {
            ERR(sh, "%s: %s\n", t, strerror(errno));
            free(t);
            return -1;
        }
        free(t);
        if (fd != r->fd) {
            dup2(fd, r->fd);
            close(fd);
        }
    }
    return 0;
}

/* For builtins run inside the engine: open files but do not touch the
 * engine's fds. slot[0..2] receive the fds the builtin should use. */
static int apply_redirs_inproc(shell *sh, cmd *c, int slot[3], int opened[], int *nopened)
{
    for (redir *r = c->redirs; r; r = r->next) {
        if (r->fd > 2) continue;
        if (r->type == R_DUP) {
            if (r->dupfd <= 2) slot[r->fd] = slot[r->dupfd];
            continue;
        }
        char *t = expand_one(sh, r->target);
        char path[PATH_MAX];
        make_abs(sh, t, path, sizeof path);
        int fd = open(path, open_flags(r->type) | O_CLOEXEC, 0644);
        if (fd < 0) {
            ERR(sh, "%s: %s\n", t, strerror(errno));
            free(t);
            return -1;
        }
        free(t);
        opened[(*nopened)++] = fd;
        slot[r->fd] = fd;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* scripts                                                             */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    str s = {0};
    char buf[8192];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) str_addn(&s, buf, (size_t)r);
    close(fd);
    return str_take(&s);
}

/* Is this file a shell script we should interpret ourselves? */
static int is_shell_script(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char head[128] = {0};
    ssize_t n = read(fd, head, sizeof head - 1);
    close(fd);
    if (n <= 0) return 0;
    if (n >= 4 && !memcmp(head, "\177ELF", 4)) return 0;
    if (head[0] == '#' && head[1] == '!') {
        char *nl = strchr(head, '\n');
        if (nl) *nl = 0;
        return strstr(head, "bash") || strstr(head, "/sh") || strstr(head, "terminator");
    }
    for (ssize_t i = 0; i < n; i++)
        if (head[i] == 0) return 0;   /* binary */
    return 1;                          /* no shebang, plain text: like bash */
}

static int run_script(shell *sh, const char *path, int argc, char **argv)
{
    char full[PATH_MAX];
    make_abs(sh, path, full, sizeof full);
    char *text = read_file(full);
    if (!text) {
        ERR(sh, "%s: %s\n", path, strerror(errno));
        return 127;
    }
    sh->argv0 = strdup(path);
    sh->pos = malloc(sizeof(char *) * (size_t)(argc + 1));
    for (int i = 0; i < argc; i++) sh->pos[i] = strdup(argv[i]);
    sh->npos = argc;
    run_string(sh, text);
    free(text);
    return sh->exiting ? sh->exit_code : sh->last_status;
}

/* ------------------------------------------------------------------ */
/* running one command in a forked child                               */
/* ------------------------------------------------------------------ */

static void child_exec(shell *sh, cmd *c) __attribute__((noreturn));
static void child_exec(shell *sh, cmd *c)
{
    if (apply_redirs_child(sh, c) < 0) _exit(1);

    if (c->ifn) {
        become_subshell(sh);
        _exit(exec_if(sh, c->ifn));
    }

    vec argv = {0};
    expand_words(sh, c, &argv);

    /* NAME=value cmd: put NAME into this command's environment only. */
    for (int i = 0; i < c->na; i++) {
        char *eq = strchr(c->assigns[i], '=');
        char *name = strndup(c->assigns[i], (size_t)(eq - c->assigns[i]));
        char *val = expand_one(sh, eq + 1);
        var_set(sh, name, val, argv.n ? 1 : 0);
        free(name);
        free(val);
    }
    if (argv.n == 0) _exit(0);

    if (find_builtin(argv.v[0]) >= 0) {
        become_subshell(sh);
        _exit(run_builtin(sh, c, argv.n, argv.v, 0, 1, 2));
    }

    /* bash script.sh / sh script.sh / ./script.sh: interpret it ourselves. */
    const char *name = argv.v[0];
    if ((!strcmp(name, "bash") || !strcmp(name, "sh")) && argv.n >= 2 && argv.v[1][0] != '-') {
        become_subshell(sh);
        _exit(run_script(sh, argv.v[1], argv.n - 2, argv.v + 2));
    }
    if (strchr(name, '/')) {
        struct stat st;
        if (stat(name, &st) == 0 && S_ISREG(st.st_mode) && is_shell_script(name)) {
            if (access(name, X_OK) < 0) {
                ERR(sh, "%s: Permission denied\n", name);
                _exit(126);
            }
            become_subshell(sh);
            _exit(run_script(sh, name, argv.n - 1, argv.v + 1));
        }
    }

    environ = build_env(sh);
    execvp(name, argv.v);
    if (errno == ENOENT) {
        if (strchr(name, '/')) ERR(sh, "%s: No such file or directory\n", name);
        else ERR(sh, "%s: command not found\n", name);
        _exit(127);
    }
    ERR(sh, "%s: %s\n", name, strerror(errno));
    _exit(126);
}

/* ------------------------------------------------------------------ */
/* jobs                                                                */
/* ------------------------------------------------------------------ */

static int status_of(int st)
{
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 0;
}

static job *job_add(shell *sh, pid_t pgid, pid_t *pids, int n, const char *text, int jin, int bg)
{
    job *j = calloc(1, sizeof *j);
    int id = 1;
    for (int k = 0; k < sh->njobs; k++)
        if (sh->jobs[k]->id >= id) id = sh->jobs[k]->id + 1;
    j->id = id;
    j->pgid = pgid;
    j->n = n;
    j->pids = pids;
    j->done = calloc((size_t)n, sizeof(int));
    j->stopped = calloc((size_t)n, sizeof(int));
    j->text = strdup(text ? text : "");
    j->jin = jin;
    j->bg = bg;
    j->state = JOB_RUNNING;
    if (sh->njobs == sh->capjobs) {
        sh->capjobs = sh->capjobs ? sh->capjobs * 2 : 8;
        sh->jobs = realloc(sh->jobs, sizeof(job *) * (size_t)sh->capjobs);
    }
    sh->jobs[sh->njobs++] = j;
    return j;
}

static void job_remove(shell *sh, job *j)
{
    for (int k = 0; k < sh->njobs; k++) {
        if (sh->jobs[k] == j) {
            memmove(sh->jobs + k, sh->jobs + k + 1, sizeof(job *) * (size_t)(sh->njobs - k - 1));
            sh->njobs--;
            break;
        }
    }
    if (j->jin >= 0) close(j->jin);
    free(j->pids); free(j->done); free(j->stopped); free(j->text);
    free(j);
}

static char job_mark(shell *sh, job *j)
{
    if (sh->njobs && sh->jobs[sh->njobs - 1] == j) return '+';
    if (sh->njobs > 1 && sh->jobs[sh->njobs - 2] == j) return '-';
    return ' ';
}

/* Collect state changes without blocking. Returns 1 if anything changed. */
static int job_poll(job *j)
{
    int changed = 0;
    for (int k = 0; k < j->n; k++) {
        if (j->done[k]) continue;
        int st;
        pid_t r = waitpid(j->pids[k], &st, WNOHANG | WUNTRACED | WCONTINUED);
        if (r <= 0) {
            if (r < 0 && errno == ECHILD) { j->done[k] = 1; changed = 1; }
            continue;
        }
        changed = 1;
        if (WIFSTOPPED(st)) j->stopped[k] = 1;
        else if (WIFCONTINUED(st)) j->stopped[k] = 0;
        else {
            j->done[k] = 1;
            j->stopped[k] = 0;
            if (k == j->n - 1) j->status = status_of(st);
        }
    }
    return changed;
}

static int job_all_done(job *j)
{
    for (int k = 0; k < j->n; k++) if (!j->done[k]) return 0;
    return 1;
}

static int job_all_stopped(job *j)
{
    int live = 0;
    for (int k = 0; k < j->n; k++) {
        if (j->done[k]) continue;
        live++;
        if (!j->stopped[k]) return 0;
    }
    return live > 0;
}

static void kill_job(job *j, int sig) { kill(-j->pgid, sig); }

/* Background jobs: report finished ones (called while idle). */
static void reap_bg(shell *sh)
{
    for (int k = 0; k < sh->njobs; k++) {
        job *j = sh->jobs[k];
        job_poll(j);
        if (job_all_done(j)) {
            char mark = job_mark(sh, j);
            if (j->status == 0) outf(sh->out_fd, "[%d]%c  Done                    %s\n", j->id, mark, j->text);
            else outf(sh->out_fd, "[%d]%c  Exit %-3d                %s\n", j->id, mark, j->status, j->text);
            job_remove(sh, j);
            k--;
        } else if (job_all_stopped(j) && j->state != JOB_STOPPED) {
            j->state = JOB_STOPPED;
            outf(sh->out_fd, "[%d]%c  Stopped                 %s\n", j->id, job_mark(sh, j), j->text);
        }
    }
}

static void handle_close(shell *sh)
{
    sh->exiting = 1;
    sh->exit_code = 0;
}

/* Wait for the foreground job while still listening to the GUI:
 * typed lines go to the job's stdin, Ctrl+C / Ctrl+Z become signals
 * sent to the job's process group (there is no TTY to do it for us). */
static int wait_fg(shell *sh, job *j)
{
    j->bg = 0;
    for (;;) {
        job_poll(j);
        if (job_all_done(j)) {
            int st = j->status;
            job_remove(sh, j);
            sh->last_status = st;
            return st;
        }
        if (job_all_stopped(j)) {
            j->state = JOB_STOPPED;
            outf(sh->out_fd, "[%d]%c  Stopped                 %s\n", j->id, '+', j->text);
            /* keep it as the most recent job */
            sh->last_status = 128 + SIGTSTP;
            return sh->last_status;
        }
        struct pollfd pf = { sh->msg_fd, POLLIN, 0 };
        int r = poll(&pf, 1, 20);
        if (r <= 0) continue;
        int n = msgbuf_read(&sh->mr, sh->msg_fd);
        if (n == 0) {                          /* GUI is gone */
            kill_job(j, SIGHUP); kill_job(j, SIGCONT); kill_job(j, SIGKILL);
            handle_close(sh);
            continue;
        }
        int type, len;
        char *data;
        while (msgbuf_next(&sh->mr, &type, &data, &len)) {
            switch (type) {
            case M_LINE:
                if (j->jin >= 0) {
                    data[len] = '\n';
                    if (write_all(j->jin, data, (size_t)len + 1) < 0) { close(j->jin); j->jin = -1; }
                }
                break;
            case M_EOF:
                if (j->jin >= 0) { close(j->jin); j->jin = -1; }
                break;
            case M_INT:  kill_job(j, SIGINT); break;
            case M_TSTP: kill_job(j, SIGTSTP); break;
            case M_CLOSE:
                kill_job(j, SIGHUP); kill_job(j, SIGCONT); kill_job(j, SIGKILL);
                handle_close(sh);
                break;
            }
            free(data);
        }
    }
}

/* ------------------------------------------------------------------ */
/* pipelines                                                           */
/* ------------------------------------------------------------------ */

static int is_simple_builtin(cmd *c)
{
    if (!c->nw) return 0;
    return find_builtin(c->words[0]) >= 0;
}

static int exec_inproc(shell *sh, cmd *c)
{
    if (c->ifn) return exec_if(sh, c->ifn);

    vec argv = {0};
    expand_words(sh, c, &argv);

    if (argv.n == 0) {
        for (int i = 0; i < c->na; i++) {
            char *eq = strchr(c->assigns[i], '=');
            char *name = strndup(c->assigns[i], (size_t)(eq - c->assigns[i]));
            char *val = expand_one(sh, eq + 1);
            var_set(sh, name, val, 0);
            free(name);
            free(val);
        }
        int slot[3] = { sh->in_fd, sh->out_fd, sh->err_fd }, opened[16], no = 0;
        int st = apply_redirs_inproc(sh, c, slot, opened, &no) < 0 ? 1 : 0;
        for (int k = 0; k < no; k++) close(opened[k]);
        vec_free(&argv);
        return st;
    }

    int slot[3] = { sh->in_fd, sh->out_fd, sh->err_fd }, opened[16], no = 0;
    int st;
    if (apply_redirs_inproc(sh, c, slot, opened, &no) < 0) st = 1;
    else st = run_builtin(sh, c, argv.n, argv.v, slot[0], slot[1], slot[2]);
    for (int k = 0; k < no; k++) close(opened[k]);
    vec_free(&argv);
    return st;
}

static int exec_pipeline(shell *sh, pipeline *p, int bg)
{
    if (p->n == 1 && !bg) {
        cmd *c = p->cmds[0];
        if (c->ifn || c->nw == 0 || is_simple_builtin(c)) {
            sh->last_status = exec_inproc(sh, c);
            return sh->last_status;
        }
    }

    int jc = sh->interactive;
    int jin[2] = { -1, -1 };
    int first_in;
    if (jc && !bg) {
        if (pipe2(jin, O_CLOEXEC) < 0) { ERR(sh, "pipe: %s\n", strerror(errno)); return 1; }
        first_in = jin[0];
    } else if (bg) {
        first_in = sh->devnull;
    } else {
        first_in = sh->in_fd;
    }

    pid_t pgid = 0;
    pid_t *pids = calloc((size_t)p->n, sizeof(pid_t));
    int prev = -1, nstarted = 0;
    for (int i = 0; i < p->n; i++) {
        int pp[2] = { -1, -1 };
        if (i < p->n - 1 && pipe2(pp, O_CLOEXEC) < 0) {
            ERR(sh, "pipe: %s\n", strerror(errno));
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            ERR(sh, "fork: %s\n", strerror(errno));
            if (pp[0] >= 0) { close(pp[0]); close(pp[1]); }
            break;
        }
        if (pid == 0) {
            if (jc) setpgid(0, pgid);           /* whole pipeline = one group */
            reset_signals();
            setup_std(i == 0 ? first_in : prev, i < p->n - 1 ? pp[1] : sh->out_fd, sh->err_fd);
            if (sh->is_thread && chdir(sh->cwd) < 0) { /* ignore */ }
            child_exec(sh, p->cmds[i]);
        }
        if (jc) setpgid(pid, pgid ? pgid : pid);  /* parent too: no race */
        if (!pgid) pgid = pid;
        pids[nstarted++] = pid;
        if (prev >= 0) close(prev);
        if (pp[1] >= 0) close(pp[1]);
        prev = pp[0];
    }
    if (prev >= 0) close(prev);
    if (jin[0] >= 0) close(jin[0]);

    if (nstarted == 0) {
        free(pids);
        if (jin[1] >= 0) close(jin[1]);
        sh->last_status = 1;
        return 1;
    }

    if (!jc) {
        int st = 0;
        if (bg) { free(pids); sh->last_status = 0; return 0; }
        for (int i = 0; i < nstarted; i++) {
            int s;
            while (waitpid(pids[i], &s, 0) < 0 && errno == EINTR)
                ;
            if (i == nstarted - 1) st = status_of(s);
        }
        free(pids);
        sh->last_status = st;
        return st;
    }

    job *j = job_add(sh, pgid, pids, nstarted, p->text, jin[1], bg);
    if (bg) {
        outf(sh->out_fd, "[%d] %d\n", j->id, (int)pgid);
        sh->last_status = 0;
        return 0;
    }
    return wait_fg(sh, j);
}

static int exec_andor(shell *sh, andor *a)
{
    int st = exec_pipeline(sh, a->p[0], 0);
    for (int i = 1; i < a->n && !sh->exiting; i++) {
        if ((a->ops[i - 1] == 1 && st == 0) || (a->ops[i - 1] == 2 && st != 0))
            st = exec_pipeline(sh, a->p[i], 0);
    }
    return st;
}

/* cmd1 && cmd2 & : run the whole and-or list in a background subshell. */
static void exec_andor_bg(shell *sh, andor *a)
{
    if (a->n == 1) { exec_pipeline(sh, a->p[0], 1); return; }
    pid_t pid = fork();
    if (pid < 0) { ERR(sh, "fork: %s\n", strerror(errno)); return; }
    if (pid == 0) {
        if (sh->interactive) setpgid(0, 0);
        reset_signals();
        setup_std(sh->devnull, sh->out_fd, sh->err_fd);
        become_subshell(sh);
        _exit(exec_andor(sh, a));
    }
    if (!sh->interactive) return;
    setpgid(pid, pid);
    pid_t *pids = malloc(sizeof(pid_t));
    pids[0] = pid;
    job *j = job_add(sh, pid, pids, 1, a->text, -1, 1);
    outf(sh->out_fd, "[%d] %d\n", j->id, (int)pid);
    sh->last_status = 0;
}

static void exec_list(shell *sh, list *l)
{
    for (int i = 0; i < l->n && !sh->exiting; i++) {
        if (l->bg[i]) exec_andor_bg(sh, l->items[i]);
        else exec_andor(sh, l->items[i]);
    }
}

static int exec_if(shell *sh, ifnode *f)
{
    for (int i = 0; i < f->n; i++) {
        exec_list(sh, f->conds[i]);
        if (sh->exiting) return sh->exit_code;
        if (sh->last_status == 0) {
            exec_list(sh, f->bodies[i]);
            return sh->last_status;
        }
    }
    if (f->elsebody) exec_list(sh, f->elsebody);
    else sh->last_status = 0;
    return sh->last_status;
}

static int run_string(shell *sh, const char *text)
{
    program pr;
    char err[256];
    int st = parse_program(text, &pr, err, sizeof err);
    if (st == P_INCOMPLETE) {
        ERR(sh, "syntax error: unexpected end of file\n");
        sh->last_status = 2;
        return 2;
    }
    if (st == P_ERROR) {
        ERR(sh, "%s\n", err);
        sh->last_status = 2;
        return 2;
    }
    exec_list(sh, pr.l);
    free_program(&pr);
    return sh->last_status;
}

/* ------------------------------------------------------------------ */
/* multiWatch                                                          */
/* ------------------------------------------------------------------ */

#define MW_DASHES "----------------------------------------------------"

typedef struct {
    char *cmd;
    pid_t pid;
    int fd;
    int running;
    double next_start;
} mwitem;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Parses ["cmd1", "cmd2"] (also accepts bare quoted strings). */
static int mw_parse(const char *s, vec *out)
{
    while (*s) {
        while (*s && (*s == ' ' || *s == ',' || *s == '[' || *s == ']' || *s == '\t')) s++;
        if (!*s) break;
        if (*s != '"' && *s != '\'') return -1;
        char q = *s++;
        str item = {0};
        while (*s && *s != q) {
            if (*s == '\\' && s[1]) { str_addc(&item, s[1]); s += 2; }
            else str_addc(&item, *s++);
        }
        if (*s != q) { free(item.s); return -1; }
        s++;
        char *t = str_take(&item);
        vec_push(out, t);
        free(t);
    }
    return out->n ? 0 : -1;
}

static void mw_start(shell *sh, mwitem *it)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return; }
    if (pid == 0) {
        setpgid(0, 0);                 /* own group: one kill() stops its pipeline */
        reset_signals();
        setup_std(sh->devnull, p[1], p[1]);
        become_subshell(sh);
        run_string(sh, it->cmd);
        _exit(sh->last_status);
    }
    setpgid(pid, pid);
    close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    it->pid = pid;
    it->fd = p[0];
    it->running = 1;
}

static void mw_print(shell *sh, mwitem *it, const char *data, size_t n)
{
    str s = {0};
    char hdr[64];
    str_addc(&s, '"');
    str_adds(&s, it->cmd);
    snprintf(hdr, sizeof hdr, "\", %ld:\n", (long)time(NULL));
    str_adds(&s, hdr);
    str_adds(&s, MW_DASHES "\n");
    str_addn(&s, data, n);
    if (n && data[n - 1] != '\n') str_addc(&s, '\n');
    str_adds(&s, MW_DASHES "\n");
    write_all(sh->out_fd, s.s, s.n);
    free(s.s);
}

static void mw_reap(mwitem *it)
{
    if (it->fd >= 0) { close(it->fd); it->fd = -1; }
    if (it->running) {
        int st;
        while (waitpid(it->pid, &st, 0) < 0 && errno == EINTR)
            ;
        it->running = 0;
    }
}

static int b_multiwatch(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)argv; (void)argc; (void)in; (void)err;
    /* Re-join the raw words: the list syntax is not shell syntax. */
    str raw = {0};
    double interval = 2.0;
    int i = 1;
    if (c && c->nw > 2 && !strcmp(c->words[1], "-n")) {
        interval = atof(c->words[2]);
        i = 3;
    }
    for (; c && i < c->nw; i++) {
        if (raw.n) str_addc(&raw, ' ');
        str_adds(&raw, c->words[i]);
    }
    vec cmds = {0};
    if (!raw.s || mw_parse(raw.s, &cmds) < 0) {
        outf(err, "usage: multiWatch [-n seconds] [\"cmd1\", \"cmd2\", ...]\n");
        free(raw.s);
        vec_free(&cmds);
        return 2;
    }
    free(raw.s);

    int saved_out = sh->out_fd;
    sh->out_fd = out;
    int n = cmds.n;
    mwitem *items = calloc((size_t)n, sizeof *items);
    for (int k = 0; k < n; k++) {
        items[k].cmd = cmds.v[k];
        items[k].fd = -1;
        mw_start(sh, &items[k]);
    }

    int stop = 0;
    while (!stop) {
        struct pollfd pf[n + 1];
        int idx[n + 1], np = 0;
        double t = now_s(), wait_s = 1.0;
        int any_left = 0;
        for (int k = 0; k < n; k++) {
            if (items[k].fd >= 0) {
                pf[np].fd = items[k].fd; pf[np].events = POLLIN; idx[np++] = k;
                any_left = 1;
            } else if (interval > 0) {
                any_left = 1;
                if (items[k].next_start <= t) {
                    mw_start(sh, &items[k]);
                    if (items[k].fd >= 0) { pf[np].fd = items[k].fd; pf[np].events = POLLIN; idx[np++] = k; }
                } else if (items[k].next_start - t < wait_s) {
                    wait_s = items[k].next_start - t;
                }
            }
        }
        if (!any_left) break;
        int msg_slot = -1;
        if (sh->msg_fd >= 0) { pf[np].fd = sh->msg_fd; pf[np].events = POLLIN; msg_slot = np++; }
        int r = poll(pf, (nfds_t)np, (int)(wait_s * 1000) + 1);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        for (int q = 0; q < np; q++) {
            if (!pf[q].revents) continue;
            if (q == msg_slot) {
                int got = msgbuf_read(&sh->mr, sh->msg_fd);
                if (got == 0) { stop = 1; handle_close(sh); break; }
                int type, len;
                char *data;
                while (msgbuf_next(&sh->mr, &type, &data, &len)) {
                    if (type == M_INT) stop = 1;
                    if (type == M_CLOSE) { stop = 1; handle_close(sh); }
                    free(data);
                }
                continue;
            }
            mwitem *it = &items[idx[q]];
            char buf[8192];
            ssize_t got = read(it->fd, buf, sizeof buf);
            if (got > 0) {
                mw_print(sh, it, buf, (size_t)got);
            } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                mw_reap(it);
                it->next_start = now_s() + interval;
            }
        }
    }
    for (int k = 0; k < n; k++) {
        if (items[k].running) {
            kill(-items[k].pid, SIGINT);
            kill(-items[k].pid, SIGKILL);
        }
        mw_reap(&items[k]);
    }
    free(items);
    vec_free(&cmds);
    sh->out_fd = saved_out;
    return stop ? 130 : 0;
}

/* ------------------------------------------------------------------ */
/* builtins                                                            */
/* ------------------------------------------------------------------ */

static void send_cwd(shell *sh)
{
    if (sh->ctl_fd >= 0) msg_send(sh->ctl_fd, C_CWD, sh->cwd, -1);
}

static int b_cd(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)in;
    const char *target;
    if (argc < 2) target = home_dir(sh);
    else if (!strcmp(argv[1], "-")) {
        if (!sh->oldpwd[0]) { outf(err, "terminator: cd: OLDPWD not set\n"); return 1; }
        target = sh->oldpwd;
        outf(out, "%s\n", target);
    } else target = argv[1];

    char path[PATH_MAX], real[PATH_MAX];
    make_abs(sh, target, path, sizeof path);
    if (!realpath(path, real)) {
        outf(err, "terminator: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    struct stat st;
    if (stat(real, &st) < 0 || !S_ISDIR(st.st_mode)) {
        outf(err, "terminator: cd: %s: Not a directory\n", target);
        return 1;
    }
    if (access(real, X_OK) < 0) {
        outf(err, "terminator: cd: %s: Permission denied\n", target);
        return 1;
    }
    /* Process mode (and scripts): the engine owns its process, so it may
     * really chdir. Thread mode: only this tab's string changes. */
    if (!sh->is_thread && chdir(real) < 0) {
        outf(err, "terminator: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    snprintf(sh->oldpwd, sizeof sh->oldpwd, "%s", sh->cwd);
    snprintf(sh->cwd, sizeof sh->cwd, "%s", real);
    var_set(sh, "OLDPWD", sh->oldpwd, 0);
    var_set(sh, "PWD", sh->cwd, 0);
    send_cwd(sh);
    return 0;
}

static int b_pwd(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)argc; (void)argv; (void)in; (void)err;
    outf(out, "%s\n", sh->cwd);
    return 0;
}

static int b_exit(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)in; (void)out; (void)err;
    sh->exiting = 1;
    sh->exit_code = argc > 1 ? atoi(argv[1]) & 255 : sh->last_status;
    return sh->exit_code;
}

static int b_history(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)sh; (void)c; (void)in; (void)err;
    char **v;
    int n = hist_snapshot(&v);
    int show = argc > 1 ? atoi(argv[1]) : 1000;
    if (show <= 0 || show > n) show = n;
    str s = {0};
    char num[32];
    for (int i = n - show; i < n; i++) {
        snprintf(num, sizeof num, "%5d  ", i + 1);
        str_adds(&s, num);
        str_adds(&s, v[i]);
        str_addc(&s, '\n');
    }
    if (s.n) write_all(out, s.s, s.n);
    free(s.s);
    hist_free_snapshot(v, n);
    return 0;
}

static job *find_job(shell *sh, int argc, char **argv, int err, const char *who)
{
    if (!sh->njobs) { outf(err, "terminator: %s: no current job\n", who); return NULL; }
    if (argc < 2) return sh->jobs[sh->njobs - 1];
    const char *a = argv[1];
    if (*a == '%') a++;
    if (!strcmp(a, "+") || !strcmp(a, "%") || !*a) return sh->jobs[sh->njobs - 1];
    if (!strcmp(a, "-")) return sh->njobs > 1 ? sh->jobs[sh->njobs - 2] : sh->jobs[0];
    int id = atoi(a);
    for (int k = 0; k < sh->njobs; k++)
        if (sh->jobs[k]->id == id) return sh->jobs[k];
    outf(err, "terminator: %s: %s: no such job\n", who, argv[1]);
    return NULL;
}

static int b_jobs(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)argc; (void)argv; (void)in; (void)err;
    if (!sh->interactive) return 0;
    for (int k = 0; k < sh->njobs; k++) job_poll(sh->jobs[k]);
    for (int k = 0; k < sh->njobs; k++) {
        job *j = sh->jobs[k];
        const char *state = job_all_done(j) ? "Done" : job_all_stopped(j) ? "Stopped" : "Running";
        outf(out, "[%d]%c  %-22s  %s%s\n", j->id, job_mark(sh, j), state, j->text,
             (!strcmp(state, "Running") && j->bg) ? " &" : "");
    }
    return 0;
}

static int b_fg(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)in;
    if (!sh->interactive) { outf(err, "terminator: fg: no job control\n"); return 1; }
    job *j = find_job(sh, argc, argv, err, "fg");
    if (!j) return 1;
    /* make it the most recent job */
    for (int k = 0; k < sh->njobs; k++)
        if (sh->jobs[k] == j) {
            memmove(sh->jobs + k, sh->jobs + k + 1, sizeof(job *) * (size_t)(sh->njobs - k - 1));
            sh->jobs[sh->njobs - 1] = j;
            break;
        }
    outf(out, "%s\n", j->text);
    for (int k = 0; k < j->n; k++) j->stopped[k] = 0;
    j->state = JOB_RUNNING;
    kill_job(j, SIGCONT);
    return wait_fg(sh, j);
}

static int b_bg(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)in;
    if (!sh->interactive) { outf(err, "terminator: bg: no job control\n"); return 1; }
    job *j = find_job(sh, argc, argv, err, "bg");
    if (!j) return 1;
    if (j->state == JOB_RUNNING && j->bg) {
        outf(err, "terminator: bg: job %d already in background\n", j->id);
        return 0;
    }
    for (int k = 0; k < j->n; k++) j->stopped[k] = 0;
    j->state = JOB_RUNNING;
    j->bg = 1;
    kill_job(j, SIGCONT);
    outf(out, "[%d]%c %s &\n", j->id, job_mark(sh, j), j->text);
    return 0;
}

static int b_source(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)in; (void)out;
    if (argc < 2) { outf(err, "terminator: source: filename argument required\n"); return 2; }
    char full[PATH_MAX];
    make_abs(sh, argv[1], full, sizeof full);
    char *text = read_file(full);
    if (!text) { outf(err, "terminator: %s: %s\n", argv[1], strerror(errno)); return 1; }
    char **saved = sh->pos;
    int saved_n = sh->npos;
    vec args = {0};
    if (argc > 2) {
        for (int i = 2; i < argc; i++) vec_push(&args, argv[i]);
        sh->pos = args.v;
        sh->npos = args.n;
    }
    int st = run_string(sh, text);
    if (argc > 2) { sh->pos = saved; sh->npos = saved_n; vec_free(&args); }
    free(text);
    return st;
}

static int b_export(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)in; (void)err;
    if (argc == 1) {
        for (int i = 0; i < sh->nvars; i++)
            if (sh->vars[i].exp) outf(out, "declare -x %s=\"%s\"\n", sh->vars[i].name, sh->vars[i].val);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            char *name = strndup(argv[i], (size_t)(eq - argv[i]));
            var_set(sh, name, eq + 1, 1);
            free(name);
        } else {
            const char *v = var_get(sh, argv[i]);
            var_set(sh, argv[i], v ? v : "", 1);
        }
    }
    return 0;
}

static int b_unset(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)in; (void)out; (void)err;
    for (int i = 1; i < argc; i++) var_unset(sh, argv[i]);
    return 0;
}

static int b_shift(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)in; (void)out; (void)err;
    int k = argc > 1 ? atoi(argv[1]) : 1;
    if (k > sh->npos) return 1;
    sh->pos += k;
    sh->npos -= k;
    return 0;
}

static int b_bind(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)in; (void)out;
    if (sh->ctl_fd < 0) { outf(err, "terminator: bind: only available in an interactive tab\n"); return 1; }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-l"))) {
        msg_send(sh->ctl_fd, C_BINDLIST, "", 0);
        return 0;
    }
    if (argc != 3) { outf(err, "usage: bind [-l] | bind <action> <key>   e.g. bind clear-screen Ctrl+Shift+L\n"); return 2; }
    char buf[256];
    snprintf(buf, sizeof buf, "%s %s", argv[1], argv[2]);
    msg_send(sh->ctl_fd, C_BIND, buf, -1);
    return 0;
}

static int b_clear(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)c; (void)argc; (void)argv; (void)in; (void)err;
    if (sh->ctl_fd >= 0) msg_send(sh->ctl_fd, C_CLEAR, "", 0);
    else write_all(out, "\033[H\033[2J", 7);
    return 0;
}

static int b_help(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err);

typedef int (*bfn)(shell *, cmd *, int, char **, int, int, int);
static const struct { const char *name; bfn f; } builtins[] = {
    { "cd", b_cd }, { "pwd", b_pwd }, { "exit", b_exit }, { "history", b_history },
    { "jobs", b_jobs }, { "fg", b_fg }, { "bg", b_bg }, { "source", b_source },
    { ".", b_source }, { "export", b_export }, { "unset", b_unset }, { "shift", b_shift },
    { "multiWatch", b_multiwatch }, { "bind", b_bind }, { "clear", b_clear }, { "help", b_help },
};

static int b_help(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    (void)sh; (void)c; (void)argc; (void)argv; (void)in; (void)err;
    outf(out, "Terminator builtins:");
    for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++) outf(out, " %s", builtins[i].name);
    outf(out, "\nOther commands run as child processes (fork + execvp).\n"
              "Keys: bind -l lists every key binding; bind <action> <key> changes one.\n");
    return 0;
}

static int find_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++)
        if (!strcmp(builtins[i].name, name)) return (int)i;
    return -1;
}

static int run_builtin(shell *sh, cmd *c, int argc, char **argv, int in, int out, int err)
{
    int k = find_builtin(argv[0]);
    if (k < 0) return 127;
    int st = builtins[k].f(sh, c, argc, argv, in, out, err);
    sh->last_status = st;
    return st;
}

/* ------------------------------------------------------------------ */
/* engine setup and main loops                                         */
/* ------------------------------------------------------------------ */

static shell *shell_new(void)
{
    shell *sh = calloc(1, sizeof *sh);
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq) continue;
        char *name = strndup(*e, (size_t)(eq - *e));
        var_set(sh, name, eq + 1, 1);
        free(name);
    }
    sh->argv0 = strdup("terminator");
    sh->shell_pid = getpid();
    sh->devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    sh->msg_fd = sh->ctl_fd = -1;
    return sh;
}

static void shell_free(shell *sh)
{
    for (int i = 0; i < sh->nvars; i++) { free(sh->vars[i].name); free(sh->vars[i].val); }
    free(sh->vars);
    while (sh->njobs) job_remove(sh, sh->jobs[0]);
    free(sh->jobs);
    free(sh->pending);
    msgbuf_free(&sh->mr);
    if (sh->devnull >= 0) close(sh->devnull);
    free(sh);
}

static void send_prompt(shell *sh, int more)
{
    if (sh->ctl_fd < 0) return;
    if (more) { msg_send(sh->ctl_fd, C_PROMPT, "> ", -1); return; }
    const char *u = var_get(sh, "USER");
    char p[256];
    snprintf(p, sizeof p, "%s@terminator> ", u && *u ? u : "user");
    msg_send(sh->ctl_fd, C_PROMPT, p, -1);
}

static void pending_add(shell *sh, const char *s, size_t n)
{
    if (sh->plen + n + 2 > sh->pcap) {
        sh->pcap = (sh->plen + n + 2) * 2;
        sh->pending = realloc(sh->pending, sh->pcap);
    }
    memcpy(sh->pending + sh->plen, s, n);
    sh->plen += n;
    sh->pending[sh->plen++] = '\n';
    sh->pending[sh->plen] = 0;
}

/* Feed one line of input. Returns 1 if a command ran or failed, 0 if
 * more input is needed (open quote, trailing \, unclosed $(, if...). */
static int feed_line(shell *sh, const char *line, size_t n)
{
    pending_add(sh, line, n);
    program pr;
    char err[256];
    int st = parse_program(sh->pending, &pr, err, sizeof err);
    if (st == P_INCOMPLETE) return 0;

    if (sh->interactive) {
        size_t l = sh->plen;
        while (l && sh->pending[l - 1] == '\n') l--;
        char *h = strndup(sh->pending, l);
        hist_add(h);
        free(h);
    }
    sh->plen = 0;
    sh->pending[0] = 0;
    if (st == P_ERROR) {
        ERR(sh, "%s\n", err);
        sh->last_status = 2;
        return 1;
    }
    exec_list(sh, pr.l);
    free_program(&pr);
    return 1;
}

void shell_main_interactive(int msg_fd, int out_fd, int ctl_fd, int is_thread, const char *cwd)
{
    shell *sh = shell_new();
    sh->interactive = 1;
    sh->is_thread = is_thread;
    sh->msg_fd = msg_fd;
    sh->ctl_fd = ctl_fd;
    sh->in_fd = sh->devnull;
    sh->out_fd = sh->err_fd = out_fd;
    snprintf(sh->cwd, sizeof sh->cwd, "%s", cwd);
    var_set(sh, "PWD", sh->cwd, 0);
    if (!is_thread && chdir(sh->cwd) < 0) { /* keep going */ }

    send_cwd(sh);
    send_prompt(sh, 0);

    while (!sh->exiting) {
        struct pollfd pf = { msg_fd, POLLIN, 0 };
        /* Sleep until the GUI sends something. Finished background jobs
         * are reported just before the next prompt, as bash does. */
        int r = poll(&pf, 1, -1);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        int got = msgbuf_read(&sh->mr, msg_fd);
        if (got == 0) break;
        int type, len;
        char *data;
        while (!sh->exiting && msgbuf_next(&sh->mr, &type, &data, &len)) {
            switch (type) {
            case M_LINE:
                if (feed_line(sh, data, (size_t)len)) {
                    if (!sh->exiting) { reap_bg(sh); send_prompt(sh, 0); }
                } else {
                    send_prompt(sh, 1);
                }
                break;
            case M_INT:
            case M_TSTP:
                sh->plen = 0;
                if (sh->pending) sh->pending[0] = 0;
                if (type == M_INT) sh->last_status = 130;
                send_prompt(sh, 0);
                break;
            case M_EOF:
                if (sh->plen) {
                    ERR(sh, "syntax error: unexpected end of file\n");
                    sh->plen = 0;
                    sh->pending[0] = 0;
                    send_prompt(sh, 0);
                } else {
                    handle_close(sh);
                }
                break;
            case M_CLOSE:
                handle_close(sh);
                break;
            }
            free(data);
        }
    }

    /* Tab is closing: take its jobs down with it. */
    for (int k = 0; k < sh->njobs; k++) {
        kill_job(sh->jobs[k], SIGHUP);
        kill_job(sh->jobs[k], SIGCONT);
    }
    for (int k = 0; k < sh->njobs; k++)
        for (int q = 0; q < sh->jobs[k]->n; q++)
            if (!sh->jobs[k]->done[q]) {
                kill(sh->jobs[k]->pids[q], SIGKILL);
                waitpid(sh->jobs[k]->pids[q], NULL, 0);
                sh->jobs[k]->done[q] = 1;
            }
    msg_send(ctl_fd, C_EXIT, "", 0);
    close(msg_fd);
    close(out_fd);
    close(ctl_fd);
    shell_free(sh);
}

int shell_run_headless(int argc, char **argv)
{
    shell *sh = shell_new();
    sh->in_fd = 0; sh->out_fd = 1; sh->err_fd = 2;
    if (!getcwd(sh->cwd, sizeof sh->cwd)) strcpy(sh->cwd, "/");
    int rc;
    if (argc >= 2 && !strcmp(argv[0], "-c")) {
        sh->pos = argv + 2;
        sh->npos = argc - 2;
        run_string(sh, argv[1]);
        rc = sh->exiting ? sh->exit_code : sh->last_status;
    } else if (argc >= 1 && strcmp(argv[0], "--headless") != 0) {
        rc = run_script(sh, argv[0], argc - 1, argv + 1);
    } else {
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        while (!sh->exiting && (n = getline(&line, &cap, stdin)) > 0) {
            if (line[n - 1] == '\n') line[--n] = 0;
            feed_line(sh, line, (size_t)n);
        }
        free(line);
        if (sh->plen) ERR(sh, "syntax error: unexpected end of file\n");
        rc = sh->exiting ? sh->exit_code : sh->last_status;
    }
    shell_free(sh);
    return rc;
}

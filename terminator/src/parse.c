#define _GNU_SOURCE
#include "parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- arena: one free() releases a whole parse tree ---------- */

typedef struct chunk {
    struct chunk *next;
    size_t used, cap;
    char data[];
} chunk;

static void *aalloc(chunk **a, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!*a || (*a)->used + n > (*a)->cap) {
        size_t cap = n > 65536 ? n : 65536;
        chunk *c = malloc(sizeof(chunk) + cap);
        if (!c) { perror("malloc"); exit(1); }
        c->next = *a; c->used = 0; c->cap = cap;
        *a = c;
    }
    void *p = (*a)->data + (*a)->used;
    (*a)->used += n;
    memset(p, 0, n);
    return p;
}

static char *astrndup(chunk **a, const char *s, size_t n)
{
    char *d = aalloc(a, n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* Grow an arena array: copies into a bigger block (the old one is simply left behind). */
#define APUSH(A, arr, n, cap, val) do {                                   \
        if ((n) == (cap)) {                                               \
            int nc_ = (cap) ? (cap) * 2 : 4;                              \
            void *na_ = aalloc((A), (size_t)nc_ * sizeof(*(arr)));        \
            if (n) memcpy(na_, (arr), (size_t)(n) * sizeof(*(arr)));     \
            (arr) = na_; (cap) = nc_;                                     \
        }                                                                 \
        (arr)[(n)++] = (val);                                             \
    } while (0)

void free_program(program *p)
{
    chunk *c = p->arena;
    while (c) {
        chunk *n = c->next;
        free(c);
        c = n;
    }
    p->arena = NULL;
    p->l = NULL;
}

/* ---------- lexer ---------- */

enum { T_WORD, T_PIPE, T_AMP, T_SEMI, T_NL, T_ANDAND, T_OROR, T_REDIR, T_EOF };

typedef struct {
    int type;
    char *s;                 /* raw word                           */
    int fd, rtype, dupfd;    /* redirections                       */
    int start, end;          /* offsets in the source              */
} tok;

typedef struct {
    const char *src;
    chunk **arena;
    tok *t; int n, cap, pos;
    int status;
    char *err; size_t errlen;
} P;

static int skip_quote(const char *s, int i)  /* s[i] == '\'' ; returns index after closing */
{
    i++;
    while (s[i] && s[i] != '\'') i++;
    return s[i] ? i + 1 : -1;
}

static int skip_dquote(const char *s, int i) /* s[i] == '"' */
{
    i++;
    while (s[i] && s[i] != '"') {
        if (s[i] == '\\') {
            if (!s[i + 1]) return -1;
            i += 2;
        } else if (s[i] == '$' && s[i + 1] == '(') {
            int j = scan_subst(s, i + 2);
            if (j < 0) return -1;
            i = j + 1;
        } else {
            i++;
        }
    }
    return s[i] ? i + 1 : -1;
}

int scan_subst(const char *s, int i)
{
    int depth = 1;
    while (s[i]) {
        char c = s[i];
        if (c == '\\') {
            if (!s[i + 1]) return -1;
            i += 2;
            continue;
        }
        if (c == '\'') { i = skip_quote(s, i); if (i < 0) return -1; continue; }
        if (c == '"')  { i = skip_dquote(s, i); if (i < 0) return -1; continue; }
        if (c == '(') depth++;
        else if (c == ')' && --depth == 0) return i;
        i++;
    }
    return -1;
}

static int is_meta(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '|' ||
           c == '&' || c == '<' || c == '>';
}

static void addtok(P *p, tok t)
{
    APUSH(p->arena, p->t, p->n, p->cap, t);
}

/* Appends to a growing malloc buffer. */
typedef struct { char *s; size_t n, cap; } buf;
static void bput(buf *b, char c)
{
    if (b->n + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->s = realloc(b->s, b->cap);
    }
    b->s[b->n++] = c;
    b->s[b->n] = 0;
}

static int lex(P *p)
{
    const char *s = p->src;
    int i = 0;
    for (;;) {
        char c = s[i];
        tok t = {0};
        t.start = i;
        if (!c) {
            t.type = T_EOF; t.end = i;
            addtok(p, t);
            return P_OK;
        }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '\\' && s[i + 1] == '\n') {
            if (!s[i + 2]) return P_INCOMPLETE;   /* trailing \: keep reading */
            i += 2;
            continue;
        }
        if (c == '\\' && !s[i + 1]) return P_INCOMPLETE;
        if (c == '#') { while (s[i] && s[i] != '\n') i++; continue; }
        if (c == '\n') { t.type = T_NL; i++; }
        else if (c == ';') { t.type = T_SEMI; i++; }
        else if (c == '|') {
            if (s[i + 1] == '|') { t.type = T_OROR; i += 2; }
            else { t.type = T_PIPE; i++; }
        } else if (c == '&') {
            if (s[i + 1] == '&') { t.type = T_ANDAND; i += 2; }
            else { t.type = T_AMP; i++; }
        } else if (c == '<' || c == '>' ||
                   (isdigit((unsigned char)c) && (s[i + 1] == '<' || s[i + 1] == '>'))) {
            t.type = T_REDIR;
            if (isdigit((unsigned char)c)) { t.fd = c - '0'; i++; c = s[i]; }
            else t.fd = (c == '<') ? 0 : 1;
            if (c == '<') { t.rtype = R_IN; i++; }
            else if (s[i + 1] == '>') { t.rtype = R_APPEND; i += 2; }
            else if (s[i + 1] == '&' && isdigit((unsigned char)s[i + 2])) {
                t.rtype = R_DUP; t.dupfd = s[i + 2] - '0'; i += 3;
            } else { t.rtype = R_OUT; i++; }
        } else {
            buf b = {0};
            while (s[i] && !is_meta(s[i])) {
                char d = s[i];
                if (d == '\\') {
                    if (s[i + 1] == '\n') {
                        if (!s[i + 2]) { free(b.s); return P_INCOMPLETE; }
                        i += 2;
                        continue;
                    }
                    if (!s[i + 1]) { free(b.s); return P_INCOMPLETE; }
                    bput(&b, d); bput(&b, s[i + 1]); i += 2;
                } else if (d == '\'') {
                    int j = skip_quote(s, i);
                    if (j < 0) { free(b.s); return P_INCOMPLETE; }
                    for (; i < j; i++) bput(&b, s[i]);
                } else if (d == '"') {
                    bput(&b, '"'); i++;
                    for (;;) {
                        if (!s[i]) { free(b.s); return P_INCOMPLETE; }
                        if (s[i] == '\\') {
                            if (s[i + 1] == '\n') { i += 2; continue; }
                            if (!s[i + 1]) { free(b.s); return P_INCOMPLETE; }
                            bput(&b, s[i]); bput(&b, s[i + 1]); i += 2;
                        } else if (s[i] == '"') {
                            bput(&b, '"'); i++; break;
                        } else if (s[i] == '$' && s[i + 1] == '(') {
                            int j = scan_subst(s, i + 2);
                            if (j < 0) { free(b.s); return P_INCOMPLETE; }
                            for (; i <= j; i++) bput(&b, s[i]);
                        } else {
                            bput(&b, s[i++]);
                        }
                    }
                } else if (d == '$' && s[i + 1] == '(') {
                    int j = scan_subst(s, i + 2);
                    if (j < 0) { free(b.s); return P_INCOMPLETE; }
                    for (; i <= j; i++) bput(&b, s[i]);
                } else {
                    bput(&b, d); i++;
                }
            }
            if (!b.s) { /* only a line continuation */ continue; }
            t.type = T_WORD;
            t.s = astrndup(p->arena, b.s, b.n);
            free(b.s);
        }
        t.end = i;
        addtok(p, t);
    }
}

/* ---------- parser ---------- */

static tok *peek(P *p) { return &p->t[p->pos]; }
static tok *next(P *p) { tok *t = &p->t[p->pos]; if (t->type != T_EOF) p->pos++; return t; }

static const char *tokname(tok *t)
{
    switch (t->type) {
    case T_WORD: return t->s;
    case T_PIPE: return "|";
    case T_AMP: return "&";
    case T_SEMI: return ";";
    case T_NL: return "newline";
    case T_ANDAND: return "&&";
    case T_OROR: return "||";
    case T_REDIR: return t->rtype == R_IN ? "<" : t->rtype == R_APPEND ? ">>" : ">";
    default: return "end of file";
    }
}

static void *fail(P *p, tok *t)
{
    if (p->status == P_OK) {
        if (t->type == T_EOF) {
            p->status = P_INCOMPLETE;
        } else {
            p->status = P_ERROR;
            snprintf(p->err, p->errlen, "syntax error near unexpected token `%s'", tokname(t));
        }
    }
    return NULL;
}

static int is_kw(tok *t, const char *kw) { return t->type == T_WORD && !strcmp(t->s, kw); }

static int is_reserved(tok *t)
{
    return is_kw(t, "then") || is_kw(t, "elif") || is_kw(t, "else") ||
           is_kw(t, "fi") || is_kw(t, "do") || is_kw(t, "done");
}

static int is_stop(tok *t, const char **stops)
{
    for (; stops && *stops; stops++)
        if (is_kw(t, *stops)) return 1;
    return 0;
}

static int is_assignment(const char *w)
{
    if (!(isalpha((unsigned char)*w) || *w == '_')) return 0;
    for (w++; *w && *w != '='; w++)
        if (!(isalnum((unsigned char)*w) || *w == '_')) return 0;
    return *w == '=';
}

static void skip_nl(P *p) { while (peek(p)->type == T_NL) next(p); }

static list *parse_list(P *p, const char **stops);

static cmd *parse_if(P *p)
{
    cmd *c = aalloc(p->arena, sizeof *c);
    ifnode *f = aalloc(p->arena, sizeof *f);
    int capc = 0, capb = 0, nb = 0;
    static const char *s_then[] = { "then", NULL };
    static const char *s_body[] = { "elif", "else", "fi", NULL };
    static const char *s_fi[] = { "fi", NULL };
    c->ifn = f;
    next(p); /* if */
    for (;;) {
        list *cond = parse_list(p, s_then);
        if (!cond) return NULL;
        if (!cond->n) return fail(p, peek(p));
        if (!is_kw(peek(p), "then")) return fail(p, peek(p));
        next(p);
        list *body = parse_list(p, s_body);
        if (!body) return NULL;
        if (!body->n) return fail(p, peek(p));
        APUSH(p->arena, f->conds, f->n, capc, cond);
        APUSH(p->arena, f->bodies, nb, capb, body);
        tok *t = peek(p);
        if (is_kw(t, "elif")) { next(p); continue; }
        if (is_kw(t, "else")) {
            next(p);
            f->elsebody = parse_list(p, s_fi);
            if (!f->elsebody) return NULL;
            if (!is_kw(peek(p), "fi")) return fail(p, peek(p));
            next(p);
            return c;
        }
        if (is_kw(t, "fi")) { next(p); return c; }
        return fail(p, t);
    }
}

static cmd *parse_command(P *p)
{
    tok *t = peek(p);
    if (is_kw(t, "if")) return parse_if(p);
    if (is_reserved(t)) return fail(p, t);

    cmd *c = aalloc(p->arena, sizeof *c);
    int capw = 0, capa = 0;
    redir *tail = NULL;
    for (;;) {
        t = peek(p);
        if (t->type == T_WORD) {
            if (c->nw == 0 && is_assignment(t->s))
                APUSH(p->arena, c->assigns, c->na, capa, t->s);
            else
                APUSH(p->arena, c->words, c->nw, capw, t->s);
            next(p);
        } else if (t->type == T_REDIR) {
            next(p);
            redir *r = aalloc(p->arena, sizeof *r);
            r->fd = t->fd; r->type = t->rtype; r->dupfd = t->dupfd;
            if (r->type != R_DUP) {
                tok *w = peek(p);
                if (w->type != T_WORD) {
                    p->status = P_ERROR;
                    snprintf(p->err, p->errlen, "syntax error near unexpected token `%s'",
                             w->type == T_EOF ? "newline" : tokname(w));
                    return NULL;
                }
                r->target = w->s;
                next(p);
            }
            if (tail) tail->next = r; else c->redirs = r;
            tail = r;
        } else {
            break;
        }
    }
    if (!c->nw && !c->na && !c->redirs) return fail(p, t);
    return c;
}

static pipeline *parse_pipeline(P *p)
{
    pipeline *pl = aalloc(p->arena, sizeof *pl);
    int cap = 0;
    int start = peek(p)->start;
    for (;;) {
        cmd *c = parse_command(p);
        if (!c) return NULL;
        APUSH(p->arena, pl->cmds, pl->n, cap, c);
        if (peek(p)->type != T_PIPE) break;
        next(p);
        skip_nl(p);
    }
    int end = p->t[p->pos - 1].end;
    pl->text = astrndup(p->arena, p->src + start, (size_t)(end - start));
    return pl;
}

static andor *parse_andor(P *p)
{
    andor *a = aalloc(p->arena, sizeof *a);
    int cap = 0, capo = 0, no = 0;
    int start = peek(p)->start;
    for (;;) {
        pipeline *pl = parse_pipeline(p);
        if (!pl) return NULL;
        APUSH(p->arena, a->p, a->n, cap, pl);
        int ty = peek(p)->type;
        if (ty != T_ANDAND && ty != T_OROR) break;
        next(p);
        APUSH(p->arena, a->ops, no, capo, ty == T_ANDAND ? 1 : 2);
        skip_nl(p);
    }
    int end = p->t[p->pos - 1].end;
    a->text = astrndup(p->arena, p->src + start, (size_t)(end - start));
    return a;
}

static list *parse_list(P *p, const char **stops)
{
    list *l = aalloc(p->arena, sizeof *l);
    int cap = 0, capb = 0, nb = 0;
    for (;;) {
        while (peek(p)->type == T_NL || peek(p)->type == T_SEMI)
            next(p);
        tok *t = peek(p);
        if (t->type == T_EOF || is_stop(t, stops)) break;
        if (t->type != T_WORD && t->type != T_REDIR) return fail(p, t);
        andor *a = parse_andor(p);
        if (!a) return NULL;
        int bg = 0;
        t = peek(p);
        if (t->type == T_AMP) { bg = 1; next(p); }
        else if (t->type == T_SEMI || t->type == T_NL) next(p);
        else if (t->type != T_EOF && !is_stop(t, stops)) return fail(p, t);
        APUSH(p->arena, l->items, l->n, cap, a);
        APUSH(p->arena, l->bg, nb, capb, bg);
    }
    return l;
}

int parse_program(const char *src, program *out, char *err, size_t errlen)
{
    chunk *arena = NULL;
    P p = { .src = src, .arena = &arena, .status = P_OK, .err = err, .errlen = errlen };
    if (errlen) err[0] = 0;
    out->l = NULL;
    out->arena = NULL;

    int st = lex(&p);
    if (st != P_OK) {
        program tmp = { NULL, arena };
        free_program(&tmp);
        return st;
    }
    list *l = parse_list(&p, NULL);
    if (l && peek(&p)->type != T_EOF) {
        fail(&p, peek(&p));
        l = NULL;
    }
    if (!l) {
        program tmp = { NULL, arena };
        free_program(&tmp);
        return p.status == P_OK ? P_ERROR : p.status;
    }
    out->l = l;
    out->arena = arena;
    return P_OK;
}

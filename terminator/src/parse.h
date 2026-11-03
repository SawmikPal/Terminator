/* parse.h - lexer and parser for Terminator's shell language.
 *
 * Grammar (a subset of bash):
 *   program   := list
 *   list      := and_or ( (';' | '&' | NEWLINE) and_or )*
 *   and_or    := pipeline ( ('&&' | '||') pipeline )*
 *   pipeline  := command ( '|' command )*
 *   command   := if_clause | simple
 *   if_clause := 'if' list 'then' list ('elif' list 'then' list)* ['else' list] 'fi'
 *   simple    := (NAME=value)* (WORD | redirection)+
 *
 * Words are kept RAW (quotes and $ still in them); expansion happens at
 * run time, so $? and $(...) see the state of the shell at that moment.
 */
#ifndef PARSE_H
#define PARSE_H

#include <stddef.h>

enum { R_IN, R_OUT, R_APPEND, R_DUP };

typedef struct redir {
    int fd;            /* fd being redirected (0, 1, 2 ...)     */
    int type;          /* R_IN, R_OUT, R_APPEND, R_DUP          */
    int dupfd;         /* for R_DUP: 2>&1 -> fd=2, dupfd=1      */
    char *target;      /* raw file word for the others          */
    struct redir *next;
} redir;

struct list;
struct ifnode;

typedef struct cmd {
    char **words; int nw;       /* raw words                        */
    char **assigns; int na;     /* raw NAME=value prefixes          */
    redir *redirs;
    struct ifnode *ifn;         /* non-NULL: this command is an if  */
} cmd;

typedef struct pipeline {
    cmd **cmds; int n;
    char *text;                 /* source text, for job listings    */
} pipeline;

typedef struct andor {
    pipeline **p; int n;
    int *ops;                   /* ops[i] joins p[i] and p[i+1]: 1 = &&, 2 = || */
    char *text;
} andor;

typedef struct list {
    andor **items; int n;
    int *bg;                    /* item ended with '&'              */
} list;

typedef struct ifnode {
    struct list **conds, **bodies; int n;
    struct list *elsebody;
} ifnode;

typedef struct program {
    list *l;
    void *arena;                /* every node lives here            */
} program;

enum { P_OK, P_INCOMPLETE, P_ERROR };

int  parse_program(const char *src, program *out, char *err, size_t errlen);
void free_program(program *p);

/* s[i] is the first character after "$(". Returns the index of the
 * matching ')', or -1 if the substitution is not closed yet. */
int  scan_subst(const char *s, int i);

#endif

/* proto.h - framed messages between the GUI and a tab's shell engine.
 *
 * Every tab has three pipes:
 *   msg  (GUI -> shell)  framed messages: typed lines, Ctrl+C, Ctrl+Z, EOF, close
 *   out  (shell -> GUI)  raw bytes: everything commands and builtins print
 *   ctl  (shell -> GUI)  framed messages: prompt, cwd, exit, bind, clear
 *
 * A frame is 1 byte type + 4 byte length + payload.
 */
#ifndef PROTO_H
#define PROTO_H

#include <stddef.h>

enum {
    /* GUI -> shell */
    M_LINE = 1,   /* one line of input (no trailing newline)          */
    M_INT,        /* Ctrl+C                                            */
    M_TSTP,       /* Ctrl+Z                                            */
    M_EOF,        /* Ctrl+D on an empty line                           */
    M_CLOSE,      /* tab is being closed: kill jobs and exit           */

    /* shell -> GUI */
    C_PROMPT = 20,/* payload = prompt text to show                     */
    C_CWD,        /* payload = new current directory of the tab        */
    C_EXIT,       /* shell finished; close the tab                     */
    C_BIND,       /* payload = "action key" from the bind builtin      */
    C_BINDLIST,   /* list current bindings                             */
    C_CLEAR       /* clear the screen                                  */
};

typedef struct {
    char *buf;
    int len, cap;
} msgbuf;

int  write_all(int fd, const void *buf, size_t n);
int  msg_send(int fd, int type, const char *data, int len);
int  msgbuf_read(msgbuf *m, int fd);      /* >0 bytes read, 0 EOF, -1 nothing/error */
int  msgbuf_next(msgbuf *m, int *type, char **data, int *len); /* 1 if a whole frame */
void msgbuf_free(msgbuf *m);

#endif

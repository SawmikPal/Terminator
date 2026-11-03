#define _GNU_SOURCE
#include "proto.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int msg_send(int fd, int type, const char *data, int len)
{
    if (fd < 0)
        return -1;
    if (len < 0)
        len = data ? (int)strlen(data) : 0;
    char *frame = malloc((size_t)len + 5);
    if (!frame)
        return -1;
    frame[0] = (char)type;
    memcpy(frame + 1, &len, 4);
    if (len)
        memcpy(frame + 5, data, (size_t)len);
    int r = write_all(fd, frame, (size_t)len + 5);
    free(frame);
    return r;
}

int msgbuf_read(msgbuf *m, int fd)
{
    if (m->cap - m->len < 4096) {
        int nc = m->cap ? m->cap * 2 : 8192;
        char *nb = realloc(m->buf, (size_t)nc);
        if (!nb)
            return -1;
        m->buf = nb;
        m->cap = nc;
    }
    for (;;) {
        ssize_t r = read(fd, m->buf + m->len, (size_t)(m->cap - m->len));
        if (r < 0 && errno == EINTR)
            continue;
        if (r > 0)
            m->len += (int)r;
        return (int)r;
    }
}

int msgbuf_next(msgbuf *m, int *type, char **data, int *len)
{
    if (m->len < 5)
        return 0;
    int l;
    memcpy(&l, m->buf + 1, 4);
    if (m->len < 5 + l)
        return 0;
    *type = (unsigned char)m->buf[0];
    *len = l;
    *data = malloc((size_t)l + 1);
    memcpy(*data, m->buf + 5, (size_t)l);
    (*data)[l] = 0;
    memmove(m->buf, m->buf + 5 + l, (size_t)(m->len - 5 - l));
    m->len -= 5 + l;
    return 1;
}

void msgbuf_free(msgbuf *m)
{
    free(m->buf);
    m->buf = NULL;
    m->len = m->cap = 0;
}

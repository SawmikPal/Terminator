/* keys.h - user-configurable hotkeys.
 *
 * Bindings start as bash-like defaults, then ~/.terminatorrc is applied
 * ("bind <action> <key>" per line). The bind builtin changes a binding at
 * run time and appends it to the rc file so it survives a restart.
 */
#ifndef KEYS_H
#define KEYS_H

#include <stddef.h>
#include <X11/Xlib.h>

enum {
    A_NONE = 0,
    A_LEFT, A_RIGHT, A_HOME, A_END,
    A_HIST_PREV, A_HIST_NEXT,
    A_KILL_START, A_KILL_END, A_KILL_WORD,
    A_CLEAR, A_EOF, A_COPY, A_PASTE,
    A_INTERRUPT, A_SUSPEND, A_SEARCH, A_COMPLETE,
    A_NEW_TAB, A_CLOSE_TAB, A_NEXT_TAB, A_PREV_TAB,
    A_SCROLL_UP, A_SCROLL_DOWN,
    A_BACKSPACE, A_DELETE, A_ENTER,
    A_COUNT
};

void keys_init(char *warnings, size_t cap);      /* defaults + ~/.terminatorrc */
int  keys_lookup(unsigned mods, KeySym ks);       /* -> action or A_NONE      */
int  keys_bind(const char *action, const char *spec, int save, char *msg, size_t cap);
void keys_describe(char *buf, size_t cap);

#endif

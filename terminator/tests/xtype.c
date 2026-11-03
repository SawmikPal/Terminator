/* xtype.c - test helper: types into the Terminator window via XSendEvent.
 *
 *   ./tests/xtype 'ls -l{Return}' '{Ctrl+c}' '{Ctrl+Shift+t}' '{Tab}'
 *
 * Plain characters are typed as-is; {Key} or {Mod+Mod+Key} sends one key.
 * Build: gcc -o tests/xtype tests/xtype.c -lX11
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

static Window find(Display *d, Window w)
{
    char *name = NULL;
    if (XFetchName(d, w, &name) && name) {
        int hit = !strncmp(name, "Terminator", 10);
        XFree(name);
        if (hit) return w;
    }
    Window root, parent, *kids = NULL;
    unsigned n = 0;
    if (!XQueryTree(d, w, &root, &parent, &kids, &n)) return 0;
    Window found = 0;
    for (unsigned i = 0; i < n && !found; i++) found = find(d, kids[i]);
    if (kids) XFree(kids);
    return found;
}

static void key(Display *d, Window w, KeySym ks, unsigned mods)
{
    KeyCode kc = XKeysymToKeycode(d, ks);
    if (!kc) { fprintf(stderr, "xtype: no keycode for %lu\n", ks); return; }
    /* use shift if the keysym lives on the shifted level */
    if (XkbKeycodeToKeysym(d, kc, 0, 0) != ks && XkbKeycodeToKeysym(d, kc, 0, 1) == ks) mods |= ShiftMask;
    XKeyEvent e = { .type = KeyPress, .display = d, .window = w, .root = DefaultRootWindow(d),
                    .subwindow = None, .time = CurrentTime, .same_screen = True,
                    .keycode = kc, .state = mods };
    XSendEvent(d, w, True, KeyPressMask, (XEvent *)&e);
    e.type = KeyRelease;
    XSendEvent(d, w, True, KeyReleaseMask, (XEvent *)&e);
    XFlush(d);
    usleep(3000);
}

int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xtype: no display\n"); return 1; }
    Window w = 0;
    for (int tries = 0; tries < 50 && !w; tries++) { w = find(d, DefaultRootWindow(d)); if (!w) usleep(100000); }
    if (!w) { fprintf(stderr, "xtype: window not found\n"); return 1; }
    for (int a = 1; a < argc; a++) {
        const char *s = argv[a];
        while (*s) {
            if (*s == '{') {
                const char *end = strchr(s, '}');
                if (!end) break;
                char spec[64];
                snprintf(spec, sizeof spec, "%.*s", (int)(end - s - 1), s + 1);
                unsigned mods = 0;
                char *save, *last = NULL;
                for (char *t = strtok_r(spec, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
                    if (last) {
                        if (!strcasecmp(last, "Ctrl")) mods |= ControlMask;
                        else if (!strcasecmp(last, "Shift")) mods |= ShiftMask;
                        else if (!strcasecmp(last, "Alt")) mods |= Mod1Mask;
                    }
                    last = t;
                }
                KeySym ks = XStringToKeysym(last);
                if (ks == NoSymbol && strlen(last) == 1) ks = (unsigned char)last[0];
                key(d, w, ks, mods);
                s = end + 1;
            } else {
                char c = *s++;
                KeySym ks;
                if (c == ' ') ks = XK_space;
                else if (c == '\n') ks = XK_Return;
                else {
                    char nm[2] = { c, 0 };
                    ks = XStringToKeysym(nm);
                    if (ks == NoSymbol) ks = (unsigned char)c;
                }
                key(d, w, ks, 0);
            }
        }
    }
    XCloseDisplay(d);
    return 0;
}

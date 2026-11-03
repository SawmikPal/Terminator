#define _GNU_SOURCE
#include "keys.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <X11/keysym.h>

#define MODMASK (ControlMask | ShiftMask | Mod1Mask)
#define MAXK 4

typedef struct { unsigned mods; KeySym ks; } keyspec;

static const struct { const char *name, *desc, *defaults; } A[A_COUNT] = {
    [A_LEFT]       = { "move-left",        "Move cursor left",            "Left" },
    [A_RIGHT]      = { "move-right",       "Move cursor right",           "Right" },
    [A_HOME]       = { "line-start",       "Start of line",               "Ctrl+A|Home" },
    [A_END]        = { "line-end",         "End of line",                 "Ctrl+E|End" },
    [A_HIST_PREV]  = { "history-prev",     "Previous command",            "Up" },
    [A_HIST_NEXT]  = { "history-next",     "Next command",                "Down" },
    [A_KILL_START] = { "delete-to-start",  "Delete to start of line",     "Ctrl+U" },
    [A_KILL_END]   = { "delete-to-end",    "Delete to end of line",       "Ctrl+K" },
    [A_KILL_WORD]  = { "delete-word",      "Delete previous word",        "Ctrl+W" },
    [A_CLEAR]      = { "clear-screen",     "Clear the screen",            "Ctrl+L" },
    [A_EOF]        = { "eof",              "Send EOF / close tab",        "Ctrl+D" },
    [A_COPY]       = { "copy",             "Copy selection",              "Ctrl+Shift+C" },
    [A_PASTE]      = { "paste",            "Paste",                       "Ctrl+Shift+V" },
    [A_INTERRUPT]  = { "interrupt",        "Interrupt job (SIGINT)",      "Ctrl+C" },
    [A_SUSPEND]    = { "suspend",          "Stop job (SIGTSTP)",          "Ctrl+Z" },
    [A_SEARCH]     = { "history-search",   "Search history",              "Ctrl+R" },
    [A_COMPLETE]   = { "complete",         "Complete file name",          "Tab" },
    [A_NEW_TAB]    = { "new-tab",          "Open a new tab",              "Ctrl+Shift+T" },
    [A_CLOSE_TAB]  = { "close-tab",        "Close this tab",              "Ctrl+Shift+W" },
    [A_NEXT_TAB]   = { "next-tab",         "Next tab",                    "Ctrl+Page_Down" },
    [A_PREV_TAB]   = { "prev-tab",         "Previous tab",                "Ctrl+Page_Up" },
    [A_SCROLL_UP]  = { "scroll-up",        "Scroll back",                 "Shift+Page_Up" },
    [A_SCROLL_DOWN]= { "scroll-down",      "Scroll forward",              "Shift+Page_Down" },
    [A_BACKSPACE]  = { "backspace",        "Delete character before",     "BackSpace" },
    [A_DELETE]     = { "delete-char",      "Delete character under",      "Delete" },
    [A_ENTER]      = { "accept-line",      "Run the line",                "Return|KP_Enter" },
};

static keyspec binds[A_COUNT][MAXK];
static int nbind[A_COUNT];
static char rcpath[PATH_MAX];

static KeySym norm(KeySym ks)
{
    if (ks >= XK_A && ks <= XK_Z) return ks - XK_A + XK_a;
    return ks;
}

static const struct { const char *alias, *real; } aliases[] = {
    { "backspace", "BackSpace" }, { "bksp", "BackSpace" }, { "enter", "Return" },
    { "return", "Return" }, { "tab", "Tab" }, { "esc", "Escape" }, { "escape", "Escape" },
    { "up", "Up" }, { "down", "Down" }, { "left", "Left" }, { "right", "Right" },
    { "home", "Home" }, { "end", "End" }, { "pageup", "Page_Up" }, { "pgup", "Page_Up" },
    { "pagedown", "Page_Down" }, { "pgdn", "Page_Down" }, { "delete", "Delete" },
    { "del", "Delete" }, { "space", "space" }, { "insert", "Insert" },
};

static int parse_spec(const char *spec, keyspec *out)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", spec);
    unsigned mods = 0;
    char *save = NULL, *last = NULL;
    for (char *t = strtok_r(tmp, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
        if (last) {
            if (!strcasecmp(last, "ctrl") || !strcasecmp(last, "control")) mods |= ControlMask;
            else if (!strcasecmp(last, "shift")) mods |= ShiftMask;
            else if (!strcasecmp(last, "alt") || !strcasecmp(last, "meta")) mods |= Mod1Mask;
            else return 0;
        }
        last = t;
    }
    if (!last || !*last) return 0;
    KeySym ks = NoSymbol;
    if (strlen(last) == 1) {
        ks = (KeySym)(unsigned char)tolower((unsigned char)last[0]);
    } else {
        for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++)
            if (!strcasecmp(last, aliases[i].alias)) { ks = XStringToKeysym(aliases[i].real); break; }
        if (ks == NoSymbol) ks = XStringToKeysym(last);
    }
    if (ks == NoSymbol) return 0;
    out->mods = mods;
    out->ks = norm(ks);
    return 1;
}

static void spec_str(keyspec k, char *buf, size_t cap)
{
    const char *name = XKeysymToString(k.ks);
    char key[64];
    if (k.ks >= XK_a && k.ks <= XK_z) snprintf(key, sizeof key, "%c", (char)toupper((int)k.ks));
    else snprintf(key, sizeof key, "%s", name ? name : "?");
    snprintf(buf, cap, "%s%s%s%s",
             (k.mods & ControlMask) ? "Ctrl+" : "",
             (k.mods & Mod1Mask) ? "Alt+" : "",
             (k.mods & ShiftMask) ? "Shift+" : "", key);
}

static int action_by_name(const char *n)
{
    for (int a = 1; a < A_COUNT; a++)
        if (A[a].name && !strcasecmp(A[a].name, n)) return a;
    return A_NONE;
}

int keys_lookup(unsigned mods, KeySym ks)
{
    mods &= MODMASK;
    ks = norm(ks);
    for (int a = 1; a < A_COUNT; a++)
        for (int k = 0; k < nbind[a]; k++)
            if (binds[a][k].ks == ks && binds[a][k].mods == mods) return a;
    return A_NONE;
}

int keys_bind(const char *action, const char *spec, int save, char *msg, size_t cap)
{
    int a = action_by_name(action);
    if (!a) {
        snprintf(msg, cap, "bind: unknown action '%s' (bind -l lists them); keeping defaults", action);
        return 0;
    }
    keyspec k;
    if (!parse_spec(spec, &k)) {
        snprintf(msg, cap, "bind: bad key '%s' for %s; keeping %s", spec, action, "the current binding");
        return 0;
    }
    /* A key does one thing: take it away from any other action. */
    char stolen[128] = "";
    for (int b = 1; b < A_COUNT; b++) {
        if (b == a) continue;
        for (int q = 0; q < nbind[b]; q++)
            if (binds[b][q].ks == k.ks && binds[b][q].mods == k.mods) {
                snprintf(stolen, sizeof stolen, " (removed it from %s)", A[b].name);
                binds[b][q] = binds[b][--nbind[b]];
                q--;
            }
    }
    binds[a][0] = k;
    nbind[a] = 1;
    char ks[64];
    spec_str(k, ks, sizeof ks);
    snprintf(msg, cap, "bound %s to %s%s", A[a].name, ks, stolen);
    if (save && rcpath[0]) {
        FILE *f = fopen(rcpath, "ae");
        if (f) {
            fprintf(f, "bind %s %s\n", A[a].name, spec);
            fclose(f);
        }
    }
    return 1;
}

void keys_init(char *warnings, size_t cap)
{
    if (cap) warnings[0] = 0;
    for (int a = 1; a < A_COUNT; a++) {
        char tmp[128];
        snprintf(tmp, sizeof tmp, "%s", A[a].defaults);
        nbind[a] = 0;
        char *save = NULL;
        for (char *t = strtok_r(tmp, "|", &save); t && nbind[a] < MAXK; t = strtok_r(NULL, "|", &save)) {
            keyspec k;
            if (parse_spec(t, &k)) binds[a][nbind[a]++] = k;
        }
    }
    const char *home = getenv("HOME");
    snprintf(rcpath, sizeof rcpath, "%s/.terminatorrc", home ? home : ".");
    FILE *f = fopen(rcpath, "re");
    if (!f) return;
    char line[512];
    int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char cmd[64] = "", act[128] = "", key[128] = "";
        int n = sscanf(p, "%63s %127s %127s", cmd, act, key);
        char msg[256];
        if (n != 3 || strcmp(cmd, "bind") != 0) {
            snprintf(msg, sizeof msg, "~/.terminatorrc:%d: expected 'bind <action> <key>'; line ignored\n", ln);
        } else if (keys_bind(act, key, 0, msg, sizeof msg)) {
            continue;
        } else {
            char m2[300];
            snprintf(m2, sizeof m2, "~/.terminatorrc:%d: %s\n", ln, msg);
            snprintf(msg, sizeof msg, "%s", m2);
        }
        size_t l = strlen(warnings);
        snprintf(warnings + l, cap - l, "%s", msg);
    }
    fclose(f);
}

void keys_describe(char *buf, size_t cap)
{
    size_t l = 0;
    l += (size_t)snprintf(buf + l, cap - l, "%-18s %-26s %s\n", "ACTION", "KEYS", "WHAT IT DOES");
    for (int a = 1; a < A_COUNT && l < cap; a++) {
        char keys[128] = "";
        for (int k = 0; k < nbind[a]; k++) {
            char one[64];
            spec_str(binds[a][k], one, sizeof one);
            if (k) strncat(keys, ", ", sizeof keys - strlen(keys) - 1);
            strncat(keys, one, sizeof keys - strlen(keys) - 1);
        }
        if (!nbind[a]) snprintf(keys, sizeof keys, "(unbound)");
        l += (size_t)snprintf(buf + l, cap - l, "%-18s %-26s %s\n", A[a].name, keys, A[a].desc);
    }
}

# Terminator — design document

## 1. Overview

Terminator has three layers:

```
X server  <--Xlib-->  GUI (gui.c)  <--3 pipes per tab-->  shell engine (shell.c)  --fork/exec-->  commands
```

- **GUI**: one X client, one thread, one `poll()` loop. It owns every tab's
  screen buffer, the input line, history browsing, `Ctrl+R`, completion and
  the clipboard.
- **Shell engine**: one per tab. It parses lines, runs commands, keeps the
  tab's variables, directory and jobs. It never touches X.
- **Commands**: always child processes. Their stdout and stderr are the
  write end of the tab's output pipe.

The engine does not care where text comes from: the same code runs a GUI
tab, a script, `-c`, or stdin (`--headless`). That is what makes scripts and
`$(...)` simple: both are just the engine run on a string.

## 2. GUI and event loop

The loop (`gui_run`) is:

1. Drain every queued X event (`XPending` + `XNextEvent`). Xlib keeps its own
   queue, so `poll()` alone would miss events already read off the socket.
2. Close tabs whose shell has finished.
3. Redraw if anything changed (`dirty` flag).
4. `poll()` on the X connection fd (`ConnectionNumber`) and on each tab's
   output and control pipes, with no timeout. Idle CPU is 0%.
5. For each ready tab, read the output pipe first, then the control pipe.

Reading output before control keeps order correct: a command's output is
written before it exits, and the shell sends the next prompt only after
`waitpid` returns, so by the time a prompt is readable the output is already
in the pipe.

**The screen is a drawing of the buffer.** Each tab has committed lines
(scrollback, 10,000 lines, trimmed in batches), the current unfinished line
(usually the prompt), and the input line. Keys and output only change these;
`render()` redraws the window from them into a back-buffer `Pixmap`, then
copies it to the window (no flicker). Switching tabs only changes which
buffer is drawn, so hidden tabs keep collecting output.

Output goes through a small filter: `\n` commits a line, `\r` restarts it,
`\t` expands to 8 columns, `\b` deletes, ANSI escape sequences are dropped,
non-ASCII bytes become `?` (core fonts are Latin-1).

There is no TTY, so the GUI does the terminal's job: it echoes keys, edits
the line (cursor, `Ctrl+U/K/W/A/E`, history), and only sends a finished line
to the shell on Enter.

Only the main thread ever calls Xlib, so `XInitThreads()` is not needed.

## 3. The two execution modes

| | Thread mode (default) | Process mode (`useProcess=True`) |
|---|---|---|
| A tab is | a `pthread` running `shell_main_interactive` | a `fork()`ed child running the same function |
| GUI ↔ tab | 3 pipes | the same 3 pipes |
| Tab's directory | a string in the engine; **never** `chdir()` | a string, and the process really `chdir()`s |
| Shared history | `MAP_SHARED` memory + pshared semaphore | the same mapping, inherited across `fork()` |
| A crash in a tab | takes the whole process down | only that tab closes |

Using pipes in both modes means the GUI code is identical for both; only
`tab_new()` and `tab_free()` differ (`pthread_create`/`pthread_join` vs
`fork`/`waitpid`). The user cannot tell the modes apart.

**Thread-mode hazards and how they are handled:**

- `chdir()`, `dup2()` and signal dispositions are per process, so they would
  hit every tab. The engine never calls `chdir()` or `dup2()` in thread mode.
  Each tab keeps `cwd` as a string; every child calls `chdir(cwd)` between
  `fork` and `exec`. Paths the engine opens itself (redirections of
  builtins, `source`, scripts, globbing) are made absolute against that
  string (`make_abs`).
- A `fork()` in one tab copies every fd in the process, including other
  tabs' pipes. Every pipe is created `O_CLOEXEC`, so exec closes them. Children
  that keep running engine code instead of exec'ing (subshells, `$(...)`,
  builtins in pipelines, scripts) call `close_range(3, ~0)` first
  (`become_subshell`). Without this, a pipeline's reader could wait forever
  for EOF because another tab's child still held the write end.
- `waitpid(-1)` would steal other tabs' children, so the engine only ever
  waits for its own pids.
- Tab threads block `SIGTSTP`, `SIGTTIN`, `SIGTTOU`; `SIGPIPE` is ignored in
  the Terminator process. Children reset all of these to defaults before exec.

## 4. IPC: the three pipes per tab

| Pipe | Direction | Content |
|---|---|---|
| msg | GUI → shell | framed: `M_LINE` (one line), `M_INT`, `M_TSTP`, `M_EOF`, `M_CLOSE` |
| out | shell + commands → GUI | raw bytes; every command gets it as fd 1 and 2 |
| ctl | shell → GUI | framed: `C_PROMPT`, `C_CWD`, `C_EXIT`, `C_BIND`, `C_BINDLIST`, `C_CLEAR` |

A frame is 1 type byte, a 4-byte length and the payload (`proto.c`).
Output is kept separate from control because commands write raw bytes
directly into the out pipe and would break any framing.

`C_CWD` keeps the GUI's copy of the tab's directory current, for the tab label
and for file-name completion (which runs in the GUI).

## 5. Shared state and synchronisation

**History** (`history.c`) is one `mmap(MAP_SHARED | MAP_ANONYMOUS)` region,
created in `main()` before any tab exists:

```c
typedef struct {
    sem_t lock;                       /* sem_init(&lock, 1, 1): process-shared */
    int start, count;                 /* ring buffer                            */
    char e[10000][1024];
} histmem;
```

Because it is created before the first `fork()`, every tab process inherits
the same physical pages, and in thread mode every thread simply sees it. A
POSIX semaphore with `pshared = 1` works across both threads and processes,
so one lock serves both modes. Every add, read and snapshot takes the lock
briefly. New entries are appended to `~/.terminator_history` inside the lock
(newlines escaped as `\n`), and on start-up the file is reloaded and
trimmed if it has grown well past 10,000 lines.

**Clipboard** lives in the GUI, which is a single thread in a single process
in both modes, so it needs no lock. It is published as the X `CLIPBOARD`
selection, so other applications can paste it too.

**Per-tab state** (directory, variables, jobs, input line, buffer) is never
shared: it lives in the tab's engine struct or the GUI's `tab` struct.

One known edge: in thread mode, if a tab forks while another tab holds the
history semaphore, a child that then runs `history` (only possible as
`history | ...`) could block. The lock is held for microseconds, so this has
not been observed.

## 6. Running commands

- **Parsing** (`parse.c`): a lexer keeps words raw (quotes and `$` intact)
  and reports `P_INCOMPLETE` for an open quote, a trailing `\`, an unclosed
  `$(`, or a parser that hits end of input inside `if`, after `|`, `&&` or
  `||`. The GUI then shows `> ` and the next line is appended. The parser
  builds `list → and_or → pipeline → command` in an arena (one `free` per
  parse).
- **Expansion** happens at run time, per word: `~`, quotes, `$VAR`, `$?`,
  `$#`, `$1..$9`, `$@`, `$$`, `$(...)` (forked engine, output read to EOF,
  trailing newlines stripped), field splitting of unquoted results, then
  globbing relative to the tab's cwd.
- **A single builtin** (`cd`, `exit`, `export`, `jobs`, `fg`...) or an `if`
  runs inside the engine. Its redirections are opened into a local fd table,
  never `dup2`'d onto the engine's own fds.
- **Everything else** is forked: for N commands, N−1 pipes and N children.
  Each child: joins the job's process group, resets signals, moves its
  stdin/stdout/stderr into place (via fds ≥ 10 first so they cannot clobber
  each other), `chdir(cwd)`, applies `<`/`>`/`>>`/`2>&1` with `open`+`dup2`,
  then `execvp` with the tab's exported variables as `environ`. The parent
  closes every pipe end it does not need, so readers see EOF.
- **Scripts**: if the program is `bash`/`sh` with a file argument, or a path
  to a text file (shebang naming `bash`/`sh`, or no shebang and not ELF), the
  child does not exec; it reads the file and runs it through the engine with
  `$0`, `$1..` set. `source` does the same in the current engine, so
  variables persist.

## 7. Jobs and signals

A **job** is one process group: every process of a pipeline calls
`setpgid(0, pgid)`, and the parent calls `setpgid(pid, pgid)` too so there is
no race. The engine keeps a job table per tab:

```c
typedef struct job {
    int id; pid_t pgid;
    int n; pid_t *pids; int *done, *stopped;
    int status, state, bg;
    int jin;          /* write end of the job's stdin pipe */
    char *text;
} job;
```

**Foreground wait** (`wait_fg`) loops: `waitpid(pid, WNOHANG | WUNTRACED |
WCONTINUED)` for each of the job's pids, then `poll()` on the msg pipe for
20 ms. Messages that arrive while a job runs are handled there:

| Message | Action |
|---|---|
| `M_LINE` | written to the job's stdin pipe (so `cat` works) |
| `M_EOF` (`Ctrl+D`) | the stdin pipe is closed |
| `M_INT` (`Ctrl+C`) | `kill(-pgid, SIGINT)` |
| `M_TSTP` (`Ctrl+Z`) | `kill(-pgid, SIGTSTP)` |
| `M_CLOSE` | `SIGHUP`, `SIGCONT`, `SIGKILL` to the group; the engine exits |

The job ends when every process is done (`$?` = last process; `128+n` if
killed by signal n). It is stopped when every live process is stopped:
`[n]+ Stopped cmd` is printed and the prompt returns.

Because there is no TTY, the kernel never generates these signals from keys;
the GUI turns `Ctrl+C`/`Ctrl+Z` into messages, and the engine sends the
signal to that tab's foreground group only. Terminator itself and other tabs'
jobs are never signalled.

- `cmd &`: stdin is `/dev/null`, `[n] pgid` is printed, the engine does not wait.
- `bg %n`: `SIGCONT`, mark background.
- `fg %n`: `SIGCONT`, make it the current job, `wait_fg`.
- `jobs`: lists Running / Stopped / Done.
- Background jobs are polled with `WNOHANG` before each prompt and reported
  as `[n]+ Done cmd` or `Exit n`.
- Closing a tab sends `SIGHUP` + `SIGCONT` (then `SIGKILL`) to all its jobs.

Stopped jobs can be stopped with `SIGTSTP` because their process group is not
orphaned: its parent (Terminator or the tab process) is in another group of
the same session.

## 8. multiWatch

`multiWatch ["cmd1", "cmd2"]` is a builtin. The raw words after the name are
re-joined and parsed as a list of quoted strings, because the list is not
shell syntax. For each command:

1. `pipe2(O_CLOEXEC)`, `fork()`.
2. Child: `setpgid(0,0)`, stdout and stderr → the pipe, then the engine runs
   the command string (so pipes inside a command work).
3. Parent: read end non-blocking, added to a `poll()` set together with the
   tab's msg pipe.

Each chunk read is printed as:

```
"cmd", <unix time>:
----------------------------------------------------
<output>
----------------------------------------------------
```

At EOF the child is reaped and re-run after the interval (2 s default,
`-n` to change, `-n 0` for once). `Ctrl+C` arrives as `M_INT` on the msg
pipe: every group gets `SIGINT` then `SIGKILL`, all are reaped, and the
prompt returns. The commands run in parallel, unlike `watch "a && b"`.

## 9. History search, completion, hotkeys

- **Ctrl+R** (GUI): prints `Enter search term:`, reads a line, then takes a
  snapshot of history under the lock. Exact match → print the most recent
  one (and put it on the input line). Otherwise compute the longest common
  substring with every entry (O(n·m) DP, rolling rows); if the best is > 2,
  print every distinct entry with that length, most recent first; else print
  `No match for search term in history`.
- **Tab** (GUI): takes the word before the cursor, splits it into directory
  and prefix (resolving `~/` and the tab's cwd from `C_CWD`), reads the
  directory, and applies the one / several / none rules. The numbered menu
  waits for a number; any other key closes it.
- **Hotkeys** (`keys.c`): a table `action → up to 4 (modifier mask, keysym)`.
  Keys are matched on the unshifted keysym plus Ctrl/Shift/Alt, so
  `Ctrl+Shift+C` is distinguishable from `Ctrl+C`. Defaults are loaded first,
  then `~/.terminatorrc` line by line; a bad line prints a warning in the
  first tab and leaves the default. The `bind` builtin sends `C_BIND` to the
  GUI, which applies it and appends it to the rc file.

## 10. Data structures (summary)

| Structure | Where | Purpose |
|---|---|---|
| `tab` | gui.c | pipes, scrollback lines, current line, input line, cursor, prompt, cwd copy, scroll, search/completion/selection state |
| `shell` | shell.c | interactive flag, fds, cwd, variables, positional args, `$?`, job table, pending multiline input |
| `job` | shell.c | pgid, pids, per-process done/stopped, stdin pipe, text |
| `program/list/andor/pipeline/cmd/ifnode/redir` | parse.h | parse tree in an arena |
| `histmem` | history.c | shared ring buffer + semaphore |
| `msgbuf` | proto.c | buffered reader for framed messages |
| `binds[action][k]` | keys.c | hotkey table |

## 11. Testing

- `make test` runs 29 engine tests (commands, quoting, redirection, pipes,
  scripts, syntax errors) through `./terminator -c`.
- `tests/xtype.c` sends key events to the window (`XSendEvent`) so the GUI
  can be driven under `Xvfb`: every feature in `examples/DEMO.md` was
  exercised this way in both modes, including with an AddressSanitizer /
  UBSan build.

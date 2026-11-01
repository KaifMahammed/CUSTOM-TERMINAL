# Terminator --- Design Document

## 1. System overview

Terminator is implemented in C in `src/main.c`. It combines an X11
graphical interface with per-tab command execution workers. The program
supports two execution modes selected by the command-line argument
`useProcess=True`.

The main design goals are:

- Keep the X11 event loop responsive while commands execute.
- Maintain independent working-directory and terminal-output state for each tab.
- Support both thread-based and process-based command workers.
- Coordinate shared history and clipboard state safely.
- Implement command execution, pipelines, redirection, and job management.

## 2. X11 GUI and event loop

The application opens the display with `XOpenDisplay`, creates one X11
window, sets window input masks, creates a graphics context, and
registers the window-manager delete protocol.

The main event loop polls both:

1. The X11 connection file descriptor (`ConnectionNumber(display)`).
2. Output file descriptors belonging to tab workers and `multiWatch` commands.

When X11 events are available, the loop dispatches events such as
expose, key press, mouse button press/release, pointer motion, selection
requests, selection notifications, and window close messages. Drawing
functions render the tabs, output lines, prompt, input buffer, and
cursor.

The X11 display and window are owned by the main/UI thread. In thread
mode, worker threads execute commands and communicate output through
pipes; they do not perform Xlib drawing operations.

## 3. Tab state and execution modes

Each tab is represented by a `TerminalTab` structure. It stores the
tab's working directory, input buffer and cursor, output lines and
scroll state, child-process information, command continuation state, and
process-worker descriptors/IDs.

### 3.1 Thread mode

Thread mode is selected when no mode argument is supplied:

``` bash
./terminator
```

The application starts one worker thread per tab slot. Each tab has a
bounded command queue. A mutex protects queue operations, condition
variables wake the relevant worker, and a command-execution mutex
coordinates command execution. History and clipboard state also have
process-local mutexes.

Because the process working directory is global to all threads, each tab
maintains its own `cwd` in its `TerminalTab` state. The `cd` built-in
updates this stored path rather than calling `chdir()`, preserving
tab-level directory independence.

### 3.2 Process mode

Process mode is selected with:

``` bash
./terminator useProcess=True
```

The GUI remains in the parent process. A worker process is started for
each tab. The parent sends a `ProcessCommand` structure containing a
magic value, the tab's working directory, and the command text through a
command pipe. Worker output is returned through a separate pipe and
consumed by the GUI event loop.

The source defines a process-shared state structure for history and
clipboard data. It is created with `shm_open`, sized with `ftruncate`,
mapped with `mmap(..., MAP_SHARED, ...)`, and protected by a pthread
mutex configured with `PTHREAD_PROCESS_SHARED`.

## 4. Shared state and synchronization

The implementation has separate synchronization mechanisms for different
kinds of state:

  -----------------------------------------------------------------------
  | State | Synchronization / sharing |
|---|---|
| Thread-mode command queues | `worker_queue_mutex` and per-tab condition variables |
| Command execution coordination | `command_execution_mutex` |
| Thread-local history | `history_mutex` |
| Thread-local clipboard | `clipboard_mutex` |
| Process-shared history and clipboard | Shared-memory `TerminatorSharedState.mutex` configured as process-shared |
| GUI/X11 state | Owned and updated by the main/UI thread |

The process-shared history structure stores up to `MAX_HISTORY` (10,000)
commands. The history display shows at most the most recent 1,000
commands. The local history is also loaded from and written to
`~/.terminator_history`.

## 5. Command parsing and execution

External commands are launched using `fork()` and `execvp()`. The
implementation maintains per-tab process information and uses process
groups for job control.

Built-in commands are handled within Terminator. The `cd` built-in
updates the selected tab's stored working directory rather than changing
the process-wide current directory.

The source also includes command tokenization, quoting/expansion
support, wildcard expansion using `glob()`, and command handling for
built-ins and external programs.

## 6. IPC, pipes, `dup2`, and redirection

### 6.1 Parent/worker IPC in process mode

The GUI parent communicates with each tab worker using pipes:

- A command pipe carries a `ProcessCommand` record from the parent to the worker.
- An output pipe carries worker output back to the parent.
- The parent polls worker output descriptors as part of its main event loop.

### 6.2 Pipelines

For a pipeline containing multiple commands, the implementation creates
pipes between adjacent commands. In child processes, `dup2()` connects
the previous command's pipe to standard input and the next command's
pipe to standard output. The final command's output is connected to the
terminal output pipe.

### 6.3 Redirection

The implementation opens redirection files and uses `dup2()` to replace
standard input or output. It supports input redirection (`<`), output
redirection (`>`), and append redirection (`>>`). File descriptors that
are no longer needed are closed in the relevant processes.

## 7. Process and job management

The source defines a `Job` structure containing a job ID, process-group
ID, command text, and running/stopped/background state. A fixed-size job
table stores up to `MAX_JOBS` (64) jobs.

External commands are managed as child processes. Process groups are
used so signals can be sent to a complete job. A `SIGCHLD` handler sets
a flag; the main event loop subsequently checks child status and updates
job state.

The built-ins `jobs`, `bg`, and `fg` expose job status and control.
Background execution is supported through the `&` operator. Ctrl+C and
Ctrl+Z are handled as terminal control actions intended to signal or
suspend the relevant foreground job rather than terminate the GUI.

## 8. Command history

History uses a fixed-size array of up to 10,000 command strings. When
full, the oldest entry is removed to make room for a new command. The
`history` built-in prints at most the most recent 1,000 entries.

History is loaded from `~/.terminator_history` at startup and appended
to that file as commands are entered. The shared process-mode history is
held in the POSIX shared-memory structure described above. Access to
history arrays is protected by mutexes.

Ctrl+R enters a search prompt. The search first looks for the most
recent exact match. If there is no exact match, the implementation
computes common-substring lengths and displays entries matching the best
common substring when that length is greater than two; otherwise it
reports no match.

## 9. Filename auto-completion

Filename completion operates on the token immediately before the cursor
and examines entries in the tab's current working directory. A unique
match is completed. For multiple matches, the implementation uses their
common prefix and provides numbered choices, subject to the configured
maximum number of choices. The input buffer and cursor are maintained so
completion can be inserted into the current command line.

## 10. Keyboard editing and clipboard

Hotkey bindings are stored in a `HotkeyBinding` array and initialized
with defaults. The source includes left/right cursor movement, line
start/end, history navigation, deletion to start/end, previous-word
deletion, clear-screen, EOF, and copy/paste bindings.

Text selection is supported with mouse events. Clipboard operations use
X11 selection atoms and selection request/notification events. A shared
clipboard string is also stored in the shared-state structure for the
application's internal sharing path.

## 11. Script interpretation

The source contains a custom script-loading and interpretation path. It
reads a script into memory, checks `if`/`elif`/`else`/`fi` block
structure before executing commands, expands variables and positional
parameters, handles simple variable assignments, and supports `exit`.

The variable expansion code includes `$?`, `$#`, numbered positional
parameters, and environment-variable forms. Test expressions include
string comparisons, integer comparisons, and file/directory checks.

This is a limited shell-like interpreter, not a full Bash parser. The
documented implementation should therefore be described as supporting
the constructs actually implemented and tested, rather than general Bash
compatibility.

## 12. `multiWatch`

The source includes a `multiWatch` feature that starts multiple watched
commands, captures their output through pipes, and periodically starts
subsequent cycles. Each watched command is launched using `/bin/sh -c`.
The watch state records the owning tab, command strings, process IDs,
file descriptors, and running status. Stopping the watch terminates its
child process groups and closes the associated descriptors.

## 13. Important implementation limits and review points

-   The source is a single C translation unit (`src/main.c`); this
    document describes the structures and mechanisms visible in that
    implementation.
-   The script interpreter supports a subset of shell syntax and should
    not be represented as a complete Bash implementation.
-   `multiWatch` intentionally invokes `/bin/sh -c` for each watched
    command.
-  - The current build may emit GCC warnings related to ignored `chdir()` return values and possible filename-completion output truncation. These warnings do not prevent compilation.

-   POSIX shared memory is required for cross-process shared
    history/clipboard. The program prints a warning and falls back to
    process-local state if initialization fails.

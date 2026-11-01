# Terminator — Custom X11 Terminal

## Project Overview

Terminator is a C-based terminal emulator with an X11 graphical interface. The application provides a tabbed terminal window, command input and output, command execution, shell-style editing, command history, job management, pipelines, redirection, filename completion, and script handling.

The implementation in `src/main.c` supports two launch modes:

- **Thread mode (default):** `./terminator`
- **Process mode:** `./terminator useProcess=True`

## Dependencies

The project requires:

- GCC compiler
- GNU Make
- X11 development libraries
- POSIX threads
- Linux or WSL with a working X11 display environment

On Ubuntu or WSL Ubuntu, install the required build dependencies:

```bash
sudo apt update
sudo apt install build-essential make libx11-dev
```

A working X11 display is required at runtime. On WSL, ensure that WSLg or an X server is configured before launching the application.

## Build Instructions

Open a terminal in the project root directory, where the `Makefile` is located.

Build the application:

```bash
make
```

This should generate the executable:

```text
./terminator
```

To rebuild from a clean state:

```bash
make clean
make
```

## Running Terminator

### Thread Mode

Run the application in the default thread mode:

```bash
./terminator
```

### Process Mode

Run the application in process mode:

```bash
./terminator useProcess=True
```

Both modes provide the same general terminal interface while using different approaches to execute commands for individual tabs.

## Implemented Features

The source code includes the following functionality:

- X11 graphical interface with terminal output and multiple tabs.
- Per-tab working-directory state.
- Thread mode with a command worker thread and command queue for each tab.
- Process mode with a worker process per tab and pipe-based command/output communication.
- Built-in commands, including `cd`, `history`, `jobs`, `fg`, `bg`, and `source`.
- External command execution.
- Input/output redirection and append redirection.
- Command pipelines.
- Foreground and background job management.
- Process-group and signal handling.
- Command history with a 10,000-entry limit.
- History display showing up to the most recent 1,000 commands.
- Reverse history search using Ctrl+R.
- History persistence through `~/.terminator_history`.
- Shared history and clipboard state using POSIX shared memory and a process-shared pthread mutex.
- Keyboard shortcuts for cursor movement, editing, history navigation, and clipboard operations.
- Mouse-based text selection and X11 clipboard handling.
- Filename auto-completion, including ambiguous completion choices.
- Script interpretation supporting a subset of shell syntax, including variable expansion, positional parameters, conditional blocks, and script exit.
- `multiWatch` command support.

## Keyboard Shortcuts

The terminal supports the following editing and navigation shortcuts:

| Shortcut | Function |
|---|---|
| Left / Right Arrow | Move the cursor |
| Ctrl+A | Move to the beginning of the line |
| Ctrl+E | Move to the end of the line |
| Up / Down Arrow | Browse command history |
| Ctrl+U | Delete text before the cursor |
| Ctrl+K | Delete text after the cursor |
| Ctrl+W | Delete the previous word |
| Ctrl+L | Clear the terminal screen |
| Ctrl+D | Send EOF or close the shell/tab, depending on context |
| Ctrl+R | Search command history |
| Ctrl+Shift+C | Copy selected text |
| Ctrl+Shift+V | Paste text |
| Tab | Complete a filename |

## Configuration and Runtime Files

- `~/.terminator_history` stores command history.
- Hotkey configuration is loaded at startup; refer to `src/main.c` for the supported configuration syntax and location.
- Shared history and clipboard memory use a POSIX shared-memory object named with the current user ID.

## Known Limitations

- The script interpreter supports a subset of shell syntax and is not a complete Bash implementation.
- `multiWatch` launches watched command strings through `/bin/sh -c`; this is separate from the custom script interpreter.
- An X11 display must be available for the graphical application to start.
- The current build may emit a GCC `-Wformat-truncation` warning in `complete_filename()` when formatting a numbered completion choice. The warning does not necessarily prevent compilation.
- The current build may emit GCC warnings related to ignored `chdir()` return values and possible filename-completion output truncation. These warnings do not prevent compilation.

- Shared-memory initialization may fall back to process-local history and clipboard state if POSIX shared memory cannot be initialized.

## Repository Structure

```text
Terminator/
├── Makefile
├── README.md
├── DESIGN.md
└── src/
    └── main.c
```



# SUBMITTED BY
SHAIK MAHAMMED KAIF
26CS60R17
MTECH CSE

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/file.h>
#define MAX_TABS 16
#define INPUT_SIZE 1024
#define MAX_LINES 1000
#define LINE_SIZE 2048
#define MAX_ARGS 128
#define MAX_HISTORY 10000
#define MAX_JOBS 64
#define MAX_WATCH 16
#define MAX_PIPE_COMMANDS 128
#define MAX_TOKENS 256
#define COMMAND_SIZE 4096
#define WIN_WIDTH 900
#define WIN_HEIGHT 600
#define TAB_HEIGHT 36
#define LINE_HEIGHT 20

typedef struct {
    int used;
    char cwd[PATH_MAX];
    char input[INPUT_SIZE]; size_t input_len; size_t input_cursor;
    char lines[MAX_LINES][LINE_SIZE]; int line_count; int scroll_offset;
    int output_fd; pid_t child_pid; pid_t child_pgid; int child_running;
    int last_status; int exit_requested;
    char output_pending[LINE_SIZE]; size_t pending_len;
    char continuation[COMMAND_SIZE]; size_t continuation_len;
    int continuation_active; int continuation_backslash;
    pid_t child_pids[MAX_PIPE_COMMANDS];
    int child_reaped[MAX_PIPE_COMMANDS]; int child_total; int child_remaining;
    pid_t last_child_pid;
    int process_cmd_fd;
    pid_t process_worker_pid;
} TerminalTab;

typedef struct {
    int used; int id; pid_t pgid; char command[INPUT_SIZE];
    int running; int stopped; int background;
} Job;

typedef struct {
    int active; int count; char commands[MAX_WATCH][INPUT_SIZE];
    pid_t pids[MAX_WATCH]; int fds[MAX_WATCH]; int running[MAX_WATCH];
    time_t next_start; int cycle_started; int owner_tab;
} WatchState;

static Display *display; static Window window; static GC gc; static int screen;
static Pixmap back_buffer;
static int back_buffer_width = 0;
static int back_buffer_height = 0;
static int current_win_width = WIN_WIDTH;
static int current_win_height = WIN_HEIGHT;
static Atom wm_delete; static TerminalTab tabs[MAX_TABS];
static int tab_count = 1, active_tab = 0; static const char *title = "Terminator - A Custom Terminal";
static Job jobs[MAX_JOBS]; static int next_job_id = 1;
static char history[MAX_HISTORY][INPUT_SIZE]; static int history_count = 0;
/* Act 4, Stage 1: protect process-local shared history and clipboard state. */
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clipboard_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Act 4 Stage 2: one command worker per tab slot. The X11 thread remains the
   only thread that calls Xlib. Commands are queued so the UI stays responsive. */
#define TAB_COMMAND_QUEUE 16
static pthread_t tab_workers[MAX_TABS];
static int tab_worker_started[MAX_TABS];
static pthread_mutex_t worker_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_queue_cond[MAX_TABS];
static pthread_mutex_t command_execution_mutex = PTHREAD_MUTEX_INITIALIZER;
static char worker_commands[MAX_TABS][TAB_COMMAND_QUEUE][COMMAND_SIZE];
static int worker_queue_head[MAX_TABS], worker_queue_count[MAX_TABS];
static int worker_shutdown = 0;
/* Stage 3: process-backed command workers. The X11 parent owns the UI and tab metadata. */
static int process_mode = 0;
#define PROCESS_COMMAND_MAGIC 0x5445524dU
typedef struct {
    uint32_t magic;
    int background;
    char cwd[PATH_MAX];
    char command[COMMAND_SIZE];
} ProcessCommand;
static int start_process_worker(TerminalTab *t);
static int dispatch_process_command(TerminalTab *t, const char *cwd,const char *command, int background);
static void *tab_worker_main(void *arg);
static int enqueue_tab_command(int tab_index, const char *command);
static void start_tab_workers(void);
static void stop_tab_workers(void);
static int history_cursor = -1; static int history_search_mode = 0; static WatchState watch_state;
#define MAX_COMPLETION_CHOICES 9
static char completion_choices[MAX_COMPLETION_CHOICES][NAME_MAX + 1];
static int completion_choice_count = 0;
static int completion_choice_tab = -1;
static size_t completion_token_start = 0;
static size_t completion_cursor = 0;
static volatile sig_atomic_t got_sigchld = 0;
static void stop_watch(TerminalTab *t);

typedef enum {
    HK_LEFT, HK_RIGHT, HK_LINE_START, HK_LINE_END, HK_HISTORY_PREV,
    HK_HISTORY_NEXT, HK_DELETE_START, HK_DELETE_END, HK_DELETE_WORD,
    HK_CLEAR_SCREEN, HK_EOF, HK_COPY, HK_PASTE, HK_COUNT
} HotkeyAction;
typedef struct { KeySym keysym; unsigned int modifiers; } HotkeyBinding;
#define HOTKEY_MODS (ControlMask | ShiftMask | Mod1Mask)
static HotkeyBinding hotkeys[HK_COUNT] = {
    [HK_LEFT] = {XK_Left, 0}, [HK_RIGHT] = {XK_Right, 0},
    [HK_LINE_START] = {XK_a, ControlMask}, [HK_LINE_END] = {XK_e, ControlMask},
    [HK_HISTORY_PREV] = {XK_Up, 0}, [HK_HISTORY_NEXT] = {XK_Down, 0},
    [HK_DELETE_START] = {XK_u, ControlMask}, [HK_DELETE_END] = {XK_k, ControlMask},
    [HK_DELETE_WORD] = {XK_w, ControlMask}, [HK_CLEAR_SCREEN] = {XK_l, ControlMask},
    [HK_EOF] = {XK_d, ControlMask}, [HK_COPY] = {XK_c, ControlMask | ShiftMask},
    [HK_PASTE] = {XK_v, ControlMask | ShiftMask}
};
static const char *hotkey_names[HK_COUNT] = {
    "move-left", "move-right", "line-start", "line-end", "history-prev",
    "history-next", "delete-to-start", "delete-to-end", "delete-word",
    "clear-screen", "eof", "copy", "paste"
};
static void reset_hotkeys_to_defaults(void) {
    hotkeys[HK_LEFT] = (HotkeyBinding){XK_Left, 0};
    hotkeys[HK_RIGHT] = (HotkeyBinding){XK_Right, 0};
    hotkeys[HK_LINE_START] = (HotkeyBinding){XK_a, ControlMask};
    hotkeys[HK_LINE_END] = (HotkeyBinding){XK_e, ControlMask};
    hotkeys[HK_HISTORY_PREV] = (HotkeyBinding){XK_Up, 0};
    hotkeys[HK_HISTORY_NEXT] = (HotkeyBinding){XK_Down, 0};
    hotkeys[HK_DELETE_START] = (HotkeyBinding){XK_u, ControlMask};
    hotkeys[HK_DELETE_END] = (HotkeyBinding){XK_k, ControlMask};
    hotkeys[HK_DELETE_WORD] = (HotkeyBinding){XK_w, ControlMask};
    hotkeys[HK_CLEAR_SCREEN] = (HotkeyBinding){XK_l, ControlMask};
    hotkeys[HK_EOF] = (HotkeyBinding){XK_d, ControlMask};
    hotkeys[HK_COPY] = (HotkeyBinding){XK_c, ControlMask | ShiftMask};
    hotkeys[HK_PASTE] = (HotkeyBinding){XK_v, ControlMask | ShiftMask};
}
static char config_warnings[32][LINE_SIZE];
static int config_warning_count = 0;
static char clipboard_text[INPUT_SIZE];

/* Act 4 Stage 4: process-shared history and clipboard. The GUI parent remains
   the owner of X11; this region is shared by concurrent Terminator instances
   and is protected with a process-shared mutex. */
#define SHARED_STATE_MAGIC 0x544d5348U
#define SHARED_STATE_VERSION 1U
typedef struct {
    uint32_t magic;
    uint32_t version;
    pthread_mutex_t mutex;
    int history_count;
    char history[MAX_HISTORY][INPUT_SIZE];
    char clipboard[INPUT_SIZE];
} TerminatorSharedState;
static TerminatorSharedState *shared_state = NULL;
static int shared_state_fd = -1;
static int shared_state_owner = 0;

static int shared_state_init(void);
static void shared_state_close(void);
static void shared_history_add(const char *cmd);
static int shared_history_snapshot(char (*out)[INPUT_SIZE]);
static void shared_history_replace_from_local(void);
static void shared_clipboard_set(const char *text);
static void shared_clipboard_get(char *out, size_t out_size);
static Atom clipboard_atom, utf8_atom, targets_atom, clipboard_property;
static Atom pending_paste_target = None;
static int paste_fallback_attempted = 0;
static int text_selection_valid = 0, text_selection_dragging = 0, text_selection_tab = -1;
static int selection_start_line = 0, selection_start_col = 0;
static int selection_end_line = 0, selection_end_col = 0;

static TerminalTab *current_tab(void) { return &tabs[active_tab]; }
static void on_sigchld(int sig) { (void)sig; got_sigchld = 1; }
static void ensure_back_buffer(void) {
    int width = current_win_width;
    int height = current_win_height;

    if (width < 1) width = 1;
    if (height < 1) height = 1;

    if (back_buffer &&
        back_buffer_width == width &&
        back_buffer_height == height) {
        return;
    }

    if (back_buffer)
        XFreePixmap(display, back_buffer);

    back_buffer = XCreatePixmap(
        display,
        window,
        (unsigned int)width,
        (unsigned int)height,
        (unsigned int)DefaultDepth(display, screen)
    );

    back_buffer_width = width;
    back_buffer_height = height;
}



static void add_line(TerminalTab *tab, const char *text) {
    char copy[LINE_SIZE];
    snprintf(copy, sizeof(copy), "%s", text ? text : "");
    if (tab->line_count >= MAX_LINES) {
        text_selection_valid = 0; text_selection_dragging = 0; text_selection_tab = -1;
        memmove(tab->lines, tab->lines + 1, sizeof(tab->lines[0]) * (MAX_LINES - 1));
        tab->line_count = MAX_LINES - 1;
    }
    memcpy(tab->lines[tab->line_count++], copy, sizeof(copy));
}
static void add_output(TerminalTab *tab, const char *text) {
    for (size_t i = 0; text[i]; i++) {
        char ch = text[i];
        if (ch == '\r') continue;

        if (ch == '\n') {
            tab->output_pending[tab->pending_len] = '\0';

            if (strncmp(tab->output_pending, "\x1eTERM_STATUS:", 13) == 0) {
                tab->last_status = atoi(tab->output_pending + 13);
            } else {
                add_line(tab, tab->output_pending);
            }

            tab->pending_len = 0;
            tab->output_pending[0] = '\0';
            continue;
        }

        if (tab->pending_len < LINE_SIZE - 1) {
            tab->output_pending[tab->pending_len++] = ch;
        } else {
            tab->output_pending[tab->pending_len] = '\0';
            add_line(tab, tab->output_pending);
            tab->pending_len = 0;
            tab->output_pending[tab->pending_len++] = ch;
        }
    }
}
static void finish_pending_output(TerminalTab *tab) {
    if (tab->pending_len) { tab->output_pending[tab->pending_len] = '\0'; add_line(tab, tab->output_pending); tab->pending_len = 0; tab->output_pending[0] = '\0'; }
}
static int resolve_directory(const char *path, char *out, size_t n) {
    char resolved[PATH_MAX]; if (!realpath(path, resolved)) return -1;
    if (strlen(resolved) >= n) { errno = ENAMETOOLONG; return -1; }
    snprintf(out, n, "%s", resolved); return 0;
}
static int make_tab_path(const TerminalTab *t, const char *path, char *out, size_t cap) {
    size_t n = strlen(path);
    if (path[0] == '/') {
        if (n >= cap) { errno = ENAMETOOLONG; return -1; }
        memcpy(out, path, n + 1);
        return 0;
    }
    size_t base = strlen(t->cwd);
    if (base + 1 + n >= cap) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, t->cwd, base);
    out[base] = '/';
    memcpy(out + base + 1, path, n + 1);
    return 0;
}
static void initialize_tab(int i, const char *cwd) {
    TerminalTab *t = &tabs[i]; memset(t, 0, sizeof(*t)); t->used = 1;
    t->output_fd = -1; t->child_pid = t->child_pgid = -1; t->last_status = 0;
    if (cwd) snprintf(t->cwd, sizeof(t->cwd), "%s", cwd);
    else if (!getcwd(t->cwd, sizeof(t->cwd))) snprintf(t->cwd, sizeof(t->cwd), "/");
    t->process_cmd_fd = -1; t->process_worker_pid = -1;
    add_line(t, "Welcome to Terminator."); 
    
    if (process_mode) {
        add_line(t, "Opened in Process Mode");
        if (start_process_worker(t) < 0) add_line(t, "failed to start this tab's process worker.");
    } else {
        add_line(t, "Opened in Thread Mode");
    }
}
static void open_new_tab(void)
{
    if (tab_count >= MAX_TABS) {
        add_line(current_tab(), "Maximum number of tabs reached.");
        return;
    }

    const char *home = getenv("HOME");
    char start_dir[PATH_MAX];

    if (home == NULL || home[0] == '\0') {
        home = "/";
    }

    snprintf(start_dir, sizeof(start_dir), "%s/terminator", home);

    initialize_tab(tab_count, start_dir);
    tab_count++;
    active_tab = tab_count - 1;
}
static void close_current_tab(void) {
    if (!process_mode) {
        pthread_mutex_lock(&command_execution_mutex);
        pthread_mutex_lock(&worker_queue_mutex);
    }
    TerminalTab *t = current_tab();
    text_selection_valid = 0; text_selection_dragging = 0; text_selection_tab = -1;
    /* A watcher belongs to the tab where it was started. Stop it if that tab closes. */
    if (watch_state.active && watch_state.owner_tab == active_tab) stop_watch(t);
    if (tab_count <= 1) {
        add_line(t, "Cannot close the last tab.");
        if (!process_mode) {
            pthread_mutex_unlock(&worker_queue_mutex);
            pthread_mutex_unlock(&command_execution_mutex);
        }
        return;
    }
    if (t->process_cmd_fd >= 0) { close(t->process_cmd_fd); t->process_cmd_fd = -1; }
    if (t->process_worker_pid > 0) { kill(t->process_worker_pid, SIGTERM); (void)waitpid(t->process_worker_pid, NULL, 0); t->process_worker_pid = -1; }
    if (t->output_fd >= 0) close(t->output_fd);
    if (t->child_running && t->child_pgid > 0) kill(-t->child_pgid, SIGTERM);
    if (watch_state.active && watch_state.owner_tab > active_tab) watch_state.owner_tab--;
    for (int i = active_tab; i < tab_count - 1; i++) {
        tabs[i] = tabs[i + 1];
        worker_queue_head[i] = worker_queue_head[i + 1];
        worker_queue_count[i] = worker_queue_count[i + 1];
        memcpy(worker_commands[i], worker_commands[i + 1], sizeof(worker_commands[i]));
        if (!process_mode && worker_queue_count[i] > 0) pthread_cond_signal(&worker_queue_cond[i]);
    }
    tab_count--; memset(&tabs[tab_count], 0, sizeof(tabs[tab_count]));
    worker_queue_head[tab_count] = worker_queue_count[tab_count] = 0;
    memset(worker_commands[tab_count], 0, sizeof(worker_commands[tab_count]));
    if (active_tab >= tab_count) active_tab = tab_count - 1;
    if (!process_mode) {
        pthread_mutex_unlock(&worker_queue_mutex);
        pthread_mutex_unlock(&command_execution_mutex);
    }
}
static void draw_text(int x, int y, const char *s) {
    XDrawString(display, back_buffer, gc, x, y,
                s, (int)strlen(s));
}
/* Measure text with the actual X11 font used by the GC. Do not assume that
   every character is exactly eight pixels wide when positioning the cursor. */
static int text_width(const char *s, size_t len) {
    if (!s || len == 0) return 0;
    if (len > (size_t)INT_MAX) len = (size_t)INT_MAX;
    XFontStruct *font = XQueryFont(display, XGContextFromGC(gc));
    if (!font) return (int)len * 8;
    int width = XTextWidth(font, s, (int)len);
    XFreeFontInfo(NULL, font, 1);
    return width;
}
static void draw_buffer_line(TerminalTab *t, int line_index, int y) {
    const char *line = t->lines[line_index];
    size_t len = strlen(line);
    if (!text_selection_valid || text_selection_tab != active_tab ||
        (selection_start_line == selection_end_line && selection_start_col == selection_end_col)) {
        draw_text(12, y, line); return;
    }
    int sl = selection_start_line, sc = selection_start_col, el = selection_end_line, ec = selection_end_col;
    if (sl > el || (sl == el && sc > ec)) { int q=sl; sl=el; el=q; q=sc; sc=ec; ec=q; }
    if (line_index < sl || line_index > el) { draw_text(12, y, line); return; }
    int a = (line_index == sl) ? sc : 0;
    int b = (line_index == el) ? ec : (int)len;
    if (a < 0) a = 0;
    if (a > (int)len) a = (int)len;
    if (b < 0) b = 0;
    if (b > (int)len) b = (int)len;
    if (b <= a) { draw_text(12, y, line); return; }
    char part[LINE_SIZE]; int prefix = a;
    if (prefix >= LINE_SIZE) prefix = LINE_SIZE - 1;
    memcpy(part, line, (size_t)prefix); part[prefix] = '\0'; draw_text(12, y, part);
    int selected_len = b - a; if (selected_len >= LINE_SIZE) selected_len = LINE_SIZE - 1;
    int sx = 12 + a * 8; XSetForeground(display, gc, BlackPixel(display, screen));
    XFillRectangle(display, back_buffer, gc, sx, y - 13, (unsigned int)(selected_len * 8), 16);
    memcpy(part, line + a, (size_t)selected_len); part[selected_len] = '\0';
    XSetForeground(display, gc, WhitePixel(display, screen)); draw_text(sx, y, part);
    XSetForeground(display, gc, BlackPixel(display, screen)); draw_text(12 + b * 8, y, line + b);
}
static void draw_tabs(void) {
    XSetForeground(display, gc, WhitePixel(display, screen)); XFillRectangle(display, back_buffer, gc, 0, 0, WIN_WIDTH, TAB_HEIGHT);
    for (int i = 0; i < tab_count; i++) {
        char label[128]; snprintf(label, sizeof(label), "%d: %.17s", i + 1, tabs[i].cwd);
        XSetForeground(display, gc, i == active_tab ? BlackPixel(display, screen) : 0x777777);
        XDrawRectangle(display, back_buffer, gc, 5 + i * 170, 4, 160, 27); draw_text(10 + i * 170, 22, label);
    }
    XSetForeground(display, gc, BlackPixel(display, screen)); draw_text(10 + tab_count * 170, 22, "[ + ]");
}
static void draw_screen(void) {
    TerminalTab *t = current_tab();

    int win_w = current_win_width;
    int win_h = current_win_height;

    if (win_w < 200) win_w = 200;
    if (win_h < 200) win_h = 200;

   ensure_back_buffer();

XSetForeground(display, gc, WhitePixel(display, screen));

XFillRectangle(display, back_buffer, gc,
               0, 0,
               (unsigned int)current_win_width,
               (unsigned int)current_win_height);

draw_tabs();

    XSetForeground(display, gc, BlackPixel(display, screen));

    draw_text(12, TAB_HEIGHT + 23,
              "Terminator - A Custom Terminal");

    char prompt[PATH_MAX + 64];

    if (history_search_mode)
        snprintf(prompt, sizeof(prompt), "Enter search term: ");
    else if (t->continuation_active)
        snprintf(prompt, sizeof(prompt), "> ");
    else
        snprintf(prompt, sizeof(prompt), "%s > ", t->cwd);

    if (t->input_cursor > t->input_len)
        t->input_cursor = t->input_len;

    int prompt_width = text_width(prompt, strlen(prompt));
    int input_start_x = 12 + prompt_width;

    /* Calculate the visual position of every input character. */
    int pos_x[INPUT_SIZE];
    int pos_line[INPUT_SIZE];

    int line = 0;
    int x = input_start_x;

    pos_x[0] = x;
    pos_line[0] = 0;

    for (size_t i = 0; i < t->input_len; i++) {
        char ch[2] = { t->input[i], '\0' };
        int cw = text_width(ch, 1);

        if (x + cw > win_w - 15) {
            line++;
            x = 12;
        }

        x += cw;

        pos_x[i + 1] = x;
        pos_line[i + 1] = line;
    }

    int total_input_lines = line + 1;

    /* Determine how many input lines fit in the window. */
    int max_visible_lines =
        (win_h - TAB_HEIGHT - 65) / LINE_HEIGHT;

    if (max_visible_lines < 1)
        max_visible_lines = 1;

    int visible_input_lines = total_input_lines;

    if (visible_input_lines > max_visible_lines)
        visible_input_lines = max_visible_lines;

    /* Keep the cursor visible. */
    int cursor_line = pos_line[t->input_cursor];

    int first_visible_line = 0;

    if (cursor_line >= visible_input_lines)
        first_visible_line =
            cursor_line - visible_input_lines + 1;

    if (first_visible_line + visible_input_lines >
        total_input_lines) {
        first_visible_line =
            total_input_lines - visible_input_lines;
    }

    int input_bottom = win_h - 17;

    int input_first_y =
        input_bottom - (visible_input_lines - 1) * LINE_HEIGHT;

    /* Draw output in the space above the input. */
    int output_top = TAB_HEIGHT + 48;
    int separator_y = input_first_y - 18;

    XDrawLine(display, back_buffer, gc,
              5, separator_y,
              win_w - 5, separator_y);

    int output_visible =
        (separator_y - output_top) / LINE_HEIGHT;

    if (output_visible < 0)
        output_visible = 0;

    int start = t->line_count - output_visible;

    if (start < 0)
        start = 0;

    start -= t->scroll_offset;

    if (start < 0)
        start = 0;

    int end = start + output_visible;

    if (end > t->line_count)
        end = t->line_count;

    int y = output_top;

    for (int i = start; i < end; i++, y += LINE_HEIGHT)
        draw_buffer_line(t, i, y);

    /* Draw the prompt if its first line is visible. */
    if (first_visible_line == 0) {
        draw_text(12, input_first_y, prompt);
    }

    /* Draw the visible portion of the input. */
    for (size_t i = 0; i < t->input_len; i++) {
        int current_line = pos_line[i];

        if (current_line < first_visible_line ||
            current_line >= first_visible_line + visible_input_lines)
            continue;

        char ch[2] = { t->input[i], '\0' };

        int draw_x = pos_x[i];

        int draw_y = input_first_y +
            (current_line - first_visible_line) * LINE_HEIGHT;

        draw_text(draw_x, draw_y, ch);
    }

    /* Draw cursor at its actual visual position. */
    int cursor_x = pos_x[t->input_cursor];

    int cursor_y = input_first_y +
        (cursor_line - first_visible_line) * LINE_HEIGHT;

    XDrawLine(display, back_buffer, gc,
              cursor_x, cursor_y - 13,
              cursor_x, cursor_y + 2);

    XCopyArea(display, back_buffer, window, gc,
          0, 0,
          (unsigned int)win_w,
          (unsigned int)win_h,
          0, 0);

XFlush(display);
}
static int shared_state_init(void) {
    char name[96];
    snprintf(name, sizeof(name), "/terminator_act4_%lu", (unsigned long)getuid());
    shared_state_fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (shared_state_fd < 0) return -1;
    if (flock(shared_state_fd, LOCK_EX) < 0) { close(shared_state_fd); shared_state_fd = -1; return -1; }
    struct stat st;
    if (fstat(shared_state_fd, &st) < 0) { flock(shared_state_fd, LOCK_UN); close(shared_state_fd); shared_state_fd = -1; return -1; }
    if (st.st_size != (off_t)sizeof(TerminatorSharedState)) {
        if (ftruncate(shared_state_fd, (off_t)sizeof(TerminatorSharedState)) < 0) { flock(shared_state_fd, LOCK_UN); close(shared_state_fd); shared_state_fd = -1; return -1; }
        shared_state_owner = 1;
    }
    shared_state = mmap(NULL, sizeof(*shared_state), PROT_READ | PROT_WRITE, MAP_SHARED, shared_state_fd, 0);
    if (shared_state == MAP_FAILED) { shared_state = NULL; flock(shared_state_fd, LOCK_UN); close(shared_state_fd); shared_state_fd = -1; return -1; }
    if (shared_state->magic != SHARED_STATE_MAGIC || shared_state->version != SHARED_STATE_VERSION) {
        memset(shared_state, 0, sizeof(*shared_state));
        pthread_mutexattr_t attr;
        if (pthread_mutexattr_init(&attr) != 0) { munmap(shared_state, sizeof(*shared_state)); shared_state = NULL; flock(shared_state_fd, LOCK_UN); close(shared_state_fd); shared_state_fd = -1; return -1; }
        int rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#ifdef PTHREAD_MUTEX_ROBUST
        if (rc == 0) rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#endif
        if (rc == 0) rc = pthread_mutex_init(&shared_state->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        if (rc != 0) { munmap(shared_state, sizeof(*shared_state)); shared_state = NULL; flock(shared_state_fd, LOCK_UN); close(shared_state_fd); shared_state_fd = -1; return -1; }
        shared_state->version = SHARED_STATE_VERSION;
        __sync_synchronize();
        shared_state->magic = SHARED_STATE_MAGIC;
    }
    (void)msync(shared_state, sizeof(*shared_state), MS_SYNC);
    (void)flock(shared_state_fd, LOCK_UN);
    return 0;
}
static void shared_state_close(void) {
    if (shared_state) { (void)msync(shared_state, sizeof(*shared_state), MS_SYNC); munmap(shared_state, sizeof(*shared_state)); shared_state = NULL; }
    if (shared_state_fd >= 0) { close(shared_state_fd); shared_state_fd = -1; }
}
static void shared_history_add(const char *cmd) {
    if (!shared_state || !cmd || !*cmd) return;
    pthread_mutex_lock(&shared_state->mutex);
    if (shared_state->history_count < 0 || shared_state->history_count > MAX_HISTORY) shared_state->history_count = 0;
    if (shared_state->history_count == MAX_HISTORY) {
        memmove(shared_state->history, shared_state->history + 1, sizeof(shared_state->history[0]) * (MAX_HISTORY - 1));
        shared_state->history_count--;
    }
    snprintf(shared_state->history[shared_state->history_count++], INPUT_SIZE, "%s", cmd);
    pthread_mutex_unlock(&shared_state->mutex);
}
static int shared_history_snapshot(char (*out)[INPUT_SIZE]) {
    if (!shared_state || !out) return -1;
    pthread_mutex_lock(&shared_state->mutex);
    int count = shared_state->history_count;
    if (count < 0 || count > MAX_HISTORY) count = 0;
    memcpy(out, shared_state->history, sizeof(shared_state->history));
    pthread_mutex_unlock(&shared_state->mutex);
    return count;
}
static void shared_history_replace_from_local(void) {
    if (!shared_state) return;
    pthread_mutex_lock(&shared_state->mutex);
    shared_state->history_count = history_count;
    memcpy(shared_state->history, history, sizeof(history));
    pthread_mutex_unlock(&shared_state->mutex);
}
static void shared_clipboard_set(const char *text) {
    if (!shared_state || !text) return;
    pthread_mutex_lock(&shared_state->mutex);
    snprintf(shared_state->clipboard, sizeof(shared_state->clipboard), "%s", text);
    pthread_mutex_unlock(&shared_state->mutex);
}
static void shared_clipboard_get(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!shared_state) return;
    pthread_mutex_lock(&shared_state->mutex);
    snprintf(out, out_size, "%s", shared_state->clipboard);
    pthread_mutex_unlock(&shared_state->mutex);
}

static void history_rewrite_file(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.terminator_history", home) >= (int)sizeof(path)) return;

    /* Take a consistent snapshot, then do file I/O without holding the mutex. */
    char (*snapshot)[INPUT_SIZE] = malloc(sizeof(history));
    if (!snapshot) return;
    pthread_mutex_lock(&history_mutex);
    int count = history_count;
    memcpy(snapshot, history, sizeof(history));
    pthread_mutex_unlock(&history_mutex);

    FILE *f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < count; ++i) fprintf(f, "%s\n", snapshot[i]);
        fclose(f);
        (void)chmod(path, 0600);
    }
    free(snapshot);
}
static void history_add(const char *cmd) {
    if (!cmd || !*cmd) return;

    pthread_mutex_lock(&history_mutex);
    int was_full = (history_count == MAX_HISTORY);
    if (was_full) {
        memmove(history, history + 1, sizeof(history[0]) * (MAX_HISTORY - 1));
        history_count--;
    }
    snprintf(history[history_count++], INPUT_SIZE, "%s", cmd);
    pthread_mutex_unlock(&history_mutex);
    shared_history_add(cmd);

    /* File operations happen outside the lock so other tabs are not blocked. */
    if (was_full) {
        history_rewrite_file();
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.terminator_history", home) >= (int)sizeof(path)) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { dprintf(fd, "%s\n", cmd); close(fd); }
}
static void history_load(void) {
    const char *home = getenv("HOME"); if (!home || !*home) return;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.terminator_history", home) >= (int)sizeof(path)) return;
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[INPUT_SIZE];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        pthread_mutex_lock(&history_mutex);
        if (history_count == MAX_HISTORY) {
            memmove(history, history + 1, sizeof(history[0]) * (MAX_HISTORY - 1));
            history_count--;
        }
        snprintf(history[history_count++], INPUT_SIZE, "%s", line);
        pthread_mutex_unlock(&history_mutex);
    }
    fclose(f);
    shared_history_replace_from_local();
    history_rewrite_file();
}
static size_t common_substring_length(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b), best = 0;
    if (!na || !nb) return 0;
    size_t *prev = calloc(nb + 1, sizeof(*prev));
    size_t *curr = calloc(nb + 1, sizeof(*curr));
    if (!prev || !curr) { free(prev); free(curr); return 0; }
    for (size_t i = 1; i <= na; ++i) {
        for (size_t j = 1; j <= nb; ++j) {
            if (a[i - 1] == b[j - 1]) {
                curr[j] = prev[j - 1] + 1;
                if (curr[j] > best) best = curr[j];
            } else curr[j] = 0;
        }
        size_t *tmp = prev; prev = curr; curr = tmp;
        memset(curr, 0, (nb + 1) * sizeof(*curr));
    }
    free(prev); free(curr);
    return best;
}
static void history_search(TerminalTab *t, const char *term) {
    if (!term || !*term) {
        add_line(t, "No match for search term in history.");
        return;
    }
    char (*snapshot)[INPUT_SIZE] = malloc(sizeof(history));
    if (!snapshot) { add_line(t, "history: out of memory"); return; }
    int count = shared_history_snapshot(snapshot);
    if (count < 0) {
        pthread_mutex_lock(&history_mutex);
        count = history_count;
        memcpy(snapshot, history, sizeof(history));
        pthread_mutex_unlock(&history_mutex);
    }

    for (int i = count - 1; i >= 0; --i) {
        if (strcmp(snapshot[i], term) == 0) {
            add_line(t, snapshot[i]);
            free(snapshot);
            return;
        }
    }
    size_t best = 0;
    for (int i = 0; i < count; ++i) {
        size_t n = common_substring_length(snapshot[i], term);
        if (n > best) best = n;
    }
    if (best <= 2) {
        add_line(t, "No match for search term in history.");
        free(snapshot);
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (common_substring_length(snapshot[i], term) == best) add_line(t, snapshot[i]);
    }
    free(snapshot);
}
static Job *find_job(int id) { for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && jobs[i].id == id) return &jobs[i]; return NULL; }
static Job *add_job(pid_t pgid, const char *cmd, int background) {
    for (int i = 0; i < MAX_JOBS; i++) if (!jobs[i].used) { jobs[i].used = 1; jobs[i].id = next_job_id++; jobs[i].pgid = pgid; jobs[i].running = 1; jobs[i].stopped = 0; jobs[i].background = background; snprintf(jobs[i].command, sizeof(jobs[i].command), "%s", cmd); return &jobs[i]; }
    return NULL;
}
static void reap_jobs(TerminalTab *t) {
    (void)t;
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (!jobs[i].used) continue;
        int status;
        pid_t r;
        while ((r = waitpid(-jobs[i].pgid, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
            if (WIFSTOPPED(status)) { jobs[i].stopped = 1; jobs[i].running = 0; }
            else if (WIFCONTINUED(status)) { jobs[i].stopped = 0; jobs[i].running = 1; }
        }
        if (kill(-jobs[i].pgid, 0) < 0 && errno == ESRCH) jobs[i].used = 0;
    }
}
static int status_code(int status) { if (WIFEXITED(status)) return WEXITSTATUS(status); if (WIFSIGNALED(status)) return 128 + WTERMSIG(status); if (WIFSTOPPED(status)) return 128 + WSTOPSIG(status); return 1; }

static int builtin_cd(TerminalTab *t, int argc, char **argv) {
    const char *home = getenv("HOME");
    const char *target =
        (argc < 2 || strcmp(argv[1], "~") == 0)
            ? home
            : argv[1];

    if (!target) target = "/";

    char expanded[PATH_MAX];

    if (target[0] == '~' && target[1] == '/') {
        snprintf(expanded, sizeof(expanded), "%s/%s",
                 home ? home : "/", target + 2);
    } else {
        snprintf(expanded, sizeof(expanded), "%s", target);
    }

    char combined[PATH_MAX];
    char resolved[PATH_MAX];

    if (make_tab_path(t, expanded, combined, sizeof(combined)) == 0 &&
        resolve_directory(combined, resolved, sizeof(resolved)) == 0) {

        /*
         * Update this tab's directory.
         * Do not call chdir(): each tab maintains its own cwd.
         */
        snprintf(t->cwd, sizeof(t->cwd), "%s", resolved);

        /*
         * PWD is process-wide, so updating it here alone would
         * overwrite the value used by every tab.
         */
        t->last_status = 0;
        return 1;
    }

    char msg[LINE_SIZE];
    snprintf(msg, sizeof(msg), "cd: %s: %s",
             target, strerror(errno));
    add_line(t, msg);
    t->last_status = 1;
    return 1;
}

static void builtin_history(TerminalTab *t) {
    char (*snapshot)[INPUT_SIZE] = malloc(sizeof(history));
    if (!snapshot) { add_line(t, "history: out of memory"); t->last_status = 1; return; }
    int count = shared_history_snapshot(snapshot);
    if (count < 0) {
        pthread_mutex_lock(&history_mutex);
        count = history_count;
        memcpy(snapshot, history, sizeof(history));
        pthread_mutex_unlock(&history_mutex);
    }
    int start = count > 1000 ? count - 1000 : 0;
    for (int i = start; i < count; i++) {
        char line[LINE_SIZE];
        snprintf(line, sizeof(line), "%5d  %s", i + 1, snapshot[i]);
        add_line(t, line);
    }
    free(snapshot);
    t->last_status = 0;
}
static void builtin_jobs(TerminalTab *t) {
    reap_jobs(t); int any = 0;
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used) { char msg[LINE_SIZE]; snprintf(msg, sizeof(msg), "[%d] %s %s", jobs[i].id, jobs[i].stopped ? "Stopped" : (jobs[i].running ? "Running" : "Done"), jobs[i].command); add_line(t, msg); any = 1; }
    if (!any) add_line(t, "No jobs.");
    t->last_status = 0;
}
static int parse_job_id(int argc, char **argv) { if (argc < 2) return -1; const char *p = argv[1]; if (*p == '%') p++; char *end = NULL; long id = strtol(p, &end, 10); return (end && *end == '\0' && id > 0 && id <= INT_MAX) ? (int)id : -1; }
static void builtin_bg(TerminalTab *t, int argc, char **argv) {
    int id = parse_job_id(argc, argv); if (id < 0) { add_line(t, "bg: usage: bg %job"); t->last_status = 1; return; }
    Job *j = find_job(id); if (!j) { add_line(t, "bg: no such job"); t->last_status = 1; return; }
    if (kill(-j->pgid, SIGCONT) < 0) { add_line(t, "bg: could not continue job"); t->last_status = 1; return; }
    j->running = 1; j->stopped = 0; j->background = 1; char msg[LINE_SIZE]; snprintf(msg, sizeof(msg), "[%d]+ %s &", j->id, j->command); add_line(t, msg); t->last_status = 0;
}
static void builtin_fg(TerminalTab *t, int argc, char **argv) {
    int id = parse_job_id(argc, argv); if (id < 0) { add_line(t, "fg: usage: fg %job"); t->last_status = 1; return; }
    Job *j = find_job(id); if (!j) { add_line(t, "fg: no such job"); t->last_status = 1; return; }
    if (kill(-j->pgid, SIGCONT) < 0) { add_line(t, "fg: could not continue job"); t->last_status = 1; return; }
    j->running = 1; j->stopped = 0; j->background = 0; t->child_pid = j->pgid; t->child_pgid = j->pgid; t->child_running = 1;
    char msg[LINE_SIZE]; snprintf(msg, sizeof(msg), "%s", j->command); add_line(t, msg); t->last_status = 0;
}
static void execute_command(TerminalTab *t, const char *raw);
static void handle_pipe_output(TerminalTab *t);

#define SCRIPT_MAX_LINES 2048
#define SCRIPT_LINE_SIZE 4096
#define SCRIPT_MAX_ARGS 64
static int script_mode = 0;

static void wait_script_command(TerminalTab *t) {
    while (t->child_running) {
        if (t->output_fd >= 0) {
            struct pollfd op = { .fd = t->output_fd, .events = POLLIN | POLLHUP, .revents = 0 };
            (void)poll(&op, 1, 50);
            if (op.revents & (POLLIN | POLLHUP | POLLERR)) handle_pipe_output(t);
        } else {
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };
            nanosleep(&pause, NULL);
        }
        for (int i = 0; i < t->child_total; ++i) {
            if (t->child_reaped[i] || t->child_pids[i] <= 0) continue;
            int st = 0; pid_t wr = waitpid(t->child_pids[i], &st, WNOHANG);
            if (wr == t->child_pids[i]) {
                t->child_reaped[i] = 1;
                if (t->child_remaining > 0) t->child_remaining--;
                if (t->child_pids[i] == t->last_child_pid) t->last_status = status_code(st);
            }
        }
        if (t->child_total > 0 && t->child_remaining == 0) {
            t->child_running = 0; t->child_pid = t->child_pgid = -1; t->child_total = 0;
        } else if (t->child_total == 0 && t->child_pid > 0) {
            int st = 0; pid_t wr = waitpid(t->child_pid, &st, WNOHANG);
            if (wr == t->child_pid) { t->last_status = status_code(st); t->child_running = 0; t->child_pid = t->child_pgid = -1; }
        }
    }
    if (t->output_fd >= 0) handle_pipe_output(t);
}

static int script_eval_test(TerminalTab *t, const char *expr) {
    char copy[SCRIPT_LINE_SIZE]; snprintf(copy, sizeof(copy), "%s", expr);
    char *p = copy; while (isspace((unsigned char)*p)) p++;
    size_t n = strlen(p); while (n && (isspace((unsigned char)p[n-1]) || p[n-1] == ';')) p[--n] = 0;
    if (n >= 3 && !strncmp(p, "if ", 3)) p += 3;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '[') p++;
    n = strlen(p); while (n && isspace((unsigned char)p[n-1])) p[--n] = 0;
    if (n && p[n-1] == ']') p[--n] = 0;
    while (isspace((unsigned char)*p)) p++;
    char *tokens[4] = {0}; int nt=0; char *save=NULL;
    for(char *q=strtok_r(p," \t",&save);q&&nt<4;q=strtok_r(NULL," \t",&save)) tokens[nt++]=q;
    if(nt==0)return 0;
    if(nt==1)return strcmp(tokens[0],"0")!=0 && tokens[0][0]!=0;
    if((!strcmp(tokens[0],"-f")||!strcmp(tokens[0],"-d")||!strcmp(tokens[0],"-e"))&&nt>=2){
        char *patharg=tokens[1]; size_t z=strlen(patharg); if(z>=2&&((patharg[0]=='"'&&patharg[z-1]=='"')||(patharg[0]=='\''&&patharg[z-1]=='\''))){patharg[z-1]=0;patharg++;}
        char path[PATH_MAX];struct stat st;if(make_tab_path(t,patharg,path,sizeof(path))<0||stat(path,&st)<0)return 0;
        if(!strcmp(tokens[0],"-f")) return S_ISREG(st.st_mode);
        if(!strcmp(tokens[0],"-d")) return S_ISDIR(st.st_mode);
        return 1;
    }
    if(nt>=3){char *a=tokens[0],*op=tokens[1],*b=tokens[2];
        size_t z=strlen(a);if(z>=2&&((a[0]=='"'&&a[z-1]=='"')||(a[0]=='\''&&a[z-1]=='\''))){a[z-1]=0;a++;}
        z=strlen(b);if(z>=2&&((b[0]=='"'&&b[z-1]=='"')||(b[0]=='\''&&b[z-1]=='\''))){b[z-1]=0;b++;}
        if(!strcmp(op,"=")||!strcmp(op,"==")) return !strcmp(a,b);
        if(!strcmp(op,"!=")) return strcmp(a,b)!=0;
        if(!strcmp(op,"-eq")||!strcmp(op,"-ne")||!strcmp(op,"-gt")||!strcmp(op,"-ge")||!strcmp(op,"-lt")||!strcmp(op,"-le")){char *ea=NULL,*eb=NULL;long x=strtol(a,&ea,10),y=strtol(b,&eb,10);if(!ea||*ea||!eb||*eb)return 0;if(!strcmp(op,"-eq"))return x==y;if(!strcmp(op,"-ne"))return x!=y;if(!strcmp(op,"-gt"))return x>y;if(!strcmp(op,"-ge"))return x>=y;if(!strcmp(op,"-lt"))return x<y;return x<=y;}
    }
    return 0;
}

static void script_expand_line(const char *src, char *dst, size_t cap, int argc, char args[SCRIPT_MAX_ARGS][LINE_SIZE], int last_status) {
    size_t used=0;
    for (size_t i=0; src[i] && used+1<cap;) {
        if (src[i]=='$') {
            char val[LINE_SIZE]=""; size_t j=i+1;
            if (src[j]=='?') { snprintf(val,sizeof(val),"%d",last_status); j++; }
            else if (src[j]=='#') { snprintf(val,sizeof(val),"%d",argc); j++; }
            else if (isdigit((unsigned char)src[j])) { int k=src[j++]-'0'; if(k>0 && k<=argc) snprintf(val,sizeof(val),"%s",args[k-1]); }
            else if (src[j]=='{') { size_t st=++j; while(src[j] && src[j]!='}') j++; size_t nn=j-st; if(nn<128){char name[128];memcpy(name,src+st,nn);name[nn]=0;const char *v=getenv(name);if(v)snprintf(val,sizeof(val),"%s",v);} if(src[j]=='}')j++; }
            else if (isalpha((unsigned char)src[j]) || src[j]=='_') { size_t st=j++; while(isalnum((unsigned char)src[j])||src[j]=='_')j++; size_t nn=j-st; if(nn<128){char name[128];memcpy(name,src+st,nn);name[nn]=0;const char*v=getenv(name);if(v)snprintf(val,sizeof(val),"%s",v);} }
            else { dst[used++]=src[i++]; continue; }
            size_t vn=strlen(val); if(vn>cap-used-1)vn=cap-used-1; memcpy(dst+used,val,vn);used+=vn;i=j;
        } else dst[used++]=src[i++];
    }
    dst[used]=0;
}

static int script_is_assignment(const char *line, char *name, size_t ncap, char *value, size_t vcap) {
    const char *eq=strchr(line,'='); if(!eq || eq==line) return 0;
    for(const char *p=line;p<eq;p++) if(!(isalnum((unsigned char)*p)||*p=='_') || (p==line && isdigit((unsigned char)*p))) return 0;
    size_t n=(size_t)(eq-line); if(n>=ncap)return 0; memcpy(name,line,n);name[n]=0;snprintf(value,vcap,"%s",eq+1);return 1;
}

static int run_script(TerminalTab *t, const char *filename, int argc, char **argv) {
    char path[PATH_MAX]; if(make_tab_path(t,filename,path,sizeof(path))<0){add_line(t,"bash: script path too long");t->last_status=1;return 1;}
    FILE *f=fopen(path,"r"); if(!f){char msg[LINE_SIZE];snprintf(msg,sizeof(msg),"%s: %s",filename,strerror(errno));add_line(t,msg);t->last_status=127;return 1;}
    char (*lines)[SCRIPT_LINE_SIZE] = calloc(SCRIPT_MAX_LINES, sizeof(*lines));
    if (!lines) { fclose(f); add_line(t, "bash: out of memory loading script"); t->last_status=1; return 1; }
    int nlines=0; char buf[SCRIPT_LINE_SIZE];
    while(fgets(buf,sizeof(buf),f) && nlines<SCRIPT_MAX_LINES){buf[strcspn(buf,"\r\n")]=0;snprintf(lines[nlines++],SCRIPT_LINE_SIZE,"%s",buf);} fclose(f);
    if(nlines>=SCRIPT_MAX_LINES){free(lines);add_line(t,"bash: script too large");t->last_status=2;return 1;}
    /* Validate block structure before executing any command. */
    int depth=0, seen_else[128]={0};
    for(int i=0;i<nlines;i++){char tmp[SCRIPT_LINE_SIZE];snprintf(tmp,sizeof(tmp),"%s",lines[i]);char *p=tmp;while(isspace((unsigned char)*p))p++; if(!*p||*p=='#')continue;
        if(!strncmp(p,"if ",3)){if(depth>=127){add_line(t,"bash: syntax error: too many nested if blocks");t->last_status=2;free(lines);return 1;}seen_else[depth++]=0;}
        else if(!strncmp(p,"elif ",5)){if(depth==0||seen_else[depth-1]){add_line(t,"bash: syntax error near elif");t->last_status=2;free(lines);return 1;}}
        else if(!strcmp(p,"else")||!strcmp(p,"else;")){if(depth==0||seen_else[depth-1]){add_line(t,"bash: syntax error near else");t->last_status=2;free(lines);return 1;}seen_else[depth-1]=1;}
        else if(!strcmp(p,"fi")||!strcmp(p,"fi;")){if(depth==0){add_line(t,"bash: syntax error near fi");t->last_status=2;free(lines);return 1;}depth--;}
    }
    if(depth){free(lines);add_line(t,"bash: syntax error: missing fi");t->last_status=2;return 1;}
    char (*args)[LINE_SIZE] = calloc(SCRIPT_MAX_ARGS, sizeof(*args));
    if (!args) { free(lines); add_line(t, "bash: out of memory loading script arguments"); t->last_status=1; return 1; }
    int ac=argc<SCRIPT_MAX_ARGS?argc:SCRIPT_MAX_ARGS;
    for(int i=0;i<ac;i++)snprintf(args[i],LINE_SIZE,"%s",argv[i+1]);
    typedef struct {int parent_active,active,taken,else_seen;} IfState; IfState stack[128]; int d=0, active=1, status=t->last_status;
    for(int i=0;i<nlines;i++){
        char rawline[SCRIPT_LINE_SIZE];snprintf(rawline,sizeof(rawline),"%s",lines[i]);char *p=rawline;while(isspace((unsigned char)*p))p++;if(!*p||*p=='#'||!strncmp(p,"#!",2))continue;
        size_t ln=strlen(p);while(ln&&isspace((unsigned char)p[ln-1]))p[--ln]=0;if(!*p)continue;
        if(!strncmp(p,"if ",3)){
            int parent=active, cond=0; if(parent){char ex[SCRIPT_LINE_SIZE];script_expand_line(p+3,ex,sizeof(ex),ac,args,t->last_status);char *then=strstr(ex,"; then");if(then)*then=0;else {then=strstr(ex," then");if(then)*then=0;}cond=script_eval_test(t,ex);}
            stack[d++]=(IfState){parent,parent&&cond,parent&&cond,0};active=stack[d-1].active;continue;
        }
        if(!strncmp(p,"elif ",5)){IfState *q=&stack[d-1];int cond=0;if(q->parent_active&&!q->taken){char ex[SCRIPT_LINE_SIZE];script_expand_line(p+5,ex,sizeof(ex),ac,args,t->last_status);char *then=strstr(ex,"; then");if(then)*then=0;else{then=strstr(ex," then");if(then)*then=0;}cond=script_eval_test(t,ex);}q->active=q->parent_active&&!q->taken&&cond;q->taken|=q->active;active=q->active;continue;}
        if(!strcmp(p,"else")||!strcmp(p,"else;")){IfState*q=&stack[d-1];q->active=q->parent_active&&!q->taken;q->taken=1;q->else_seen=1;active=q->active;continue;}
        if(!strcmp(p,"fi")||!strcmp(p,"fi;")){d--;active=d?stack[d-1].active:1;continue;}
        if(!active)continue;
        /* Allow the common single-line form: if [ test ]; then command */
        if(!strncmp(p,"if ",3)) continue;
        char expanded[SCRIPT_LINE_SIZE];script_expand_line(p,expanded,sizeof(expanded),ac,args,t->last_status);char *q=expanded;while(isspace((unsigned char)*q))q++;
        char name[256], value[SCRIPT_LINE_SIZE]; if(script_is_assignment(q,name,sizeof(name),value,sizeof(value)) && !strchr(q,' ')) { if(setenv(name,value,1)<0){add_line(t,"bash: unable to set variable");status=1;} else status=0;t->last_status=status;continue; }
        if(!strncmp(q,"exit",4)&&(q[4]==0||isspace((unsigned char)q[4]))){int code=q[4]?atoi(q+5):t->last_status;t->last_status=code;status=code;break;}
        execute_command(t,q);wait_script_command(t);status=t->last_status;
        if(t->exit_requested)break;
    }
    t->last_status=status;free(args);free(lines);return 1;
}

static int status_code(int status);
static void builtin_source(TerminalTab *t, int argc, char **argv) {
    if (argc < 2) { add_line(t, "source: filename required"); t->last_status = 2; return; }
    char path[PATH_MAX];
    if (make_tab_path(t, argv[1], path, sizeof(path)) < 0) { add_line(t, "source: path is too long"); t->last_status = 1; return; }
    FILE *f = fopen(path, "r"); if (!f) { char msg[LINE_SIZE]; snprintf(msg, sizeof(msg), "source: %s: %s", argv[1], strerror(errno)); add_line(t, msg); t->last_status = 1; return; }
    char line[INPUT_SIZE]; int status = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        execute_command(t, p);
        /* source executes each line in order; wait for an external command before continuing. */
        while (t->child_running) {
            if (t->output_fd >= 0) {
                struct pollfd op = { .fd = t->output_fd, .events = POLLIN | POLLHUP, .revents = 0 };
                (void)poll(&op, 1, 50);
                if (op.revents & (POLLIN | POLLHUP | POLLERR)) handle_pipe_output(t);
            } else { struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L }; nanosleep(&pause, NULL); }
            int child_status = 0;
            pid_t wr = waitpid(t->child_pid, &child_status, WNOHANG);
            if (wr == t->child_pid) {
                t->last_status = status_code(child_status);
                t->child_running = 0; t->child_pid = -1; t->child_pgid = -1;
            }
        }
        status = t->last_status;
        if (t->exit_requested) break;
    }
    fclose(f); t->last_status = status;
}
static int split_mult_watch(const char *s, char out[MAX_WATCH][INPUT_SIZE]) {
    /* Expected syntax: multiWatch ["cmd1", "cmd2"]. Handles quoted command strings. */
    const char *p = strchr(s, '['); if (!p) return 0; p++; int count = 0;
    while (*p && *p != ']' && count < MAX_WATCH) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (*p == ']') break;
        char q = *p; if (q != '\'' && q != '"') return 0; p++; size_t n = 0;
        while (*p && *p != q && n < INPUT_SIZE - 1) { if (*p == '\\' && p[1]) p++; out[count][n++] = *p++; }
        out[count][n] = 0; if (*p != q) return 0; p++; if (n) count++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') p++;
    }
    return count;
}
static void stop_watch(TerminalTab *t) {
    pid_t pids[MAX_WATCH];
    int count = watch_state.count;

    if (count > MAX_WATCH)
        count = MAX_WATCH;

    /* Save process IDs before resetting watch_state. */
    for (int i = 0; i < count; i++)
        pids[i] = watch_state.pids[i];

    /* Signal all watched process groups first. */
    for (int i = 0; i < count; i++) {
        if (pids[i] > 0)
            (void)kill(-pids[i], SIGTERM);
    }

    /* Give children a short grace period without blocking the GUI. */
    for (int attempt = 0; attempt < 10; attempt++) {
        int remaining = 0;

        for (int i = 0; i < count; i++) {
            if (pids[i] <= 0)
                continue;

            int status;
            pid_t r = waitpid(pids[i], &status, WNOHANG);

            if (r == pids[i] || (r < 0 && errno == ECHILD)) {
                pids[i] = 0;
            } else {
                remaining++;
            }
        }

        if (remaining == 0)
            break;

        struct timespec pause = {
            .tv_sec = 0,
            .tv_nsec = 10000000L
        };
        nanosleep(&pause, NULL);
    }

    /* Force-stop any remaining watched process groups. */
    for (int i = 0; i < count; i++) {
        if (pids[i] > 0)
            (void)kill(-pids[i], SIGKILL);
    }

    /* Reap without waiting indefinitely. */
    for (int i = 0; i < count; i++) {
        if (pids[i] > 0)
            (void)waitpid(pids[i], NULL, WNOHANG);
    }

    for (int i = 0; i < MAX_WATCH; i++) {
        if (watch_state.fds[i] >= 0)
            close(watch_state.fds[i]);
    }

    memset(&watch_state, 0, sizeof(watch_state));
    for (int i = 0; i < MAX_WATCH; i++)
        watch_state.fds[i] = -1;

    add_line(t, "^C");
    t->last_status = 130;
}
static void start_watch_cycle(TerminalTab *t) {
    watch_state.cycle_started = 1;
    for (int i = 0; i < watch_state.count; i++) {
        int pfd[2]; if (pipe(pfd) < 0) { add_line(t, "multiWatch: pipe failed"); continue; }
        pid_t pid = fork(); if (pid < 0) { close(pfd[0]); close(pfd[1]); continue; }
        if (pid == 0) { setpgid(0, 0); close(pfd[0]); dup2(pfd[1], 1); dup2(pfd[1], 2); if (pfd[1] > 2) close(pfd[1]); if (chdir(t->cwd) < 0) _exit(126); execl("/bin/sh", "sh", "-c", watch_state.commands[i], (char *)NULL); _exit(127); }
        setpgid(pid, pid); close(pfd[1]); int fl = fcntl(pfd[0], F_GETFL, 0); if (fl >= 0) fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);
        watch_state.pids[i] = pid; watch_state.fds[i] = pfd[0]; watch_state.running[i] = 1;
        char hdr[LINE_SIZE]; snprintf(hdr, sizeof(hdr), "\"%s\", %ld:", watch_state.commands[i], (long)time(NULL)); add_line(t, hdr); add_line(t, "----------------------------------------------------");
    }
}
static void builtin_multiwatch(TerminalTab *t, const char *raw) {
    if (watch_state.active) { add_line(t, "multiWatch is already active; press Ctrl+C to stop it."); return; }
    memset(&watch_state, 0, sizeof(watch_state)); for (int i = 0; i < MAX_WATCH; i++) watch_state.fds[i] = -1;
    watch_state.count = split_mult_watch(raw, watch_state.commands);
    if (watch_state.count == 0) { add_line(t, "multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\"]"); t->last_status = 2; return; }
    watch_state.owner_tab = active_tab;
    watch_state.active = 1;
    watch_state.next_start = 0;
    start_watch_cycle(t); t->last_status = 0;
}
/* ---------- Tasks 3-5: input continuation, expansion, redirection and pipelines ---------- */
typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *input_path;
    char *output_path;
    int append_output;
} ParsedCommand;

static int is_shell_operator(char c) {
    return c == '|' || c == '<' || c == '>' || c == '&';
}

static int append_text(char *dst, size_t cap, size_t *len, const char *src) {
    size_t n = strlen(src);
    if (*len + n >= cap) return -1;
    memcpy(dst + *len, src, n);
    *len += n;
    dst[*len] = '\0';
    return 0;
}

static int run_command_substitution(TerminalTab *t, const char *cmd, char *out, size_t cap) {
    int pfd[2];
    if (pipe(pfd) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        if (dup2(pfd[1], STDOUT_FILENO) < 0) _exit(126);
        close(pfd[1]);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) { (void)dup2(nullfd, STDIN_FILENO); close(nullfd); }
        if (chdir(t->cwd) < 0) _exit(126);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    size_t used = 0;
    char discard[1024];
    for (;;) {
        char *dest = used + 1 < cap ? out + used : discard;
        size_t room = used + 1 < cap ? cap - used - 1 : sizeof(discard);
        ssize_t n = read(pfd[0], dest, room);
        if (n > 0) { if (dest == out + used) used += (size_t)n; }
        else if (n == 0) break;
        else if (errno == EINTR) continue;
        else break;
    }
    out[used] = '\0';
    close(pfd[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    /* Bash removes all trailing newlines from command-substitution output. */
    while (used > 0 && out[used - 1] == '\n') out[--used] = '\0';
    return 0;
}

static const char *find_substitution_end(const char *p) {
    /* p points at the '$(' opening. Count nested parentheses outside quotes. */
    int depth = 1, quote = 0;
    p += 2;
    for (; *p; ++p) {
        if (*p == '\\' && quote != '\'') { if (p[1]) ++p; continue; }
        if (quote == '\'') { if (*p == '\'') quote = 0; continue; }
        if (quote == '"') {
            if (*p == '"') quote = 0;
            else if (*p == '$' && p[1] == '(') { depth++; ++p; }
            continue;
        }
        if (*p == '\'') quote = '\'';
        else if (*p == '"') quote = '"';
        else if (*p == '$' && p[1] == '(') { depth++; ++p; }
        else if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) return p;
    }
    return NULL;
}

static int lex_shell(TerminalTab *t, const char *input,
                     char tokens[MAX_TOKENS][LINE_SIZE], int *token_count, int glob_flags[MAX_TOKENS]) {
    const char *p = input;
    int count = 0;
    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (count >= MAX_TOKENS) return -1;

        if (is_shell_operator(*p)) {
            size_t n = 1;
            if (*p == '>' && p[1] == '>') n = 2;
            if (*p == '&' && p[1] == '&') n = 2;
            if (*p == '|' && p[1] == '|') n = 2;
            memcpy(tokens[count], p, n); tokens[count][n] = '\0'; glob_flags[count] = 0;
            ++count; p += n; continue;
        }

        char word[LINE_SIZE] = "";
        size_t used = 0;
        int quote = 0, started = 0, word_glob = 0;
        while (*p) {
            if (!quote && (isspace((unsigned char)*p) || is_shell_operator(*p))) break;
            started = 1;
            if (quote == '\'') {
                if (*p == '\'') { quote = 0; ++p; }
                else { if (used + 1 >= sizeof(word)) return -1; word[used++] = *p++; word[used] = 0; }
                continue;
            }
            if (*p == '\\' && quote != '\'') {
                if (!p[1]) { if (used + 1 >= sizeof(word)) return -1; word[used++] = *p++; word[used] = 0; continue; }
                if (quote == '"' && p[1] != '"' && p[1] != '\\' && p[1] != '$' && p[1] != '`') {
                    if (used + 2 >= sizeof(word)) return -1;
                    word[used++] = *p++; word[used++] = *p++; word[used] = 0;
                } else {
                    if (used + 1 >= sizeof(word)) return -1;
                    word[used++] = p[1]; word[used] = 0; p += 2;
                }
                continue;
            }
            if (quote == 0 && *p == '\'') { quote = '\''; ++p; continue; }
            if (*p == '"') { quote = (quote == '"') ? 0 : (quote == 0 ? '"' : quote); ++p; continue; }

            if (*p == '`' && quote != '\'') {
                const char *end = p + 1;
                while (*end && (*end != '`' || (end > p + 1 && end[-1] == '\\'))) ++end;
                if (!*end) { /* unmatched backtick is handled by continuation reader */
                    if (used + 1 >= sizeof(word)) return -1;
                    word[used++] = *p++; word[used] = 0; continue;
                }
                size_t inner_len = (size_t)(end - (p + 1));
                char inner[LINE_SIZE], sub[LINE_SIZE];
                if (inner_len >= sizeof(inner)) return -1;
                memcpy(inner, p + 1, inner_len); inner[inner_len] = 0;
                if (run_command_substitution(t, inner, sub, sizeof(sub)) < 0) return -1;
                if (append_text(word, sizeof(word), &used, sub) < 0) return -1;
                p = end + 1; continue;
            }
            if (*p == '$' && p[1] == '(') {
                const char *end = find_substitution_end(p);
                if (!end) { if (used + 1 >= sizeof(word)) return -1; word[used++] = *p++; word[used] = 0; continue; }
                size_t inner_len = (size_t)(end - (p + 2));
                char inner[LINE_SIZE], sub[LINE_SIZE];
                if (inner_len >= sizeof(inner)) return -1;
                memcpy(inner, p + 2, inner_len); inner[inner_len] = 0;
                if (run_command_substitution(t, inner, sub, sizeof(sub)) < 0) return -1;
                if (append_text(word, sizeof(word), &used, sub) < 0) return -1;
                p = end + 1; continue;
            }
            if (*p == '$' && p[1] == '?') {
                char status[32]; snprintf(status, sizeof(status), "%d", t->last_status);
                if (append_text(word, sizeof(word), &used, status) < 0) return -1;
                p += 2; continue;
            }
            if (*p == '$' && (isalpha((unsigned char)p[1]) || p[1] == '_' || p[1] == '{')) {
                char name[256]; size_t nn = 0;
                if (p[1] == '{') {
                    p += 2;
                    while (*p && *p != '}' && nn + 1 < sizeof(name)) name[nn++] = *p++;
                    if (*p == '}') ++p;
                } else {
                    ++p;
                    while ((isalnum((unsigned char)*p) || *p == '_') && nn + 1 < sizeof(name)) name[nn++] = *p++;
                }
                
name[nn] = 0;

const char *value;

if (strcmp(name, "PWD") == 0) {
    /* PWD must reflect the active tab, not the process environment. */
    value = t->cwd;
} else {
    value = getenv(name);
}

if (!value) value = "";

if (append_text(word, sizeof(word), &used, value) < 0) return -1;
continue;

            }
            if (quote == 0 && strchr("*?[", *p)) word_glob = 1;
            if (used + 1 >= sizeof(word)) return -1;
            word[used++] = *p++; word[used] = 0;
        }
        if (quote != 0) return -2; /* caller should only execute completed input */
        if (started) {
            word[used] = '\0';
            snprintf(tokens[count], LINE_SIZE, "%s", word); glob_flags[count] = word_glob; count++;
        }
    }
    *token_count = count;
    return 0;
}


static int expand_glob_tokens(TerminalTab *t, char tokens[MAX_TOKENS][LINE_SIZE],
                              int glob_flags[MAX_TOKENS], int *token_count) {
    char expanded[MAX_TOKENS][LINE_SIZE];
    int expanded_flags[MAX_TOKENS];
    int count = 0;
    char saved_cwd[PATH_MAX];
    int can_restore = getcwd(saved_cwd, sizeof(saved_cwd)) != NULL;
    for (int i = 0; i < *token_count; ++i) {
        if (!glob_flags[i] || !strpbrk(tokens[i], "*?[")) {
            if (count >= MAX_TOKENS) return -1;
            size_t copy_len = strnlen(tokens[i], LINE_SIZE - 1);
            memcpy(expanded[count], tokens[i], copy_len); expanded[count][copy_len] = '\0';
            expanded_flags[count++] = glob_flags[i];
            continue;
        }
        glob_t g; memset(&g, 0, sizeof(g));
        int changed = can_restore && (chdir(t->cwd) == 0);
        int gr = changed ? glob(tokens[i], 0, NULL, &g) : GLOB_NOMATCH;
        if (changed && can_restore) (void)chdir(saved_cwd);
        if (gr == 0 && g.gl_pathc > 0) {
            for (size_t j = 0; j < g.gl_pathc; ++j) {
                if (count >= MAX_TOKENS) { globfree(&g); return -1; }
                snprintf(expanded[count], LINE_SIZE, "%s", g.gl_pathv[j]);
                expanded_flags[count++] = 0;
            }
        } else {
            if (count >= MAX_TOKENS) { globfree(&g); return -1; }
            size_t copy_len = strnlen(tokens[i], LINE_SIZE - 1);
            memcpy(expanded[count], tokens[i], copy_len); expanded[count][copy_len] = '\0';
            expanded_flags[count++] = 0;
        }
        globfree(&g);
    }
    for (int i = 0; i < count; ++i) {
        snprintf(tokens[i], LINE_SIZE, "%s", expanded[i]);
        glob_flags[i] = expanded_flags[i];
    }
    *token_count = count;
    return 0;
}

static int ends_with_unescaped_backslash(const char *s) {
    size_t n = strlen(s), count = 0;
    while (n > 0 && s[n - 1] == '\\') { ++count; --n; }
    return (count % 2) != 0;
}

static int command_is_incomplete(const char *s, int *trailing_backslash) {
    int quote = 0;
    size_t n = strlen(s);
    int slash = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '\\' && quote != '\'') {
            if (i + 1 == n) { slash = 1; break; }
            ++i; continue;
        }
        if (quote == '\'') { if (c == '\'') quote = 0; continue; }
        if (quote == '"') {
            if (c == '"') { quote = 0; continue; }
            if (c == '$' && i + 1 < n && s[i+1] == '(') {
                const char *end = find_substitution_end(s + i);
                if (!end) { if (trailing_backslash) *trailing_backslash = ends_with_unescaped_backslash(s); return 1; }
                i = (size_t)(end - s); continue;
            }
            if (c == '`') {
                size_t j = i + 1;
                for (; j < n; ++j) if (s[j] == '`' && (j == 0 || s[j-1] != '\\')) break;
                if (j == n) { if (trailing_backslash) *trailing_backslash = ends_with_unescaped_backslash(s); return 1; }
                i = j; continue;
            }
            continue;
        }
        if (c == '\'') quote = '\'';
        else if (c == '"') quote = '"';
        else if (c == '`') {
            size_t j = i + 1;
            for (; j < n; ++j) if (s[j] == '`' && (j == 0 || s[j-1] != '\\')) break;
            if (j == n) { if (trailing_backslash) *trailing_backslash = ends_with_unescaped_backslash(s); return 1; }
            i = j;
        } else if (c == '$' && i + 1 < n && s[i+1] == '(') {
            const char *end = find_substitution_end(s + i);
            if (!end) { if (trailing_backslash) *trailing_backslash = ends_with_unescaped_backslash(s); return 1; }
            i = (size_t)(end - s);
        }
    }
    if (trailing_backslash) *trailing_backslash = slash;
    return slash || quote != 0;
}

static int parse_pipeline_tokens(char tokens[MAX_TOKENS][LINE_SIZE], int ntok,
                                 ParsedCommand cmds[MAX_PIPE_COMMANDS],
                                 int *ncmd_out, int *background_out) {
    int ncmd = 1, background = 0;
    memset(cmds, 0, sizeof(ParsedCommand) * MAX_PIPE_COMMANDS);
    if (ntok > 0 && strcmp(tokens[ntok - 1], "&") == 0) { background = 1; ntok--; }
    for (int i = 0; i < ntok; ++i) {
        char *tok = tokens[i];
        if (strcmp(tok, "|") == 0) {
            if (cmds[ncmd - 1].argc == 0 || ncmd >= MAX_PIPE_COMMANDS) return -1;
            ncmd++; continue;
        }
        if (strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
            int is_input = strcmp(tok, "<") == 0;
            int append = strcmp(tok, ">>") == 0;
            if (++i >= ntok || is_shell_operator(tokens[i][0])) return -1;
            if (is_input) cmds[ncmd - 1].input_path = tokens[i];
            else { cmds[ncmd - 1].output_path = tokens[i]; cmds[ncmd - 1].append_output = append; }
            continue;
        }
        if (strcmp(tok, "&&") == 0 || strcmp(tok, "||") == 0 || strcmp(tok, "&") == 0) return -1;
        if (cmds[ncmd - 1].argc >= MAX_ARGS - 1) return -1;
        cmds[ncmd - 1].argv[cmds[ncmd - 1].argc++] = tok;
    }
    if (cmds[ncmd - 1].argc == 0) return -1;
    for (int i = 0; i < ncmd; ++i) cmds[i].argv[cmds[i].argc] = NULL;
    *ncmd_out = ncmd; *background_out = background;
    return 0;
}

static int read_full_fd(int fd, void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return got == 0 ? 0 : -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 1;
}

static int write_full_fd(int fd, const void *buf, size_t n)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

static volatile sig_atomic_t process_worker_pgid = -1;
static void process_worker_signal(int sig)
{
    pid_t pg = (pid_t)process_worker_pgid;
    if (pg > 0) (void)kill(-pg, sig);
}

static int start_process_worker(TerminalTab *t)
{
    int cmdpipe[2] = {-1, -1}, outpipe[2] = {-1, -1};
    if (pipe(cmdpipe) < 0) return -1;
    if (pipe(outpipe) < 0) { close(cmdpipe[0]); close(cmdpipe[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(cmdpipe[0]); close(cmdpipe[1]); close(outpipe[0]); close(outpipe[1]); return -1; }
    if (pid == 0) {
        close(cmdpipe[1]); close(outpipe[0]);
        if (dup2(outpipe[1], STDOUT_FILENO) < 0 || dup2(outpipe[1], STDERR_FILENO) < 0) _exit(126);
        close(outpipe[1]);
        long maxfd = sysconf(_SC_OPEN_MAX); if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
        for (int fd = 3; fd < maxfd; ++fd) if (fd != cmdpipe[0]) close(fd);
        struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = process_worker_signal; sigemptyset(&sa.sa_mask);
        (void)sigaction(SIGINT, &sa, NULL); (void)sigaction(SIGTERM, &sa, NULL); (void)sigaction(SIGTSTP, &sa, NULL);
        for (;;) {
    ProcessCommand msg;
    int rr = read_full_fd(cmdpipe[0], &msg, sizeof(msg));
    if (rr <= 0) break;
        if (msg.magic != PROCESS_COMMAND_MAGIC) continue;

    /* Handle job-control built-ins inside the process worker. */
    reap_jobs(NULL);

    char job_command[COMMAND_SIZE];
    snprintf(job_command, sizeof(job_command), "%s", msg.command);

    char *job_saveptr = NULL;
    char *job_name = strtok_r(job_command, " \t\r\n", &job_saveptr);

    if (job_name && strcmp(job_name, "jobs") == 0) {
        int found = 0;

        for (int i = 0; i < MAX_JOBS; ++i) {
            if (jobs[i].used) {
                dprintf(STDOUT_FILENO, "[%d] %s %s\n",
                        jobs[i].id,
                        jobs[i].stopped ? "Stopped" : "Running",
                        jobs[i].command);
                found = 1;
            }
        }

        if (!found)
            dprintf(STDOUT_FILENO, "No jobs\n");

        continue;
    }
        if (job_name && strcmp(job_name, "bg") == 0) {
        char *arg = strtok_r(NULL, " \t\r\n", &job_saveptr);

        if (!arg) {
            dprintf(STDERR_FILENO, "bg: usage: bg %%job\n");
            continue;
        }

        if (*arg == '%') arg++;

        char *end = NULL;
        long id = strtol(arg, &end, 10);

        if (!end || *end != '\0' || id <= 0 || id > INT_MAX) {
            dprintf(STDERR_FILENO, "bg: invalid job ID\n");
            continue;
        }

        Job *job = find_job((int)id);

        if (!job) {
            dprintf(STDERR_FILENO, "bg: no such job\n");
            continue;
        }

        if (kill(-job->pgid, SIGCONT) < 0) {
            dprintf(STDERR_FILENO, "bg: could not continue job\n");
            continue;
        }

        job->running = 1;
        job->stopped = 0;
        job->background = 1;

        dprintf(STDOUT_FILENO, "[%d]+ %s &\n",
                job->id, job->command);
        continue;
    }

        if (job_name && strcmp(job_name, "fg") == 0) {
        char *arg = strtok_r(NULL, " \t\r\n", &job_saveptr);

        if (!arg) {
            dprintf(STDERR_FILENO, "fg: usage: fg %%job\n");
            continue;
        }

        if (*arg == '%') arg++;

        char *end = NULL;
        long id = strtol(arg, &end, 10);

        if (!end || *end != '\0' || id <= 0 || id > INT_MAX) {
            dprintf(STDERR_FILENO, "fg: invalid job ID\n");
            continue;
        }

        Job *job = find_job((int)id);

        if (!job) {
            dprintf(STDERR_FILENO, "fg: no such job\n");
            continue;
        }

        job->background = 0;
        job->running = 1;
        job->stopped = 0;

        if (kill(-job->pgid, SIGCONT) < 0) {
            dprintf(STDERR_FILENO, "fg: could not continue job\n");
            continue;
        }

        process_worker_pgid = (sig_atomic_t)job->pgid;

        int status = 0;
        pid_t result;

        do {
            result = waitpid(-job->pgid, &status, WUNTRACED);
        } while (result < 0 && errno == EINTR);

        process_worker_pgid = -1;

        if (result > 0 && WIFSTOPPED(status)) {
            job->stopped = 1;
            job->running = 0;
            job->background = 1;
            dprintf(STDOUT_FILENO, "[%d]+ Stopped %s\n",
                    job->id, job->command);
        } else {
            job->used = 0;
        }

        continue;
    }
    pid_t cpid = fork();

    if (cpid < 0) {
        dprintf(STDERR_FILENO,
                "terminator: process worker fork failed: %s\n",
                strerror(errno));
        continue;
    }

    if (cpid == 0) {
        (void)setpgid(0, 0);

        if (chdir(msg.cwd) < 0) {
            dprintf(STDERR_FILENO, "chdir: %s\n", strerror(errno));
            _exit(126);
        }

        execl("/bin/sh", "sh", "-c", msg.command, (char *)NULL);

        dprintf(STDERR_FILENO,
                "terminator: cannot execute shell: %s\n",
                strerror(errno));
        _exit(127);
    }

    (void)setpgid(cpid, cpid);

        if (msg.background) {
        reap_jobs(NULL);

        Job *job = add_job(cpid, msg.command, 1);

        if (job != NULL) {
            dprintf(STDOUT_FILENO, "[%d] %ld\n",
                    job->id, (long)cpid);
        } else {
            dprintf(STDERR_FILENO,
                    "terminator: job table is full\n");
            (void)kill(-cpid, SIGTERM);
            (void)waitpid(cpid, NULL, 0);
        }

        /* Background process continues independently. */
        continue;
    }

   process_worker_pgid = (sig_atomic_t)cpid;

int st = 0;
pid_t waited;

do {
    waited = waitpid(cpid, &st, 0);
} while (waited < 0 && errno == EINTR);

process_worker_pgid = -1;

int command_status = 1;

if (waited == cpid) {
    command_status = status_code(st);
}

dprintf(STDOUT_FILENO, "\x1eTERM_STATUS:%d\n", command_status);
}
        close(cmdpipe[0]); _exit(0);
    }
    close(cmdpipe[0]); close(outpipe[1]);
    int fl = fcntl(outpipe[0], F_GETFL, 0);
    if (fl >= 0) (void)fcntl(outpipe[0], F_SETFL, fl | O_NONBLOCK);
    t->process_cmd_fd = cmdpipe[1]; t->process_worker_pid = pid; t->output_fd = outpipe[0];
    return 0;
}

static int dispatch_process_command(TerminalTab *t, const char *cwd,
                                    const char *command, int background)
{
    if (!t || t->process_cmd_fd < 0 || t->process_worker_pid <= 0) {
        if (t) add_line(t, "Process mode: tab worker is unavailable.");
        return -1;
    }

    ProcessCommand msg;
    memset(&msg, 0, sizeof(msg));

    msg.magic = PROCESS_COMMAND_MAGIC;
    msg.background = background;

    snprintf(msg.cwd, sizeof(msg.cwd), "%s", cwd ? cwd : "/");
    snprintf(msg.command, sizeof(msg.command), "%s", command ? command : "");

    if (write_full_fd(t->process_cmd_fd, &msg, sizeof(msg)) < 0) {
        add_line(t, "Process mode: failed to send command to tab worker.");
        return -1;
    }

    return 0;
}

static void start_pipeline(TerminalTab *t, const char *raw,
                           ParsedCommand cmds[MAX_PIPE_COMMANDS], int ncmd, int background) {
    if (process_mode) {
    (void)cmds;
    (void)ncmd;

    if (dispatch_process_command(t, t->cwd, raw, background) == 0) {
        if (background) {
            t->last_status = 0;
        }
        /* Foreground status will be updated when the worker
           reports the actual command exit status. */
    } else {
        t->last_status = 1;
    }

    return;
}
    int pipes[MAX_PIPE_COMMANDS - 1][2];
    int output_pipe[2];
    pid_t pids[MAX_PIPE_COMMANDS] = {0};
    int made = 0;
    for (int i = 0; i < ncmd - 1; ++i) {
        if (pipe(pipes[i]) < 0) {
            for (int j = 0; j < made; ++j) { close(pipes[j][0]); close(pipes[j][1]); }
            add_line(t, "terminator: pipe failed"); t->last_status = 1; return;
        }
        made++;
    }
    if (pipe(output_pipe) < 0) {
        for (int j = 0; j < made; ++j) { close(pipes[j][0]); close(pipes[j][1]); }
        add_line(t, "terminator: output pipe failed"); t->last_status = 1; return;
    }
    pid_t pgid = 0;
    int fork_failed = 0;
    for (int i = 0; i < ncmd; ++i) {
        pid_t pid = fork();
        if (pid < 0) { fork_failed = 1; break; }
        if (pid == 0) {
            if (pgid == 0) setpgid(0, 0); else setpgid(0, pgid);
            close(output_pipe[0]);
            /* All child stderr is routed back into the X11 output buffer. */
            if (dup2(output_pipe[1], STDERR_FILENO) < 0) _exit(126);
            if (i > 0 && dup2(pipes[i-1][0], STDIN_FILENO) < 0) _exit(126);
            if (i < ncmd - 1 && dup2(pipes[i][1], STDOUT_FILENO) < 0) _exit(126);
            if (i == ncmd - 1 && dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(126);
            for (int j = 0; j < made; ++j) { close(pipes[j][0]); close(pipes[j][1]); }
            close(output_pipe[1]);
            if (chdir(t->cwd) < 0) { dprintf(STDERR_FILENO, "chdir: %s\n", strerror(errno)); _exit(126); }
            if (cmds[i].input_path) {
                int fd = open(cmds[i].input_path, O_RDONLY);
                if (fd < 0) { dprintf(STDERR_FILENO, "%s: %s\n", cmds[i].input_path, strerror(errno)); _exit(1); }
                if (dup2(fd, STDIN_FILENO) < 0) { dprintf(STDERR_FILENO, "dup2: %s\n", strerror(errno)); _exit(126); }
                close(fd);
            } else if (i == 0) {
                int nullfd = open("/dev/null", O_RDONLY);
                if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); close(nullfd); }
            }
            if (cmds[i].output_path) {
                int flags = O_WRONLY | O_CREAT | (cmds[i].append_output ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].output_path, flags, 0666);
                if (fd < 0) { dprintf(STDERR_FILENO, "%s: %s\n", cmds[i].output_path, strerror(errno)); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) < 0) { dprintf(STDERR_FILENO, "dup2: %s\n", strerror(errno)); _exit(126); }
                close(fd);
            }
            execvp(cmds[i].argv[0], cmds[i].argv);
            dprintf(STDERR_FILENO, "terminator: %s: %s\n", cmds[i].argv[0], errno == ENOENT ? "command not found" : strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }
        if (pgid == 0) pgid = pid;
        (void)setpgid(pid, pgid);
        pids[i] = pid;
    }
    for (int j = 0; j < made; ++j) { close(pipes[j][0]); close(pipes[j][1]); }
    close(output_pipe[1]);
    if (fork_failed) {
        if (pgid > 0) kill(-pgid, SIGTERM);
        close(output_pipe[0]);
        for (int i = 0; i < ncmd; ++i) if (pids[i] > 0) waitpid(pids[i], NULL, 0);
        add_line(t, "terminator: fork failed"); t->last_status = 1; return;
    }
    int fl = fcntl(output_pipe[0], F_GETFL, 0);
    if (fl >= 0) (void)fcntl(output_pipe[0], F_SETFL, fl | O_NONBLOCK);
    if (t->output_fd >= 0) { finish_pending_output(t); close(t->output_fd); }
    t->output_fd = output_pipe[0];
    if (background) {
        Job *j = add_job(pgid, raw, 1);
        char msg[LINE_SIZE];
        if (j) snprintf(msg, sizeof(msg), "[%d] %ld", j->id, (long)pgid);
        else snprintf(msg, sizeof(msg), "Started background process group %ld", (long)pgid);
        add_line(t, msg);
        t->child_running = 0; t->child_pid = t->child_pgid = -1;
        t->child_total = t->child_remaining = 0;
        return;
    }
    t->child_running = 1; t->child_pgid = pgid; t->child_pid = pids[ncmd - 1];
    t->last_child_pid = pids[ncmd - 1]; t->child_total = ncmd; t->child_remaining = ncmd;
    for (int i = 0; i < ncmd; ++i) { t->child_pids[i] = pids[i]; t->child_reaped[i] = 0; }
}
static int valid_variable_name(const char *name, size_t len) {
    if (len == 0 ||
        !(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        return 0;
    }

    for (size_t i = 1; i < len; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return 0;
        }
    }

    return 1;
}


static int handle_variable_command(TerminalTab *t, const char *command) {
    char tokens[MAX_TOKENS][LINE_SIZE];
    int glob_flags[MAX_TOKENS] = {0};
    int ntok = 0;

    /*
     * Use the existing shell lexer so assignments and export arguments
     * follow the same tokenization rules as other commands.
     */
    int lex_result = lex_shell(t, command, tokens, &ntok, glob_flags);

    if (lex_result < 0 || ntok == 0) {
        return 0;
    }

    /* Handle: NAME=value */
    if (strcmp(tokens[0], "export") != 0) {
        char *eq = strchr(tokens[0], '=');

        if (!eq) return 0;

        size_t name_len = (size_t)(eq - tokens[0]);

        if (!valid_variable_name(tokens[0], name_len)) {
            return 0;
        }

        if (ntok != 1) {
            add_line(t, "Variable assignment: unexpected extra arguments.");
            t->last_status = 2;
            return 1;
        }

        char name[LINE_SIZE];

        if (name_len >= sizeof(name)) {
            add_line(t, "Variable name is too long.");
            t->last_status = 2;
            return 1;
        }

        memcpy(name, tokens[0], name_len);
        name[name_len] = '\0';

        if (setenv(name, eq + 1, 1) != 0) {
            add_line(t, "Variable assignment failed.");
            t->last_status = 1;
        } else {
            t->last_status = 0;
        }

        return 1;
    }

    /* Handle: export NAME=value or export NAME */
    if (ntok < 2) {
        add_line(t, "export: variable name required");
        t->last_status = 2;
        return 1;
    }

    for (int i = 1; i < ntok; i++) {
        char *eq = strchr(tokens[i], '=');

        size_t name_len = eq
            ? (size_t)(eq - tokens[i])
            : strlen(tokens[i]);

        if (!valid_variable_name(tokens[i], name_len)) {
            add_line(t, "export: invalid variable name");
            t->last_status = 2;
            return 1;
        }

        char name[LINE_SIZE];

        if (name_len >= sizeof(name)) {
            add_line(t, "export: variable name is too long");
            t->last_status = 2;
            return 1;
        }

        memcpy(name, tokens[i], name_len);
        name[name_len] = '\0';

        if (eq) {
            if (setenv(name, eq + 1, 1) != 0) {
                add_line(t, "export: assignment failed");
                t->last_status = 1;
                return 1;
            }
        } else if (!getenv(name)) {
            if (setenv(name, "", 1) != 0) {
                add_line(t, "export: failed");
                t->last_status = 1;
                return 1;
            }
        }
    }

    t->last_status = 0;
    return 1;
}

static void execute_command(TerminalTab *t, const char *raw) {
    char command[COMMAND_SIZE];
    snprintf(command, sizeof(command), "%s", raw ? raw : "");
    size_t len = strlen(command);
    while (len && isspace((unsigned char)command[len - 1])) command[--len] = '\0';
    size_t begin = 0; while (command[begin] && isspace((unsigned char)command[begin])) begin++;
    if (begin) memmove(command, command + begin, strlen(command + begin) + 1);
    if (!*command) return;

    /* Interpret script files internally for ./script.sh, bash script.sh, and source script.sh. */
    if (!script_mode) {
        char dispatch[COMMAND_SIZE]; snprintf(dispatch, sizeof(dispatch), "%s", command);
        char *av[8] = {0}; int ac = 0; char *saveptr = NULL;
        for (char *tok = strtok_r(dispatch, " \t", &saveptr); tok && ac < 7; tok = strtok_r(NULL, " \t", &saveptr)) av[ac++] = tok;
        if (ac > 0) {
            int is_script = 0; const char *file = NULL; int first_arg = 0;
            if ((!strcmp(av[0], "bash") || !strcmp(av[0], "source") || !strcmp(av[0], ".")) && ac >= 2) { file=av[1]; first_arg=1; is_script=1; }
            else if (av[0][0]=='.' && av[0][1]=='/' && strstr(av[0], ".sh")) { file=av[0]; first_arg=0; is_script=1; }
            if (is_script) {
                script_mode=1; int handled=run_script(t,file,ac-first_arg,av+first_arg); script_mode=0;
                if (handled) return;
            }
        }
    }

    char prompt[LINE_SIZE]; snprintf(prompt, sizeof(prompt), "%.700s > %.1300s", t->cwd, command); add_line(t, prompt);
    history_add(command); history_cursor = -1;
        if (handle_variable_command(t, command)) {
        return;
    }
    if (strncasecmp(command, "multiWatch", 10) == 0 &&
    (command[10] == '\0' || isspace((unsigned char)command[10]))) {
    builtin_multiwatch(t, command);
    return;
}

    char tokens[MAX_TOKENS][LINE_SIZE]; int glob_flags[MAX_TOKENS] = {0}; int ntok = 0;
    int lex_result = lex_shell(t, command, tokens, &ntok, glob_flags);
    if (lex_result == -2) { add_line(t, "terminator: incomplete quote; press Enter after completing the command"); t->last_status = 2; return; }
    if (lex_result < 0) { add_line(t, "terminator: command is too long or has too many tokens"); t->last_status = 2; return; }
    if (ntok == 0) { t->last_status = 0; return; }
    if (expand_glob_tokens(t, tokens, glob_flags, &ntok) < 0) { add_line(t, "terminator: too many expanded arguments"); t->last_status = 2; return; }

    ParsedCommand cmds[MAX_PIPE_COMMANDS]; int ncmd = 0, background = 0;
    if (parse_pipeline_tokens(tokens, ntok, cmds, &ncmd, &background) < 0) {
        add_line(t, "terminator: syntax error near pipe, redirection, or background marker"); t->last_status = 2; return;
    }

    /* Preserve shell-state built-ins in the parent when no pipe/redirection is present. */
    ParsedCommand *c = &cmds[0];
    int has_redirect = c->input_path != NULL || c->output_path != NULL;
    if (ncmd == 1 && !has_redirect && !background) {
    int argc = c->argc;
    char **argv = c->argv;

    if (!strcmp(argv[0], "cd")) {
        builtin_cd(t, argc, argv);
        return;
    }
    if (!strcmp(argv[0], "exit")) {
        t->exit_requested = 1;
        t->last_status = argc > 1 ? atoi(argv[1]) : t->last_status;
        return;
    }
    if (!strcmp(argv[0], "history")) {
        builtin_history(t);
        return;
    }

    /* Thread mode handles job-control built-ins in the parent.
       Process mode must send them to its worker. */
    if (!process_mode) {
        if (!strcmp(argv[0], "jobs")) {
            builtin_jobs(t);
            return;
        }
        if (!strcmp(argv[0], "bg")) {
            builtin_bg(t, argc, argv);
            return;
        }
        if (!strcmp(argv[0], "fg")) {
            builtin_fg(t, argc, argv);
            return;
        }
    }

    if (!strcmp(argv[0], "source") || !strcmp(argv[0], ".")) {
        builtin_source(t, argc, argv);
        return;
    }
    if (!strcmp(argv[0], "true")) {
        t->last_status = 0;
        return;
    }
    if (!strcmp(argv[0], "false")) {
        t->last_status = 1;
        return;
    }
}
    if (t->child_running && !background) { add_line(t, "A foreground command is already running in this tab."); t->last_status = 1; return; }
    start_pipeline(t, command, cmds, ncmd, background);
}
static void handle_pipe_output(TerminalTab *t) {
    if (t->output_fd < 0) return;
    char buf[4096];
    for (;;) {
        ssize_t n = read(t->output_fd, buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = 0; add_output(t, buf); continue; }
        if (n == 0) { finish_pending_output(t); close(t->output_fd); t->output_fd = -1; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        finish_pending_output(t); close(t->output_fd); t->output_fd = -1; break;
    }
}
static void *tab_worker_main(void *arg)
{
    int slot = (int)(intptr_t)arg;
    for (;;) {
        pthread_mutex_lock(&worker_queue_mutex);
        while (!worker_shutdown && worker_queue_count[slot] == 0)
            pthread_cond_wait(&worker_queue_cond[slot], &worker_queue_mutex);
        int shutting_down = worker_shutdown;
        pthread_mutex_unlock(&worker_queue_mutex);
        if (shutting_down) break;

        /* Acquire execution lock before dequeueing. Tab close/reorder uses the
           same lock, so a queued command follows its tab when slots shift. */
        pthread_mutex_lock(&command_execution_mutex);
        pthread_mutex_lock(&worker_queue_mutex);
        if (worker_shutdown) {
            pthread_mutex_unlock(&worker_queue_mutex);
            pthread_mutex_unlock(&command_execution_mutex);
            break;
        }
        if (worker_queue_count[slot] == 0) {
            pthread_mutex_unlock(&worker_queue_mutex);
            pthread_mutex_unlock(&command_execution_mutex);
            continue;
        }
        char command[COMMAND_SIZE];
        snprintf(command, sizeof(command), "%s", worker_commands[slot][worker_queue_head[slot]]);
        worker_queue_head[slot] = (worker_queue_head[slot] + 1) % TAB_COMMAND_QUEUE;
        worker_queue_count[slot]--;
        pthread_mutex_unlock(&worker_queue_mutex);

        if (slot < tab_count && tabs[slot].used)
            execute_command(&tabs[slot], command);

        pthread_mutex_unlock(&command_execution_mutex);
    }
    return NULL;
}

static int enqueue_tab_command(int tab_index, const char *command)
{
    if (tab_index < 0 || tab_index >= MAX_TABS || !command) return -1;
    pthread_mutex_lock(&worker_queue_mutex);
    if (worker_queue_count[tab_index] >= TAB_COMMAND_QUEUE) {
        pthread_mutex_unlock(&worker_queue_mutex);
        if (tab_index < tab_count) add_line(&tabs[tab_index], "terminator: command queue full; wait for a command to finish");
        return -1;
    }
    int tail = (worker_queue_head[tab_index] + worker_queue_count[tab_index]) % TAB_COMMAND_QUEUE;
    snprintf(worker_commands[tab_index][tail], COMMAND_SIZE, "%s", command);
    worker_queue_count[tab_index]++;
    pthread_cond_signal(&worker_queue_cond[tab_index]);
    pthread_mutex_unlock(&worker_queue_mutex);
    return 0;
}

static void start_tab_workers(void)
{
    for (int i = 0; i < MAX_TABS; i++) {
        pthread_cond_init(&worker_queue_cond[i], NULL);
        if (pthread_create(&tab_workers[i], NULL, tab_worker_main, (void *)(intptr_t)i) == 0)
            tab_worker_started[i] = 1;
        else
            fprintf(stderr, "Warning: could not start worker for tab slot %d\n", i + 1);
    }
}

static void stop_tab_workers(void)
{
    pthread_mutex_lock(&worker_queue_mutex);
    worker_shutdown = 1;
    for (int i = 0; i < MAX_TABS; i++) pthread_cond_broadcast(&worker_queue_cond[i]);
    pthread_mutex_unlock(&worker_queue_mutex);
    for (int i = 0; i < MAX_TABS; i++) {
        if (tab_worker_started[i]) pthread_join(tab_workers[i], NULL);
        pthread_cond_destroy(&worker_queue_cond[i]);
    }
}

static void check_children(void) {
    for (int i = 0; i < tab_count; i++) {
        TerminalTab *t = &tabs[i];
        if (!t->child_running) continue;
        if (t->child_total > 0) {
            for (int k = 0; k < t->child_total; ++k) {
                if (t->child_reaped[k]) continue;
                int st = 0;
                pid_t r = waitpid(t->child_pids[k], &st, WNOHANG | WUNTRACED | WCONTINUED);
                if (r != t->child_pids[k]) continue;
                if (WIFSTOPPED(st)) {
                    Job *j = NULL;
                    for (int q = 0; q < MAX_JOBS; ++q) if (jobs[q].used && jobs[q].pgid == t->child_pgid) j = &jobs[q];
                    if (!j) j = add_job(t->child_pgid, "(stopped pipeline)", 0);
                    if (j) { j->stopped = 1; j->running = 0; }
                    char msg[LINE_SIZE]; snprintf(msg, sizeof(msg), "[%d]+ Stopped", j ? j->id : 0); add_line(t, msg);
                    t->child_running = 0; t->child_pid = t->child_pgid = -1;
                    t->child_total = t->child_remaining = 0;
                    break;
                }
                if (WIFEXITED(st) || WIFSIGNALED(st)) {
                    t->child_reaped[k] = 1; t->child_remaining--;
                    if (t->child_pids[k] == t->last_child_pid) t->last_status = status_code(st);
                }
            }
            if (t->child_running && t->child_remaining == 0) {
                t->child_running = 0; t->child_pid = t->child_pgid = -1;
                t->child_total = 0;
            }
        } else if (t->child_pid > 0) {
            int st; pid_t r = waitpid(t->child_pid, &st, WNOHANG | WUNTRACED);
            if (r == t->child_pid) {
                if (WIFSTOPPED(st)) {
                    Job *j = NULL; for (int k = 0; k < MAX_JOBS; k++) if (jobs[k].used && jobs[k].pgid == t->child_pgid) j = &jobs[k];
                    if (!j) j = add_job(t->child_pgid, "(stopped command)", 0);
                    if (j) { j->stopped = 1; j->running = 0; }
                    char msg[LINE_SIZE]; snprintf(msg, sizeof(msg), "[%d]+ Stopped", j ? j->id : 0); add_line(t, msg);
                    t->last_status = status_code(st); t->child_running = 0; t->child_pid = t->child_pgid = -1;
                } else if (WIFEXITED(st) || WIFSIGNALED(st)) {
                    t->last_status = status_code(st); t->child_running = 0; t->child_pid = t->child_pgid = -1;
                }
            }
        }
    }
    if (got_sigchld) {
        got_sigchld = 0;
        for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && jobs[i].running) {
            int st; pid_t r;
            while ((r = waitpid(-jobs[i].pgid, &st, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
                if (WIFSTOPPED(st)) { jobs[i].stopped = 1; jobs[i].running = 0; }
                else if (WIFCONTINUED(st)) { jobs[i].stopped = 0; jobs[i].running = 1; }
            }
            if (kill(-jobs[i].pgid, 0) < 0 && errno == ESRCH) jobs[i].used = 0;
        }
    }
}
static void handle_watch_pipe(TerminalTab *t, int i) {
    if (watch_state.fds[i] < 0) return;
    char b[2048];
    for (;;) {
        ssize_t n = read(watch_state.fds[i], b, sizeof(b) - 1);
        if (n > 0) { b[n] = 0; add_output(t, b); continue; }
        if (n == 0) {
            if (t->pending_len) finish_pending_output(t);
            close(watch_state.fds[i]); watch_state.fds[i] = -1;
            watch_state.running[i] = 0; watch_state.pids[i] = 0;
            add_line(t, "----------------------------------------------------"); break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close(watch_state.fds[i]); watch_state.fds[i] = -1; watch_state.running[i] = 0; break;
    }
}
static void service_watch(TerminalTab *t) {
    if (!watch_state.active) return;
    int running = 0;
    for (int i = 0; i < watch_state.count; i++) if (watch_state.running[i]) running++;
    if (running == 0 && time(NULL) >= watch_state.next_start) { watch_state.next_start = time(NULL) + 2; start_watch_cycle(t); }
}
static KeySym normalize_keysym(KeySym ks) {
    if (ks >= XK_A && ks <= XK_Z) return ks + (XK_a - XK_A);
    return ks;
}
static int hotkey_matches(HotkeyAction action, unsigned int state, KeySym ks) {
    return normalize_keysym(ks) == normalize_keysym(hotkeys[action].keysym) &&
           (state & HOTKEY_MODS) == hotkeys[action].modifiers;
}
static void config_warning(const char *message) {
    if (config_warning_count < (int)(sizeof(config_warnings) / sizeof(config_warnings[0])))
        snprintf(config_warnings[config_warning_count++], LINE_SIZE, "terminatorrc: %s", message);
}
static int find_hotkey_action(const char *name) {
    for (int i = 0; i < HK_COUNT; ++i) if (strcmp(name, hotkey_names[i]) == 0) return i;
    return -1;
}
static int parse_key_combo(const char *text, HotkeyBinding *out) {
    if (!text || !*text || strlen(text) >= 128) return -1;
    char copy[128]; snprintf(copy, sizeof(copy), "%s", text);
    unsigned int mods = 0; KeySym key = NoSymbol; char *save = NULL;
    for (char *part = strtok_r(copy, "+", &save); part; part = strtok_r(NULL, "+", &save)) {
        if (strcasecmp(part, "Ctrl") == 0 || strcasecmp(part, "Control") == 0) {
            if (mods & ControlMask) return -1;
            mods |= ControlMask;
        } else if (strcasecmp(part, "Shift") == 0) {
            if (mods & ShiftMask) return -1;
            mods |= ShiftMask;
        } else if (strcasecmp(part, "Alt") == 0) {
            if (mods & Mod1Mask) return -1;
            mods |= Mod1Mask;
        } else {
            if (key != NoSymbol) return -1;
            key = XStringToKeysym(part);
            if (key == NoSymbol && strlen(part) == 1) key = (KeySym)(unsigned char)part[0];
            key = normalize_keysym(key);
        }
    }
    if (key == NoSymbol) return -1;
    out->keysym = key; out->modifiers = mods; return 0;
}
static void load_hotkey_config(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.terminatorrc", home) >= (int)sizeof(path)) return;
    FILE *f = fopen(path, "r");
    if (!f) { if (errno != ENOENT) { char m[LINE_SIZE]; snprintf(m, sizeof(m), "cannot read %.700s: %.200s", path, strerror(errno)); config_warning(m); } return; }
    char line[256]; int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineno; char *p = line;
        while (isspace((unsigned char)*p)) ++p;
        if (!*p || *p == '#') continue;
        char *save = NULL; char *directive = strtok_r(p, " \t\r\n", &save);
        char *action_name = strtok_r(NULL, " \t\r\n", &save);
        char *combo = strtok_r(NULL, " \t\r\n", &save);
        char *extra = strtok_r(NULL, " \t\r\n", &save);
        char why[LINE_SIZE]; int action = action_name ? find_hotkey_action(action_name) : -1;
        if (!directive || strcmp(directive, "bind") != 0 || action < 0 || !combo || extra) {
            snprintf(why, sizeof(why), "line %d: expected `bind ACTION KEY`; defaults restored", lineno); config_warning(why); reset_hotkeys_to_defaults(); break;
        }
        HotkeyBinding candidate;
        if (parse_key_combo(combo, &candidate) != 0) {
            snprintf(why, sizeof(why), "line %d: invalid key binding `%.100s`; defaults restored", lineno, combo); config_warning(why); reset_hotkeys_to_defaults(); break;
        }
        int conflict = 0;
        for (int i = 0; i < HK_COUNT; ++i) if (i != action && hotkeys[i].keysym == candidate.keysym && hotkeys[i].modifiers == candidate.modifiers) { conflict = i + 1; break; }
        if (conflict) {
            snprintf(why, sizeof(why), "line %d: `%.100s` conflicts with %s; defaults restored", lineno, combo, hotkey_names[conflict - 1]); config_warning(why); reset_hotkeys_to_defaults(); break;
        }
        hotkeys[action] = candidate;
    }
    fclose(f);
}
static void insert_input_text(TerminalTab *t, const char *text, size_t len) {
    if (!t || !text) return;
    /* Keep the input-buffer invariants intact even after history/paste events. */
    if (t->input_len >= INPUT_SIZE) t->input_len = INPUT_SIZE - 1;
    if (t->input_cursor > t->input_len) t->input_cursor = t->input_len;
    t->input[t->input_len] = '\0';
    for (size_t i = 0; i < len && t->input_len < INPUT_SIZE - 1; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || c == '\r' || c == '\t' || c < 32 || c == 127) continue;
        memmove(t->input + t->input_cursor + 1,
                t->input + t->input_cursor,
                t->input_len - t->input_cursor + 1);
        t->input[t->input_cursor] = (char)c;
        ++t->input_cursor;
        ++t->input_len;
    }
    t->input[t->input_len] = '\0';
}
static int selection_is_nonempty(void) {
    return text_selection_valid && text_selection_tab >= 0 &&
           (selection_start_line != selection_end_line || selection_start_col != selection_end_col);
}
static void update_selection_endpoint(int x, int y) {
    if (!text_selection_dragging || text_selection_tab < 0 || text_selection_tab >= tab_count) return;
    TerminalTab *t = &tabs[text_selection_tab];
    if (t->line_count <= 0) { selection_end_line = selection_end_col = 0; return; }
    int visible = (WIN_HEIGHT - TAB_HEIGHT - 70) / LINE_HEIGHT;
    int start = t->line_count - visible; if (start < 0) start = 0; start -= t->scroll_offset; if (start < 0) start = 0;
    int row = (y - (TAB_HEIGHT + 35)) / LINE_HEIGHT; if (row < 0) row = 0;
    if (row >= visible) row = visible - 1;
    int li = start + row; if (li >= t->line_count) li = t->line_count - 1; if (li < 0) li = 0;
    int col = (x - 12) / 8; if (col < 0) col = 0;
    int len = (int)strlen(t->lines[li]); if (col > len) col = len;
    selection_end_line = li; selection_end_col = col;
}
static void copy_selected_text(TerminalTab *t) {
    pthread_mutex_lock(&clipboard_mutex);
    clipboard_text[0] = '\0'; size_t used = 0;
    int sl = selection_start_line, sc = selection_start_col, el = selection_end_line, ec = selection_end_col;
    if (sl > el || (sl == el && sc > ec)) { int q=sl; sl=el; el=q; q=sc; sc=ec; ec=q; }
    for (int li = sl; li <= el && li >= 0 && li < t->line_count && used < sizeof(clipboard_text)-1; ++li) {
        const char *line = t->lines[li]; size_t len = strlen(line);
        size_t a = (li == sl && sc > 0) ? (size_t)sc : 0;
        size_t b = (li == el && ec >= 0) ? (size_t)ec : len;
        if (a > len) a = len;
        if (b > len) b = len;
        if (b < a) b = a;
        size_t n = b-a; if (n > sizeof(clipboard_text)-1-used) n = sizeof(clipboard_text)-1-used;
        memcpy(clipboard_text+used, line+a, n); used += n;
        if (li < el && used < sizeof(clipboard_text)-1) clipboard_text[used++] = '\n';
    }
    clipboard_text[used] = '\0';
    shared_clipboard_set(clipboard_text);
    pthread_mutex_unlock(&clipboard_mutex);
}
static void copy_input_to_clipboard(TerminalTab *t) {
    if (selection_is_nonempty() && text_selection_tab == active_tab) copy_selected_text(t);
    else {
        pthread_mutex_lock(&clipboard_mutex);
        size_t n = t->input_len; if (n >= sizeof(clipboard_text)) n = sizeof(clipboard_text) - 1;
        memcpy(clipboard_text, t->input, n); clipboard_text[n] = '\0';
        shared_clipboard_set(clipboard_text);
        pthread_mutex_unlock(&clipboard_mutex);
    }
    XSetSelectionOwner(display, clipboard_atom, window, CurrentTime);
    if (XGetSelectionOwner(display, clipboard_atom) != window) add_line(t, "terminator: could not own X11 clipboard");
}
static void request_clipboard_paste(void) {
    paste_fallback_attempted = 0; pending_paste_target = utf8_atom;
    XConvertSelection(display, clipboard_atom, utf8_atom, clipboard_property, window, CurrentTime);
    XFlush(display);
}
static void handle_selection_request(XSelectionRequestEvent *req) {
    XSelectionEvent response; memset(&response, 0, sizeof(response));
    response.type = SelectionNotify; response.display = req->display; response.requestor = req->requestor;
    response.selection = req->selection; response.target = req->target; response.time = req->time; response.property = None;
    Atom property = req->property == None ? req->target : req->property;
    if (req->selection == clipboard_atom && (req->target == utf8_atom || req->target == XA_STRING)) {
        char clipboard_snapshot[INPUT_SIZE];
        shared_clipboard_get(clipboard_snapshot, sizeof(clipboard_snapshot));
        if (!shared_state) {
            pthread_mutex_lock(&clipboard_mutex);
            memcpy(clipboard_snapshot, clipboard_text, sizeof(clipboard_snapshot));
            clipboard_snapshot[sizeof(clipboard_snapshot) - 1] = '\0';
            pthread_mutex_unlock(&clipboard_mutex);
        }
        XChangeProperty(display, req->requestor, property, req->target, 8, PropModeReplace,
                        (unsigned char *)clipboard_snapshot, (int)strlen(clipboard_snapshot));
        response.property = property;
    } else if (req->selection == clipboard_atom && req->target == targets_atom) {
        Atom supported[] = {targets_atom, utf8_atom, XA_STRING};
        XChangeProperty(display, req->requestor, property, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)supported, (int)(sizeof(supported) / sizeof(supported[0])));
        response.property = property;
    }
    XSendEvent(display, req->requestor, False, 0, (XEvent *)&response); XFlush(display);
}
static void handle_selection_notify(XSelectionEvent *ev) {
    if (ev->selection != clipboard_atom || pending_paste_target == None) return;
    if (ev->property == None) {
        if (pending_paste_target == utf8_atom && !paste_fallback_attempted) {
            paste_fallback_attempted = 1; pending_paste_target = XA_STRING;
            XConvertSelection(display, clipboard_atom, XA_STRING, clipboard_property, window, CurrentTime); XFlush(display);
        } else pending_paste_target = None;
        return;
    }
    Atom actual_type; int actual_format; unsigned long nitems, bytes_after; unsigned char *data = NULL;
    if (XGetWindowProperty(display, window, clipboard_property, 0, INPUT_SIZE, True, AnyPropertyType,
                           &actual_type, &actual_format, &nitems, &bytes_after, &data) == Success && data && actual_format == 8) {
        TerminalTab *t = current_tab(); insert_input_text(t, (const char *)data, (size_t)nitems); XFree(data); draw_screen();
    } else if (data) XFree(data);
    pending_paste_target = None;
}
static void delete_before_cursor(TerminalTab *t) {
    if (t->input_cursor > t->input_len) t->input_cursor = t->input_len;
    if (!t->input_cursor) return;
    memmove(t->input + t->input_cursor - 1, t->input + t->input_cursor, t->input_len - t->input_cursor + 1);
    --t->input_cursor; --t->input_len;
}
static void delete_at_cursor(TerminalTab *t) {
    if (t->input_cursor > t->input_len) t->input_cursor = t->input_len;
    if (t->input_cursor >= t->input_len) return;
    memmove(t->input + t->input_cursor, t->input + t->input_cursor + 1, t->input_len - t->input_cursor);
    --t->input_len;
}
static void delete_previous_word(TerminalTab *t) {
    size_t start = t->input_cursor;
    while (start > 0 && isspace((unsigned char)t->input[start - 1])) --start;
    while (start > 0 && !isspace((unsigned char)t->input[start - 1])) --start;
    memmove(t->input + start, t->input + t->input_cursor, t->input_len - t->input_cursor + 1);
    t->input_len -= t->input_cursor - start; t->input_cursor = start;
}

/* ---------- Task 10: filename completion ---------- */
static void clear_completion_choices(void) {
    completion_choice_count = 0;
    completion_choice_tab = -1;
}

static void replace_completion_prefix(TerminalTab *t, const char *name) {
    size_t name_len = strlen(name);
    if (completion_token_start > t->input_cursor || completion_token_start > t->input_len) return;
    size_t suffix_len = t->input_len - t->input_cursor;
    if (completion_token_start + name_len + suffix_len >= sizeof(t->input)) return;
    memmove(t->input + completion_token_start + name_len,
            t->input + t->input_cursor, suffix_len);
    memcpy(t->input + completion_token_start, name, name_len);
    t->input_len = completion_token_start + name_len + suffix_len;
    t->input[t->input_len] = '\0';
    t->input_cursor = completion_token_start + name_len;
}

static void select_completion(TerminalTab *t, int choice) {
    if (completion_choice_tab != active_tab || choice < 1 || choice > completion_choice_count) {
        clear_completion_choices();
        return;
    }
    char selected[NAME_MAX + 1];
    snprintf(selected, sizeof(selected), "%s", completion_choices[choice - 1]);
    replace_completion_prefix(t, selected);
    clear_completion_choices();
}

static void complete_filename(TerminalTab *t) {
    /* Completion only replaces the token prefix before the cursor. Text after
       the cursor is retained exactly as typed. */
    clear_completion_choices();
    size_t start = t->input_cursor;
    while (start > 0 && !isspace((unsigned char)t->input[start - 1])) --start;
    size_t prefix_len = t->input_cursor - start;
    if (prefix_len >= INPUT_SIZE) return;
    char prefix[INPUT_SIZE];
    memcpy(prefix, t->input + start, prefix_len);
    prefix[prefix_len] = '\0';

    /* This task searches entries in the current tab's working directory. */
    char saved_cwd[PATH_MAX];
    int restore_cwd = getcwd(saved_cwd, sizeof(saved_cwd)) != NULL;
    if (chdir(t->cwd) < 0) return;
    DIR *dir = opendir(".");
    if (!dir) { if (restore_cwd) (void)chdir(saved_cwd); return; }

    char matches[MAX_COMPLETION_CHOICES][NAME_MAX + 1];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!*name || strncmp(name, prefix, prefix_len) != 0) continue;
        if (name[0] == '.' && (prefix_len == 0 || prefix[0] != '.')) continue;
        if (strlen(name) > NAME_MAX) continue;
        if (count < MAX_COMPLETION_CHOICES) {
            snprintf(matches[count], sizeof(matches[count]), "%s", name);
            ++count;
        } else {
            /* More than nine matches: retain first nine alphabetically later
               is not guaranteed, so signal ambiguity with the collected set. */
            continue;
        }
    }
    closedir(dir);
    if (restore_cwd) (void)chdir(saved_cwd);
    if (count == 0) return;

    completion_token_start = start;
    completion_cursor = t->input_cursor;
    if (count == 1) {
        replace_completion_prefix(t, matches[0]);
        return;
    }

    size_t common = strlen(matches[0]);
    for (int i = 1; i < count; ++i) {
        size_t j = 0;
        while (j < common && matches[i][j] && matches[0][j] == matches[i][j]) ++j;
        common = j;
    }
    if (common > prefix_len) {
        char common_prefix[NAME_MAX + 1];
        memcpy(common_prefix, matches[0], common);
        common_prefix[common] = '\0';
        replace_completion_prefix(t, common_prefix);
        return;
    }

    completion_choice_count = count;
    completion_choice_tab = active_tab;
    for (int i = 0; i < count; ++i)
        snprintf(completion_choices[i], sizeof(completion_choices[i]), "%s", matches[i]);
    add_line(t, "Ambiguous filename. Type a number to choose:");
    for (int i = 0; i < count; ++i) {
        char line[LINE_SIZE];
        snprintf(line, sizeof(line), "%d. %s", i + 1, matches[i]);
        add_line(t, line);
    }
    if (count == MAX_COMPLETION_CHOICES)
        add_line(t, "Showing up to 9 matches; narrow the filename prefix for more.");
}

static void handle_keypress(XKeyEvent *e) {
    TerminalTab *t = current_tab(); char b[64]; KeySym ks; int n = XLookupString(e, b, sizeof(b), &ks, NULL);
    ks = normalize_keysym(ks);
    int ctrl = (e->state & ControlMask) != 0;
    int shift = (e->state & ShiftMask) != 0;
    if (completion_choice_count > 0 && completion_choice_tab == active_tab && !ctrl && !(e->state & Mod1Mask)) {
        if (ks >= XK_1 && ks <= XK_9) {
            select_completion(t, (int)(ks - XK_0));
            draw_screen();
            return;
        }
        /* Any other key cancels the numbered-choice prompt and is handled normally. */
        clear_completion_choices();
    }
    if (ks == XK_Tab && !ctrl && !shift) { complete_filename(t); draw_screen(); return; }
    if (hotkey_matches(HK_COPY, e->state, ks)) { copy_input_to_clipboard(t); draw_screen(); return; }
    if (hotkey_matches(HK_PASTE, e->state, ks)) { request_clipboard_paste(); return; }
    if (watch_state.active && ctrl && !shift && ks == XK_c && watch_state.owner_tab == active_tab) {
    stop_watch(t);
    draw_screen();
    return;
}
    if (ctrl && (ks == XK_n || ks == XK_N)) { open_new_tab(); draw_screen(); return; }
    if (ctrl && ks == XK_Tab) { active_tab = (active_tab + 1) % tab_count; draw_screen(); return; }
    /* Ctrl+W is the shell-style delete-word shortcut; close-tab is Ctrl+Shift+W. */
    if (ctrl && shift && ks == XK_w) { close_current_tab(); draw_screen(); return; }
    if (ctrl && !shift && (ks == XK_r || ks == XK_R)) {
        history_search_mode = 1;
        t->input_len = 0; t->input[0] = '\0'; t->input_cursor = 0;
        history_cursor = -1;
        draw_screen();
        return;
    }
    if (hotkey_matches(HK_CLEAR_SCREEN, e->state, ks)) { t->line_count = 0; t->scroll_offset = 0; draw_screen(); return; }
   if (ctrl && !shift && ks == XK_c) {
    if (process_mode && t->process_worker_pid > 0) { (void)kill(t->process_worker_pid, SIGINT); draw_screen(); return; }
    if (t->child_running && t->child_pgid > 0) {
        if (kill(-t->child_pgid, SIGINT) == -1) {
            perror("kill(SIGINT)");
            add_line(t, "terminator: failed to interrupt command");
        } else {
            add_line(t, "^C");
        }
    } else {
        /* No foreground command is running: clear current input */
        t->input_len = 0;
        t->input[0] = '\0';
        t->input_cursor = 0;
        add_line(t, "^C");
    }

    draw_screen();
    return;
}
    if (ctrl && (ks == XK_z || ks == XK_Z)) { if (process_mode && t->process_worker_pid > 0) (void)kill(t->process_worker_pid, SIGTSTP); else if (t->child_running && t->child_pgid > 0) kill(-t->child_pgid, SIGTSTP); draw_screen(); return; }
    if (ks == XK_Page_Up) { t->scroll_offset += 10; if (t->scroll_offset > t->line_count) t->scroll_offset = t->line_count; draw_screen(); return; }
    if (ks == XK_Page_Down) { t->scroll_offset -= 10; if (t->scroll_offset < 0) t->scroll_offset = 0; draw_screen(); return; }
    if (hotkey_matches(HK_HISTORY_PREV, e->state, ks)) {
        char entry[INPUT_SIZE] = "";
        pthread_mutex_lock(&history_mutex);
        int count = history_count;
        if (count) {
            if (history_cursor < 0) history_cursor = count - 1;
            else if (history_cursor > 0) history_cursor--;
            snprintf(entry, sizeof(entry), "%s", history[history_cursor]);
        }
        pthread_mutex_unlock(&history_mutex);
        if (count) { snprintf(t->input, sizeof(t->input), "%s", entry); t->input_len = strlen(t->input); t->input_cursor = t->input_len; }
        draw_screen(); return;
    }
    if (hotkey_matches(HK_HISTORY_NEXT, e->state, ks)) {
        char entry[INPUT_SIZE] = "";
        pthread_mutex_lock(&history_mutex);
        int count = history_count;
        int has_next = history_cursor >= 0 && history_cursor < count - 1;
        if (has_next) { history_cursor++; snprintf(entry, sizeof(entry), "%s", history[history_cursor]); }
        pthread_mutex_unlock(&history_mutex);
        if (has_next) { snprintf(t->input, sizeof(t->input), "%s", entry); t->input_len = strlen(t->input); t->input_cursor = t->input_len; }
        else { history_cursor = -1; t->input_len = 0; t->input[0] = 0; t->input_cursor = 0; }
        draw_screen(); return;
    }
    if (ks == XK_Return || ks == XK_KP_Enter) {
        if (history_search_mode) {
            t->input[t->input_len] = '\0';
            history_search(t, t->input);
            history_search_mode = 0;
            t->input_len = 0; t->input[0] = '\0'; t->input_cursor = 0;
            history_cursor = -1;
            draw_screen();
            return;
        }
        t->input[t->input_len] = 0;
        char line[INPUT_SIZE]; snprintf(line, sizeof(line), "%s", t->input);
        t->input_len = 0; t->input[0] = 0; t->input_cursor = 0; history_cursor = -1; t->scroll_offset = 0;
        char combined[COMMAND_SIZE]; combined[0] = '\0';
        if (t->continuation_active) {
            snprintf(combined, sizeof(combined), "%s", t->continuation);
            size_t used = strlen(combined);
            if (t->continuation_backslash && used && combined[used - 1] == '\\') combined[--used] = '\0';
            else if (used + 1 < sizeof(combined)) combined[used++] = '\n', combined[used] = '\0';
            if (strlen(line) >= sizeof(combined) - used) { add_line(t, "terminator: multiline command is too long"); t->continuation_active = 0; t->continuation_len = 0; draw_screen(); return; }
            strcat(combined, line);
        } else snprintf(combined, sizeof(combined), "%s", line);
        int trailing_slash = 0;
        if (command_is_incomplete(combined, &trailing_slash)) {
            if (strlen(combined) >= sizeof(t->continuation)) { add_line(t, "terminator: multiline command is too long"); t->continuation_active = 0; t->continuation_len = 0; draw_screen(); return; }
            snprintf(t->continuation, sizeof(t->continuation), "%s", combined);
            t->continuation_len = strlen(t->continuation); t->continuation_active = 1; t->continuation_backslash = trailing_slash;
            char shown[LINE_SIZE]; snprintf(shown, sizeof(shown), "> %s", line); add_line(t, shown);
            draw_screen(); return;
        }
        t->continuation_active = 0; t->continuation_backslash = 0; t->continuation_len = 0; t->continuation[0] = '\0';
        if (process_mode) {
            execute_command(t, combined);
            draw_screen();
        } else if (enqueue_tab_command(active_tab, combined) == 0) {
            /* Execution happens on the tab worker; the GUI thread stays responsive. */
            draw_screen();
        } else draw_screen();
        return;
    }
    if (hotkey_matches(HK_LEFT, e->state, ks)) { if (t->input_cursor > 0) --t->input_cursor; draw_screen(); return; }
    if (hotkey_matches(HK_RIGHT, e->state, ks)) { if (t->input_cursor < t->input_len) ++t->input_cursor; draw_screen(); return; }
    if (hotkey_matches(HK_LINE_START, e->state, ks)) { t->input_cursor = 0; draw_screen(); return; }
    if (hotkey_matches(HK_LINE_END, e->state, ks)) { t->input_cursor = t->input_len; draw_screen(); return; }
    if (hotkey_matches(HK_DELETE_START, e->state, ks)) {
        memmove(t->input, t->input + t->input_cursor, t->input_len - t->input_cursor + 1);
        t->input_len -= t->input_cursor; t->input_cursor = 0; draw_screen(); return;
    }
    if (hotkey_matches(HK_DELETE_END, e->state, ks)) { t->input[t->input_cursor] = '\0'; t->input_len = t->input_cursor; draw_screen(); return; }
    if (hotkey_matches(HK_DELETE_WORD, e->state, ks)) { delete_previous_word(t); draw_screen(); return; }
    if (ks == XK_BackSpace) { delete_before_cursor(t); draw_screen(); return; }
    if (ks == XK_Delete) { delete_at_cursor(t); draw_screen(); return; }
    if (hotkey_matches(HK_EOF, e->state, ks) && t->input_len == 0) { if (tab_count > 1) close_current_tab(); else t->exit_requested = 1; draw_screen(); return; }
    if (n > 0 && !ctrl && !(e->state & Mod1Mask)) {
        for (int i = 0; i < n; i++) { unsigned char ch = (unsigned char)b[i]; if (ch >= 32 && ch != 127) insert_input_text(t, (const char *)&b[i], 1); }
    }
    draw_screen();
}

static void handle_mouse_release(XButtonEvent *e) {
    if (e->button == Button1) { text_selection_dragging = 0; draw_screen(); }
}
static void handle_mouse_motion(XMotionEvent *e) {
    if (!text_selection_dragging) return;
    update_selection_endpoint(e->x, e->y); draw_screen();
}
static void handle_mouse_click(XButtonEvent *e)
{
    TerminalTab *t = current_tab();

    /* Mouse wheel: Button4 = scroll up, Button5 = scroll down */
    if (e->button == Button4 || e->button == Button5)
    {
        /* Ignore wheel events outside the output area */
        if (e->y <= TAB_HEIGHT || e->y >= WIN_HEIGHT - 43)
            return;

        int visible = (WIN_HEIGHT - TAB_HEIGHT - 70) / LINE_HEIGHT;
        int max_offset = t->line_count - visible;

        if (max_offset < 0)
            max_offset = 0;

        if (e->button == Button4)
            t->scroll_offset += 3;
        else
            t->scroll_offset -= 3;

        if (t->scroll_offset < 0)
            t->scroll_offset = 0;

        if (t->scroll_offset > max_offset)
            t->scroll_offset = max_offset;

        draw_screen();
        return;
    }

    /* Existing tab selection and new-tab handling */
    if (e->y >= 4 && e->y <= 32 && e->x >= 5)
    {
        int idx = (e->x - 5) / 170;

        if (idx >= 0 && idx < tab_count)
            active_tab = idx;
        else if (idx == tab_count)
            open_new_tab();

        draw_screen();
        return;
    }

    /* Drag across output text to select it for Ctrl+Shift+C / X11 clipboard use. */
    if (e->button == Button1 && e->y >= TAB_HEIGHT + 35 && e->y < WIN_HEIGHT - 43 && current_tab()->line_count > 0) {
        text_selection_tab = active_tab; text_selection_valid = 1; text_selection_dragging = 1;
        update_selection_endpoint(e->x, e->y);
        selection_start_line = selection_end_line; selection_start_col = selection_end_col;
        draw_screen(); return;
    }
}

int main(int argc, char **argv) {
    for (int ai = 1; ai < argc; ++ai) {
        if (strcmp(argv[ai], "useProcess=True") == 0) process_mode = 1;
        else { fprintf(stderr, "Usage: %s [useProcess=True]\n", argv[0]); return 2; }
    }
    signal(SIGCHLD, on_sigchld); signal(SIGPIPE, SIG_IGN);
    if (shared_state_init() < 0) fprintf(stderr, "Warning: POSIX shared memory unavailable; using process-local history/clipboard.\n");
    history_load(); load_hotkey_config();
    display = XOpenDisplay(NULL); if (!display) { fprintf(stderr, "Cannot open X11 display.\n"); return 1; }
    clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
    utf8_atom = XInternAtom(display, "UTF8_STRING", False);
    targets_atom = XInternAtom(display, "TARGETS", False);
    clipboard_property = XInternAtom(display, "TERMINATOR_CLIPBOARD", False);
    screen = DefaultScreen(display); window = XCreateSimpleWindow(display, RootWindow(display, screen), 100, 100, WIN_WIDTH, WIN_HEIGHT, 1, BlackPixel(display, screen), WhitePixel(display, screen));
    XStoreName(display, window, title); XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    gc = XCreateGC(display, window, 0, NULL); wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False); XSetWMProtocols(display, window, &wm_delete, 1);
    initialize_tab(0, NULL);
    for (int i = 0; i < config_warning_count; ++i) add_line(&tabs[0], config_warnings[i]);
    if (!process_mode) start_tab_workers();
    XMapWindow(display, window); XFlush(display);
    int running = 1;
    while (running) {
        struct pollfd fds[MAX_TABS + MAX_WATCH + 2]; int tabidx[MAX_TABS + MAX_WATCH + 2]; int watchidx[MAX_TABS + MAX_WATCH + 2]; int nfds = 1;
        fds[0].fd = ConnectionNumber(display); fds[0].events = POLLIN; fds[0].revents = 0; tabidx[0] = watchidx[0] = -1;
        for (int i = 0; i < tab_count; i++) if (tabs[i].output_fd >= 0) { fds[nfds].fd = tabs[i].output_fd; fds[nfds].events = POLLIN | POLLHUP; fds[nfds].revents = 0; tabidx[nfds] = i; watchidx[nfds++] = -1; }
        for (int i = 0; i < watch_state.count; i++) if (watch_state.active && watch_state.fds[i] >= 0) { fds[nfds].fd = watch_state.fds[i]; fds[nfds].events = POLLIN | POLLHUP; fds[nfds].revents = 0; tabidx[nfds] = -1; watchidx[nfds++] = i; }
        int timeout = watch_state.active ? 200 : -1; if (watch_state.active) { time_t now = time(NULL); int ms = (int)(watch_state.next_start - now) * 1000; if (ms < 0) ms = 0; if (ms < timeout) timeout = ms; }
        int r = poll(fds, nfds, timeout); if (r < 0 && errno != EINTR) { perror("poll"); break; }
        if (fds[0].revents & POLLIN) while (XPending(display) > 0) { XEvent ev; XNextEvent(display, &ev); switch (ev.type) { case Expose:
    if (!ev.xexpose.count)
        draw_screen();
    break;

case MapNotify:
    draw_screen();
    break;

case ConfigureNotify:
    current_win_width = ev.xconfigure.width;
    current_win_height = ev.xconfigure.height;
    draw_screen();
    break;

case KeyPress:
    handle_keypress(&ev.xkey);
     break; case ButtonPress: handle_mouse_click(&ev.xbutton); break; case ButtonRelease: handle_mouse_release(&ev.xbutton); break; case MotionNotify: handle_mouse_motion(&ev.xmotion); break; case SelectionRequest: handle_selection_request(&ev.xselectionrequest); break; case SelectionNotify: handle_selection_notify(&ev.xselection); break; case ClientMessage: if ((Atom)ev.xclient.data.l[0] == wm_delete) running = 0; break; default: break; } }
        for (int i = 1; i < nfds; i++) if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) { if (tabidx[i] >= 0 && tabidx[i] < tab_count) handle_pipe_output(&tabs[tabidx[i]]); else if (watchidx[i] >= 0 &&
         watch_state.active &&
         watch_state.owner_tab >= 0 &&
         watch_state.owner_tab < tab_count &&
         tabs[watch_state.owner_tab].used) {
    handle_watch_pipe(&tabs[watch_state.owner_tab], watchidx[i]);
} }
        check_children(); if (watch_state.active &&
    watch_state.owner_tab >= 0 &&
    watch_state.owner_tab < tab_count &&
    tabs[watch_state.owner_tab].used) {
    service_watch(&tabs[watch_state.owner_tab]);
} for (int i = 0; i < tab_count; i++) if (tabs[i].exit_requested) { if (tab_count > 1) { active_tab = i; close_current_tab(); } else running = 0; break; } 
    }
    if (!process_mode) stop_tab_workers();
    if (watch_state.active) stop_watch(current_tab());
    for (int i = 0; i < tab_count; i++) { if (tabs[i].process_cmd_fd >= 0) close(tabs[i].process_cmd_fd); if (tabs[i].process_worker_pid > 0) { kill(tabs[i].process_worker_pid, SIGTERM); (void)waitpid(tabs[i].process_worker_pid, NULL, 0); } if (tabs[i].output_fd >= 0) close(tabs[i].output_fd); if (tabs[i].child_running && tabs[i].child_pgid > 0) { kill(-tabs[i].child_pgid, SIGTERM); waitpid(tabs[i].child_pid, NULL, 0); } }
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && jobs[i].pgid > 0) kill(-jobs[i].pgid, SIGTERM);
    if (back_buffer) {
    XFreePixmap(display, back_buffer);
    back_buffer = 0;
}

XFreeGC(display, gc);
XDestroyWindow(display, window);
XCloseDisplay(display); shared_state_close(); return 0;
}

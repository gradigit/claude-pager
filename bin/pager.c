/*
 * pager.c — Pure C scrollable pager for Claude Code transcripts.
 * Zero dependencies beyond libc + POSIX.  Renders in ~3ms.
 */
#define _DARWIN_C_SOURCE
#include "pager.h"

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* ── ANSI ──────────────────────────────────────────────────────────────── */

#define RS      "\033[0m"
#define BO      "\033[1m"
#define DI      "\033[2m"
#define C_HUM   "\033[38;2;242;169;59m"
#define C_AST   "\033[38;2;246;241;254m"
#define C_TOL   "\033[38;2;178;185;244m"
#define C_RES   "\033[38;2;153;153;153m"
#define C_ERR   "\033[38;2;220;80;80m"
#define C_CBG   "\033[48;2;35;35;35m"
#define C_CFG   "\033[38;2;200;230;200m"
#define C_CIN   "\033[38;2;116;173;234m"
#define C_SEP   "\033[38;2;80;80;80m"
#define C_HDM   "\033[38;2;140;138;144m"
#define C_BAN   "\033[1;33m"
#define C_DFG   "\033[38;2;160;233;160m"
#define C_DFR   "\033[38;2;244;149;149m"
#define C_DFC   "\033[38;2;136;197;232m"
#define C_DABG  "\033[48;2;18;93;28m\033[38;2;160;233;160m"
#define C_DDBG  "\033[48;2;105;19;24m\033[38;2;244;149;149m"
#define C_DCBG  "\033[48;2;47;53;66m\033[38;2;178;185;244m"
#define C_DAHL  BO "\033[48;2;55;128;65m\033[38;2;236;255;236m"
#define C_DDHL  BO "\033[48;2;140;41;47m\033[38;2;255;222;222m"
#define C_DMBG  "\033[38;2;214;214;214m"
#define C_DMETA "\033[38;2;136;197;232m"
#define C_SYN_STR "\033[38;2;223;204;255m"
#define C_SYN_NUM "\033[38;2;255;178;79m"
#define C_SYN_KW  "\033[38;2;255;178;79m"
#define C_BRG   "\033[38;2;100;220;100m"
#define C_BRY   "\033[38;2;255;165;0m"
#define C_BRR   "\033[38;2;255;80;80m"
#define C_CONN  "\033[38;2;88;87;90m"
#define C_URL   "\033[38;2;242;169;59m"
#define C_FLINK "\033[38;2;136;197;232m"
#define C_LHOV  "\033[7m"
#define C_UBG   "\033[48;2;66;66;66m\033[38;2;246;241;254m"
#define C_QBG   "\033[48;2;31;36;44m\033[38;2;181;188;205m"
#define C_QSEL  "\033[48;2;40;56;84m\033[38;2;196;224;255m"
#define C_QACC  "\033[38;2;136;197;232m"
#define UL_ON   "\033[4m"
#define UL_OFF  "\033[24m"

#define HL  "\xe2\x94\x80"
#define VL  "\xe2\x94\x82"
#define BUL "\xe2\x80\xa2"
#define CHV "\xe2\x80\xba"
#define REC "\xe2\x8f\xba"
#define ELL "\xe2\x80\xa6"
#define EMD "\xe2\x80\x94"
#define UAR "\xe2\x86\x91"
#define FBLK "\xe2\x96\x88"
#define EBLK "\xe2\x96\x91"
#define DOT "\xc2\xb7"
#define TL  "\xe2\x94\x8c"
#define TR  "\xe2\x94\x90"
#define BL  "\xe2\x94\x94"
#define BR  "\xe2\x94\x98"

#define MOUSE_ON  "\033[>0s\033[?1007l\033[?1000h\033[?1003h\033[?1006h"
#define MOUSE_OFF "\033[?1006l\033[?1003l\033[?1000l\033[?1007l"

/* ── Globals ───────────────────────────────────────────────────────────── */

static int g_cols = 100, g_rows = 24, g_crows = 21;
static int g_fd = -1;
static struct termios g_old;
static volatile sig_atomic_t g_resize = 0, g_quit = 0;
static int g_term_saved = 0;
static int g_term_raw = 0;
static int g_mouse_enabled = 0;
static int g_restored = 0;
static FILE *g_dbg = NULL;
static long long g_log_t0_us = 0;
static int g_bench_mode = 0;
static int g_sync_enabled = 0;
static int g_sync_begin_count = 0;
static int g_sync_end_count = 0;
static int g_sync_unwind_end_count = 0;
static int g_allow_remote_file_links = -1;
static int g_oom = 0;
static int g_exit_after_first_draw = 0;
static int g_perf_compat = 0;
static int g_last_capped_lines = 0;
static int g_queue_rows = 0;
static int g_mouse_x = 0;
static int g_mouse_y = 0;
static int g_hover_row = 0;
static char g_hover_uri[8192] = "";

#define QUEUE_INPUT_MAX 4096
#define QUEUE_SHOW_MAX 5
#define INPUT_BOX_MAX_LINES 5
#define QUEUE_IMAGE_MAX_BYTES (20 * 1024 * 1024)

typedef struct {
    char *prompt;
    char *persisted_id;
    long long added_us;
    int has_added_us;
    int encoding_json;
    char *raw_json;
} QueueItem;

typedef struct {
    QueueItem *d;
    int n;
    int cap;
    int selected;
    int scroll_off;
} QueueItems;

typedef struct {
    dev_t dev;
    ino_t ino;
    off_t size;
    time_t mtime_sec;
    long mtime_nsec;
    int valid;
} FileStamp;

typedef struct {
    int row;
    int x0;
    int x1;
    char *uri;
} LinkSpan;

typedef struct {
    LinkSpan *d;
    int n;
    int cap;
} LinkMap;

static LinkMap g_link_map = {0};

static QueueItems g_queue = {0};
static FileStamp g_queue_stamp = {0};
static char g_queue_path[PATH_MAX] = "";
static char g_queue_fingerprint[17] = "";
static int g_queue_enabled = 0;
static int g_ctrl_quit_supported = 0;
static int g_input_mode = 0;
static char g_input_buf[QUEUE_INPUT_MAX];
static int g_input_len = 0;
static int g_input_cursor = 0;
static int g_input_goal_col = -1;
static char g_input_draft[QUEUE_INPUT_MAX];
static int g_input_draft_len = 0;
static int g_input_draft_cursor = 0;
static int g_input_draft_saved = 0;
static char g_queue_notice[160];
static int g_edit_index = -1;
static unsigned char g_input_pending[QUEUE_INPUT_MAX * 2];
static int g_input_pending_len = 0;
static int g_input_pending_pos = 0;

typedef struct {
    int total_lines;
    int visible_lines;
    int visible_start;
    int cursor_line;
    int cursor_col;
    int starts[QUEUE_INPUT_MAX + 2];
    int ends[QUEUE_INPUT_MAX + 2];
} InputLayout;

static long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + (long long)tv.tv_usec;
}

static void dbg_open(void) {
    if (g_dbg) return;
    g_dbg = fopen("/tmp/claude-pager-open.log", "a");
    if (!g_dbg) return;
    setbuf(g_dbg, NULL);
    const char *t0 = getenv("_CLAUDE_PAGER_T0_US");
    if (t0 && *t0) {
        char *end = NULL;
        long long parsed = strtoll(t0, &end, 10);
        if (end && *end == '\0' && parsed > 0) g_log_t0_us = parsed;
    }
    if (g_log_t0_us <= 0) g_log_t0_us = now_us();
}

static double log_elapsed_ms(void) {
    long long n = now_us();
    if (g_log_t0_us <= 0) g_log_t0_us = n;
    return (double)(n - g_log_t0_us) / 1000.0;
}

static void PDBG(const char *fmt, ...) {
    if (!g_dbg) dbg_open();
    if (!g_dbg) return;
    fprintf(g_dbg, "[%7.2fms] pager: ", log_elapsed_ms());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_dbg, fmt, ap);
    va_end(ap);
}

static void pdebug_input_bytes(const char *tag, const unsigned char *buf, ssize_t n) {
    if (!buf || n <= 0) return;
    if (!g_dbg) dbg_open();
    if (!g_dbg) return;
    fprintf(g_dbg, "[%7.2fms] pager: %s n=%zd hex=", log_elapsed_ms(), tag ? tag : "input", n);
    for (ssize_t i = 0; i < n; i++) {
        fprintf(g_dbg, "%s%02X", (i == 0 ? "" : " "), buf[i]);
    }
    fprintf(g_dbg, " text=");
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c >= 32 && c <= 126) fputc((int)c, g_dbg);
        else fprintf(g_dbg, "\\x%02X", c);
    }
    fputc('\n', g_dbg);
}

static int env_enabled(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    return strcmp(v, "1") == 0 ||
           strcasecmp(v, "true") == 0 ||
           strcasecmp(v, "yes") == 0 ||
           strcasecmp(v, "on") == 0;
}

static int env_enabled_default_on(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 1;
    return strcmp(v, "1") == 0 ||
           strcasecmp(v, "true") == 0 ||
           strcasecmp(v, "yes") == 0 ||
           strcasecmp(v, "on") == 0;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) g_oom = 1;
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) g_oom = 1;
    return q;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int parse_env_int_range(const char *name, int lo, int hi, int defv) {
    const char *v = getenv(name);
    if (!v || !*v) return defv;
    errno = 0;
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if (errno != 0 || !end || *end != '\0' || x < lo || x > hi) {
        PDBG("env parse invalid %s=%s; using default=%d\n", name, v, defv);
        return defv;
    }
    return (int)x;
}

static long stat_mtime_nsec(const struct stat *st) {
#if defined(__APPLE__)
    return st->st_mtimespec.tv_nsec;
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    return st->st_mtim.tv_nsec;
#else
    (void)st;
    return 0;
#endif
}

static int file_stamp_changed(const char *path, FileStamp *stamp) {
    if (!path || !*path || !stamp) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;

    long nsec = stat_mtime_nsec(&st);
    int changed =
        !stamp->valid ||
        stamp->dev != st.st_dev ||
        stamp->ino != st.st_ino ||
        stamp->size != st.st_size ||
        stamp->mtime_sec != st.st_mtime ||
        stamp->mtime_nsec != nsec;

    stamp->dev = st.st_dev;
    stamp->ino = st.st_ino;
    stamp->size = st.st_size;
    stamp->mtime_sec = st.st_mtime;
    stamp->mtime_nsec = nsec;
    stamp->valid = 1;
    return changed;
}

static int parse_env_mode(const char *v, int *out_mode) {
    /* 0=auto, 1=on, 2=off */
    if (!v || !*v) { *out_mode = 0; return 1; }
    if (strcasecmp(v, "auto") == 0) { *out_mode = 0; return 1; }
    if (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
        strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0) {
        *out_mode = 1; return 1;
    }
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
        strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0) {
        *out_mode = 2; return 1;
    }
    return 0;
}

static int is_ghostty_env(void) {
    const char *tp = getenv("TERM_PROGRAM");
    if (tp && strcasecmp(tp, "ghostty") == 0) return 1;
    const char *term = getenv("TERM");
    if (term && strcasestr(term, "ghostty")) return 1;
    return 0;
}

static int parse_decrqm_2026_status(const char *buf, int len) {
    /* ESC [ ? 2 0 2 6 ; Ps $ y */
    if (!buf || len <= 0) return -1;
    const char *needle = "\033[?2026;";
    int nl = (int)strlen(needle);
    for (int i = 0; i + nl + 2 < len; i++) {
        if (memcmp(buf + i, needle, (size_t)nl) != 0) continue;
        int j = i + nl;
        int ps = 0;
        int have_digit = 0;
        while (j < len && buf[j] >= '0' && buf[j] <= '9') {
            have_digit = 1;
            ps = ps * 10 + (buf[j] - '0');
            j++;
        }
        if (!have_digit) continue;
        if (j + 1 < len && buf[j] == '$' && buf[j + 1] == 'y') return ps;
    }
    return -1;
}

static int probe_sync_2026(int fd, int timeout_ms, int *status_out, int *raw_len_out) {
    if (status_out) *status_out = -1;
    if (raw_len_out) *raw_len_out = 0;
    if (fd < 0) return 0;
    if (timeout_ms < 1) timeout_ms = 1;
    if (timeout_ms > 1000) timeout_ms = 1000;

    tcflush(fd, TCIFLUSH);
    static const char q[] = "\033[?2026$p";
    if (write(fd, q, sizeof(q) - 1) < 0) return 0;

    char buf[256];
    int got = 0;
    long long deadline = now_us() + (long long)timeout_ms * 1000LL;
    while (now_us() < deadline && got < (int)sizeof(buf)) {
        long long rem = deadline - now_us();
        if (rem < 0) rem = 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = (time_t)(rem / 1000000LL);
        tv.tv_usec = (suseconds_t)((long)(rem % 1000000LL));
        int rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) break;
        if (!FD_ISSET(fd, &rfds)) continue;
        ssize_t n = read(fd, buf + got, sizeof(buf) - (size_t)got);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        got += (int)n;
        if (parse_decrqm_2026_status(buf, got) >= 0) break;
    }
    if (raw_len_out) *raw_len_out = got;
    int st = parse_decrqm_2026_status(buf, got);
    if (status_out) *status_out = st;
    return (st == 1 || st == 2) ? 1 : 0;
}

static void sync_init(int fd) {
    const char *m = getenv("CLAUDE_PAGER_SYNC");
    int mode = 0;
    if (m && *m && !parse_env_mode(m, &mode)) {
        PDBG("sync init invalid env CLAUDE_PAGER_SYNC=%s; using auto\n", m);
        mode = 0;
    }

    if (mode == 2) {
        g_sync_enabled = 0;
        PDBG("sync init mode=off\n");
        return;
    }
    if (mode == 1) {
        g_sync_enabled = 1;
        PDBG("sync init mode=on forced\n");
        return;
    }

    if (!is_ghostty_env()) {
        g_sync_enabled = 0;
        PDBG("sync init auto decision=off reason=not_ghostty\n");
        return;
    }

    int probe_ms = parse_env_int_range("CLAUDE_PAGER_SYNC_PROBE_MS", 5, 1000, 30);
    int st = -1, raw = 0;
    g_sync_enabled = probe_sync_2026(fd, probe_ms, &st, &raw);
    PDBG("sync init auto decision=%s decrqm_status=%d raw_len=%d probe_ms=%d tmux=%d\n",
         g_sync_enabled ? "on" : "off", st, raw, probe_ms, getenv("TMUX") ? 1 : 0);
}

static void bench_probe_terminal_ready(int tty_fd, const char *label) {
    if (!g_bench_mode || tty_fd < 0) return;

    long long t0 = now_us();
    if (tcdrain(tty_fd) != 0) {
        PDBG("bench term-ready label=%s tcdrain_err=%d\n", label, errno);
        return;
    }
    long long t1 = now_us();

    static const char dsr[] = "\033[6n";
    if (write(tty_fd, dsr, sizeof(dsr) - 1) < 0) {
        PDBG("bench term-ready label=%s dsr_write_err=%d\n", label, errno);
        return;
    }
    long long t2 = now_us();

    int ok = 0;
    int got = 0;
    char c = 0;
    long long deadline = now_us() + 250000; /* 250ms */
    while (now_us() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(tty_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000; /* 10ms */
        int r = select(tty_fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;
        if (!FD_ISSET(tty_fd, &rfds)) continue;
        ssize_t n = read(tty_fd, &c, 1);
        if (n <= 0) continue;
        got += 1;
        if (c == 'R') { ok = 1; break; }
    }
    long long t3 = now_us();

    PDBG(
        "bench term-ready label=%s tcdrain=%.2fms dsr=%.2fms total=%.2fms ok=%d bytes=%d\n",
        label,
        (double)(t1 - t0) / 1000.0,
        (double)(t3 - t2) / 1000.0,
        (double)(t3 - t0) / 1000.0,
        ok, got
    );
}

static void geo_update(void) {
    struct winsize ws;
    if (g_fd >= 0 && ioctl(g_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        int cols = (int)ws.ws_col;
        int clamp = parse_env_int_range("CLAUDE_PAGER_MAX_COLS", 60, 600, 0);
        if (clamp > 0 && cols > clamp) cols = clamp;
        g_cols = cols;
        g_rows = (int)ws.ws_row;
    }
    int qr = g_queue_rows;
    int qr_cap = g_rows - 4;
    if (qr_cap < 0) qr_cap = 0;
    if (qr > qr_cap) qr = qr_cap;
    if (qr < 0) qr = 0;
    int base = 5;
    g_crows = g_rows - base - qr;
    if (g_crows < 1) g_crows = 1;
}

static void on_winch(int s) { (void)s; g_resize = 1; }
static void on_term(int s)  { (void)s; g_quit = 1; }

static void install_signal_handlers(void) {
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = on_term;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGHUP, &sa_term, NULL);

    struct sigaction sa_winch;
    memset(&sa_winch, 0, sizeof(sa_winch));
    sa_winch.sa_handler = on_winch;
    sigemptyset(&sa_winch.sa_mask);
    sigaction(SIGWINCH, &sa_winch, NULL);
}

/* ── Output buffer ─────────────────────────────────────────────────────── */

static char g_ob[256*1024];
static int g_ol;

static void ob_flush(void) {
    if (g_ol <= 0 || g_fd < 0) return;
    if (write_all(g_fd, g_ob, (size_t)g_ol) != 0) g_quit = 1;
    g_ol = 0;
}

static void ob_raw(const char *s, int n) {
    if (!s || n <= 0) return;
    while (n > 0) {
        int room = (int)sizeof(g_ob) - g_ol;
        if (room <= 0) {
            ob_flush();
            room = (int)sizeof(g_ob) - g_ol;
            if (room <= 0) return;
        }
        int take = (n < room) ? n : room;
        memcpy(g_ob + g_ol, s, (size_t)take);
        g_ol += take;
        s += take;
        n -= take;
        if (g_ol == (int)sizeof(g_ob)) ob_flush();
    }
}

static void ob(const char *s) {
    if (!s) return;
    ob_raw(s, (int)strlen(s));
}

static void obf(const char *fmt, ...) {
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    va_list cp;
    va_copy(cp, ap);
    int need = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (need <= 0) { va_end(ap); return; }
    if (need < 4096) {
        char tmp[4096];
        vsnprintf(tmp, sizeof(tmp), fmt, ap);
        ob_raw(tmp, need);
        va_end(ap);
        return;
    }
    char *buf = xmalloc((size_t)need + 1);
    if (!buf) { va_end(ap); return; }
    vsnprintf(buf, (size_t)need + 1, fmt, ap);
    ob_raw(buf, need);
    free(buf);
    va_end(ap);
}

/* ── Minimal JSON scanner ──────────────────────────────────────────────── */

static const char *jws(const char *p) {
    while (*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++;
    return p;
}

static const char *jskip_s(const char *p) {
    p++;
    while (*p) { if (*p=='\\') { p+=2; continue; } if (*p=='"') return p+1; p++; }
    return p;
}

static const char *jskip(const char *p) {
    p = jws(p);
    if (*p == '"') return jskip_s(p);
    if (*p == '{' || *p == '[') {
        char o = *p, c = (o=='{') ? '}' : ']';
        int d = 1; p++;
        while (*p && d > 0) {
            if (*p=='"') { p = jskip_s(p); continue; }
            if (*p==o) d++; else if (*p==c) d--;
            p++;
        }
        return p;
    }
    while (*p && *p!=',' && *p!='}' && *p!=']' && !isspace((unsigned char)*p)) p++;
    return p;
}

static const char *jfind(const char *p, const char *key) {
    if (!p) return NULL;
    p = jws(p);
    if (*p == '{') p++;
    size_t kl = strlen(key);
    while (*p && *p != '}') {
        p = jws(p);
        if (*p != '"') break;
        const char *ks = p + 1;
        p = jskip_s(p);
        size_t kn = (size_t)(p - ks - 1);
        p = jws(p);
        if (*p == ':') p = jws(p + 1);
        if (kn == kl && strncmp(ks, key, kl) == 0) return p;
        p = jskip(p);
        p = jws(p);
        if (*p == ',') p++;
    }
    return NULL;
}

static int jstr(const char *p, char *buf, int mx) {
    if (!p || *p != '"') return 0;
    p++; int i = 0;
    while (*p && i < mx - 1) {
        if (*p == '"') break;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': buf[i++]='\n'; break;
            case 't': buf[i++]='\t'; break;
            case 'r': break;
            case '"': buf[i++]='"'; break;
            case '\\': buf[i++]='\\'; break;
            case '/': buf[i++]='/'; break;
            case 'u': {
                unsigned cp = 0;
                for (int j=1; j<=4 && p[j]; j++) {
                    cp <<= 4;
                    char ch = p[j];
                    if (ch>='0'&&ch<='9') cp |= (unsigned)(ch-'0');
                    else if (ch>='a'&&ch<='f') cp |= 10+(unsigned)(ch-'a');
                    else if (ch>='A'&&ch<='F') cp |= 10+(unsigned)(ch-'A');
                }
                p += 4;
                if (cp<0x80) { buf[i++]=(char)cp; }
                else if (cp<0x800 && i+1<mx) {
                    buf[i++]=(char)(0xC0|(cp>>6));
                    buf[i++]=(char)(0x80|(cp&0x3F));
                } else if (i+2<mx) {
                    buf[i++]=(char)(0xE0|(cp>>12));
                    buf[i++]=(char)(0x80|((cp>>6)&0x3F));
                    buf[i++]=(char)(0x80|(cp&0x3F));
                }
                break;
            }
            default: buf[i++]=*p; break;
            }
            p++;
        } else { buf[i++]=*p++; }
    }
    buf[i]='\0'; return i;
}

static int jstreq(const char *p, const char *s) {
    if (!p || *p!='"') return 0;
    size_t n = strlen(s);
    return strncmp(p+1, s, n)==0 && p[n+1]=='"';
}

static int jint(const char *p) { return p ? atoi(jws(p)) : 0; }

/* ── Prompt queue (session-scoped) ─────────────────────────────────────── */

static void queue_set_notice(const char *msg) {
    if (!msg) msg = "";
    snprintf(g_queue_notice, sizeof(g_queue_notice), "%s", msg);
}

static void queue_item_free(QueueItem *item) {
    if (!item) return;
    free(item->prompt);
    free(item->persisted_id);
    free(item->raw_json);
    memset(item, 0, sizeof(*item));
}

static void queue_clear_items(void) {
    for (int i = 0; i < g_queue.n; i++) queue_item_free(&g_queue.d[i]);
    free(g_queue.d);
    memset(&g_queue, 0, sizeof(g_queue));
    g_queue_fingerprint[0] = '\0';
}

static int queue_push_item(
    const char *prompt,
    const char *persisted_id,
    long long added_us,
    int has_added_us,
    int encoding_json,
    const char *raw_json
) {
    if (!prompt || !*prompt) return 0;
    if (g_queue.n >= g_queue.cap) {
        int ncap = g_queue.cap ? g_queue.cap * 2 : 16;
        QueueItem *nd = xrealloc(g_queue.d, sizeof(QueueItem) * (size_t)ncap);
        if (!nd) return 0;
        g_queue.d = nd;
        g_queue.cap = ncap;
    }
    QueueItem item = {0};
    item.prompt = xstrdup(prompt);
    if (!item.prompt) return 0;
    if (persisted_id && *persisted_id) {
        item.persisted_id = xstrdup(persisted_id);
        if (!item.persisted_id) {
            queue_item_free(&item);
            return 0;
        }
    }
    item.added_us = added_us;
    item.has_added_us = has_added_us ? 1 : 0;
    item.encoding_json = encoding_json ? 1 : 0;
    if (raw_json && *raw_json) {
        item.raw_json = xstrdup(raw_json);
        if (!item.raw_json) {
            queue_item_free(&item);
            return 0;
        }
    }
    g_queue.d[g_queue.n++] = item;
    return 1;
}

static void queue_clamp_selection(void) {
    if (g_queue.n <= 0) {
        g_queue.selected = 0;
        g_queue.scroll_off = 0;
        return;
    }
    if (g_queue.selected < 0) g_queue.selected = 0;
    if (g_queue.selected >= g_queue.n) g_queue.selected = g_queue.n - 1;
    if (g_queue.scroll_off < 0) g_queue.scroll_off = 0;
    if (g_queue.scroll_off > g_queue.selected) g_queue.scroll_off = g_queue.selected;
}

static void input_reset_goal(void) {
    g_input_goal_col = -1;
}

static void input_clear_buffer(void) {
    g_input_len = 0;
    g_input_cursor = 0;
    g_input_buf[0] = '\0';
    input_reset_goal();
}

static void input_set_text(const char *src) {
    snprintf(g_input_buf, sizeof(g_input_buf), "%s", src ? src : "");
    g_input_len = (int)strlen(g_input_buf);
    g_input_cursor = g_input_len;
    input_reset_goal();
}

static int input_prev_boundary(const char *buf, int pos) {
    if (!buf || pos <= 0) return 0;
    pos--;
    while (pos > 0 && (((unsigned char)buf[pos] & 0xC0) == 0x80)) pos--;
    return pos;
}

static int input_next_boundary(const char *buf, int len, int pos) {
    if (!buf || pos >= len) return len;
    pos++;
    while (pos < len && (((unsigned char)buf[pos] & 0xC0) == 0x80)) pos++;
    return pos;
}

static int input_codepoint_cells(const char *buf, int start, int end) {
    if (!buf) return 0;
    if (start < 0) start = 0;
    if (end < start) end = start;
    int cells = 0;
    for (int i = start; i < end;) {
        i = input_next_boundary(buf, end, i);
        cells++;
    }
    return cells;
}

static void input_snapshot_draft_if_needed(void) {
    if (g_input_draft_saved) return;
    snprintf(g_input_draft, sizeof(g_input_draft), "%s", g_input_buf);
    g_input_draft_len = g_input_len;
    g_input_draft_cursor = g_input_cursor;
    g_input_draft_saved = 1;
}

static void input_discard_draft(void) {
    g_input_draft_saved = 0;
    g_input_draft_len = 0;
    g_input_draft_cursor = 0;
    g_input_draft[0] = '\0';
}

static int input_restore_draft(void) {
    if (!g_input_draft_saved) return 0;
    snprintf(g_input_buf, sizeof(g_input_buf), "%s", g_input_draft);
    g_input_len = g_input_draft_len;
    if (g_input_len < 0) g_input_len = 0;
    if (g_input_len >= (int)sizeof(g_input_buf)) g_input_len = (int)sizeof(g_input_buf) - 1;
    g_input_buf[g_input_len] = '\0';
    g_input_cursor = g_input_draft_cursor;
    if (g_input_cursor < 0) g_input_cursor = 0;
    if (g_input_cursor > g_input_len) g_input_cursor = g_input_len;
    input_reset_goal();
    input_discard_draft();
    return 1;
}

static void input_layout_compute(InputLayout *lo, int inner_w) {
    if (!lo) return;
    memset(lo, 0, sizeof(*lo));
    if (inner_w < 1) inner_w = 1;

    int len = g_input_len;
    int ls = 0;
    int cells = 0;
    int line = 0;
    int cursor_line = 0;
    int cursor_col = 0;
    int cursor_found = 0;

    if (g_input_cursor == 0) {
        cursor_found = 1;
        cursor_line = 0;
        cursor_col = 0;
    }

    for (int i = 0; i < len;) {
        if (cells >= inner_w) {
            lo->starts[line] = ls;
            lo->ends[line] = i;
            line++;
            ls = i;
            cells = 0;
            if (!cursor_found && g_input_cursor == i) {
                cursor_found = 1;
                cursor_line = line;
                cursor_col = 0;
            }
            continue;
        }

        if (g_input_buf[i] == '\n') {
            lo->starts[line] = ls;
            lo->ends[line] = i;
            if (!cursor_found && g_input_cursor == i) {
                cursor_found = 1;
                cursor_line = line;
                cursor_col = cells;
            }
            line++;
            i++;
            ls = i;
            cells = 0;
            if (!cursor_found && g_input_cursor == i) {
                cursor_found = 1;
                cursor_line = line;
                cursor_col = 0;
            }
            continue;
        }

        i = input_next_boundary(g_input_buf, len, i);
        cells++;
        if (!cursor_found && g_input_cursor == i) {
            cursor_found = 1;
            cursor_line = line;
            cursor_col = cells;
        }
    }

    lo->starts[line] = ls;
    lo->ends[line] = len;
    if (!cursor_found) {
        cursor_line = line;
        cursor_col = input_codepoint_cells(g_input_buf, ls, g_input_cursor);
    }
    line++;

    lo->total_lines = line > 0 ? line : 1;
    lo->cursor_line = cursor_line;
    lo->cursor_col = cursor_col;
    lo->visible_lines = lo->total_lines;
    if (lo->visible_lines < 1) lo->visible_lines = 1;
    if (lo->visible_lines > INPUT_BOX_MAX_LINES) lo->visible_lines = INPUT_BOX_MAX_LINES;
    lo->visible_start = 0;
    if (lo->total_lines > lo->visible_lines && lo->cursor_line >= lo->visible_lines) {
        lo->visible_start = lo->cursor_line - lo->visible_lines + 1;
    }
}

static int input_box_visible_lines(int inner_w) {
    InputLayout lo;
    input_layout_compute(&lo, inner_w);
    return lo.visible_lines;
}

static void input_move_cursor_left(void) {
    if (g_input_cursor <= 0) return;
    g_input_cursor = input_prev_boundary(g_input_buf, g_input_cursor);
    input_reset_goal();
}

static void input_move_cursor_right(void) {
    if (g_input_cursor >= g_input_len) return;
    g_input_cursor = input_next_boundary(g_input_buf, g_input_len, g_input_cursor);
    input_reset_goal();
}

static void input_move_cursor_home(void) {
    g_input_cursor = 0;
    input_reset_goal();
}

static void input_move_cursor_end(void) {
    g_input_cursor = g_input_len;
    input_reset_goal();
}

static void input_move_cursor_vert(int dir) {
    int inner_w = g_cols - 6;
    if (inner_w < 8) inner_w = 8;
    InputLayout lo;
    input_layout_compute(&lo, inner_w);
    if (lo.total_lines <= 1) return;

    int target = lo.cursor_line + dir;
    if (target < 0) target = 0;
    if (target >= lo.total_lines) target = lo.total_lines - 1;
    if (target == lo.cursor_line) return;

    int goal = (g_input_goal_col >= 0) ? g_input_goal_col : lo.cursor_col;
    g_input_goal_col = goal;

    int target_len = input_codepoint_cells(g_input_buf, lo.starts[target], lo.ends[target]);
    if (goal > target_len) goal = target_len;

    int pos = lo.starts[target];
    for (int cells = 0; cells < goal && pos < lo.ends[target]; cells++) {
        pos = input_next_boundary(g_input_buf, lo.ends[target], pos);
    }
    g_input_cursor = pos;
}

static void queue_recalc_rows(void) {
    int old = g_queue_rows;
    int item_rows = g_queue.n;
    if (item_rows > QUEUE_SHOW_MAX) item_rows = QUEUE_SHOW_MAX;
    if (item_rows < 0) item_rows = 0;

    int queue_ui_active = g_queue_enabled ? 1 : 0;
    if (queue_ui_active) {
        int inner_w = g_cols - 6;
        if (inner_w < 8) inner_w = 8;
        int box_lines = input_box_visible_lines(inner_w);
        g_queue_rows = 2 + box_lines + ((g_queue.n > 0) ? (1 + item_rows) : 0);
    } else {
        g_queue_rows = 0;
    }

    int cap = g_rows - 6;
    if (cap < 0) cap = 0;
    if (g_queue_rows > cap) g_queue_rows = cap;
    if (g_queue_rows < 0) g_queue_rows = 0;

    queue_clamp_selection();
    if (g_queue_rows != old) geo_update();
}

static void queue_compact_key(char *dst, size_t dstlen, const char *src) {
    if (!dst || dstlen == 0) return;
    size_t i = 0;
    if (src && *src) {
        for (const unsigned char *p = (const unsigned char *)src; *p && i + 1 < dstlen; p++) {
            unsigned char c = *p;
            if (isalnum(c) || c == '-' || c == '_' || c == '.') dst[i++] = (char)c;
            else dst[i++] = '_';
        }
    }
    if (i == 0) snprintf(dst, dstlen, "default");
    else dst[i] = '\0';
}

int pager_queue_attachment_for_transcript(
    const char *transcript,
    char *queue_path,
    size_t queue_path_len,
    char *queue_key,
    size_t queue_key_len
) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;

    if (queue_path && queue_path_len > 0) queue_path[0] = '\0';
    if (queue_key && queue_key_len > 0) queue_key[0] = '\0';

    char key[160];
    const char *sid = getenv("CLAUDE_SESSION_ID");
    if (sid && *sid) {
        queue_compact_key(key, sizeof(key), sid);
    } else if (transcript && *transcript) {
        const char *base = strrchr(transcript, '/');
        base = base ? base + 1 : transcript;
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "%s", base);
        char *dot = strrchr(tmp, '.');
        if (dot) *dot = '\0';
        queue_compact_key(key, sizeof(key), tmp);
    } else {
        queue_compact_key(key, sizeof(key), "default");
    }

    if (queue_key && queue_key_len > 0) snprintf(queue_key, queue_key_len, "%s", key);
    if (!queue_path || queue_path_len == 0) return 0;

    char queue_dir[PATH_MAX];
    snprintf(queue_dir, sizeof(queue_dir), "%s/.claude/queues", home);
    snprintf(queue_path, queue_path_len, "%s/%s.queue", queue_dir, key);
    return 0;
}

static void queue_init_path(const char *transcript) {
    g_queue_enabled = 0;
    g_queue_path[0] = '\0';

    char key[160];
    if (pager_queue_attachment_for_transcript(
            transcript, g_queue_path, sizeof(g_queue_path), key, sizeof(key)
        ) != 0) {
        return;
    }

    char claude_dir[PATH_MAX];
    char queue_dir[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(claude_dir, sizeof(claude_dir), "%s/.claude", home);
    snprintf(queue_dir, sizeof(queue_dir), "%s/.claude/queues", home);
    (void)mkdir(claude_dir, 0700);
    (void)mkdir(queue_dir, 0700);
    g_queue_enabled = 1;
}

static int queue_token_to_path(const char *tok, char *out, size_t outlen);
static int input_append_token(const char *tok);
static const char *file_uri_host(void);
static int expand_path_to_abs(char *dst, int dstmax, const char *path, int plen);

static int queue_ensure_dir(const char *path) {
    if (!path || !*path) return -1;
    if (mkdir(path, 0700) == 0) return 0;
    if (errno != EEXIST) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return S_ISDIR(st.st_mode) ? 0 : -1;
}

static int queue_assets_dir(char *out, size_t outlen) {
    if (!out || outlen == 0) return -1;
    out[0] = '\0';
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;

    char claude_dir[PATH_MAX];
    char queue_dir[PATH_MAX];
    snprintf(claude_dir, sizeof(claude_dir), "%s/.claude", home);
    snprintf(queue_dir, sizeof(queue_dir), "%s/.claude/queues", home);
    snprintf(out, outlen, "%s/.claude/queues/assets", home);

    if (queue_ensure_dir(claude_dir) != 0) return -1;
    if (queue_ensure_dir(queue_dir) != 0) return -1;
    if (queue_ensure_dir(out) != 0) return -1;
    return 0;
}

static int queue_export_clipboard_png(const char *dst_path) {
    if (!dst_path || !*dst_path) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp(
            "osascript", "osascript",
            "-e", "on run argv",
            "-e", "set outPath to item 1 of argv",
            "-e", "set imgData to the clipboard as «class PNGf»",
            "-e", "set fRef to open for access POSIX file outPath with write permission",
            "-e", "set eof fRef to 0",
            "-e", "write imgData to fRef",
            "-e", "close access fRef",
            "-e", "end run",
            "--",
            dst_path,
            (char *)NULL
        );
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int queue_attach_clipboard_image(char *out_path, size_t outlen) {
    if (!out_path || outlen == 0) return -1;
    out_path[0] = '\0';

    char assets_dir[PATH_MAX];
    if (queue_assets_dir(assets_dir, sizeof(assets_dir)) != 0) return -1;

    long long ts = now_us();
    pid_t pid = getpid();
    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/clip-%lld-%d.png", assets_dir, ts, (int)pid);

    if (queue_export_clipboard_png(dst) != 0) {
        (void)unlink(dst);
        return -2;
    }

    struct stat st;
    if (stat(dst, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)unlink(dst);
        return -3;
    }
    if (st.st_size <= 0) {
        (void)unlink(dst);
        return -4;
    }
    if (st.st_size > QUEUE_IMAGE_MAX_BYTES) {
        (void)unlink(dst);
        return -5;
    }

    snprintf(out_path, outlen, "%s", dst);
    return 0;
}

static int queue_make_ref_token(const char *path, char *out, size_t outlen) {
    if (!path || !*path || !out || outlen < 4) return 0;
    size_t j = 0;
    out[j++] = '@';
    for (const char *p = path; *p && j + 2 < outlen; p++) {
        char c = *p;
        if (c == ' ' || c == '\\' || c == '"' || c == '\'') out[j++] = '\\';
        out[j++] = c;
    }
    out[j] = '\0';
    return path[0] != '\0' && out[1] != '\0';
}

static int queue_read_pbpaste_text(char *out, size_t outlen) {
    if (!out || outlen == 0) return -1;
    out[0] = '\0';
    FILE *fp = popen("pbpaste 2>/dev/null", "r");
    if (!fp) return -1;
    size_t n = fread(out, 1, outlen - 1, fp);
    out[n] = '\0';
    int st = pclose(fp);
    if (st == -1) return -1;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
    return (int)n;
}

static int queue_attach_clipboard_file_refs(void) {
    char clip[QUEUE_INPUT_MAX * 2];
    int n = queue_read_pbpaste_text(clip, sizeof(clip));
    if (n <= 0) return 0;

    int attached = 0;
    char *p = clip;
    while (*p) {
        while (*p == '\r' || *p == '\n') p++;
        if (!*p) break;

        char *line = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = '\0';

        while (*line && isspace((unsigned char)*line)) line++;
        size_t len = strlen(line);
        while (len > 0 && isspace((unsigned char)line[len - 1])) line[--len] = '\0';
        if (len == 0) continue;

        char path[PATH_MAX];
        if (!queue_token_to_path(line, path, sizeof(path))) continue;

        char ref[PATH_MAX * 2];
        if (!queue_make_ref_token(path, ref, sizeof(ref))) continue;
        if (!input_append_token(ref)) return attached > 0 ? attached : -1;
        attached++;
    }
    return attached;
}

static int input_insert_raw(const char *src, int n) {
    if (!src || n <= 0) return 0;
    int cap = (int)sizeof(g_input_buf) - 1;
    if (g_input_len + n > cap) return 0;
    memmove(g_input_buf + g_input_cursor + n, g_input_buf + g_input_cursor,
            (size_t)(g_input_len - g_input_cursor + 1));
    memcpy(g_input_buf + g_input_cursor, src, (size_t)n);
    g_input_len += n;
    g_input_cursor += n;
    input_reset_goal();
    return 1;
}

static int input_insert_byte(unsigned char c) {
    char ch = (char)c;
    return input_insert_raw(&ch, 1);
}

static int input_delete_prev(void) {
    if (g_input_cursor <= 0) return 0;
    int prev = input_prev_boundary(g_input_buf, g_input_cursor);
    memmove(g_input_buf + prev, g_input_buf + g_input_cursor,
            (size_t)(g_input_len - g_input_cursor + 1));
    g_input_len -= (g_input_cursor - prev);
    g_input_cursor = prev;
    input_reset_goal();
    return 1;
}

static int input_append_token(const char *tok) {
    if (!tok || !*tok) return 0;
    int cap = (int)sizeof(g_input_buf) - 1;
    if (g_input_len >= cap) return 0;
    size_t toklen = strlen(tok);
    int need_leading = (g_input_cursor > 0 && !isspace((unsigned char)g_input_buf[g_input_cursor - 1])) ? 1 : 0;
    int need_trailing = (g_input_cursor < g_input_len && !isspace((unsigned char)g_input_buf[g_input_cursor])) ? 1 : 0;
    if ((size_t)g_input_len + (size_t)need_leading + toklen + (size_t)need_trailing > (size_t)cap) return 0;

    if (need_leading && !input_insert_byte(' ')) return 0;
    if (!input_insert_raw(tok, (int)toklen)) return 0;
    if (need_trailing && !input_insert_byte(' ')) return 0;
    return 1;
}

static void queue_json_escape(const char *src, char *dst, size_t dstlen) {
    if (!dst || dstlen == 0) return;
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < dstlen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' || c == '"') {
            if (j + 2 >= dstlen) break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 2 >= dstlen) break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= dstlen) break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= dstlen) break;
            dst[j++] = '\\';
            dst[j++] = 't';
        } else if (c >= 0x20) {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

#define QUEUE_WRITE_CONFLICT (-2)

static unsigned long long queue_hash_update(
    unsigned long long hash,
    const unsigned char *buf,
    size_t len
) {
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned long long)buf[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void queue_hash_hex(unsigned long long hash, char *out, size_t outlen) {
    if (!out || outlen == 0) return;
    snprintf(out, outlen, "%016llx", hash);
}

static int queue_fingerprint_file(const char *path, char *out, size_t outlen) {
    if (!out || outlen == 0) return 0;
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return 1;
        return 0;
    }

    unsigned long long hash = 1469598103934665603ULL;
    unsigned char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        hash = queue_hash_update(hash, buf, n);
    }
    int ok = ferror(f) == 0;
    fclose(f);
    if (!ok) return 0;
    queue_hash_hex(hash, out, outlen);
    return 1;
}

static int queue_lock_path_for(const char *path, char *out, size_t outlen) {
    if (!path || !*path || !out || outlen == 0) return 0;
    const char *slash = strrchr(path, '/');
    if (!slash) return 0;
    size_t dirlen = (size_t)(slash - path + 1);
    int n = snprintf(out, outlen, "%.*s.%s.lock", (int)dirlen, path, slash + 1);
    return n > 0 && (size_t)n < outlen;
}

static int queue_lock_open(const char *path) {
    char lock_path[PATH_MAX];
    if (!queue_lock_path_for(path, lock_path, sizeof(lock_path))) return -1;
    int fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void queue_lock_close(int fd) {
    if (fd < 0) return;
    (void)flock(fd, LOCK_UN);
    (void)close(fd);
}

static const char *queue_skip_json_string(const char *p) {
    if (!p || *p != '"') return NULL;
    p++;
    while (*p) {
        if (*p == '\\') {
            p++;
            if (*p) p++;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

static const char *queue_skip_json_value(const char *p) {
    if (!p || !*p) return NULL;
    if (*p == '"') return queue_skip_json_string(p);
    if (*p == '{') {
        int depth = 1;
        p++;
        while (*p) {
            if (*p == '"') {
                p = queue_skip_json_string(p);
                if (!p) return NULL;
                continue;
            }
            if (*p == '{') depth++;
            else if (*p == '}') {
                depth--;
                p++;
                if (depth == 0) return p;
                continue;
            }
            p++;
        }
        return NULL;
    }
    if (*p == '[') {
        int depth = 1;
        p++;
        while (*p) {
            if (*p == '"') {
                p = queue_skip_json_string(p);
                if (!p) return NULL;
                continue;
            }
            if (*p == '[') depth++;
            else if (*p == ']') {
                depth--;
                p++;
                if (depth == 0) return p;
                continue;
            }
            p++;
        }
        return NULL;
    }
    while (*p && !strchr(",}]\r\n\t ", *p)) p++;
    return p;
}

static int queue_buf_append(char *dst, size_t dstlen, size_t *used, const char *src, size_t len) {
    if (!dst || !used || !src) return 0;
    if (*used + len >= dstlen) return 0;
    memcpy(dst + *used, src, len);
    *used += len;
    dst[*used] = '\0';
    return 1;
}

static int queue_buf_appendf(char *dst, size_t dstlen, size_t *used, const char *fmt, ...) {
    if (!dst || !used || !fmt) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + *used, dstlen - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || *used + (size_t)n >= dstlen) return 0;
    *used += (size_t)n;
    return 1;
}

static int queue_serialize_json_item(const QueueItem *item, char *out, size_t outlen) {
    if (!item || !out || outlen == 0) return 0;
    char esc_prompt[QUEUE_INPUT_MAX * 2];
    char esc_id[512];
    queue_json_escape(item->prompt ? item->prompt : "", esc_prompt, sizeof(esc_prompt));
    queue_json_escape(item->persisted_id ? item->persisted_id : "", esc_id, sizeof(esc_id));

    size_t used = 0;
    int need_comma = 0;
    out[0] = '\0';
    if (!queue_buf_append(out, outlen, &used, "{", 1)) return 0;

    if (item->raw_json && item->raw_json[0] == '{') {
        const char *p = jws(item->raw_json + 1);
        while (*p && *p != '}') {
            if (*p != '"') return 0;
            const char *entry_start = p;
            const char *key_end = queue_skip_json_string(p);
            if (!key_end) return 0;
            char key[64];
            if (jstr(p, key, sizeof(key)) <= 0) return 0;
            p = jws(key_end);
            if (*p != ':') return 0;
            p = jws(p + 1);
            const char *value_end = queue_skip_json_value(p);
            if (!value_end) return 0;
            if (strcmp(key, "id") != 0 &&
                strcmp(key, "prompt") != 0 &&
                strcmp(key, "added_us") != 0) {
                if (need_comma && !queue_buf_append(out, outlen, &used, ",", 1)) return 0;
                if (!queue_buf_append(out, outlen, &used, entry_start, (size_t)(value_end - entry_start))) {
                    return 0;
                }
                need_comma = 1;
            }
            p = jws(value_end);
            if (*p == ',') p = jws(p + 1);
        }
        if (*p != '}') return 0;
    }

    if (item->persisted_id && *item->persisted_id) {
        if (need_comma && !queue_buf_append(out, outlen, &used, ",", 1)) return 0;
        if (!queue_buf_appendf(out, outlen, &used, "\"id\":\"%s\"", esc_id)) return 0;
        need_comma = 1;
    }
    if (need_comma && !queue_buf_append(out, outlen, &used, ",", 1)) return 0;
    if (!queue_buf_appendf(out, outlen, &used, "\"prompt\":\"%s\"", esc_prompt)) return 0;
    need_comma = 1;
    if (item->has_added_us) {
        if (need_comma && !queue_buf_append(out, outlen, &used, ",", 1)) return 0;
        if (!queue_buf_appendf(out, outlen, &used, "\"added_us\":%lld", item->added_us)) return 0;
    }
    if (!queue_buf_append(out, outlen, &used, "}", 1)) return 0;
    return 1;
}

static int queue_serialize_item_line(const QueueItem *item, char *out, size_t outlen) {
    if (!item || !out || outlen == 0) return 0;
    if (!item->encoding_json &&
        !item->persisted_id &&
        !item->has_added_us &&
        !item->raw_json &&
        item->prompt &&
        !strchr(item->prompt, '\n') &&
        !strchr(item->prompt, '\r')) {
        int n = snprintf(out, outlen, "%s", item->prompt);
        return n >= 0 && (size_t)n < outlen;
    }
    return queue_serialize_json_item(item, out, outlen);
}

static int queue_hex_nybble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int queue_uri_decode_path(char *dst, size_t dstlen, const char *src) {
    if (!dst || dstlen == 0 || !src) return 0;
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dstlen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            int hi = queue_hex_nybble(src[i + 1]);
            int lo = queue_hex_nybble(src[i + 2]);
            if (hi < 0 || lo < 0) return 0;
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (c == '\0' || c == '\n' || c == '\r') return 0;
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
    return src[0] == '\0' ? 0 : 1;
}

static int queue_uri_authority_is_local(const char *authority) {
    if (!authority || !*authority) return 1;
    if (strcasecmp(authority, "localhost") == 0) return 1;

    const char *local = file_uri_host();
    if (local && *local && strcasecmp(authority, local) == 0) return 1;

    char auth_short[256];
    char local_short[256];
    size_t ai = 0;
    while (authority[ai] && authority[ai] != '.' && authority[ai] != ':' &&
           ai + 1 < sizeof(auth_short)) {
        auth_short[ai] = authority[ai];
        ai++;
    }
    auth_short[ai] = '\0';

    size_t li = 0;
    while (local && local[li] && local[li] != '.' && local[li] != ':' &&
           li + 1 < sizeof(local_short)) {
        local_short[li] = local[li];
        li++;
    }
    local_short[li] = '\0';

    return auth_short[0] && local_short[0] &&
           strcasecmp(auth_short, local_short) == 0;
}

static int queue_token_to_path(const char *tok, char *out, size_t outlen) {
    if (!tok || !*tok || !out || outlen == 0) return 0;
    while (*tok == '"' || *tok == '\'' || *tok == '(' || *tok == '[') tok++;
    size_t n = strlen(tok);
    while (n > 0) {
        char c = tok[n - 1];
        if (c == '"' || c == '\'' || c == ')' || c == ']' || c == ',' || c == '.' || c == ';' || c == ':') n--;
        else break;
    }
    if (n == 0) return 0;

    char tmp[PATH_MAX];
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < sizeof(tmp); i++) {
        char c = tok[i];
        if (c == '\\' && i + 1 < n) {
            char nx = tok[i + 1];
            if (nx == ' ' || nx == '\\' || nx == '"' || nx == '\'') {
                tmp[j++] = nx;
                i++;
                continue;
            }
        }
        tmp[j++] = c;
    }
    tmp[j] = '\0';

    const char *p = tmp;
    int uri_mode = 0;
    if (*p == '@') p++;
    if (strncmp(p, "file://", 7) == 0) {
        uri_mode = 1;
        p += 7;
        if (*p && *p != '/') {
            const char *slash = strchr(p, '/');
            if (!slash) return 0;
            char authority[256];
            size_t alen = (size_t)(slash - p);
            if (alen == 0 || alen >= sizeof(authority)) return 0;
            memcpy(authority, p, alen);
            authority[alen] = '\0';
            if (!queue_uri_authority_is_local(authority)) return 0;
            p = slash;
        }
    }
    if (*p == '@') p++;
    if (*p != '/' && !(*p == '~' && p[1] == '/')) return 0;

    char decoded[PATH_MAX];
    if (uri_mode) {
        if (!queue_uri_decode_path(decoded, sizeof(decoded), p)) return 0;
    } else {
        size_t plen = strlen(p);
        if (plen >= sizeof(decoded)) return 0;
        memcpy(decoded, p, plen + 1);
    }

    char abs_path[PATH_MAX];
    const char *path = decoded;
    if (decoded[0] == '~' && decoded[1] == '/') {
        int alen = expand_path_to_abs(abs_path, sizeof(abs_path), decoded, (int)strlen(decoded));
        if (alen <= 0) return 0;
        path = abs_path;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    snprintf(out, outlen, "%s", path);
    return 1;
}

static void queue_build_prompt_with_refs(const char *in, char *out, size_t outlen) {
    if (!out || outlen == 0) return;
    out[0] = '\0';
    if (!in || !*in) return;

    char refs[8][PATH_MAX];
    int refn = 0;
    char body[QUEUE_INPUT_MAX * 2];
    size_t body_used = 0;
    body[0] = '\0';

    const char *body_start = in;
    const char *preamble = in;
    int saw_preamble_ref = 0;
    while (*preamble) {
        const char *line_end = strchr(preamble, '\n');
        if (!line_end) line_end = preamble + strlen(preamble);

        const char *trim = preamble;
        while (trim < line_end && (*trim == ' ' || *trim == '\t' || *trim == '\r')) trim++;
        const char *trim_end = line_end;
        while (trim_end > trim && (trim_end[-1] == ' ' || trim_end[-1] == '\t' || trim_end[-1] == '\r')) trim_end--;

        if (trim == trim_end) {
            if (saw_preamble_ref) {
                body_start = (*line_end == '\n') ? line_end + 1 : line_end;
            }
            break;
        }

        char path[PATH_MAX];
        char token[PATH_MAX];
        size_t tlen = (size_t)(trim_end - trim);
        if (tlen >= sizeof(token)) tlen = sizeof(token) - 1;
        memcpy(token, trim, tlen);
        token[tlen] = '\0';
        if (!queue_token_to_path(token, path, sizeof(path))) break;

        int dup = 0;
        for (int i = 0; i < refn; i++) {
            if (strcmp(refs[i], path) == 0) { dup = 1; break; }
        }
        if (!dup && refn < (int)(sizeof(refs) / sizeof(refs[0]))) {
            snprintf(refs[refn++], sizeof(refs[0]), "%s", path);
        }
        saw_preamble_ref = 1;
        body_start = (*line_end == '\n') ? line_end + 1 : line_end;
        preamble = body_start;
    }

    const char *line = body_start;
    int body_lines = 0;
    while (1) {
        const char *line_end = strchr(line, '\n');
        int has_nl = line_end != NULL;
        if (!line_end) line_end = line + strlen(line);

        char linebuf[QUEUE_INPUT_MAX * 2];
        size_t line_used = 0;
        int skipped_ref = 0;
        const char *cursor = line;

        while (cursor < line_end) {
            const char *seg = cursor;
            while (cursor < line_end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')) cursor++;
            if (cursor >= line_end) break;
            const char *tok_start = cursor;

            char tok[PATH_MAX];
            int tlen = 0;
            const char *tok_end = cursor;
            if (*cursor == '"' || *cursor == '\'') {
                char q = *cursor++;
                tok_end = cursor;
                while (tok_end < line_end && *tok_end != q && tlen + 1 < (int)sizeof(tok)) {
                    if (*tok_end == '\\' && tok_end + 1 < line_end) {
                        tok[tlen++] = tok_end[1];
                        tok_end += 2;
                        continue;
                    }
                    tok[tlen++] = *tok_end++;
                }
                if (tok_end < line_end && *tok_end == q) tok_end++;
            } else {
                while (tok_end < line_end && !isspace((unsigned char)*tok_end) &&
                       tlen + 1 < (int)sizeof(tok)) {
                    tok[tlen++] = *tok_end++;
                }
                while (tok_end < line_end && !isspace((unsigned char)*tok_end)) tok_end++;
            }
            tok[tlen] = '\0';
            if (tlen <= 0) {
                cursor = tok_end;
                continue;
            }

            char path[PATH_MAX];
            if (queue_token_to_path(tok, path, sizeof(path))) {
                int dup = 0;
                for (int i = 0; i < refn; i++) {
                    if (strcmp(refs[i], path) == 0) { dup = 1; break; }
                }
                if (!dup && refn < (int)(sizeof(refs) / sizeof(refs[0]))) {
                    snprintf(refs[refn++], sizeof(refs[0]), "%s", path);
                }
                skipped_ref = 1;
                cursor = tok_end;
                continue;
            }

            const char *copy_from = (line_used == 0 && skipped_ref) ? tok_start : seg;
            size_t seglen = (size_t)(tok_end - copy_from);
            if (seglen > 0 && line_used + seglen < sizeof(linebuf)) {
                memcpy(linebuf + line_used, copy_from, seglen);
                line_used += seglen;
            }
            cursor = tok_end;
        }

        linebuf[line_used] = '\0';
        int keep_line = 0;
        if (line_used > 0) {
            keep_line = 1;
        } else {
            const char *q = line;
            while (q < line_end && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
            keep_line = (!skipped_ref && q == line_end) ? 1 : 0;
        }

        if (keep_line && body_used < sizeof(body) - 1) {
            if (body_lines > 0 && body_used + 1 < sizeof(body)) body[body_used++] = '\n';
            if (line_used > 0) {
                size_t copy = line_used;
                if (copy > sizeof(body) - body_used - 1) copy = sizeof(body) - body_used - 1;
                memcpy(body + body_used, linebuf, copy);
                body_used += copy;
            }
            body[body_used] = '\0';
            body_lines++;
        }

        if (!has_nl) break;
        line = line_end + 1;
    }

    size_t used = 0;
    for (int i = 0; i < refn; i++) {
        int n = snprintf(out + used, outlen - used, "@%s\n", refs[i]);
        if (n <= 0 || (size_t)n >= outlen - used) {
            out[outlen - 1] = '\0';
            return;
        }
        used += (size_t)n;
    }
    if (refn > 0 && body[0] && used + 1 < outlen) out[used++] = '\n';
    out[used] = '\0';
    if (body[0]) strncat(out, body, outlen - used - 1);
}

static int queue_append_prompt(const char *prompt) {
    if (!g_queue_enabled || !g_queue_path[0] || !prompt || !*prompt) return -1;

    char enriched[QUEUE_INPUT_MAX * 2];
    queue_build_prompt_with_refs(prompt, enriched, sizeof(enriched));
    const char *src = enriched[0] ? enriched : prompt;

    long long ts = now_us();
    char id[64];
    snprintf(id, sizeof(id), "%lld", ts);
    QueueItem item = {
        .prompt = (char *)src,
        .persisted_id = id,
        .added_us = ts,
        .has_added_us = 1,
        .encoding_json = 1,
        .raw_json = NULL,
    };
    char line[QUEUE_INPUT_MAX * 2 + 512];
    if (!queue_serialize_item_line(&item, line, sizeof(line))) return -1;
    size_t line_len = strlen(line);
    if (line_len + 1 >= sizeof(line)) return -1;
    line[line_len++] = '\n';
    line[line_len] = '\0';

    int lock_fd = queue_lock_open(g_queue_path);
    if (lock_fd < 0) return -1;

    int fd = open(g_queue_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    int ok = -1;
    if (fd >= 0) {
        ok = write_all(fd, line, line_len) == 0 ? 0 : -1;
        close(fd);
    }
    queue_lock_close(lock_fd);
    g_queue_stamp.valid = 0;
    g_queue_fingerprint[0] = '\0';
    return ok;
}

static int queue_write_all_items(void) {
    if (!g_queue_enabled || !g_queue_path[0]) return -1;
    int lock_fd = queue_lock_open(g_queue_path);
    if (lock_fd < 0) return -1;

    char current_fp[17];
    if (!queue_fingerprint_file(g_queue_path, current_fp, sizeof(current_fp))) {
        queue_lock_close(lock_fd);
        return -1;
    }
    if (g_queue_fingerprint[0] != '\0' && strcmp(current_fp, g_queue_fingerprint) != 0) {
        queue_lock_close(lock_fd);
        return QUEUE_WRITE_CONFLICT;
    }

    if (g_queue.n <= 0) {
        (void)unlink(g_queue_path);
        g_queue_stamp.valid = 0;
        g_queue_fingerprint[0] = '\0';
        queue_lock_close(lock_fd);
        return 0;
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", g_queue_path);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        queue_lock_close(lock_fd);
        return -1;
    }
    (void)fchmod(fd, 0600);

    int ok = 0;
    for (int i = 0; i < g_queue.n; i++) {
        char line[QUEUE_INPUT_MAX * 2 + 512];
        if (!queue_serialize_item_line(&g_queue.d[i], line, sizeof(line))) {
            ok = -1;
            break;
        }
        size_t n = strlen(line);
        if (n + 1 >= sizeof(line)) {
            ok = -1;
            break;
        }
        line[n++] = '\n';
        if (write_all(fd, line, n) != 0) {
            ok = -1;
            break;
        }
    }
    close(fd);

    if (ok == 0) {
        if (rename(tmp, g_queue_path) != 0) ok = -1;
    }
    if (ok != 0) (void)unlink(tmp);

    g_queue_stamp.valid = 0;
    g_queue_fingerprint[0] = '\0';
    queue_lock_close(lock_fd);
    return ok;
}

static void queue_set_input_from_selected(void) {
    if (g_queue.n <= 0) return;
    queue_clamp_selection();
    if (g_queue.selected < 0 || g_queue.selected >= g_queue.n) return;
    const char *src = g_queue.d[g_queue.selected].prompt ? g_queue.d[g_queue.selected].prompt : "";
    input_set_text(src);
    g_edit_index = g_queue.selected;
    queue_set_notice("editing queued prompt");
}

static int queue_cycle_edit(int dir) {
    if (g_queue.n <= 0) return 0;
    if (dir == 0) return 0;

    if (g_edit_index < 0) {
        input_snapshot_draft_if_needed();
        g_queue.selected = (dir < 0) ? (g_queue.n - 1) : 0;
        queue_set_input_from_selected();
        return 1;
    }

    int next = g_queue.selected + dir;
    if (next < 0 || next >= g_queue.n) {
        if (input_restore_draft()) {
            g_edit_index = -1;
            queue_set_notice("draft restored");
            return 1;
        }
        return 0;
    }

    g_queue.selected = next;
    queue_set_input_from_selected();
    return 1;
}

static int queue_load_from_disk(void) {
    if (!g_queue_enabled || !g_queue_path[0]) return 0;

    struct stat st;
    if (stat(g_queue_path, &st) != 0) {
        g_queue_stamp.valid = 0;
        if (g_queue.n > 0) {
            queue_clear_items();
            queue_recalc_rows();
            return 1;
        }
        return 0;
    }
    if (!file_stamp_changed(g_queue_path, &g_queue_stamp)) return 0;

    FILE *f = fopen(g_queue_path, "r");
    if (!f) return 0;

    int old_n = g_queue.n;
    queue_clear_items();
    unsigned long long hash = 1469598103934665603ULL;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len = 0;
    while ((len = getline(&line, &cap, f)) != -1) {
        hash = queue_hash_update(hash, (const unsigned char *)line, (size_t)len);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len <= 0) continue;
        const char *pv = (*line == '{') ? jfind(line, "prompt") : NULL;
        if (pv && *pv == '"') {
            char prompt[QUEUE_INPUT_MAX];
            char item_id[256];
            item_id[0] = '\0';
            const char *iv = jfind(line, "id");
            if (iv && *iv == '"') (void)jstr(iv, item_id, sizeof(item_id));
            const char *av = jfind(line, "added_us");
            long long added_us = av ? strtoll(jws(av), NULL, 10) : 0;
            int has_added_us = av != NULL;
            if (jstr(pv, prompt, sizeof(prompt)) > 0) {
                (void)queue_push_item(
                    prompt,
                    item_id[0] ? item_id : NULL,
                    added_us,
                    has_added_us,
                    1,
                    line
                );
            }
        } else {
            (void)queue_push_item(line, NULL, 0, 0, 0, NULL);
        }
    }
    int read_ok = ferror(f) == 0;
    free(line);
    fclose(f);
    if (!read_ok) return 0;
    queue_hash_hex(hash, g_queue_fingerprint, sizeof(g_queue_fingerprint));

    if (g_queue.n > old_n) g_queue.selected = g_queue.n - 1;
    queue_recalc_rows();
    return 1;
}

static void queue_compact_prompt(const char *src, char *dst, int max_chars) {
    if (!dst || max_chars <= 0) return;
    int truncated = 0;
    int i = 0;
    for (const char *p = src ? src : ""; *p && i < max_chars; p++) {
        if (*p == '\n' || *p == '\r') {
            truncated = 1;
            break;
        }
        char c = *p;
        if ((unsigned char)c < 0x20) c = ' ';
        dst[i++] = c;
    }
    if (!truncated && src) {
        const char *tail = src + i;
        truncated = (*tail != '\0');
    }
    if (truncated) {
        if (i == 0) {
            int dots = max_chars < 3 ? max_chars : 3;
            for (int j = 0; j < dots; j++) dst[i++] = '.';
        } else if (i + 4 <= max_chars) {
            dst[i++] = ' ';
            dst[i++] = '.';
            dst[i++] = '.';
            dst[i++] = '.';
        } else if (i + 3 <= max_chars) {
            dst[i++] = '.';
            dst[i++] = '.';
            dst[i++] = '.';
        } else {
            if (i >= 1) dst[i - 1] = '.';
            if (i >= 2) dst[i - 2] = '.';
            if (i >= 3) dst[i - 3] = '.';
        }
    }
    dst[i] = '\0';
}

static void input_pending_reset(void) {
    g_input_pending_len = 0;
    g_input_pending_pos = 0;
}

static void input_pending_push(unsigned char c) {
    if (g_input_pending_len >= (int)sizeof(g_input_pending)) return;
    g_input_pending[g_input_pending_len++] = c;
}

static int input_pending_pop(void) {
    if (g_input_pending_pos >= g_input_pending_len) {
        input_pending_reset();
        return -1;
    }
    int c = (int)g_input_pending[g_input_pending_pos++];
    if (g_input_pending_pos >= g_input_pending_len) input_pending_reset();
    return c;
}

static void input_pending_consume_chunk(const unsigned char *buf, ssize_t n) {
    if (!buf || n <= 0) return;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == 0x1b && i + 1 < n && buf[i + 1] == '[') {
            ssize_t j = i + 2;
            while (j < n) {
                unsigned char c = buf[j];
                if (c >= 0x40 && c <= 0x7e) break;
                j++;
            }
            i = (j < n) ? j : (n - 1);
            continue;
        }
        if (buf[i] == 0x1b && i + 2 < n && buf[i + 1] == 'O') {
            i += 2;
            continue;
        }
        if (i + 5 < n &&
            buf[i] == 0x1b && buf[i + 1] == '[' &&
            buf[i + 2] == '2' && buf[i + 3] == '0' &&
            buf[i + 4] == '0' && buf[i + 5] == '~') {
            i += 5; /* bracketed-paste start */
            continue;
        }
        if (i + 5 < n &&
            buf[i] == 0x1b && buf[i + 1] == '[' &&
            buf[i + 2] == '2' && buf[i + 3] == '0' &&
            buf[i + 4] == '1' && buf[i + 5] == '~') {
            i += 5; /* bracketed-paste end */
            continue;
        }

        unsigned char c = buf[i];
        if (c == '\r' || c == '\n') {
            input_pending_push(' ');
            continue;
        }
        if (c == '\t') {
            input_pending_push(' ');
            continue;
        }
        if (c < 0x20 || c == 0x7f) continue;
        input_pending_push(c);
    }
}

/* ── Dynamic line array ────────────────────────────────────────────────── */

typedef struct {
    char **d;
    int n, cap;
    int max_keep;
    int drop_chunk;
    int dropped_total;
} Lines;

static void L_init(Lines *l) {
    memset(l, 0, sizeof(*l));
    l->drop_chunk = 256;
}

static void L_set_limit(Lines *l, int max_keep) {
    if (!l) return;
    l->max_keep = max_keep > 0 ? max_keep : 0;
    if (l->max_keep > 0) {
        int chunk = l->max_keep / 8;
        if (chunk < 1) chunk = 1;
        if (chunk > 8192) chunk = 8192;
        if (chunk > l->max_keep) chunk = l->max_keep;
        l->drop_chunk = chunk;
    }
}

static int L_drop_head(Lines *l, int drop) {
    if (!l || drop <= 0 || l->n <= 0) return 0;
    if (drop > l->n) drop = l->n;
    for (int i = 0; i < drop; i++) free(l->d[i]);
    memmove(l->d, l->d + drop, (size_t)(l->n - drop) * sizeof(char *));
    l->n -= drop;
    l->dropped_total += drop;
    return drop;
}

static void L_push(Lines *l, const char *s) {
    if (!l || !s || g_oom) return;
    if (l->max_keep > 0) {
        int chunk = l->drop_chunk > 0 ? l->drop_chunk : 1;
        int high = l->max_keep + chunk;
        if (l->n >= high) {
            int drop = l->n - l->max_keep + 1;
            if (drop > 0) L_drop_head(l, drop);
        }
    }
    if (l->n >= l->cap) {
        int ncap = l->cap ? l->cap * 2 : 128;
        char **nd = xrealloc(l->d, sizeof(char*) * (size_t)ncap);
        if (!nd) return;
        l->d = nd;
        l->cap = ncap;
    }
    char *cp = xstrdup(s);
    if (!cp) return;
    l->d[l->n++] = cp;
}

static void L_push_blank_once(Lines *l) {
    if (!l) return;
    if (l->n == 0 || !l->d || !l->d[l->n - 1] || l->d[l->n - 1][0] != '\0') {
        L_push(l, "");
    }
}

static void L_prepend(Lines *l, const char *s) {
    if (!l || !s || g_oom) return;
    if (l->n >= l->cap) {
        int ncap = l->cap ? l->cap * 2 : 128;
        char **nd = xrealloc(l->d, sizeof(char*) * (size_t)ncap);
        if (!nd) return;
        l->d = nd;
        l->cap = ncap;
    }
    char *cp = xstrdup(s);
    if (!cp) return;
    memmove(l->d + 1, l->d, (size_t)l->n * sizeof(char *));
    l->d[0] = cp;
    l->n++;
    if (l->max_keep > 0 && l->n > l->max_keep) {
        free(l->d[l->n - 1]);
        l->n--;
    }
}

static void L_free(Lines *l) {
    if (!l) return;
    int keep = l->max_keep;
    int chunk = l->drop_chunk;
    for (int i=0; i<l->n; i++) free(l->d[i]);
    free(l->d);
    L_init(l);
    l->max_keep = keep;
    l->drop_chunk = chunk;
}

/* ── ANSI-aware visible length ─────────────────────────────────────────── */

static int vlen(const char *s) {
    int n = 0;
    while (*s) {
        if (*s == '\033') {
            s++;
            if (*s=='[') { s++; while (*s && !isalpha((unsigned char)*s) && *s!='~') s++; if (*s) s++; }
            else if (*s==']') {
                while (*s) {
                    if (*s=='\a') { s++; break; }
                    if (s[0]=='\033' && s[1]=='\\') { s+=2; break; }
                    s++;
                }
            }
            else if (*s) s++;
        } else { n++; s++; }
    }
    return n;
}

static void L_pushw(Lines *l, const char *s) {
    L_push(l, s);
    int v = vlen(s);
    if (v > g_cols) { int extra = (v + g_cols - 1) / g_cols - 1; for (int i=0; i<extra; i++) L_push(l, ""); }
}

/* ── OSC-8 linkification ──────────────────────────────────────────────── */

static int is_urlch(char c) {
    if (c <= ' ') return 0;
    if (c=='<'||c=='>'||c=='"'||c=='\''||c=='\\'||c==')'||c=='}'||c==']') return 0;
    return 1;
}

static int is_path_token_char(unsigned char c) {
    if (c == 0 || c == '\n' || c == '\r' || c == '\t' || c == '\033') return 0;
    return 1;
}

static int is_path_lead_boundary(unsigned char c) {
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-' || c == '.' || c == '/' || c == '~') {
        return 0;
    }
    return 1;
}

static int path_looks_valid(const char *s, int n) {
    if (!s || n < 2) return 0;
    if (!(s[0] == '/' || (s[0] == '~' && n >= 3 && s[1] == '/'))) return 0;
    int slash = 0;
    for (int i = 1; i < n; i++) if (s[i] == '/') { slash = 1; break; }
    return slash;
}

static const char *file_uri_host(void) {
    static char host[256];
    static int inited = 0;
    if (inited) return host;
    inited = 1;
    if (gethostname(host, sizeof(host)) != 0 || !host[0]) {
        strncpy(host, "localhost", sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }
    for (size_t i = 0; host[i]; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c <= ' ' || c == '/' || c == '\\') host[i] = '-';
    }
    return host;
}

static int allow_remote_file_links(void) {
    if (g_allow_remote_file_links >= 0) return g_allow_remote_file_links;
    int remote = (getenv("SSH_CONNECTION") || getenv("SSH_TTY")) ? 1 : 0;
    if (!remote) {
        g_allow_remote_file_links = 1;
        return 1;
    }
    g_allow_remote_file_links = env_enabled("CLAUDE_PAGER_LINK_REMOTE") ? 1 : 0;
    return g_allow_remote_file_links;
}

static int uri_path_is_safe(const char *path, int plen);
static void uri_encode_path(char *dst, int dstmax, const char *src, int srclen);
static int expand_path_to_abs(char *dst, int dstmax, const char *path, int plen);

static int build_file_uri_target(char *dst, int dstmax, const char *path, int plen) {
    if (!dst || dstmax < 16 || !path || plen <= 0) return 0;
    if (!allow_remote_file_links()) return 0;

    if (path[0] == '/' && uri_path_is_safe(path, plen)) {
        int n = snprintf(dst, (size_t)dstmax, "file://%.*s", plen, path);
        return (n > 0 && n < dstmax) ? 1 : 0;
    }

    char abs_path[8192];
    int alen = expand_path_to_abs(abs_path, sizeof(abs_path), path, plen);
    if (alen <= 0) return 0;

    char enc[16384];
    uri_encode_path(enc, sizeof(enc), abs_path, alen);
    int n = snprintf(dst, (size_t)dstmax, "file://%s", enc);
    return (n > 0 && n < dstmax) ? 1 : 0;
}

static int uri_is_unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
}

static int uri_path_is_safe(const char *path, int plen) {
    if (!path || plen <= 0) return 0;
    for (int i = 0; i < plen; i++) {
        if (!uri_is_unreserved((unsigned char)path[i])) return 0;
    }
    return 1;
}

static void uri_encode_path(char *dst, int dstmax, const char *src, int srclen) {
    int o = 0;
    for (int i = 0; i < srclen && o < dstmax - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (uri_is_unreserved(c)) {
            dst[o++] = (char)c;
        } else if (o < dstmax - 3) {
            static const char hx[] = "0123456789ABCDEF";
            dst[o++] = '%';
            dst[o++] = hx[(c >> 4) & 0xF];
            dst[o++] = hx[c & 0xF];
        } else {
            break;
        }
    }
    dst[o] = '\0';
}

static int expand_path_to_abs(char *dst, int dstmax, const char *path, int plen) {
    if (!dst || dstmax < 4 || !path || plen <= 0) return 0;
    if (path[0] == '/' && plen < dstmax) {
        memcpy(dst, path, (size_t)plen);
        dst[plen] = '\0';
        return plen;
    }
    if (path[0] == '~' && plen >= 2 && path[1] == '/') {
        const char *home = getenv("HOME");
        if (!home || !home[0]) return 0;
        int hlen = (int)strlen(home);
        if (hlen + plen - 1 >= dstmax) return 0;
        memcpy(dst, home, (size_t)hlen);
        memcpy(dst + hlen, path + 1, (size_t)(plen - 1));
        dst[hlen + plen - 1] = '\0';
        return hlen + plen - 1;
    }
    return 0;
}

static int looks_like_table_row(const char *s) {
    if (!s || !*s) return 0;
    if (strstr(s, "│") || strstr(s, "┌") || strstr(s, "├") || strstr(s, "└")) return 1;
    int pipes = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '|') pipes++;
    }
    if (pipes < 2) return 0;
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '|') return 1;
    return strstr(s, " | ") != NULL;
}

static int looks_like_tool_header_row(const char *s) {
    if (!s || !*s) return 0;
    if (!strstr(s, BUL)) return 0;
    if (strstr(s, "Update(") || strstr(s, "Create(") ||
        strstr(s, "Edit(") || strstr(s, "Write(") ||
        strstr(s, "Read ")) {
        return 1;
    }
    return 0;
}

/* Shorten URL for display: strip protocol, truncate with … */
static void shorten_url(char *dst, int dstmax, const char *url, int ulen) {
    const char *d = url;
    int dlen = ulen;
    if (dlen >= 8 && memcmp(d, "https://", 8) == 0) { d += 8; dlen -= 8; }
    else if (dlen >= 7 && memcmp(d, "http://", 7) == 0) { d += 7; dlen -= 7; }

    if (dlen <= 60) {
        int n = dlen < dstmax-1 ? dlen : dstmax-1;
        memcpy(dst, d, n); dst[n] = '\0';
        return;
    }

    const char *sl = memchr(d, '/', dlen);
    if (!sl) {
        int n = 59 < dstmax-4 ? 59 : dstmax-4;
        memcpy(dst, d, n); memcpy(dst+n, ELL, 3); dst[n+3] = '\0';
        return;
    }

    int domlen = (int)(sl - d) + 1;
    int avail = 60 - domlen - 1; /* 1 visible char for … */
    if (avail < 8) {
        int n = 59 < dstmax-4 ? 59 : dstmax-4;
        memcpy(dst, d, n); memcpy(dst+n, ELL, 3); dst[n+3] = '\0';
        return;
    }

    int tail = avail / 3; if (tail > 20) tail = 20;
    int head = avail - tail;
    const char *path = sl + 1;
    int pathlen = dlen - domlen;
    if (tail > pathlen) tail = pathlen;
    if (head > pathlen) head = pathlen;

    int o = 0;
    memcpy(dst+o, d, domlen); o += domlen;
    int hc = head < pathlen ? head : pathlen;
    memcpy(dst+o, path, hc); o += hc;
    memcpy(dst+o, ELL, 3); o += 3;
    if (tail > 0 && pathlen > tail) {
        memcpy(dst+o, path + pathlen - tail, tail); o += tail;
    }
    dst[o] = '\0';
}

/* Shorten file path for display: …/parent/filename */
static void shorten_path(char *dst, int dstmax, const char *path, int plen) {
    if (plen <= 50) {
        int n = plen < dstmax-1 ? plen : dstmax-1;
        memcpy(dst, path, n); dst[n] = '\0';
        return;
    }

    /* Find last two slashes (scanning backwards) */
    const char *last = NULL, *prev = NULL;
    for (int i = plen-1; i >= 0; i--) {
        if (path[i] == '/') {
            if (!last) last = path + i;
            else if (!prev) { prev = path + i; break; }
        }
    }

    if (!last) {
        snprintf(dst, dstmax, ELL "/%.*s", plen < 48 ? plen : 48, path);
        return;
    }

    if (prev) {
        int slen = (int)(path + plen - prev);
        if (slen + 1 <= 50) { /* +1 for … visible char */
            snprintf(dst, dstmax, ELL "%.*s", slen, prev);
            return;
        }
    }

    int flen = (int)(path + plen - last);
    if (flen + 1 <= 50) {
        snprintf(dst, dstmax, ELL "%.*s", flen, last);
        return;
    }
    snprintf(dst, dstmax, ELL "/%.*s", 48, last + 1);
}

static void linkify(char *dst, int dstmax, const char *src) {
    int o = 0;
    const char *p = src;
    int compact_labels = !(looks_like_table_row(src) || looks_like_tool_header_row(src));
    int in_osc8_label = 0;

    #define LF_CH(ch)  do { if (o < dstmax-1) dst[o++] = (ch); } while(0)
    #define LF_S(s)    do { const char *_s=(s); while(*_s && o<dstmax-1) dst[o++]=*_s++; } while(0)

    while (*p && o < dstmax - 200) {
        /* Pass through ANSI CSI sequences */
        if (p[0]=='\033' && p[1]=='[') {
            LF_CH(*p++); LF_CH(*p++);
            while (*p && !isalpha((unsigned char)*p) && *p!='~') LF_CH(*p++);
            if (*p) LF_CH(*p++);
            continue;
        }
        /* Pass through existing OSC-8 sequences */
        if (p[0]=='\033' && p[1]==']' && p[2]=='8' && p[3]==';') {
            const char *payload = p + 4;
            int is_close = (*payload == '\a') || (payload[0] == '\033' && payload[1] == '\\');
            while (*p) {
                if (*p=='\a') { LF_CH(*p++); break; }
                if (p[0]=='\033' && p[1]=='\\') { LF_CH(*p++); LF_CH(*p++); break; }
                LF_CH(*p++);
            }
            in_osc8_label = is_close ? 0 : 1;
            continue;
        }
        /* Pass through other OSC sequences */
        if (p[0]=='\033' && p[1]==']') {
            while (*p) {
                if (*p=='\a') { LF_CH(*p++); break; }
                if (p[0]=='\033' && p[1]=='\\') { LF_CH(*p++); LF_CH(*p++); break; }
                LF_CH(*p++);
            }
            continue;
        }
        /* Pass through other ESC sequences */
        if (p[0]=='\033') {
            LF_CH(*p++);
            if (*p) LF_CH(*p++);
            continue;
        }
        if (in_osc8_label) {
            LF_CH(*p++);
            continue;
        }
        /* Detect URL */
        if (strncmp(p,"http://",7)==0 || strncmp(p,"https://",8)==0) {
            const char *start = p;
            while (*p && is_urlch(*p)) p++;
            while (p>start && (p[-1]=='.'||p[-1]==','||p[-1]==';'||p[-1]==':')) p--;
            int ulen = (int)(p - start);
            if (ulen > 10 && o + ulen + 200 < dstmax) {
                char label[256];
                if (compact_labels) {
                    shorten_url(label, sizeof(label), start, ulen);
                } else {
                    int n = ulen < (int)sizeof(label) - 1 ? ulen : (int)sizeof(label) - 1;
                    memcpy(label, start, (size_t)n);
                    label[n] = '\0';
                }
                LF_S("\033]8;;");
                for (int i=0; i<ulen; i++) LF_CH(start[i]);
                LF_CH('\a');
                LF_S(BO C_URL UL_ON);
                LF_S(label);
                LF_S(UL_OFF "\033[22m");
                LF_S("\033]8;;\a");
            } else {
                for (int i=0; i<ulen; i++) LF_CH(start[i]);
            }
            continue;
        }
        /* Detect file path: /segment/segment... or ~/segment... */
        if ((p[0]=='/' || (p[0]=='~' && p[1]=='/')) &&
            (p == src || is_path_lead_boundary((unsigned char)p[-1]))) {
            const char *start = p;
            const char *sp = p + 1;
            while (*sp && is_path_token_char((unsigned char)*sp)) {
                if (*sp == ' ') {
                    const char *q = sp + 1;
                    if (*q == '/' || (*q == '~' && q[1] == '/')) break;
                    int path_hint = 0;
                    while (*q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t') {
                        if (*q == '/' || *q == '.' || *q == '_' || *q == '-') path_hint = 1;
                        if (*q == ')' || *q == ']' || *q == '}' || *q == '"' || *q == '\'' || *q == '>') break;
                        q++;
                    }
                    if (!path_hint) break;
                }
                sp++;
            }
            while (sp > start + 1 &&
                   (sp[-1] == ' ' || sp[-1] == '.' || sp[-1] == ',' || sp[-1] == ';' ||
                    sp[-1] == ':' || sp[-1] == ')' || sp[-1] == ']' || sp[-1] == '}' ||
                    sp[-1] == '"' || sp[-1] == '\'' || sp[-1] == '>')) {
                sp--;
            }

            int fplen = (int)(sp - start);
            if (path_looks_valid(start, fplen)) {
                p = sp;
                if (o + fplen + 512 < dstmax) {
                    char label[256];
                    if (compact_labels) {
                        shorten_path(label, sizeof(label), start, fplen);
                    } else {
                        int n = fplen < (int)sizeof(label) - 1 ? fplen : (int)sizeof(label) - 1;
                        memcpy(label, start, (size_t)n);
                        label[n] = '\0';
                    }

                    char uri[16384];
                    if (build_file_uri_target(uri, sizeof(uri), start, fplen)) {
                        LF_S("\033]8;;");
                        LF_S(uri);
                        LF_CH('\a');
                        LF_S(BO C_FLINK UL_ON);
                        LF_S(label);
                        LF_S(UL_OFF "\033[22m");
                        LF_S("\033]8;;\a");
                        continue;
                    }

                    /* Non-clickable fallback path (remote default or expansion failure). */
                    for (int i=0; i<fplen; i++) LF_CH(start[i]);
                } else {
                    for (int i=0; i<fplen; i++) LF_CH(start[i]);
                }
                continue;
            }
        }
        LF_CH(*p++);
    }
    while (*p && o < dstmax-1) dst[o++] = *p++;
    dst[o] = '\0';

    #undef LF_CH
    #undef LF_S
}

static void L_pushw_link(Lines *l, const char *s) {
    if (!s) { L_pushw(l, ""); return; }
    if (!strstr(s, "http://") &&
        !strstr(s, "https://") &&
        !strstr(s, "~/") &&
        !strstr(s, "/") &&
        !strstr(s, "\033]8;;")) {
        L_pushw(l, s);
        return;
    }
    char lb[32768];
    linkify(lb, sizeof(lb), s);
    L_pushw(l, lb);
}

static void link_map_clear(void) {
    for (int i = 0; i < g_link_map.n; i++) free(g_link_map.d[i].uri);
    free(g_link_map.d);
    g_link_map.d = NULL;
    g_link_map.n = 0;
    g_link_map.cap = 0;
}

static void link_map_add(int row, int x0, int x1, const char *uri) {
    if (!uri || !*uri || row <= 0 || x0 <= 0 || x1 < x0) return;
    if (g_link_map.n > 0) {
        LinkSpan *last = &g_link_map.d[g_link_map.n - 1];
        if (last->row == row && last->x1 + 1 == x0 && strcmp(last->uri, uri) == 0) {
            last->x1 = x1;
            return;
        }
    }
    if (g_link_map.n >= g_link_map.cap) {
        int nc = g_link_map.cap ? g_link_map.cap * 2 : 128;
        LinkSpan *nd = realloc(g_link_map.d, sizeof(LinkSpan) * (size_t)nc);
        if (!nd) return;
        g_link_map.d = nd;
        g_link_map.cap = nc;
    }
    g_link_map.d[g_link_map.n].row = row;
    g_link_map.d[g_link_map.n].x0 = x0;
    g_link_map.d[g_link_map.n].x1 = x1;
    g_link_map.d[g_link_map.n].uri = strdup(uri);
    if (!g_link_map.d[g_link_map.n].uri) return;
    g_link_map.n++;
}

static int link_map_track_line(const char *s, int start_row) {
    if (!s || start_row <= 0) return 0;
    int row = start_row;
    int col = 1;
    char active_uri[8192] = "";
    int active = 0;
    int slen = (int)strlen(s);

    for (int i = 0; s[i]; ) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b && s[i + 1] == '[') {
            i += 2;
            while (s[i] && !isalpha((unsigned char)s[i]) && s[i] != '~') i++;
            if (s[i]) i++;
            continue;
        }
        if (c == 0x1b && s[i + 1] == ']' && s[i + 2] == '8' && s[i + 3] == ';') {
            i += 4;
            int sep = 0;
            while (s[i]) {
                if (s[i] == ';') { sep = 1; i++; break; }
                if (s[i] == '\a') { i++; break; }
                if (s[i] == 0x1b && s[i + 1] == '\\') { i += 2; break; }
                i++;
            }
            if (!sep) { active = 0; active_uri[0] = '\0'; continue; }
            int o = 0;
            while (s[i]) {
                if (s[i] == '\a') { i++; break; }
                if (s[i] == 0x1b && s[i + 1] == '\\') { i += 2; break; }
                if (o + 1 < (int)sizeof(active_uri)) active_uri[o++] = s[i];
                i++;
            }
            active_uri[o] = '\0';
            active = (active_uri[0] != '\0');
            continue;
        }
        if (c == 0x1b && s[i + 1] == ']') {
            i += 2;
            while (s[i]) {
                if (s[i] == '\a') { i++; break; }
                if (s[i] == 0x1b && s[i + 1] == '\\') { i += 2; break; }
                i++;
            }
            continue;
        }
        if (c == 0x1b) {
            i += s[i + 1] ? 2 : 1;
            continue;
        }

        if (col > g_cols) {
            row++;
            col = 1;
        }

        int next = input_next_boundary(s, slen, i);
        if (active) link_map_add(row, col, col, active_uri);
        col++;
        i = next;
    }

    return row - start_row + 1;
}

static const char *link_map_hit(int row, int x) {
    for (int i = 0; i < g_link_map.n; i++) {
        LinkSpan *sp = &g_link_map.d[i];
        if (sp->row == row && x >= sp->x0 && x <= sp->x1) return sp->uri;
    }
    return NULL;
}

static int hover_link_set(const char *uri, int row) {
    if (!uri) uri = "";
    if (row < 0) row = 0;
    if (g_hover_row == row && strcmp(g_hover_uri, uri) == 0) return 0;
    g_hover_row = row;
    size_t n = strlen(uri);
    if (n >= sizeof(g_hover_uri)) n = sizeof(g_hover_uri) - 1;
    memcpy(g_hover_uri, uri, n);
    g_hover_uri[n] = '\0';
    return 1;
}

static int link_mouse_row(void) {
    return g_mouse_y;
}

static int refresh_hover_from_pointer(void) {
    if (g_mouse_x <= 0 || g_mouse_y <= 0) return hover_link_set("", 0);
    int hit_row = link_mouse_row();
    const char *uri = link_map_hit(hit_row, g_mouse_x);
    return hover_link_set(uri, uri && *uri ? hit_row : 0);
}

static void footer_emit_plain(const char *s, int *used, int max_cells) {
    if (!s || !used || max_cells <= 0) return;
    int slen = (int)strlen(s);
    for (int i = 0; s[i] && *used < max_cells; ) {
        int next = input_next_boundary(s, slen, i);
        ob_raw(s + i, next - i);
        (*used)++;
        i = next;
    }
}

static void footer_emit_styled(const char *style, const char *text, int *used, int max_cells) {
    if (!text || !*text || !used || *used >= max_cells) return;
    if (style && *style) ob(style);
    footer_emit_plain(text, used, max_cells);
    ob(RS);
    ob(C_QBG);
}

static void draw_hover_footer_uri(void) {
    const char *prefix = "  open: ";
    int maxw = g_cols - 2;
    if (maxw <= 0) return;
    ob(C_QBG);
    int used = 0;
    footer_emit_styled(C_HDM, prefix, &used, maxw);
    footer_emit_styled(C_QBG BO C_URL UL_ON, g_hover_uri, &used, maxw);
}

static void emit_line_with_hover(const char *s, int start_row) {
    if (!s || !*s || !g_hover_uri[0] || g_hover_row < start_row) { ob(s ? s : ""); return; }

    int row = start_row;
    int col = 1;
    int slen = (int)strlen(s);
    int hover_on = 0;
    int active_hover = 0;
    char active_uri[8192] = "";

    for (int i = 0; s[i]; ) {
        unsigned char c = (unsigned char)s[i];

        if (c == 0x1b && s[i + 1] == '[') {
            int j = i + 2;
            while (s[j] && !isalpha((unsigned char)s[j]) && s[j] != '~') j++;
            if (s[j]) j++;
            ob_raw(s + i, j - i);
            i = j;
            continue;
        }

        if (c == 0x1b && s[i + 1] == ']' && s[i + 2] == '8' && s[i + 3] == ';') {
            int j = i + 4;
            int sep = 0;
            while (s[j]) {
                if (s[j] == ';') { sep = 1; j++; break; }
                if (s[j] == '\a') { j++; break; }
                if (s[j] == 0x1b && s[j + 1] == '\\') { j += 2; break; }
                j++;
            }
            if (!sep) {
                active_uri[0] = '\0';
                active_hover = 0;
                ob_raw(s + i, j - i);
                i = j;
                continue;
            }
            int ulen = 0;
            while (s[j]) {
                if (s[j] == '\a') break;
                if (s[j] == 0x1b && s[j + 1] == '\\') break;
                if (ulen + 1 < (int)sizeof(active_uri)) active_uri[ulen++] = s[j];
                j++;
            }
            active_uri[ulen] = '\0';
            active_hover = (active_uri[0] && strcmp(active_uri, g_hover_uri) == 0);
            while (s[j]) {
                if (s[j] == '\a') { j++; break; }
                if (s[j] == 0x1b && s[j + 1] == '\\') { j += 2; break; }
                j++;
            }
            ob_raw(s + i, j - i);
            i = j;
            continue;
        }

        if (c == 0x1b && s[i + 1] == ']') {
            int j = i + 2;
            while (s[j]) {
                if (s[j] == '\a') { j++; break; }
                if (s[j] == 0x1b && s[j + 1] == '\\') { j += 2; break; }
                j++;
            }
            ob_raw(s + i, j - i);
            i = j;
            continue;
        }

        if (c == 0x1b) {
            int j = i + (s[i + 1] ? 2 : 1);
            ob_raw(s + i, j - i);
            i = j;
            continue;
        }

        if (col > g_cols) {
            if (hover_on) { ob("\033[27m"); hover_on = 0; }
            row++;
            col = 1;
        }

        int want_hover = (active_hover && row == g_hover_row);
        if (want_hover != hover_on) {
            ob(want_hover ? C_LHOV : "\033[27m");
            hover_on = want_hover;
        }

        int next = input_next_boundary(s, slen, i);
        ob_raw(s + i, next - i);
        col++;
        i = next;
    }

    if (hover_on) ob("\033[27m");
}

static void open_uri_async(const char *uri) {
    if (!uri || !*uri) return;
    pid_t pid = fork();
    if (pid != 0) return;
    setsid();
#if defined(__APPLE__)
    execlp("open", "open", uri, (char *)NULL);
#else
    execlp("xdg-open", "xdg-open", uri, (char *)NULL);
#endif
    _exit(127);
}

/* ── Transcript items ──────────────────────────────────────────────────── */

enum { IT_HUM, IT_AST, IT_TU, IT_TR };
typedef struct { int type; char *text; char *label; int is_err; } Item;
typedef struct { Item *d; int n, cap; } Items;

static void I_push(Items *it, int type, char *text, char *label, int err) {
    if (!it || g_oom) {
        free(text);
        free(label);
        return;
    }
    if (it->n >= it->cap) {
        int ncap = it->cap ? it->cap * 2 : 64;
        Item *nd = xrealloc(it->d, sizeof(Item) * (size_t)ncap);
        if (!nd) {
            free(text);
            free(label);
            return;
        }
        it->d = nd;
        it->cap = ncap;
    }
    Item *e = &it->d[it->n++];
    e->type = type; e->text = text; e->label = label; e->is_err = err;
}

static void I_free(Items *it) {
    for (int i=0; i<it->n; i++) { free(it->d[i].text); free(it->d[i].label); }
    free(it->d); memset(it, 0, sizeof(*it));
}

static void skip_st_terminated(const char *s, int len, int *idx, int allow_bel) {
    int i = idx ? *idx : 0;
    while (i < len) {
        if (allow_bel && s[i] == '\a') { i++; break; }
        if (i + 1 < len && s[i] == '\033' && s[i + 1] == '\\') { i += 2; break; }
        i++;
    }
    if (idx) *idx = i;
}

/* Strip inbound terminal escape/control sequences from transcript payloads. */
static char *sanitize(const char *s) {
    if (!s) return NULL;
    int len = (int)strlen(s);
    char *d = xmalloc((size_t)len + 1);
    if (!d) return NULL;
    int j = 0;
    for (int i = 0; i < len; ) {
        if (s[i]=='\033') {
            i++;
            if (i<len && s[i]=='[') {
                i++;
                while (i<len && !isalpha((unsigned char)s[i]) && s[i] != '~') i++;
                if (i<len) i++;
            } else if (i<len && s[i]==']') {
                i++;
                skip_st_terminated(s, len, &i, 1);
            } else if (i<len && (s[i]=='P' || s[i]=='X' || s[i]=='^' || s[i]=='_')) {
                /* DCS, SOS, PM, APC (ST-terminated strings) */
                i++;
                skip_st_terminated(s, len, &i, 0);
            } else if (i<len) {
                i++;
            }
        } else {
            unsigned char c = (unsigned char)s[i++];
            if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) continue;
            d[j++] = (char)c;
        }
    }
    d[j] = '\0'; return d;
}

static int sanitize_copy(char *dst, int dstmax, const char *s, int slen) {
    if (!dst || dstmax < 1) return 0;
    if (!s || slen <= 0) { dst[0] = '\0'; return 0; }
    int i = 0, j = 0;
    while (i < slen && j < dstmax - 1) {
        if (s[i] == '\033') {
            i++;
            if (i < slen && s[i] == '[') {
                i++;
                while (i < slen && !isalpha((unsigned char)s[i]) && s[i] != '~') i++;
                if (i < slen) i++;
            } else if (i < slen && s[i] == ']') {
                i++;
                skip_st_terminated(s, slen, &i, 1);
            } else if (i < slen && (s[i] == 'P' || s[i] == 'X' || s[i] == '^' || s[i] == '_')) {
                i++;
                skip_st_terminated(s, slen, &i, 0);
            } else if (i < slen) {
                i++;
            }
        } else {
            unsigned char c = (unsigned char)s[i++];
            if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) continue;
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
    return j;
}

static const char *sanitize_line_view(const char *src, int slen, char *buf, int bufmax, int *out_len) {
    if (out_len) *out_len = 0;
    if (!src || slen <= 0) return "";
    if (g_perf_compat) {
        if (out_len) *out_len = slen;
        return src;
    }
    int need = 0;
    for (int i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0x1b || c == 0x7f || (c < 0x20 && c != '\t')) {
            need = 1;
            break;
        }
    }
    if (!need) {
        if (out_len) *out_len = slen;
        return src;
    }
    int n = sanitize_copy(buf, bufmax, src, slen);
    if (out_len) *out_len = n;
    return buf;
}

static int is_systag(const char *s) {
    return strstr(s,"<local-command-caveat") || strstr(s,"<command-name") ||
           strstr(s,"<system-reminder") || strstr(s,"<user-prompt-submit-hook");
}

static const char *lbl_keys[] = {
    "command","file_path","path","pattern","query","url","content","description",NULL
};

/* Extract trimmed, sanitized string from JSON value at p */
static char *extract_text(const char *p, int bufmax, int sanitize_out) {
    if (!p || *p != '"') return NULL;
    if (bufmax < 4) return NULL;
    char *buf = xmalloc((size_t)bufmax);
    if (!buf) return NULL;
    int n = jstr(p, buf, bufmax);
    if (n <= 0) { free(buf); return NULL; }
    /* Trim */
    char *s = buf;
    while (*s==' '||*s=='\n') s++;
    char *e = s + strlen(s);
    while (e>s && (e[-1]==' '||e[-1]=='\n')) e--;
    *e = '\0';
    if (!*s) { free(buf); return NULL; }
    if (!sanitize_out) {
        char *raw = xstrdup(s);
        free(buf);
        return raw;
    }
    char *clean = sanitize(s);
    free(buf);
    return clean;
}

typedef struct {
    char *d;
    int n;
    int cap;
} SBuf;

static void sb_init(SBuf *b) {
    if (!b) return;
    b->d = NULL;
    b->n = 0;
    b->cap = 0;
}

static int sb_reserve(SBuf *b, int extra) {
    if (!b || extra < 0) return 0;
    int need = b->n + extra + 1;
    if (need <= b->cap) return 1;
    int ncap = b->cap > 0 ? b->cap : 256;
    while (ncap < need) {
        if (ncap > (1 << 28)) return 0;
        ncap *= 2;
    }
    char *nd = xrealloc(b->d, (size_t)ncap);
    if (!nd) return 0;
    b->d = nd;
    b->cap = ncap;
    return 1;
}

static int sb_putn(SBuf *b, const char *s, int n) {
    if (!b || !s || n <= 0) return 1;
    if (!sb_reserve(b, n)) return 0;
    memcpy(b->d + b->n, s, (size_t)n);
    b->n += n;
    b->d[b->n] = '\0';
    return 1;
}

static int sb_puts(SBuf *b, const char *s) {
    if (!s) return 1;
    return sb_putn(b, s, (int)strlen(s));
}

static int sb_putc(SBuf *b, char c) {
    if (!sb_reserve(b, 1)) return 0;
    b->d[b->n++] = c;
    b->d[b->n] = '\0';
    return 1;
}

static int sb_printf(SBuf *b, const char *fmt, ...) {
    if (!b || !fmt) return 0;
    va_list ap;
    va_start(ap, fmt);
    va_list cp;
    va_copy(cp, ap);
    int need = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (need < 0) { va_end(ap); return 0; }
    if (!sb_reserve(b, need)) { va_end(ap); return 0; }
    vsnprintf(b->d + b->n, (size_t)(b->cap - b->n), fmt, ap);
    b->n += need;
    va_end(ap);
    return 1;
}

static char *sb_take(SBuf *b) {
    if (!b) return NULL;
    if (!b->d) return xstrdup("");
    char *out = b->d;
    b->d = NULL;
    b->n = 0;
    b->cap = 0;
    return out;
}

static void sb_free(SBuf *b) {
    if (!b) return;
    free(b->d);
    b->d = NULL;
    b->n = 0;
    b->cap = 0;
}

static char *build_structured_patch_payload(const char *tool_use_result,
                                            int *out_add,
                                            int *out_del) {
    if (out_add) *out_add = 0;
    if (out_del) *out_del = 0;
    if (!tool_use_result) return NULL;
    const char *sp = jfind(tool_use_result, "structuredPatch");
    if (!sp) return NULL;
    sp = jws(sp);
    if (*sp != '[') return NULL;

    SBuf sb;
    sb_init(&sb);
    if (!sb_puts(&sb, "CP_SP1\n")) { sb_free(&sb); return NULL; }

    char file_path[4096] = "";
    const char *fp = jfind(tool_use_result, "filePath");
    if (fp && *fp == '"') jstr(fp, file_path, sizeof(file_path));
    if (file_path[0]) {
        if (!sb_puts(&sb, "F\t") ||
            !sb_puts(&sb, file_path) ||
            !sb_putc(&sb, '\n')) {
            sb_free(&sb);
            return NULL;
        }
    }

    int adds = 0, dels = 0, patch_count = 0;
    const char *el = jws(sp);
    if (*el == '[') el = jws(el + 1);
    while (el && *el && *el != ']') {
        if (*el == '{') {
            int old_start = jint(jfind(el, "oldStart"));
            int old_lines = jint(jfind(el, "oldLines"));
            int new_start = jint(jfind(el, "newStart"));
            int new_lines = jint(jfind(el, "newLines"));
            if (!sb_printf(&sb, "P\t%d\t%d\t%d\t%d\n", old_start, old_lines, new_start, new_lines)) {
                sb_free(&sb);
                return NULL;
            }

            const char *lv = jfind(el, "lines");
            if (lv) lv = jws(lv);
            if (lv && *lv == '[') {
                const char *lp = jws(lv);
                if (*lp == '[') lp = jws(lp + 1);
                while (lp && *lp && *lp != ']') {
                    if (*lp == '"') {
                        char line_buf[16384];
                        jstr(lp, line_buf, sizeof(line_buf));
                        for (char *q = line_buf; *q; q++) {
                            if (*q == '\n' || *q == '\r') *q = ' ';
                        }
                        if (line_buf[0] == '+') adds++;
                        else if (line_buf[0] == '-') dels++;
                        if (!sb_puts(&sb, "L\t") ||
                            !sb_puts(&sb, line_buf) ||
                            !sb_putc(&sb, '\n')) {
                            sb_free(&sb);
                            return NULL;
                        }
                    }
                    lp = jskip(lp);
                    lp = jws(lp);
                    if (*lp == ',') lp = jws(lp + 1);
                }
            }
            if (!sb_puts(&sb, "E\n")) {
                sb_free(&sb);
                return NULL;
            }
            patch_count++;
        }
        el = jskip(el);
        el = jws(el);
        if (*el == ',') el = jws(el + 1);
    }

    if (patch_count == 0) {
        sb_free(&sb);
        return NULL;
    }
    if (out_add) *out_add = adds;
    if (out_del) *out_del = dels;
    return sb_take(&sb);
}

static void extract_tool_use_result_meta(const char *tool_use_result,
                                         char *kind, int kind_sz,
                                         char *file_path, int file_path_sz) {
    if (kind && kind_sz > 0) kind[0] = '\0';
    if (file_path && file_path_sz > 0) file_path[0] = '\0';
    if (!tool_use_result) return;
    if (kind && kind_sz > 0) {
        const char *kv = jfind(tool_use_result, "type");
        if (kv && *kv == '"') jstr(kv, kind, kind_sz);
    }
    if (file_path && file_path_sz > 0) {
        const char *fp = jfind(tool_use_result, "filePath");
        if (fp && *fp == '"') jstr(fp, file_path, file_path_sz);
    }
}

static void relabel_last_tool_use(Items *items, const char *op_name, const char *file_path) {
    if (!items || items->n <= 0 || !op_name || !*op_name) return;
    for (int i = items->n - 1; i >= 0; i--) {
        Item *it = &items->d[i];
        if (it->type != IT_TU) continue;
        if (it->text && (strncmp(it->text, "Write", 5) == 0 ||
                         strncmp(it->text, "Edit", 4) == 0 ||
                         strncmp(it->text, "MultiEdit", 9) == 0 ||
                         strncmp(it->text, "Update", 6) == 0 ||
                         strncmp(it->text, "Create", 6) == 0)) {
            char nb[2048];
            if (file_path && *file_path) snprintf(nb, sizeof(nb), "%s(%s)", op_name, file_path);
            else snprintf(nb, sizeof(nb), "%s", op_name);
            free(it->text);
            it->text = sanitize(nb);
            if (it->label) {
                free(it->label);
                it->label = xstrdup("");
            }
        }
        break;
    }
}

/* ── Transcript parser ─────────────────────────────────────────────────── */

static void parse_transcript(const char *path, Items *items,
                             int *out_tok, double *out_pct, int ctx_lim) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char *line = NULL; size_t lsz = 0; ssize_t len;
    int li = 0, lcc = 0, lcr = 0;

    while ((len = getline(&line, &lsz, f)) != -1) {
        while (len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]='\0';
        if (len == 0) continue;

        const char *tv = jfind(line, "type");
        const char *msg = jfind(line, "message");
        if (!tv || !msg) continue;
        const char *ct = jfind(msg, "content");

        if (jstreq(tv, "assistant")) {
            const char *usg = jfind(msg, "usage");
            if (usg) {
                const char *v;
                if ((v = jfind(usg, "input_tokens"))) li = jint(v);
                if ((v = jfind(usg, "cache_creation_input_tokens"))) lcc = jint(v);
                if ((v = jfind(usg, "cache_read_input_tokens"))) lcr = jint(v);
            }
            if (!ct || *jws(ct) != '[') continue;
            const char *el = jws(ct);
            if (*el=='[') el = jws(el+1);
            while (el && *el && *el!=']') {
                if (*el=='{') {
                    const char *bt = jfind(el, "type");
                    if (jstreq(bt, "text")) {
                        char *t = extract_text(jfind(el, "text"), (int)len+1, g_perf_compat ? 1 : 0);
                        if (t) I_push(items, IT_AST, t, NULL, 0);
                    } else if (jstreq(bt, "tool_use")) {
                        char nm[128] = "?";
                        char nm_disp[1536] = "";
                        const char *nv = jfind(el, "name");
                        if (nv) jstr(nv, nm, sizeof(nm));
                        snprintf(nm_disp, sizeof(nm_disp), "%s", nm);
                        char lbl[256] = "";
                        const char *inp = jfind(el, "input");
                        if (inp) {
                            for (int k=0; lbl_keys[k]; k++) {
                                const char *lv = jfind(inp, lbl_keys[k]);
                                if (lv && *lv=='"') { jstr(lv, lbl, sizeof(lbl)); break; }
                            }
                            if (!lbl[0]) {
                                const char *p2 = jws(inp);
                                if (*p2=='{') p2++;
                                p2 = jws(p2);
                                if (*p2=='"') { p2=jskip_s(p2); p2=jws(p2); if(*p2==':') p2=jws(p2+1); if(*p2=='"') jstr(p2,lbl,sizeof(lbl)); }
                            }
                        }
                        if (strcasecmp(nm, "Read") == 0 && inp) {
                            int lim = jint(jfind(inp, "limit"));
                            if (lim > 0) {
                                snprintf(nm_disp, sizeof(nm_disp), "Read %d lines", lim);
                                lbl[0] = '\0';
                            }
                        } else if ((strcasecmp(nm, "Edit") == 0 || strcasecmp(nm, "MultiEdit") == 0) && inp) {
                            char fpb[1200] = "";
                            const char *fpv = jfind(inp, "file_path");
                            if (fpv && *fpv == '"') jstr(fpv, fpb, sizeof(fpb));
                            if (!fpb[0]) {
                                fpv = jfind(inp, "path");
                                if (fpv && *fpv == '"') jstr(fpv, fpb, sizeof(fpb));
                            }
                            if (fpb[0]) {
                                snprintf(nm_disp, sizeof(nm_disp), "Update(%s)", fpb);
                                lbl[0] = '\0';
                            } else {
                                snprintf(nm_disp, sizeof(nm_disp), "Update");
                            }
                        }
                        if (strlen(lbl)>72) { lbl[69]='.'; lbl[70]='.'; lbl[71]='.'; lbl[72]='\0'; }
                        I_push(items, IT_TU, sanitize(nm_disp), sanitize(lbl), 0);
                    }
                }
                el = jskip(el); el = jws(el); if (*el==',') el=jws(el+1);
            }
        } else if (jstreq(tv, "user")) {
            if (ct && *jws(ct)=='"') {
                char *t = extract_text(ct, (int)len+1, g_perf_compat ? 1 : 0);
                if (t && !is_systag(t)) I_push(items, IT_HUM, t, NULL, 0);
                else free(t);
            } else if (ct && *jws(ct)=='[') {
                const char *tur = jfind(line, "toolUseResult");
                int sp_add = 0, sp_del = 0;
                int sp_used = 0;
                char *sp_payload = build_structured_patch_payload(tur, &sp_add, &sp_del);
                char tur_kind[64] = "";
                char tur_path[1200] = "";
                extract_tool_use_result_meta(tur, tur_kind, sizeof(tur_kind), tur_path, sizeof(tur_path));
                if (sp_payload) {
                    if (strcasecmp(tur_kind, "create") == 0) {
                        relabel_last_tool_use(items, "Create", tur_path);
                    } else if (strcasecmp(tur_kind, "update") == 0 || strcasecmp(tur_kind, "edit") == 0) {
                        relabel_last_tool_use(items, "Update", tur_path);
                    } else if (tur_path[0]) {
                        relabel_last_tool_use(items, "Update", tur_path);
                    }
                }
                const char *el = jws(ct);
                if (*el=='[') el=jws(el+1);
                while (el && *el && *el!=']') {
                    if (*el=='{') {
                        const char *bt = jfind(el, "type");
                        if (jstreq(bt, "tool_result")) {
                            const char *rc = jfind(el, "content");
                            char *text = NULL;
                            int ie = 0;
                            int handled_struct_patch = 0;
                            const char *ev = jfind(el, "is_error");
                            if (ev && (*ev=='t'||*ev=='T')) ie=1;
                            if (!ie && sp_payload && !sp_used) {
                                char sbuf[128];
                                snprintf(sbuf, sizeof(sbuf), "Added %d lines, removed %d lines", sp_add, sp_del);
                                I_push(items, IT_TR, sanitize(sbuf), NULL, 0);
                                I_push(items, IT_TR, sp_payload, NULL, 0);
                                sp_payload = NULL;
                                sp_used = 1;
                                handled_struct_patch = 1;
                            }
                            if (!handled_struct_patch && rc && *jws(rc)=='"') {
                                text = extract_text(rc, (int)len+1, g_perf_compat ? 1 : 0);
                            } else if (!handled_struct_patch && rc && *jws(rc)=='[') {
                                int bmax = (int)len+1;
                                char *buf = xmalloc((size_t)bmax); int bi=0;
                                if (!buf) break;
                                const char *sub = jws(rc);
                                if (*sub=='[') sub=jws(sub+1);
                                while (sub && *sub && *sub!=']') {
                                    if (*sub=='{' && jstreq(jfind(sub,"type"),"text")) {
                                        const char *sv = jfind(sub,"text");
                                        if (sv && *sv=='"') {
                                            if (bi>0 && bi<bmax-1) buf[bi++]='\n';
                                            bi += jstr(sv, buf+bi, bmax-bi);
                                        }
                                    }
                                    sub=jskip(sub); sub=jws(sub); if(*sub==',') sub=jws(sub+1);
                                }
                                buf[bi]='\0';
                                char *s=buf; while(*s==' '||*s=='\n') s++;
                                char *e=s+strlen(s); while(e>s&&(e[-1]==' '||e[-1]=='\n')) e--; *e='\0';
                                if (*s) { text = g_perf_compat ? sanitize(s) : xstrdup(s); }
                                free(buf);
                            }
                            if (text) {
                                I_push(items, IT_TR, text, NULL, ie);
                            }
                        }
                    }
                    el=jskip(el); el=jws(el); if(*el==',') el=jws(el+1);
                }
                if (sp_payload) free(sp_payload);
            }
        }
    }
    free(line); fclose(f);
    int tot = li + lcc + lcr;
    if (tot > 0) { *out_tok = tot; *out_pct = (double)tot / ctx_lim * 100.0; }
}

/* ── Inline markdown: **bold** and `code` ──────────────────────────────── */

static void fmt_inline(char *dst, int mx, const char *src) {
    int i = 0;
    #define A(s) do { const char*_=(s); while(*_ && i<mx-1) dst[i++]=*_++; } while(0)
    A(C_AST);
    while (*src && i < mx - 40) {
        if (src[0]=='*' && src[1]=='*') {
            src+=2; A(BO);
            while (*src && i<mx-20 && !(src[0]=='*'&&src[1]=='*')) dst[i++]=*src++;
            A(RS C_AST);
            if (src[0]=='*'&&src[1]=='*') src+=2;
        } else if (src[0]=='`' && src[1]!='`') {
            src++; A(C_CIN);
            while (*src && *src!='`' && i<mx-20) dst[i++]=*src++;
            A(RS C_AST);
            if (*src=='`') src++;
        } else { dst[i++]=*src++; }
    }
    A(RS); dst[i]='\0';
    #undef A
}

/* ── Markdown renderer ─────────────────────────────────────────────────── */

static const char *md_tail_start(const char *text, int keep_lines, int *omitted_lines) {
    if (omitted_lines) *omitted_lines = 0;
    if (!text || keep_lines <= 0) return text;
    int total = 1;
    for (const char *p = text; *p; p++) if (*p == '\n') total++;
    if (total <= keep_lines) return text;

    int omit = total - keep_lines;
    const char *p = text;
    for (int i = 0; i < omit; i++) {
        const char *nl = strchr(p, '\n');
        if (!nl) return text;
        p = nl + 1;
    }
    if (omitted_lines) *omitted_lines = omit;
    return p;
}

static int md_read_line(const char *p, char *out, int outsz, int *out_len, const char **out_next) {
    if (out_len) *out_len = 0;
    if (out_next) *out_next = p;
    if (!p || !*p || !out || outsz < 2) {
        if (out && outsz > 0) out[0] = '\0';
        return 0;
    }
    const char *eol = strchr(p, '\n');
    int ll = eol ? (int)(eol - p) : (int)strlen(p);
    if (ll >= outsz) ll = outsz - 1;
    memcpy(out, p, (size_t)ll);
    out[ll] = '\0';
    if (out_len) *out_len = ll;
    if (out_next) *out_next = eol ? (eol + 1) : (p + ll);
    return 1;
}

static void md_trim_span(const char **s, const char **e) {
    while (*s < *e && ((*s)[0] == ' ' || (*s)[0] == '\t')) (*s)++;
    while (*e > *s && ((*e)[-1] == ' ' || (*e)[-1] == '\t')) (*e)--;
}

static int md_is_table_sep_line(const char *line) {
    if (!line || !*line) return 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '|') p++;

    int cols = 0;
    while (*p) {
        const char *s = p;
        while (*p && *p != '|') p++;
        const char *e = p;
        md_trim_span(&s, &e);
        if (e > s) {
            int dashes = 0;
            for (const char *q = s; q < e; q++) {
                if (*q == '-') dashes++;
                else if (*q != ':') return 0;
            }
            if (dashes < 1) return 0;
            cols++;
        }
        if (*p == '|') p++;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
    }
    return cols >= 1;
}

#define MD_TBL_MAX_COLS 8
#define MD_TBL_MAX_ROWS 32
#define MD_TBL_CELL_MAX 192

static int md_split_table_cells(const char *line, char cells[][MD_TBL_CELL_MAX], int max_cols) {
    if (!line || !*line || !cells || max_cols <= 0) return 0;
    for (int i = 0; i < max_cols; i++) cells[i][0] = '\0';

    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '|') p++;

    int n = 0;
    while (*p && n < max_cols) {
        const char *s = p;
        while (*p && *p != '|') p++;
        const char *e = p;
        md_trim_span(&s, &e);
        int ll = (int)(e - s);
        if (ll >= MD_TBL_CELL_MAX) ll = MD_TBL_CELL_MAX - 1;
        if (ll > 0) memcpy(cells[n], s, (size_t)ll);
        cells[n][ll] = '\0';
        n++;
        if (*p == '|') p++;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
    }
    while (n > 0 && cells[n - 1][0] == '\0') n--;
    return n;
}

static void md_fit_cell(char *dst, int dstmax, const char *src, int width) {
    if (!dst || dstmax < 2) return;
    if (!src) src = "";
    int sl = (int)strlen(src);
    if (sl <= width) {
        int n = sl < dstmax - 1 ? sl : dstmax - 1;
        memcpy(dst, src, (size_t)n);
        dst[n] = '\0';
        return;
    }
    if (width <= 1) {
        dst[0] = '.';
        dst[1] = '\0';
        return;
    }
    int n = width - 1;
    if (n > dstmax - 2) n = dstmax - 2;
    memcpy(dst, src, (size_t)n);
    dst[n++] = '.';
    dst[n] = '\0';
}

static int md_cell_target(const char *src, char *dst, int dstmax) {
    if (!src || !*src || !dst || dstmax < 8) return 0;
    int sl = (int)strlen(src);
    if (sl >= 8 && memcmp(src, "https://", 8) == 0) {
        if (sl >= dstmax) return 0;
        memcpy(dst, src, (size_t)sl + 1);
        return 1;
    }
    if (sl >= 7 && memcmp(src, "http://", 7) == 0) {
        if (sl >= dstmax) return 0;
        memcpy(dst, src, (size_t)sl + 1);
        return 1;
    }
    if (!(src[0] == '/' || (src[0] == '~' && src[1] == '/'))) return 0;
    return build_file_uri_target(dst, dstmax, src, sl);
}

static void md_cell_label(char *dst, int dstmax, const char *src, int width) {
    if (!dst || dstmax < 2) return;
    if (!src) src = "";
    int sl = (int)strlen(src);
    if (sl > width) {
        char shortb[256];
        if ((sl >= 8 && memcmp(src, "https://", 8) == 0) ||
            (sl >= 7 && memcmp(src, "http://", 7) == 0)) {
            shorten_url(shortb, sizeof(shortb), src, sl);
            md_fit_cell(dst, dstmax, shortb, width);
            return;
        }
        if (src[0] == '/' || (src[0] == '~' && src[1] == '/')) {
            shorten_path(shortb, sizeof(shortb), src, sl);
            md_fit_cell(dst, dstmax, shortb, width);
            return;
        }
    }
    md_fit_cell(dst, dstmax, src, width);
}

static void md_table_border_line(char *out, int outsz,
                                 const int *widths, int ncol,
                                 const char *left, const char *mid, const char *right) {
    int o = 0;
    o += snprintf(out + o, outsz - o, C_HDM "  %s", left);
    for (int c = 0; c < ncol && o < outsz - 8; c++) {
        for (int k = 0; k < widths[c] + 2 && o < outsz - 8; k++) {
            o += snprintf(out + o, outsz - o, HL);
        }
        o += snprintf(out + o, outsz - o, "%s", (c + 1 < ncol) ? mid : right);
    }
    snprintf(out + o, outsz - o, RS);
}

static void md_render_table_block(Lines *L,
                                  char header[][MD_TBL_CELL_MAX], int hcols,
                                  char rows[][MD_TBL_MAX_COLS][MD_TBL_CELL_MAX], int rown,
                                  int ncol) {
    if (!L || ncol <= 0 || rown < 0) return;
    if (ncol > MD_TBL_MAX_COLS) ncol = MD_TBL_MAX_COLS;

    int widths[MD_TBL_MAX_COLS];
    for (int c = 0; c < ncol; c++) widths[c] = 3;

    for (int c = 0; c < hcols && c < ncol; c++) {
        int wl = (int)strlen(header[c]);
        if (wl > widths[c]) widths[c] = wl;
    }
    for (int r = 0; r < rown; r++) {
        for (int c = 0; c < ncol; c++) {
            int wl = (int)strlen(rows[r][c]);
            if (wl > widths[c]) widths[c] = wl;
        }
    }

    int sum = 0;
    for (int c = 0; c < ncol; c++) {
        if (widths[c] > 32) widths[c] = 32;
        if (widths[c] < 3) widths[c] = 3;
        sum += widths[c];
    }
    int max_sum = g_cols - (3 * ncol + 3);
    int min_sum = ncol * 3;
    if (max_sum < min_sum) max_sum = min_sum;
    while (sum > max_sum) {
        int shrunk = 0;
        for (int c = 0; c < ncol && sum > max_sum; c++) {
            if (widths[c] > 3) {
                widths[c]--;
                sum--;
                shrunk = 1;
            }
        }
        if (!shrunk) break;
    }

    char line[16384];
    md_table_border_line(line, sizeof(line), widths, ncol, "┌", "┬", "┐");
    L_push(L, line);

    int o = 0;
    o += snprintf(line + o, sizeof(line) - o, C_HDM "  │" RS);
    for (int c = 0; c < ncol && o < (int)sizeof(line) - 16; c++) {
        char cell[MD_TBL_CELL_MAX + 8];
        const char *src = (c < hcols) ? header[c] : "";
        md_cell_label(cell, sizeof(cell), src, widths[c]);
        o += snprintf(line + o, sizeof(line) - o, BO C_AST " %-*s " RS C_HDM "│" RS, widths[c], cell);
    }
    L_pushw_link(L, line);

    md_table_border_line(line, sizeof(line), widths, ncol, "├", "┼", "┤");
    L_push(L, line);

    for (int r = 0; r < rown; r++) {
        o = 0;
        o += snprintf(line + o, sizeof(line) - o, C_HDM "  │" RS);
        for (int c = 0; c < ncol && o < (int)sizeof(line) - 16; c++) {
            char cell[MD_TBL_CELL_MAX + 8];
            char target[4096];
            const char *src = rows[r][c];
            md_cell_label(cell, sizeof(cell), src, widths[c]);
            int vis = (int)strlen(cell);
            int pad = widths[c] - vis;
            if (pad < 0) pad = 0;
            o += snprintf(line + o, sizeof(line) - o, C_AST " ");
            if (md_cell_target(src, target, sizeof(target))) {
                o += snprintf(line + o, sizeof(line) - o, "\033]8;;%s\a%s\033]8;;\a", target, cell);
            } else {
                o += snprintf(line + o, sizeof(line) - o, "%s", cell);
            }
            if (pad > 0) o += snprintf(line + o, sizeof(line) - o, "%*s", pad, "");
            o += snprintf(line + o, sizeof(line) - o, " " RS C_HDM "│" RS);
        }
        L_pushw_link(L, line);
        if (r + 1 < rown) {
            md_table_border_line(line, sizeof(line), widths, ncol, "├", "┼", "┤");
            L_push(L, line);
        }
    }

    md_table_border_line(line, sizeof(line), widths, ncol, "└", "┴", "┘");
    L_push(L, line);
}

static void render_md(Lines *L, const char *text, int keep_tail_lines) {
    if (!text) return;
    static int table_enabled = -1;
    static int table_max_rows = -1;
    static int table_max_cols = -1;
    if (table_enabled < 0) {
        table_enabled = env_enabled_default_on("CLAUDE_PAGER_MD_TABLES") ? 1 : 0;
        table_max_rows = parse_env_int_range("CLAUDE_PAGER_MD_TABLE_MAX_ROWS", 1, MD_TBL_MAX_ROWS, 24);
        table_max_cols = parse_env_int_range("CLAUDE_PAGER_MD_TABLE_MAX_COLS", 1, MD_TBL_MAX_COLS, 8);
    }
    int in_code = 0;
    char lb[8192], sb[8192], fb[16384];
    int omitted = 0;
    const char *p = md_tail_start(text, keep_tail_lines, &omitted);

    if (omitted > 0) {
        snprintf(fb, sizeof(fb), C_HDM ELL " (%d earlier lines omitted)" RS, omitted);
        L_push(L, fb);
    }

    while (*p) {
        const char *eol = strchr(p, '\n');
        int ll = eol ? (int)(eol-p) : (int)strlen(p);
        if (ll >= (int)sizeof(lb)) ll = (int)sizeof(lb)-1;
        memcpy(lb, p, ll); lb[ll]='\0';
        p = eol ? eol+1 : p+ll;
        const char *line = sanitize_line_view(lb, ll, sb, sizeof(sb), NULL);

        if (line[0]=='`' && line[1]=='`' && line[2]=='`') { in_code = !in_code; continue; }

        if (in_code) {
            int vl = (int)strlen(line), pad = g_cols-6-vl;
            if (pad<0) pad=0;
            snprintf(fb, sizeof(fb), C_SEP VL RS C_CBG C_CFG " %s%*s" RS, line, pad, "");
            L_pushw_link(L, fb); continue;
        }

        /* Tables */
        if (table_enabled && g_cols >= 72 && looks_like_table_row(line)) {
            char sep_raw[8192], sep_san[8192];
            int sep_len = 0;
            const char *after_sep = p;
            if (md_read_line(p, sep_raw, sizeof(sep_raw), &sep_len, &after_sep)) {
                const char *sep_line = sanitize_line_view(sep_raw, sep_len, sep_san, sizeof(sep_san), NULL);
                if (md_is_table_sep_line(sep_line)) {
                    char header[MD_TBL_MAX_COLS][MD_TBL_CELL_MAX];
                    char rows[MD_TBL_MAX_ROWS][MD_TBL_MAX_COLS][MD_TBL_CELL_MAX];
                    memset(header, 0, sizeof(header));
                    memset(rows, 0, sizeof(rows));

                    int hcols = md_split_table_cells(line, header, MD_TBL_MAX_COLS);
                    if (hcols > table_max_cols) hcols = table_max_cols;
                    if (hcols > 0) {
                        int ncol = hcols;
                        int rown = 0;
                        const char *scan = after_sep;
                        while (rown < MD_TBL_MAX_ROWS && rown < table_max_rows) {
                            char rr[8192], rs[8192];
                            int rl = 0;
                            const char *nxt = scan;
                            if (!md_read_line(scan, rr, sizeof(rr), &rl, &nxt)) break;
                            const char *rline = sanitize_line_view(rr, rl, rs, sizeof(rs), NULL);
                            if (!looks_like_table_row(rline)) break;
                            int cols = md_split_table_cells(rline, rows[rown], table_max_cols);
                            if (cols <= 0) break;
                            if (cols > ncol) ncol = cols;
                            rown++;
                            scan = nxt;
                        }
                        if (rown > 0) {
                            md_render_table_block(L, header, hcols, rows, rown, ncol);
                            p = scan;
                            continue;
                        }
                    }
                }
            }
        }

        /* Headers */
        if (line[0]=='#') {
            int lv=0; while(line[lv]=='#') lv++;
            if (line[lv]==' ') {
                const char *ht = line+lv+1;
                if (lv==1) {
                    L_push_blank_once(L);
                    snprintf(fb, sizeof(fb), BO C_AST "%s" RS, ht);
                    L_push(L, fb);
                    int ul = (int)strlen(ht)+2; if(ul>g_cols) ul=g_cols;
                    char sep[512]; int si=0;
                    si += snprintf(sep+si, sizeof(sep)-si, "%s", C_SEP);
                    for (int i=0; i<ul && si+4<(int)sizeof(sep); i++) si+=snprintf(sep+si,sizeof(sep)-si,HL);
                    snprintf(sep+si, sizeof(sep)-si, "%s", RS);
                    L_push(L, sep);
                } else if (lv==2) {
                    L_push_blank_once(L);
                    snprintf(fb, sizeof(fb), BO C_AST "%s" RS, ht); L_push(L, fb);
                } else {
                    snprintf(fb, sizeof(fb), BO DI C_AST "%s" RS, ht); L_push(L, fb);
                }
                continue;
            }
        }

        /* Bullets */
        int ind = 0; while(line[ind]==' ') ind++;
        if ((line[ind]=='-'||line[ind]=='*') && line[ind+1]==' ') {
            fmt_inline(fb, sizeof(fb), line+ind+2);
            char ob2[16384];
            snprintf(ob2, sizeof(ob2), "%*s" C_AST BUL " %s" RS, ind, "", fb);
            L_pushw_link(L, ob2); continue;
        }

        /* Numbered lists */
        if (isdigit((unsigned char)line[0])) {
            const char *d = line; while(isdigit((unsigned char)*d)) d++;
            if (d[0]=='.' && d[1]==' ') {
                int nl = (int)(d-line); char num[16];
                memcpy(num,line,nl); num[nl]='\0';
                fmt_inline(fb, sizeof(fb), d+2);
                char ob2[16384];
                snprintf(ob2, sizeof(ob2), C_AST "%s. %s" RS, num, fb);
                L_pushw_link(L, ob2); continue;
            }
        }

        /* Default text */
        if (line[0]) { fmt_inline(fb, sizeof(fb), line); L_pushw_link(L, fb); }
        else L_push(L, "");
    }
}

/* ── Item renderer ─────────────────────────────────────────────────────── */

#define MX_HUM 20
#define MX_RES 24
#define MX_DIF 80

static int count_lines(const char *t) {
    int n=1; for (const char *p=t; *p; p++) if (*p=='\n') n++; return n;
}

static int parse_hunk_header(const char *line, int *old_ln, int *new_ln);

static int is_diff_meta_line(const char *view) {
    return strncmp(view, "diff --git ", 11) == 0 ||
           strncmp(view, "index ", 6) == 0 ||
           strncmp(view, "--- ", 4) == 0 ||
           strncmp(view, "+++ ", 4) == 0 ||
           strncmp(view, "old mode ", 9) == 0 ||
           strncmp(view, "new mode ", 9) == 0 ||
           strncmp(view, "new file mode ", 14) == 0 ||
           strncmp(view, "deleted file mode ", 18) == 0 ||
           strncmp(view, "rename from ", 12) == 0 ||
           strncmp(view, "rename to ", 10) == 0 ||
           strncmp(view, "similarity index ", 17) == 0 ||
           strncmp(view, "Binary files ", 13) == 0 ||
           strncmp(view, "GIT binary patch", 16) == 0 ||
           strncmp(view, "\\ No newline at end of file", 27) == 0;
}

static int is_structured_diff_text(const char *t) {
    int has_add = 0, has_del = 0, has_ctx = 0;
    int has_valid_hunk = 0, has_invalid_hunk = 0;
    int has_diff_git_valid = 0;
    int has_old = 0, has_new = 0;
    int has_meta_detail = 0, has_binary = 0;
    const char *p = t;

    while (*p) {
        const char *ln = p;
        if (ln[0] == '@' && ln[1] == '@') {
            int o = 0, n = 0;
            if (parse_hunk_header(ln, &o, &n)) has_valid_hunk = 1;
            else has_invalid_hunk = 1;
        }
        else if (ln[0] == '+' && strncmp(ln, "+++", 3) != 0) has_add = 1;
        else if (ln[0] == '-' && strncmp(ln, "---", 3) != 0) has_del = 1;
        else if (ln[0] == ' ') has_ctx = 1;

        if (strncmp(ln, "diff --git ", 11) == 0) {
            const char *q = ln + 11;
            while (*q == ' ' || *q == '\t') q++;
            if (strncmp(q, "a/", 2) == 0) {
                const char *sp = q;
                while (*sp && *sp != ' ' && *sp != '\t') sp++;
                while (*sp == ' ' || *sp == '\t') sp++;
                if (strncmp(sp, "b/", 2) == 0) has_diff_git_valid = 1;
            }
        }
        if (strncmp(ln, "--- ", 4) == 0) has_old = 1;
        if (strncmp(ln, "+++ ", 4) == 0) has_new = 1;
        if (strncmp(ln, "Binary files ", 13) == 0 || strncmp(ln, "GIT binary patch", 16) == 0) {
            has_binary = 1;
        }
        if (strncmp(ln, "index ", 6) == 0 ||
            strncmp(ln, "old mode ", 9) == 0 ||
            strncmp(ln, "new mode ", 9) == 0 ||
            strncmp(ln, "new file mode ", 14) == 0 ||
            strncmp(ln, "deleted file mode ", 18) == 0 ||
            strncmp(ln, "rename from ", 12) == 0 ||
            strncmp(ln, "rename to ", 10) == 0 ||
            strncmp(ln, "similarity index ", 17) == 0) {
            has_meta_detail = 1;
        }

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    int has_file_headers = has_old && has_new;
    if (has_invalid_hunk) return 0;
    if (has_valid_hunk &&
        ((has_file_headers || has_diff_git_valid) || (has_add && has_del && has_ctx))) return 1;
    if (has_file_headers && has_binary) return 1;
    if (has_diff_git_valid && (has_binary || has_meta_detail || has_file_headers)) return 1;
    if (has_file_headers && has_meta_detail) return 1;
    return 0;
}

static void analyze_tool_result(const char *t, int preview_limit,
                                int *out_preview_lines, int *out_is_diff,
                                int *out_has_more, int *out_omitted_lines,
                                int *out_total_lines) {
    int lines = 1;
    int is_diff = 0;
    if (!t || !*t) {
        if (out_preview_lines) *out_preview_lines = 0;
        if (out_is_diff) *out_is_diff = 0;
        if (out_has_more) *out_has_more = 0;
        if (out_omitted_lines) *out_omitted_lines = 0;
        if (out_total_lines) *out_total_lines = 0;
        return;
    }
    if (preview_limit < 1) preview_limit = 1;
    const char *p = t;
    while (*p) {
        while (*p && *p != '\n') p++;
        if (*p == '\n') {
            lines++;
            p++;
        }
    }
    is_diff = is_structured_diff_text(t);
    int show = lines > preview_limit ? preview_limit : lines;
    int omitted = lines > show ? lines - show : 0;
    if (out_preview_lines) *out_preview_lines = show;
    if (out_is_diff) *out_is_diff = is_diff;
    if (out_has_more) *out_has_more = omitted > 0;
    if (out_omitted_lines) *out_omitted_lines = omitted;
    if (out_total_lines) *out_total_lines = lines;
}

static int parse_hunk_header(const char *line, int *old_ln, int *new_ln) {
    if (!line || line[0] != '@' || line[1] != '@') return 0;
    const char *dash = strchr(line, '-');
    const char *plus = strchr(line, '+');
    if (!dash || !plus || plus < dash) return 0;
    errno = 0;
    char *end_o = NULL;
    char *end_n = NULL;
    long o = strtol(dash + 1, &end_o, 10);
    if (errno != 0 || end_o == dash + 1) return 0;
    errno = 0;
    long n = strtol(plus + 1, &end_n, 10);
    if (errno != 0 || end_n == plus + 1) return 0;
    if (!(*end_o == ',' || *end_o == ' ' || *end_o == '\t')) return 0;
    if (!(*end_n == ',' || *end_n == ' ' || *end_n == '\t')) return 0;
    if (o < 0 || o > INT_MAX || n < 0 || n > INT_MAX) return 0;
    if (old_ln) *old_ln = (int)o;
    if (new_ln) *new_ln = (int)n;
    return 1;
}

static void normalize_diff_path(char *dst, int dstmax, const char *raw) {
    if (!dst || dstmax < 2) return;
    dst[0] = '\0';
    if (!raw) return;
    while (*raw == ' ' || *raw == '\t') raw++;
    if (strncmp(raw, "a/", 2) == 0 || strncmp(raw, "b/", 2) == 0) raw += 2;
    if (strcmp(raw, "/dev/null") == 0) return;

    int o = 0;
    if (*raw == '"') {
        raw++;
        while (*raw && *raw != '"' && o < dstmax - 1) {
            if (*raw == '\\' && raw[1]) raw++;
            dst[o++] = *raw++;
        }
    } else {
        while (*raw && *raw != '\n' && *raw != '\r' && *raw != '\t' && o < dstmax - 1) {
            dst[o++] = *raw++;
        }
    }
    dst[o] = '\0';
}

static void make_abs_path(char *dst, int dstmax, const char *path) {
    if (!dst || dstmax < 2) return;
    dst[0] = '\0';
    if (!path || !*path) return;
    if (path[0] == '/') {
        snprintf(dst, dstmax, "%s", path);
        return;
    }
    const char *pwd = getenv("PWD");
    if (!pwd || !*pwd) {
        snprintf(dst, dstmax, "%s", path);
        return;
    }
    snprintf(dst, dstmax, "%s/%s", pwd, path);
}

static int dec_digits10(int v) {
    int d = 1;
    while (v >= 10) {
        v /= 10;
        d++;
    }
    return d;
}

static int compute_diff_gutter_width(const char *text, int max_show) {
    if (!text || !*text || max_show <= 0) return 4;
    char line[12000], sline[12000];
    const char *p = text;
    int shown = 0;
    int old_ln = 0, new_ln = 0;
    int max_ln = 0;

    while (*p && shown < max_show) {
        const char *eol = strchr(p, '\n');
        int ll = eol ? (int)(eol - p) : (int)strlen(p);
        if (ll >= (int)sizeof(line)) ll = (int)sizeof(line) - 1;
        memcpy(line, p, (size_t)ll);
        line[ll] = '\0';
        int view_len = 0;
        const char *view = sanitize_line_view(line, ll, sline, sizeof(sline), &view_len);
        (void)view_len;

        if (view[0] == '@' && view[1] == '@') {
            parse_hunk_header(view, &old_ln, &new_ln);
            if (old_ln > max_ln) max_ln = old_ln;
            if (new_ln > max_ln) max_ln = new_ln;
        } else if (view[0] == '+' && strncmp(view, "+++", 3) != 0) {
            if (new_ln > max_ln) max_ln = new_ln;
            if (new_ln > 0) new_ln++;
        } else if (view[0] == '-' && strncmp(view, "---", 3) != 0) {
            if (old_ln > max_ln) max_ln = old_ln;
            if (old_ln > 0) old_ln++;
        } else if (view[0] == ' ') {
            if (old_ln > max_ln) max_ln = old_ln;
            if (new_ln > max_ln) max_ln = new_ln;
            if (old_ln > 0) old_ln++;
            if (new_ln > 0) new_ln++;
        }

        shown++;
        if (!eol) break;
        p = eol + 1;
    }

    int w = dec_digits10(max_ln > 0 ? max_ln : 0);
    if (w < 4) w = 4;
    if (w > 10) w = 10;
    return w;
}

static void diff_append_raw(char *out, int outsz, int *o, const char *s, int n) {
    if (!out || outsz <= 1 || !o || !s || n <= 0) return;
    if (*o >= outsz - 1) return;
    int room = outsz - 1 - *o;
    if (n > room) n = room;
    memcpy(out + *o, s, (size_t)n);
    *o += n;
    out[*o] = '\0';
}

static void diff_append_str(char *out, int outsz, int *o, const char *s) {
    if (!s) return;
    diff_append_raw(out, outsz, o, s, (int)strlen(s));
}

static int diff_is_word_char(unsigned char c) {
    return (isalnum(c) || c == '_' || c >= 0x80);
}

static int diff_is_keyword_token(const char *text, int s, int e) {
    int n = e - s;
    if (n == 4 && !memcmp(text + s, "true", 4)) return 1;
    if (n == 5 && !memcmp(text + s, "false", 5)) return 1;
    if (n == 4 && !memcmp(text + s, "null", 4)) return 1;
    if (n == 4 && !memcmp(text + s, "None", 4)) return 1;
    return 0;
}

static void diff_append_syntax(char *out, int outsz, int *o, const char *base, const char *text, int tlen) {
    if (!out || outsz <= 1 || !o || !base || !text || tlen <= 0) return;
    int i = 0;
    while (i < tlen && *o < outsz - 1) {
        unsigned char c = (unsigned char)text[i];
        if (c == '#') {
            diff_append_str(out, outsz, o, C_HDM);
            diff_append_raw(out, outsz, o, text + i, tlen - i);
            diff_append_str(out, outsz, o, base);
            break;
        }
        if (c == '"' || c == '\'') {
            char q = (char)c;
            int s = i++;
            while (i < tlen) {
                if (text[i] == '\\' && i + 1 < tlen) { i += 2; continue; }
                if (text[i] == q) { i++; break; }
                i++;
            }
            diff_append_str(out, outsz, o, C_SYN_STR);
            diff_append_raw(out, outsz, o, text + s, i - s);
            diff_append_str(out, outsz, o, base);
            continue;
        }
        if (isdigit(c)) {
            int s = i++;
            while (i < tlen) {
                unsigned char d = (unsigned char)text[i];
                if (!isdigit(d) && d != '.' && d != '_') break;
                i++;
            }
            diff_append_str(out, outsz, o, C_SYN_NUM);
            diff_append_raw(out, outsz, o, text + s, i - s);
            diff_append_str(out, outsz, o, base);
            continue;
        }
        if (diff_is_word_char(c) && (isalpha(c) || c == '_')) {
            int s = i++;
            while (i < tlen && diff_is_word_char((unsigned char)text[i])) i++;
            int j = i;
            while (j < tlen && (text[j] == ' ' || text[j] == '\t')) j++;
            int assign = (j < tlen && text[j] == '=') ? 1 : 0;
            int kw = diff_is_keyword_token(text, s, i);
            if (assign) diff_append_str(out, outsz, o, C_HUM);
            else if (kw) diff_append_str(out, outsz, o, C_SYN_KW);
            diff_append_raw(out, outsz, o, text + s, i - s);
            if (assign || kw) diff_append_str(out, outsz, o, base);
            continue;
        }
        diff_append_raw(out, outsz, o, text + i, 1);
        i++;
    }
}

static void render_diff_row(Lines *L, const char *conn, const char *style, int gutter_w,
                            int old_ln, int new_ln, char mark, const char *text, int tlen,
                            int allow_linkify) {
    char b[32768];
    char body[24576];
    char lo[24], ln[24];
    const char *mc = (mark == '+') ? C_DFG : (mark == '-') ? C_DFR : (mark == '@' || mark == '*') ? C_DFC : C_HDM;
    if (old_ln > 0) snprintf(lo, sizeof(lo), "%*d", gutter_w, old_ln);
    else snprintf(lo, sizeof(lo), "%*s", gutter_w, "");
    if (new_ln > 0) snprintf(ln, sizeof(ln), "%*d", gutter_w, new_ln);
    else snprintf(ln, sizeof(ln), "%*s", gutter_w, "");
    int bo = 0;
    body[0] = '\0';
    if (mark == '+' || mark == '-' || mark == ' ') diff_append_syntax(body, sizeof(body), &bo, style, text, tlen);
    else diff_append_raw(body, sizeof(body), &bo, text, tlen);
    snprintf(b, sizeof(b), RS "%s%s%s %s %s%c%s %s", conn, style, lo, ln, mc, mark, style, body);
    if (allow_linkify) L_pushw_link(L, b);
    else L_pushw(L, b);
}

static void diff_inline_bounds(const char *old_t, int old_len,
                               const char *new_t, int new_len,
                               int *old_s, int *old_e,
                               int *new_s, int *new_e) {
    int pre = 0;
    while (pre < old_len && pre < new_len && old_t[pre] == new_t[pre]) pre++;

    int os = old_len - 1;
    int ns = new_len - 1;
    while (os >= pre && ns >= pre && old_t[os] == new_t[ns]) { os--; ns--; }

    if (old_s) *old_s = pre;
    if (old_e) *old_e = os + 1;
    if (new_s) *new_s = pre;
    if (new_e) *new_e = ns + 1;
}

static int utf8_is_cont_byte(unsigned char c) {
    return (c & 0xC0u) == 0x80u;
}

static int utf8_clamp_left(const char *s, int len, int pos) {
    if (!s || len <= 0) return 0;
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    while (pos > 0 && pos < len && utf8_is_cont_byte((unsigned char)s[pos])) pos--;
    return pos;
}

static int utf8_clamp_right(const char *s, int len, int pos) {
    if (!s || len <= 0) return 0;
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    while (pos < len && utf8_is_cont_byte((unsigned char)s[pos])) pos++;
    return pos;
}

#define DIFF_TOK_MAX 192
#define DIFF_RNG_MAX 48

static int diff_tok_class(unsigned char c) {
    return (isalnum(c) || c == '_' || c >= 0x80) ? 1 : 0;
}

static int diff_split_tokens(const char *t, int len, int *ts, int *te, int max_tok) {
    if (!t || len <= 0 || !ts || !te || max_tok <= 0) return 0;
    int n = 0, i = 0;
    while (i < len && n < max_tok) {
        int s = i;
        int cls = diff_tok_class((unsigned char)t[i]);
        i++;
        while (i < len && diff_tok_class((unsigned char)t[i]) == cls) i++;
        ts[n] = s;
        te[n] = i;
        n++;
    }
    return n;
}

static int diff_tok_eq(const char *a, int as, int ae, const char *b, int bs, int be) {
    int al = ae - as, bl = be - bs;
    if (al != bl) return 0;
    if (al <= 0) return 1;
    return memcmp(a + as, b + bs, (size_t)al) == 0;
}

static int diff_token_ranges(const char *old_t, int old_len,
                             const char *new_t, int new_len,
                             int *ors, int *ore, int *orc,
                             int *nrs, int *nre, int *nrc) {
    if (orc) *orc = 0;
    if (nrc) *nrc = 0;
    if (!old_t || !new_t || old_len <= 0 || new_len <= 0) return 0;

    int ots[DIFF_TOK_MAX], ote[DIFF_TOK_MAX];
    int nts[DIFF_TOK_MAX], nte[DIFF_TOK_MAX];
    int on = diff_split_tokens(old_t, old_len, ots, ote, DIFF_TOK_MAX);
    int nn = diff_split_tokens(new_t, new_len, nts, nte, DIFF_TOK_MAX);
    if (on <= 0 || nn <= 0 || on >= DIFF_TOK_MAX - 1 || nn >= DIFF_TOK_MAX - 1) return 0;

    static int dp[DIFF_TOK_MAX + 1][DIFF_TOK_MAX + 1];
    memset(dp, 0, sizeof(dp));
    for (int i = on - 1; i >= 0; i--) {
        for (int j = nn - 1; j >= 0; j--) {
            if (diff_tok_eq(old_t, ots[i], ote[i], new_t, nts[j], nte[j])) dp[i][j] = dp[i + 1][j + 1] + 1;
            else dp[i][j] = dp[i + 1][j] >= dp[i][j + 1] ? dp[i + 1][j] : dp[i][j + 1];
        }
    }

    int old_match[DIFF_TOK_MAX];
    int new_match[DIFF_TOK_MAX];
    memset(old_match, 0, sizeof(old_match));
    memset(new_match, 0, sizeof(new_match));
    int i = 0, j = 0;
    while (i < on && j < nn) {
        if (diff_tok_eq(old_t, ots[i], ote[i], new_t, nts[j], nte[j])) {
            old_match[i] = 1;
            new_match[j] = 1;
            i++;
            j++;
        } else if (dp[i + 1][j] >= dp[i][j + 1]) {
            i++;
        } else {
            j++;
        }
    }

    int oc = 0;
    i = 0;
    while (i < on && oc < DIFF_RNG_MAX) {
        while (i < on && old_match[i]) i++;
        if (i >= on) break;
        int s = i;
        while (i < on && !old_match[i]) i++;
        int e = i - 1;
        if (ors && ore) { ors[oc] = ots[s]; ore[oc] = ote[e]; }
        oc++;
    }
    int nc = 0;
    j = 0;
    while (j < nn && nc < DIFF_RNG_MAX) {
        while (j < nn && new_match[j]) j++;
        if (j >= nn) break;
        int s = j;
        while (j < nn && !new_match[j]) j++;
        int e = j - 1;
        if (nrs && nre) { nrs[nc] = nts[s]; nre[nc] = nte[e]; }
        nc++;
    }

    if (orc) *orc = oc;
    if (nrc) *nrc = nc;
    return (oc > 0 && nc > 0) ? 1 : 0;
}

static void render_diff_row_hl(Lines *L, const char *conn,
                               const char *style, const char *hl_style,
                               int gutter_w, int old_ln, int new_ln, char mark,
                               const char *text, int tlen, int hs, int he,
                               int allow_linkify) {
    if (!text || tlen <= 0 || hs < 0 || he <= hs || hs >= tlen) {
        render_diff_row(L, conn, style, gutter_w, old_ln, new_ln, mark, text ? text : "", tlen > 0 ? tlen : 0, allow_linkify);
        return;
    }
    if (he > tlen) he = tlen;
    hs = utf8_clamp_left(text, tlen, hs);
    he = utf8_clamp_right(text, tlen, he);
    if (hs >= he) {
        render_diff_row(L, conn, style, gutter_w, old_ln, new_ln, mark, text, tlen, allow_linkify);
        return;
    }

    char b[32768];
    char body[24576];
    char lo[24], ln[24];
    const char *mc = (mark == '+') ? C_DFG : (mark == '-') ? C_DFR : (mark == '@' || mark == '*') ? C_DFC : C_HDM;
    if (old_ln > 0) snprintf(lo, sizeof(lo), "%*d", gutter_w, old_ln);
    else snprintf(lo, sizeof(lo), "%*s", gutter_w, "");
    if (new_ln > 0) snprintf(ln, sizeof(ln), "%*d", gutter_w, new_ln);
    else snprintf(ln, sizeof(ln), "%*s", gutter_w, "");

    int bo = 0;
    body[0] = '\0';
    if (hs > 0) diff_append_syntax(body, sizeof(body), &bo, style, text, hs);
    if (he > hs) diff_append_syntax(body, sizeof(body), &bo, hl_style, text + hs, he - hs);
    if (tlen > he) diff_append_syntax(body, sizeof(body), &bo, style, text + he, tlen - he);

    snprintf(
        b, sizeof(b),
        RS "%s%s%s %s %s%c%s %s" RS,
        conn, style, lo, ln, mc, mark, style,
        body
    );
    if (allow_linkify) L_pushw_link(L, b);
    else L_pushw(L, b);
}

static void render_diff_row_multi_hl(Lines *L, const char *conn,
                                     const char *style, const char *hl_style,
                                     int gutter_w, int old_ln, int new_ln, char mark,
                                     const char *text, int tlen,
                                     const int *rs, const int *re, int rc,
                                     int allow_linkify) {
    if (!text || tlen <= 0 || !rs || !re || rc <= 0) {
        render_diff_row(L, conn, style, gutter_w, old_ln, new_ln, mark, text ? text : "", tlen > 0 ? tlen : 0, allow_linkify);
        return;
    }

    char b[32768];
    char body[24576];
    char lo[24], ln[24];
    const char *mc = (mark == '+') ? C_DFG : (mark == '-') ? C_DFR : (mark == '@' || mark == '*') ? C_DFC : C_HDM;
    if (old_ln > 0) snprintf(lo, sizeof(lo), "%*d", gutter_w, old_ln);
    else snprintf(lo, sizeof(lo), "%*s", gutter_w, "");
    if (new_ln > 0) snprintf(ln, sizeof(ln), "%*d", gutter_w, new_ln);
    else snprintf(ln, sizeof(ln), "%*s", gutter_w, "");

    int bo = 0;
    body[0] = '\0';
    int pos = 0;
    for (int i = 0; i < rc; i++) {
        int s = utf8_clamp_left(text, tlen, rs[i]);
        int e = utf8_clamp_right(text, tlen, re[i]);
        if (s < pos) s = pos;
        if (e <= s || s >= tlen) continue;
        if (s > pos) diff_append_syntax(body, sizeof(body), &bo, style, text + pos, s - pos);
        diff_append_syntax(body, sizeof(body), &bo, hl_style, text + s, e - s);
        pos = e;
    }
    if (pos < tlen) diff_append_syntax(body, sizeof(body), &bo, style, text + pos, tlen - pos);
    snprintf(b, sizeof(b), RS "%s%s%s %s %s%c%s %s" RS, conn, style, lo, ln, mc, mark, style, body);
    if (allow_linkify) L_pushw_link(L, b);
    else L_pushw(L, b);
}

static void render_diff_block(Lines *L, const char *text, const char *conn, int max_show, int omitted_lines) {
    char line[12000], sline[12000];
    char cur_path[2048] = "";
    char last_anchor_path[2048] = "";
    char abs_path[4096];
    int old_ln = 0, new_ln = 0;
    int shown = 0;
    int gutter_w = compute_diff_gutter_width(text, max_show);
    static int show_anchors = -1;
    static int show_hunk_refs = -1;
    if (show_anchors < 0) show_anchors = env_enabled("CLAUDE_PAGER_DIFF_ANCHOR") ? 1 : 0;
    if (show_hunk_refs < 0) show_hunk_refs = env_enabled_default_on("CLAUDE_PAGER_DIFF_HUNK_REF") ? 1 : 0;
    const char *p = text;

    while (*p && shown < max_show) {
        const char *eol = strchr(p, '\n');
        int ll = eol ? (int)(eol - p) : (int)strlen(p);
        if (ll >= (int)sizeof(line)) ll = (int)sizeof(line) - 1;
        memcpy(line, p, (size_t)ll);
        line[ll] = '\0';
        int view_len = 0;
        const char *view = sanitize_line_view(line, ll, sline, sizeof(sline), &view_len);
        ll = view_len;

        if (strncmp(view, "+++ ", 4) == 0 || strncmp(view, "--- ", 4) == 0) {
            normalize_diff_path(cur_path, sizeof(cur_path), view + 4);
            if (show_anchors && cur_path[0] && strcmp(last_anchor_path, cur_path) != 0 && shown < max_show) {
                make_abs_path(abs_path, sizeof(abs_path), cur_path);
                if (abs_path[0]) {
                    char anchor[8192];
                    snprintf(anchor, sizeof(anchor), "%s" C_DFC "↳ %s" RS, conn, abs_path);
                    L_pushw_link(L, anchor);
                    shown++;
                }
                snprintf(last_anchor_path, sizeof(last_anchor_path), "%s", cur_path);
                if (shown >= max_show) break;
            }
        }

        if (view[0] == '@' && view[1] == '@') {
            parse_hunk_header(view, &old_ln, &new_ln);
            render_diff_row(L, conn, C_DCBG, gutter_w, 0, 0, '@', view, ll, 0);
            shown++;
            if (show_hunk_refs && cur_path[0] && shown < max_show) {
                make_abs_path(abs_path, sizeof(abs_path), cur_path);
                if (abs_path[0]) {
                    char href[8192];
                    if (old_ln > 0 && new_ln > 0) {
                        snprintf(href, sizeof(href), "%s" C_HDM "↳ %s  L%d -> L%d" RS,
                                 conn, abs_path, old_ln, new_ln);
                    } else if (new_ln > 0) {
                        snprintf(href, sizeof(href), "%s" C_HDM "↳ %s  L%d" RS,
                                 conn, abs_path, new_ln);
                    } else {
                        snprintf(href, sizeof(href), "%s" C_HDM "↳ %s" RS, conn, abs_path);
                    }
                    L_pushw_link(L, href);
                    shown++;
                }
            }
        } else if (view[0] == '-' && strncmp(view, "---", 3) != 0) {
            const char *np = eol ? (eol + 1) : NULL;
            if (np && *np && shown + 1 < max_show) {
                const char *neol = strchr(np, '\n');
                int nll = neol ? (int)(neol - np) : (int)strlen(np);
                char nline[12000], nsline[12000];
                if (nll >= (int)sizeof(nline)) nll = (int)sizeof(nline) - 1;
                memcpy(nline, np, (size_t)nll);
                nline[nll] = '\0';
                int nview_len = 0;
                const char *nview = sanitize_line_view(nline, nll, nsline, sizeof(nsline), &nview_len);
                nll = nview_len;
                if (nview[0] == '+' && strncmp(nview, "+++", 3) != 0) {
                    int old_txt = ll > 0 ? ll - 1 : 0;
                    int new_txt = nll > 0 ? nll - 1 : 0;
                    int os = 0, oe = old_txt, ns = 0, ne = new_txt;
                    int ors[DIFF_RNG_MAX], ore[DIFF_RNG_MAX], orc = 0;
                    int nrs[DIFF_RNG_MAX], nre[DIFF_RNG_MAX], nrc = 0;
                    int used_multi = diff_token_ranges(
                        view + 1, old_txt, nview + 1, new_txt,
                        ors, ore, &orc, nrs, nre, &nrc
                    );
                    if (!used_multi) {
                        diff_inline_bounds(view + 1, old_txt, nview + 1, new_txt, &os, &oe, &ns, &ne);
                    }
                    int oo = old_ln > 0 ? old_ln : 0;
                    int nn = new_ln > 0 ? new_ln : 0;
                    if (used_multi && orc > 0 && nrc > 0) {
                        render_diff_row_multi_hl(L, conn, C_DDBG, C_DDHL, gutter_w, oo, 0, '-', view + 1, old_txt, ors, ore, orc, 1);
                        render_diff_row_multi_hl(L, conn, C_DABG, C_DAHL, gutter_w, 0, nn, '+', nview + 1, new_txt, nrs, nre, nrc, 1);
                    } else {
                        render_diff_row_hl(L, conn, C_DDBG, C_DDHL, gutter_w, oo, 0, '-', view + 1, old_txt, os, oe, 1);
                        render_diff_row_hl(L, conn, C_DABG, C_DAHL, gutter_w, 0, nn, '+', nview + 1, new_txt, ns, ne, 1);
                    }
                    if (old_ln > 0) old_ln++;
                    if (new_ln > 0) new_ln++;
                    shown += 2;
                    if (!neol) break;
                    p = neol + 1;
                    continue;
                }
            }
            int oo = old_ln > 0 ? old_ln : 0;
            render_diff_row(L, conn, C_DDBG, gutter_w, oo, 0, '-', view + 1, ll > 0 ? ll - 1 : 0, 1);
            if (old_ln > 0) old_ln++;
            shown++;
        } else if (view[0] == '+' && strncmp(view, "+++", 3) != 0) {
            int nn = new_ln > 0 ? new_ln : 0;
            render_diff_row(L, conn, C_DABG, gutter_w, 0, nn, '+', view + 1, ll > 0 ? ll - 1 : 0, 1);
            if (new_ln > 0) new_ln++;
            shown++;
        } else if (view[0] == ' ') {
            int oo = old_ln > 0 ? old_ln : 0;
            int nn = new_ln > 0 ? new_ln : 0;
            render_diff_row(L, conn, C_DMBG, gutter_w, oo, nn, ' ', view + 1, ll > 0 ? ll - 1 : 0, 1);
            if (old_ln > 0) old_ln++;
            if (new_ln > 0) new_ln++;
            shown++;
        } else if (is_diff_meta_line(view)) {
            render_diff_row(L, conn, C_DMETA, gutter_w, 0, 0, '*', view, ll, 0);
            shown++;
        } else {
            render_diff_row(L, conn, C_DMBG, gutter_w, 0, 0, '|', view, ll, 1);
            shown++;
        }

        if (!eol) break;
        p = eol + 1;
    }

    if (omitted_lines > 0) {
        char b[256];
        snprintf(b, sizeof(b), "%s" C_HDM ELL " (+%d more lines)" RS, conn, omitted_lines);
        L_push(L, b);
    }
}

static int is_structured_patch_payload(const char *text) {
    return text && strncmp(text, "CP_SP1\n", 7) == 0;
}

static int parse_patch_header_fields(const char *s, int n, int *old_start, int *old_lines, int *new_start, int *new_lines) {
    if (!s || n < 3 || s[0] != 'P' || s[1] != '\t') return 0;
    char tmp[256];
    int take = n < (int)sizeof(tmp) - 1 ? n : (int)sizeof(tmp) - 1;
    memcpy(tmp, s, (size_t)take);
    tmp[take] = '\0';
    int os = 0, ol = 0, ns = 0, nl = 0;
    if (sscanf(tmp + 2, "%d\t%d\t%d\t%d", &os, &ol, &ns, &nl) != 4) return 0;
    if (old_start) *old_start = os;
    if (old_lines) *old_lines = ol;
    if (new_start) *new_start = ns;
    if (new_lines) *new_lines = nl;
    return 1;
}

static void analyze_structured_patch_payload(const char *text, int *out_total_rows, int *out_max_ln) {
    if (out_total_rows) *out_total_rows = 0;
    if (out_max_ln) *out_max_ln = 0;
    if (!is_structured_patch_payload(text)) return;

    int total_rows = 0, max_ln = 0;
    int old_ln = 0, new_ln = 0;
    const char *p = text + 7;
    while (*p) {
        const char *e = strchr(p, '\n');
        int ll = e ? (int)(e - p) : (int)strlen(p);
        if (ll > 2 && p[1] == '\t') {
            if (p[0] == 'P') {
                int os = 0, ol = 0, ns = 0, nl = 0;
                if (parse_patch_header_fields(p, ll, &os, &ol, &ns, &nl)) {
                    old_ln = os;
                    new_ln = ns;
                    int end_old = os + (ol > 0 ? ol : 0);
                    int end_new = ns + (nl > 0 ? nl : 0);
                    if (end_old > max_ln) max_ln = end_old;
                    if (end_new > max_ln) max_ln = end_new;
                }
            } else if (p[0] == 'L') {
                total_rows++;
                char mark = p[2];
                if (mark == '\\') {
                    /* metadata row, counters unchanged */
                } else if (mark == '+') {
                    if (new_ln > max_ln) max_ln = new_ln;
                    if (new_ln > 0) new_ln++;
                } else if (mark == '-') {
                    if (old_ln > max_ln) max_ln = old_ln;
                    if (old_ln > 0) old_ln++;
                } else {
                    int ln = new_ln > 0 ? new_ln : old_ln;
                    if (ln > max_ln) max_ln = ln;
                    if (old_ln > 0) old_ln++;
                    if (new_ln > 0) new_ln++;
                }
            }
        }
        if (!e) break;
        p = e + 1;
    }
    if (out_total_rows) *out_total_rows = total_rows;
    if (out_max_ln) *out_max_ln = max_ln;
}

static void build_structured_patch_body(char *out, int outsz,
                                        const char *base, const char *hl,
                                        const char *text, int tlen,
                                        const int *rs, const int *re, int rc) {
    int o = 0;
    if (!out || outsz <= 1) return;
    out[0] = '\0';
    if (!text || tlen <= 0) return;
    if (!hl || !rs || !re || rc <= 0) {
        diff_append_syntax(out, outsz, &o, base, text, tlen);
        return;
    }
    int pos = 0;
    for (int i = 0; i < rc; i++) {
        int s = utf8_clamp_left(text, tlen, rs[i]);
        int e = utf8_clamp_right(text, tlen, re[i]);
        if (s < pos) s = pos;
        if (e <= s || s >= tlen) continue;
        if (s > pos) diff_append_syntax(out, outsz, &o, base, text + pos, s - pos);
        diff_append_syntax(out, outsz, &o, hl, text + s, e - s);
        pos = e;
    }
    if (pos < tlen) diff_append_syntax(out, outsz, &o, base, text + pos, tlen - pos);
}

static void render_structured_patch_row(Lines *L, const char *conn,
                                        const char *style, const char *hl_style, int gutter_w,
                                        int ln, char mark, const char *text, int tlen,
                                        const int *rs, const int *re, int rc) {
    char b[32768];
    char body[24576];
    char nb[32];
    const char *mc = (mark == '+') ? C_DFG : (mark == '-') ? C_DFR : C_HDM;
    if (ln > 0) snprintf(nb, sizeof(nb), "%*d", gutter_w, ln);
    else snprintf(nb, sizeof(nb), "%*s", gutter_w, "");

    build_structured_patch_body(body, sizeof(body), style, hl_style, text, tlen, rs, re, rc);
    snprintf(b, sizeof(b), RS "%s%s%s%s%c%s %s" RS, conn, style, nb, mc, mark, style, body);
    L_pushw_link(L, b);
}

static void render_structured_patch_block(Lines *L, const char *text, const char *conn, int max_show) {
    int total_rows = 0, max_ln = 0;
    analyze_structured_patch_payload(text, &total_rows, &max_ln);
    int gutter_w = dec_digits10(max_ln > 0 ? max_ln : 0);
    if (gutter_w < 4) gutter_w = 4;
    if (gutter_w > 10) gutter_w = 10;

    int shown = 0;
    int old_ln = 0, new_ln = 0;

    const char *p = text + 7;
    while (*p && shown < max_show) {
        const char *e = strchr(p, '\n');
        int ll = e ? (int)(e - p) : (int)strlen(p);
        if (ll > 2 && p[1] == '\t') {
            if (p[0] == 'P') {
                int os = 0, ol = 0, ns = 0, nl = 0;
                if (parse_patch_header_fields(p, ll, &os, &ol, &ns, &nl)) {
                    old_ln = os;
                    new_ln = ns;
                }
            } else if (p[0] == 'L') {
                const char *line = p + 2;
                int tlen = ll - 2;
                char mark = tlen > 0 ? line[0] : ' ';
                if (mark == '\\') {
                    render_structured_patch_row(L, conn, C_DMETA, NULL, gutter_w, 0, '*', line, tlen, NULL, NULL, 0);
                    shown++;
                } else if (mark == '+') {
                    int ln = new_ln > 0 ? new_ln : 0;
                    render_structured_patch_row(L, conn, C_DABG, NULL, gutter_w, ln, '+',
                                                tlen > 1 ? line + 1 : "", tlen > 1 ? tlen - 1 : 0, NULL, NULL, 0);
                    if (new_ln > 0) new_ln++;
                    shown++;
                } else if (mark == '-') {
                    int ln = old_ln > 0 ? old_ln : 0;
                    const char *next = e ? e + 1 : NULL;
                    int paired = 0;
                    if (next && shown + 1 < max_show && next[0] == 'L' && next[1] == '\t') {
                        const char *ne = strchr(next, '\n');
                        int nll = ne ? (int)(ne - next) : (int)strlen(next);
                        const char *nline = next + 2;
                        int ntlen = nll - 2;
                        char nmark = ntlen > 0 ? nline[0] : ' ';
                        if (nmark == '+') {
                            const char *old_txt = tlen > 1 ? line + 1 : "";
                            int old_tlen = tlen > 1 ? tlen - 1 : 0;
                            const char *new_txt = ntlen > 1 ? nline + 1 : "";
                            int new_tlen = ntlen > 1 ? ntlen - 1 : 0;

                            int ors[DIFF_RNG_MAX], ore[DIFF_RNG_MAX], orc = 0;
                            int nrs[DIFF_RNG_MAX], nre[DIFF_RNG_MAX], nrc = 0;
                            int used_multi = diff_token_ranges(
                                old_txt, old_tlen, new_txt, new_tlen,
                                ors, ore, &orc, nrs, nre, &nrc
                            );

                            if (used_multi && orc > 0 && nrc > 0) {
                                render_structured_patch_row(L, conn, C_DDBG, C_DDHL, gutter_w, ln, '-',
                                                            old_txt, old_tlen, ors, ore, orc);
                                render_structured_patch_row(L, conn, C_DABG, C_DAHL, gutter_w,
                                                            new_ln > 0 ? new_ln : 0, '+',
                                                            new_txt, new_tlen, nrs, nre, nrc);
                            } else {
                                int os = 0, oe = old_tlen, ns = 0, ne2 = new_tlen;
                                diff_inline_bounds(old_txt, old_tlen, new_txt, new_tlen, &os, &oe, &ns, &ne2);
                                if (oe > os && ne2 > ns) {
                                    int or1[1] = { os }, oe1[1] = { oe };
                                    int nr1[1] = { ns }, ne1[1] = { ne2 };
                                    render_structured_patch_row(L, conn, C_DDBG, C_DDHL, gutter_w, ln, '-',
                                                                old_txt, old_tlen, or1, oe1, 1);
                                    render_structured_patch_row(L, conn, C_DABG, C_DAHL, gutter_w,
                                                                new_ln > 0 ? new_ln : 0, '+',
                                                                new_txt, new_tlen, nr1, ne1, 1);
                                } else {
                                    render_structured_patch_row(L, conn, C_DDBG, NULL, gutter_w, ln, '-',
                                                                old_txt, old_tlen, NULL, NULL, 0);
                                    render_structured_patch_row(L, conn, C_DABG, NULL, gutter_w,
                                                                new_ln > 0 ? new_ln : 0, '+',
                                                                new_txt, new_tlen, NULL, NULL, 0);
                                }
                            }
                            if (old_ln > 0) old_ln++;
                            if (new_ln > 0) new_ln++;
                            shown += 2;
                            if (!ne) break;
                            p = ne + 1;
                            paired = 1;
                        }
                    }
                    if (paired) continue;
                    render_structured_patch_row(L, conn, C_DDBG, NULL, gutter_w, ln, '-',
                                                tlen > 1 ? line + 1 : "", tlen > 1 ? tlen - 1 : 0, NULL, NULL, 0);
                    if (old_ln > 0) old_ln++;
                    shown++;
                } else {
                    int ln = new_ln > 0 ? new_ln : (old_ln > 0 ? old_ln : 0);
                    render_structured_patch_row(L, conn, C_DMBG, NULL, gutter_w, ln, ' ', line, tlen, NULL, NULL, 0);
                    if (old_ln > 0) old_ln++;
                    if (new_ln > 0) new_ln++;
                    shown++;
                }
            }
        }
        if (!e) break;
        p = e + 1;
    }

    if (total_rows > shown) {
        char b[256];
        snprintf(b, sizeof(b), "%s" C_HDM ELL " (+%d more lines)" RS, conn, total_rows - shown);
        L_push(L, b);
    }
}

static void render_items(Lines *L, Items *items) {
    int prev_tu = 0;
    char b[16384];
    static int max_tool_lines = -1;
    static int max_diff_lines = -1;
    static int show_tool_rail = -1;
    if (max_tool_lines < 0) {
        int def_tool = g_perf_compat ? 6 : MX_RES;
        max_tool_lines = parse_env_int_range("CLAUDE_PAGER_MAX_RESULT_LINES", 1, 400, def_tool);
    }
    if (max_diff_lines < 0) {
        int def_diff = g_perf_compat ? 6 : MX_DIF;
        max_diff_lines = parse_env_int_range("CLAUDE_PAGER_MAX_DIFF_LINES", 1, 800, def_diff);
    }
    if (show_tool_rail < 0) {
        show_tool_rail = env_enabled("CLAUDE_PAGER_TOOL_RAIL") ? 1 : 0;
    }

    for (int i = 0; i < items->n; i++) {
        Item *it = &items->d[i];

        switch (it->type) {
        case IT_HUM: {
            L_push_blank_once(L);
            int nl = count_lines(it->text);
            int show = nl > MX_HUM ? MX_HUM : nl;
            const char *p = it->text;
            char sb[16384];
            for (int ln=0; ln<show; ln++) {
                const char *eol = strchr(p, '\n');
                int ll = eol ? (int)(eol-p) : (int)strlen(p);
                int raw_ll = ll;
                if (raw_ll >= (int)sizeof(sb)) raw_ll = (int)sizeof(sb) - 1;
                int view_len = 0;
                const char *view = sanitize_line_view(p, raw_ll, sb, sizeof(sb), &view_len);
                if (view_len > 0) {
                    if (ln == 0) snprintf(b, sizeof(b), C_UBG " " CHV " %.*s " RS, view_len, view);
                    else snprintf(b, sizeof(b), C_UBG "   %.*s " RS, view_len, view);
                    L_pushw_link(L, b);
                } else {
                    L_push(L, "");
                }
                p = eol ? eol+1 : p+ll;
            }
            if (nl > MX_HUM) {
                snprintf(b, sizeof(b), C_HDM "  " ELL " (%d more lines)" RS, nl-MX_HUM);
                L_push(L, b);
            }
            break;
        }
        case IT_AST:
            L_push_blank_once(L);
            {
                int md_keep = 0;
                if (L->max_keep > 0) {
                    md_keep = L->max_keep - L->n - 8;
                    if (md_keep < 1) md_keep = 1;
                }
                render_md(L, it->text, md_keep);
            }
            break;

        case IT_TU:
            L_push_blank_once(L);
            if (it->label && it->label[0])
                snprintf(b, sizeof(b), C_TOL BUL RS " " BO C_AST "%s" RS " " DI C_HDM "%s" RS, it->text, it->label);
            else
                snprintf(b, sizeof(b), C_TOL BUL RS " " BO C_AST "%s" RS, it->text);
            L_pushw_link(L, b);
            break;

        case IT_TR: {
            const char *col = it->is_err ? C_ERR : C_RES;
            const char *conn = (prev_tu && show_tool_rail) ? "  " C_CONN VL RS " " : "  ";
            if (!it->is_err && is_structured_patch_payload(it->text)) {
                render_structured_patch_block(L, it->text, conn, max_diff_lines);
                break;
            }
            int df = 0, show = 0, has_more = 0, omitted = 0, total = 0;
            int detect_preview = max_diff_lines > max_tool_lines ? max_diff_lines : max_tool_lines;
            analyze_tool_result(it->text, detect_preview, &show, &df, &has_more, &omitted, &total);
            if (!df) {
                show = total > max_tool_lines ? max_tool_lines : total;
                omitted = total > show ? total - show : 0;
                has_more = omitted > 0;
            } else {
                show = total > max_diff_lines ? max_diff_lines : total;
                omitted = total > show ? total - show : 0;
                has_more = omitted > 0;
            }
            if (df) {
                render_diff_block(L, it->text, conn, show, omitted);
            } else {
                const char *p = it->text;
                char sb[16384];
                for (int ln=0; ln<show; ln++) {
                    const char *eol = strchr(p, '\n');
                    int ll = eol ? (int)(eol-p) : (int)strlen(p);
                    int raw_ll = ll;
                    if (raw_ll >= (int)sizeof(sb)) raw_ll = (int)sizeof(sb) - 1;
                    int view_len = 0;
                    const char *view = sanitize_line_view(p, raw_ll, sb, sizeof(sb), &view_len);
                    snprintf(b, sizeof(b), "%s%s%.*s" RS, conn, col, view_len, view);
                    L_pushw_link(L, b);
                    if (!eol) break;
                    p = eol+1;
                }
                if (has_more) {
                    snprintf(b, sizeof(b), "%s" C_HDM ELL " (+%d more lines, showing first %d)" RS, conn, omitted, show);
                    L_push(L, b);
                }
            }
            break;
        }
        }
        prev_tu = (it->type == IT_TU);
    }
}

/* ── Drawing ───────────────────────────────────────────────────────────── */

static void draw_sep(void) {
    ob(C_SEP); for (int i=0; i<g_cols; i++) ob(HL); ob(RS);
}

static void draw_status(int tok, double pct, int cl) {
    ob(C_BAN "  Editor open " EMD " edit and close to send" RS);
    ob(DI);
    if (g_queue_enabled) obf("  " DOT "  queue:%d", g_queue.n);
    if (g_queue_notice[0]) obf("  " DOT "  %s", g_queue_notice);
    ob(RS);
    if (tok <= 0) return;

    static const char *bar_levels[] = { " ", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d", "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89", "\xe2\x96\x88" };
    int bw = 8;
    double scaled = pct / 100.0 * bw * 8.0;
    if (scaled < 0.0) scaled = 0.0;
    if (scaled > bw * 8.0) scaled = bw * 8.0;

    char ct[64];
    snprintf(ct, sizeof(ct), "%.0f%%  %.0fk/%dk", pct, tok/1000.0, cl/1000);

    int bvl = 70, cvl = bw + 3 + (int)strlen(ct), svl = 9;
    int pad = g_cols - bvl - svl - cvl;
    if (pad<0) pad=0;

    obf("%*s" DI "  " DOT "  " RS, pad, "");
    ob(C_HDM "ctx " RS);
    for (int i = 0; i < bw; i++) {
        double rem = scaled - (double)(i * 8);
        int level = (int)(rem + 0.0001);
        if (level < 0) level = 0;
        if (level > 8) level = 8;
        if (i == 0) ob(C_SEP "[");
        if (level > 0) {
            const char *segc = (i < 4) ? C_BRG : (i < 6) ? C_BRY : C_BRR;
            ob(segc);
            ob(bar_levels[level]);
        } else {
            ob(C_SEP DOT);
        }
        if (i == bw - 1) ob(C_SEP "]");
    }
    ob(RS DI " "); ob(ct); ob(RS);
}

static void draw_queue_panel(void) {
    if (g_queue_rows <= 0) return;

    int qsep_row = g_rows - 3 - g_queue_rows;
    if (qsep_row < 2) qsep_row = 2;

    obf("\033[%d;1H", qsep_row);
    ob(C_SEP);
    for (int i = 0; i < g_cols; i++) ob(HL);
    ob(RS);
    ob("\033[K");

    int row = qsep_row + 1;
    int item_rows = g_queue.n;
    if (item_rows > QUEUE_SHOW_MAX) item_rows = QUEUE_SHOW_MAX;
    if (item_rows < 0) item_rows = 0;

    if (g_queue.n > 0) {
        obf("\033[%d;1H", row++);
        ob(C_QBG);
        obf("  " BUL " Queue(%d)", g_queue.n);
        if (g_edit_index >= 0) ob(C_QACC "  editing history" RS C_QBG);
        else if (g_input_draft_saved) ob(C_QACC "  draft stashed" RS C_QBG);
        ob("\033[K");
        ob(RS);

        int max_scroll = g_queue.n - item_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (g_queue.scroll_off > max_scroll) g_queue.scroll_off = max_scroll;
        if (g_queue.selected < g_queue.scroll_off) g_queue.scroll_off = g_queue.selected;
        if (g_queue.selected >= g_queue.scroll_off + item_rows) {
            g_queue.scroll_off = g_queue.selected - item_rows + 1;
        }
        if (g_queue.scroll_off < 0) g_queue.scroll_off = 0;
    }

    for (int i = 0; i < item_rows; i++) {
        int idx = g_queue.scroll_off + i;
        obf("\033[%d;1H", row++);
        ob(C_QBG);
        if (idx < g_queue.n) {
            char compact[1024];
            int max_chars = g_cols - 12;
            if (max_chars < 12) max_chars = 12;
            if (idx == g_edit_index) queue_compact_prompt(g_input_buf, compact, max_chars);
            else queue_compact_prompt(g_queue.d[idx].prompt, compact, max_chars);
            if (idx == g_queue.selected) {
                ob(C_QSEL);
                obf("  " CHV " %2d. %s", idx + 1, compact);
                ob(RS);
            } else {
                ob(C_QBG);
                obf("    %2d. ", idx + 1);
                ob(C_AST);
                obf("%s", compact);
                ob(RS);
            }
        }
        ob("\033[K");
        ob(RS);
    }

    int outer_w = g_cols - 4;
    if (outer_w < 8) outer_w = 8;
    int inner_w = outer_w - 2;
    if (inner_w < 6) inner_w = 6;

    InputLayout lo;
    input_layout_compute(&lo, inner_w);

    char label[128];
    if (g_edit_index >= 0) snprintf(label, sizeof(label), " Editing Queue #%d ", g_edit_index + 1);
    else snprintf(label, sizeof(label), " Prompt ");
    int label_len = (int)strlen(label);

    obf("\033[%d;1H", row++);
    ob("  ");
    ob(C_QACC);
    ob(TL);
    if (label_len + 1 < inner_w) {
        ob(HL);
        ob(BO);
        ob(label);
        ob(RS);
        ob(C_QACC);
        for (int i = 0; i < inner_w - label_len - 1; i++) ob(HL);
    } else {
        for (int i = 0; i < inner_w; i++) ob(HL);
    }
    ob(TR);
    ob(RS);
    ob("\033[K");

    for (int vis = 0; vis < lo.visible_lines; vis++) {
        int li = lo.visible_start + vis;
        int start = lo.starts[li];
        int end = lo.ends[li];
        int cursor_on_line = (li == lo.cursor_line);
        int cursor_drawn = 0;
        int cells = 0;

        obf("\033[%d;1H", row++);
        ob("  ");
        ob(C_QACC); ob(VL); ob(RS);
        ob(C_QBG);

        for (int pos = start; pos < end;) {
            int next = input_next_boundary(g_input_buf, end, pos);
            if (cursor_on_line && !cursor_drawn && pos == g_input_cursor) {
                ob(C_QSEL);
                ob_raw(g_input_buf + pos, next - pos);
                ob(RS);
                ob(C_QBG);
                cursor_drawn = 1;
            } else {
                ob_raw(g_input_buf + pos, next - pos);
            }
            pos = next;
            cells++;
        }
        if (cursor_on_line && !cursor_drawn) {
            ob(C_QSEL " " RS);
            ob(C_QBG);
            cells++;
        }
        while (cells < inner_w) {
            ob(" ");
            cells++;
        }
        ob(RS);
        ob(C_QACC); ob(VL); ob(RS);
        ob("\033[K");
    }

    obf("\033[%d;1H", row++);
    ob("  ");
    ob(C_QACC);
    ob(BL);
    for (int i = 0; i < inner_w; i++) ob(HL);
    ob(BR);
    ob(RS);
    ob("\033[K");

}

static void draw_hotkeys_footer(void) {
    ob(C_QBG);
    if (g_hover_uri[0]) {
        draw_hover_footer_uri();
        ob(RS);
        return;
    }
    int maxw = g_cols - 2;
    int used = 0;
    if (g_input_mode) {
        footer_emit_plain("  ", &used, maxw);
        footer_emit_styled(C_HDM, "keys: ", &used, maxw);
        footer_emit_styled(C_QACC, "⇧↑/↓", &used, maxw);
        footer_emit_plain(" hist  ", &used, maxw);
        footer_emit_styled(C_QACC, "⇧Enter", &used, maxw);
        footer_emit_plain(" nl  ", &used, maxw);
        footer_emit_styled(C_QACC, "Enter", &used, maxw);
        footer_emit_plain(" save  ", &used, maxw);
        footer_emit_styled(C_QACC, "Esc", &used, maxw);
        footer_emit_plain(" clear  ", &used, maxw);
        footer_emit_styled(C_QACC, "^V", &used, maxw);
        footer_emit_plain(" attach  ", &used, maxw);
        footer_emit_styled(C_QACC, "^D", &used, maxw);
        footer_emit_plain(" del  ", &used, maxw);
        if (g_ctrl_quit_supported) {
            footer_emit_styled(C_QACC, "^Q", &used, maxw);
            footer_emit_plain(" close", &used, maxw);
        }
    } else {
        footer_emit_plain("  ", &used, maxw);
        footer_emit_styled(C_HDM, "keys: ", &used, maxw);
        footer_emit_styled(C_QACC, "⇧↑/↓", &used, maxw);
        footer_emit_plain(" cycle/edit  ", &used, maxw);
        if (g_ctrl_quit_supported) {
            footer_emit_styled(C_QACC, "^Q", &used, maxw);
            footer_emit_plain(" close", &used, maxw);
        }
    }
    ob(RS);
}

static void draw(Lines *L, int off, int tok, double pct, int cl, int first) {
    g_ol = 0;
    link_map_clear();
    if (g_sync_enabled) { ob("\033[?2026h"); g_sync_begin_count++; }

    if (first) ob("\033[?25l\033[2J\033[H");
    else       ob("\033[?25l\033[H");

    draw_sep(); ob("\033[K\n");
    int row = 2;

    if (off > 0) {
        if (g_last_capped_lines > 0) {
            obf(C_HDM "  " UAR " %d lines above  (scroll to view)  " ELL " (+%d capped)" RS "\033[K\n", off, g_last_capped_lines);
        } else {
            obf(C_HDM "  " UAR " %d lines above  (scroll to view)" RS "\033[K\n", off);
        }
        row++;
    }

    int avail = g_crows - (off>0 ? 1 : 0);
    int end = off + avail;
    if (end > L->n) end = L->n;

    for (int i = off; i < end; i++) {
        int used_rows = link_map_track_line(L->d[i], row);
        emit_line_with_hover(L->d[i], row);
        ob("\033[K\n");
        row += (used_rows > 0 ? used_rows : 1);
    }

    int body_end = (g_queue_rows > 0) ? (g_rows - 3 - g_queue_rows) : (g_rows - 3);
    if (body_end < row) body_end = row;
    while (row < body_end) { ob("\033[K\n"); row++; }

    draw_queue_panel();

    if (g_queue_rows <= 0) {
        obf("\033[%d;1H", g_rows - 3);
        ob(C_SEP);
        for (int i = 0; i < g_cols; i++) ob(HL);
        ob(RS);
        ob("\033[K");
        obf("\033[%d;1H", g_rows - 2);
        draw_status(tok, pct, cl);
        ob("\033[K");
        obf("\033[%d;1H", g_rows - 1);
        ob(C_QBG);
        ob("\033[K");
        obf("\033[%d;1H", g_rows); draw_hotkeys_footer();
        ob("\033[K");
    } else {
        obf("\033[%d;1H", g_rows - 2);
        ob(C_SEP);
        for (int i = 0; i < g_cols; i++) ob(HL);
        ob(RS);
        ob("\033[K");
        obf("\033[%d;1H", g_rows - 1);
        draw_status(tok, pct, cl);
        ob("\033[K");
        obf("\033[%d;1H", g_rows); draw_hotkeys_footer();
        ob("\033[K");
    }
    if (g_sync_enabled) { ob("\033[?2026l"); g_sync_end_count++; }

    ob_flush();
}

/* ── Terminal ──────────────────────────────────────────────────────────── */

static int term_raw(int fd) {
    struct termios t;
    if (tcgetattr(fd, &g_old) != 0) return -1;
    g_term_saved = 1;
    t = g_old;
    t.c_iflag &= ~(unsigned long)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                  INLCR | IGNCR | ICRNL | IXON | IXOFF);
    t.c_lflag &= ~(unsigned long)(ICANON | ECHO | IEXTEN | ISIG);
    t.c_cflag |= (unsigned long)CS8;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) return -1;
    g_term_raw = 1;
    return 0;
}

static void term_restore(void) {
    if (g_fd < 0) return;
    if (g_restored) return;
    g_restored = 1;
    write_all(g_fd, "\033[?2026l", strlen("\033[?2026l"));
    g_sync_unwind_end_count++;
    if (g_mouse_enabled) write_all(g_fd, MOUSE_OFF, strlen(MOUSE_OFF));
    write_all(g_fd, "\033[?25h", 6);
    if (g_term_saved && g_term_raw) tcsetattr(g_fd, TCSANOW, &g_old);
    g_term_raw = 0;
    g_mouse_enabled = 0;
}

/* ── Input ─────────────────────────────────────────────────────────────── */

#define INP_NONE 0
#define INP_HOME 10000
#define INP_END  10001
#define INP_QUIT 10002
#define INP_QUEUE_ADD 10003
#define INP_ENTER 10004
#define INP_BACKSPACE 10005
#define INP_ESC 10006
#define INP_QUEUE_UP 10007
#define INP_QUEUE_DOWN 10008
#define INP_TAB 10009
#define INP_CTRL_QUIT 10010
#define INP_QCYCLE_UP 10011
#define INP_QCYCLE_DOWN 10012
#define INP_QDELETE 10013
#define INP_ATTACH_CLIP 10014
#define INP_LEFT 10015
#define INP_RIGHT 10016
#define INP_NEWLINE 10017
#define INP_MOUSE_IGNORE 10018
#define INP_WHEEL_UP 10019
#define INP_WHEEL_DOWN 10020
#define INP_MOUSE_OPEN 10021
#define INP_MOUSE_MOVE 10022
#define INP_CHAR_BASE 20000

static int decode_sgr_mouse(const unsigned char *buf, ssize_t n) {
    if (!buf || n < 6) return INP_NONE;
    if (buf[0] != 0x1b || buf[1] != '[' || buf[2] != '<') return INP_NONE;

    int params[3] = {0, 0, 0};
    int param_count = 0;
    int current = 0;
    int saw_digit = 0;

    for (ssize_t i = 3; i < n; i++) {
        unsigned char c = buf[i];
        if (c >= '0' && c <= '9') {
            current = current * 10 + (c - '0');
            saw_digit = 1;
        } else if (c == ';') {
            if (!saw_digit || param_count >= 3) return INP_NONE;
            params[param_count++] = current;
            current = 0;
            saw_digit = 0;
        } else if (c == 'M' || c == 'm') {
            if (!saw_digit || param_count >= 3) return INP_NONE;
            params[param_count++] = current;
            if (param_count != 3) return INP_NONE;
            int button = params[0] & 0x43;
            int press = (c == 'M');
            g_mouse_x = params[1];
            g_mouse_y = params[2];
            if (button == 64) return INP_WHEEL_UP;
            if (button == 65) return INP_WHEEL_DOWN;
            if (params[0] & 32) return INP_MOUSE_MOVE;
            if (press && button == 0) return INP_MOUSE_OPEN;
            return INP_MOUSE_IGNORE;
        } else {
            return INP_NONE;
        }
    }

    return INP_NONE;
}

static int decode_escape_key(const unsigned char *buf, ssize_t n) {
    if (!buf || n < 2 || buf[0] != 0x1b) return INP_NONE;

    if (buf[1] == 'O' && n >= 3) {
        if (buf[2] == 'A') return -1;
        if (buf[2] == 'B') return 1;
        if (buf[2] == 'H') return INP_HOME;
        if (buf[2] == 'F') return INP_END;
        return INP_NONE;
    }

    if (buf[1] != '[') return INP_NONE;
    if (n < 3) return INP_NONE;

    char final = 0;
    int params[4] = {0, 0, 0, 0};
    int param_count = 0;
    int current = 0;
    int saw_digit = 0;
    ssize_t i = 2;
    for (; i < n; i++) {
        unsigned char c = buf[i];
        if (c >= '0' && c <= '9') {
            current = current * 10 + (c - '0');
            saw_digit = 1;
        } else if (c == ';') {
            if (param_count < (int)(sizeof(params) / sizeof(params[0]))) {
                params[param_count++] = current;
            }
            current = 0;
            saw_digit = 0;
        } else {
            if (saw_digit && param_count < (int)(sizeof(params) / sizeof(params[0]))) {
                params[param_count++] = current;
            }
            final = (char)c;
            break;
        }
    }
    if (!final) return INP_NONE;

    int p1 = (param_count >= 1) ? params[0] : 0;
    int p2 = (param_count >= 2) ? params[1] : 0;
    int p3 = (param_count >= 3) ? params[2] : 0;
    int have_p2 = (param_count >= 2);
    int have_p3 = (param_count >= 3);

    if (final == 'A') {
        if (have_p2 && p2 == 2) return INP_QCYCLE_UP;
        return -1;
    }
    if (final == 'B') {
        if (have_p2 && p2 == 2) return INP_QCYCLE_DOWN;
        return 1;
    }
    if (final == 'C') return INP_RIGHT;
    if (final == 'D') return INP_LEFT;
    if (final == 'H') return INP_HOME;
    if (final == 'F') return INP_END;
    if (final == 'u') {
        if (p1 == 13) {
            if (have_p2 && p2 == 2) return INP_NEWLINE;
            return INP_ENTER;
        }
    }
    if (final == '~') {
        if (p1 == 5) return -(g_crows - 1);
        if (p1 == 6) return g_crows - 1;
        if (p1 == 3 && have_p2 && p2 == 2) return INP_QDELETE; /* Shift+Delete */
        if (p1 == 27 && have_p2 && have_p3 && p2 == 2 && p3 == 13) return INP_NEWLINE; /* modifyOtherKeys Shift+Enter */
    }

    return INP_NONE;
}

static ssize_t read_escape_suffix(int fd, unsigned char *buf, ssize_t n, size_t cap) {
    if (!buf || n <= 0 || (size_t)n >= cap) return n;
    if (buf[0] != 0x1b) return n;

    for (int i = 0; i < 4 && (size_t)n < cap; i++) {
        fd_set fds;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 2000; /* 2ms coalesce for split escape sequences */
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) break;
        ssize_t r = read(fd, buf + n, cap - (size_t)n);
        if (r <= 0) break;
        n += r;
        if (n >= 2) {
            if (buf[1] == '[' || buf[1] == 'O') {
                unsigned char tail = buf[n - 1];
                if (tail >= 0x40 && tail <= 0x7e) break;
            } else {
                break;
            }
        }
    }
    return n;
}

static int poll_input(int fd, int timeout_ms, int input_mode) {
    if (input_mode) {
        int pc = input_pending_pop();
        if (pc >= 0) return INP_CHAR_BASE + pc;
    }

    if (timeout_ms < 0) timeout_ms = 0;
    fd_set fds;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    FD_ZERO(&fds); FD_SET(fd, &fds);
    if (select(fd+1, &fds, NULL, NULL, &tv) <= 0) return INP_NONE;

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) return INP_NONE;
    if ((unsigned char)buf[0] == 0x1b) n = read_escape_suffix(fd, (unsigned char *)buf, n, sizeof(buf));

    int mouse_ev = decode_sgr_mouse((const unsigned char *)buf, n);
    if (mouse_ev != INP_NONE) return mouse_ev;

    int esc_ev = decode_escape_key((const unsigned char *)buf, n);
    if (esc_ev != INP_NONE) {
        if (input_mode && (esc_ev == INP_NEWLINE || esc_ev == INP_ENTER)) {
            pdebug_input_bytes("input esc decoded", (const unsigned char *)buf, n);
        }
        if (!input_mode) return esc_ev;
        if (esc_ev == -1 || esc_ev == 1 || esc_ev == INP_LEFT || esc_ev == INP_RIGHT ||
            esc_ev == INP_HOME || esc_ev == INP_END || esc_ev == INP_QCYCLE_UP ||
            esc_ev == INP_QCYCLE_DOWN || esc_ev == INP_QDELETE || esc_ev == INP_ENTER ||
            esc_ev == INP_NEWLINE) {
            return esc_ev;
        }
        return INP_NONE;
    }

    if (input_mode) {
        if (n == 1) {
            unsigned char c = (unsigned char)buf[0];
            if (c == '\r' || c == '\n') {
                pdebug_input_bytes("input char enter", (const unsigned char *)buf, n);
            }
            if (c == 0x11) return INP_CTRL_QUIT; /* Ctrl+Q */
            if (c == 0x04) return INP_QDELETE;   /* Ctrl+D */
            if (c == 0x16) return INP_ATTACH_CLIP; /* Ctrl+V */
            if (c == '\t') return INP_CHAR_BASE + (int)' ';
            if (c == 0x1b) return INP_ESC;
            if (c == '\r') return INP_ENTER;
            if (c == '\n') return INP_NEWLINE;
            if (c == 127 || c == 8) return INP_BACKSPACE;
            if (c >= 32 && c != 127) return INP_CHAR_BASE + (int)c;
            return INP_NONE;
        }

        input_pending_consume_chunk((const unsigned char *)buf, n);
        int pc = input_pending_pop();
        if (pc >= 0) return INP_CHAR_BASE + pc;
        if ((unsigned char)buf[0] == 0x1b) {
            pdebug_input_bytes("input esc unhandled", (const unsigned char *)buf, n);
        }
        return INP_NONE;
    }

    int delta = 0;
    for (ssize_t i=0; i<n; i++) {
        if ((unsigned char)buf[i] == 0x11) {
            return INP_CTRL_QUIT;
        } else if (buf[i] == '\t') {
            return INP_NONE;
        } else if (buf[i] == 'a' || buf[i] == 'A' || buf[i] == 'Q') {
            return INP_QUEUE_ADD;
        } else if (buf[i] == 'k') {
            return INP_QUEUE_UP;
        } else if (buf[i] == 'j') {
            return INP_QUEUE_DOWN;
        }
    }
    return delta;
}

/* ── Main loop ─────────────────────────────────────────────────────────── */

void run_pager(int tty_fd, const char *transcript, int editor_pid, int ctx_limit, int control_fd) {
    g_fd = tty_fd;
    g_quit = 0;
    g_resize = 0;
    g_term_saved = 0;
    g_term_raw = 0;
    g_mouse_enabled = 0;
    g_restored = 0;
    g_oom = 0;
    g_sync_begin_count = 0;
    g_sync_end_count = 0;
    g_sync_unwind_end_count = 0;
    g_allow_remote_file_links = -1;
    g_exit_after_first_draw = env_enabled("CLAUDE_PAGER_EXIT_AFTER_FIRST_DRAW");
    g_perf_compat = env_enabled("CLAUDE_PAGER_PERF_COMPAT");
    g_last_capped_lines = 0;
    g_queue_rows = 0;
    g_queue_enabled = 0;
    g_ctrl_quit_supported = (control_fd >= 0);
    g_queue_stamp.valid = 0;
    g_input_mode = 0;
    input_clear_buffer();
    input_discard_draft();
    g_edit_index = -1;
    input_pending_reset();
    queue_set_notice("");
    queue_clear_items();
    if (ctx_limit <= 0) ctx_limit = 200000;
    g_bench_mode = env_enabled("CLAUDE_PAGER_BENCH");
    dbg_open();
    PDBG("run start transcript=%s editor_pid=%d ctx_limit=%d control_fd=%d\n",
         (transcript && transcript[0]) ? transcript : "(none)",
         editor_pid, ctx_limit, control_fd);
    if (g_bench_mode) PDBG("bench probes enabled\n");

    install_signal_handlers();

    geo_update();
    if (term_raw(tty_fd) != 0) {
        PDBG("term_raw failed errno=%d; exiting pager loop\n", errno);
        return;
    }
    atexit(term_restore);
    sync_init(tty_fd);
    if (write_all(tty_fd, MOUSE_ON, strlen(MOUSE_ON)) == 0) g_mouse_enabled = 1;

    queue_init_path(transcript);
    g_input_mode = g_queue_enabled ? 1 : 0;
    if (queue_load_from_disk()) {
        queue_set_notice("queue loaded");
    }
    queue_recalc_rows();

    Lines L; L_init(&L);
    int off = 0, uscroll = 0;
    int tok = 0; double pct = 0;
    int first = 1;
    int first_draw_logged = 0;
    int rehit_hover_after_draw = 0;
    int prev_dropped_total = 0;
    int prev_had_capped_banner = 0;
    int load_seq = 0;
    FileStamp st = {0};
    int default_render_cap = g_perf_compat ? 0 : 20000;
    int max_render_lines = parse_env_int_range("CLAUDE_PAGER_MAX_RENDER_LINES", 0, 2000000, default_render_cap);
    if (max_render_lines > 0) {
        L_set_limit(&L, max_render_lines);
        PDBG("render line cap=%d\n", max_render_lines);
    }

    while (!g_quit) {
        if (editor_pid > 0 && kill(editor_pid, 0) != 0) break;

        if (g_resize) {
            g_resize = 0; geo_update();
            queue_recalc_rows();
            first = 1;
        }

        int cc = 0;
        if (transcript && transcript[0]) {
            if (file_stamp_changed(transcript, &st)) {
                cc = 1;
                load_seq++;
                Items items; memset(&items, 0, sizeof(items));
                tok = 0; pct = 0;
                long long t_parse0 = now_us();
                PDBG("parse start load=%d\n", load_seq);
                parse_transcript(transcript, &items, &tok, &pct, ctx_limit);
                long long t_parse1 = now_us();
                PDBG("parse end load=%d duration=%.2fms tok=%d pct=%.3f\n",
                     load_seq, (double)(t_parse1 - t_parse0) / 1000.0, tok, pct);
                L_free(&L);
                long long t_render0 = now_us();
                PDBG("markdown render start load=%d\n", load_seq);
                render_items(&L, &items);
                long long t_render1 = now_us();
                L_push(&L, C_HDM "  " EMD " end of transcript " EMD RS);
                L_push(&L, ""); L_push(&L, "");
                if (L.max_keep > 0 && L.n > L.max_keep) {
                    L_drop_head(&L, L.n - L.max_keep);
                }
                int new_dropped_total = L.dropped_total;
                int new_had_capped_banner = new_dropped_total > 0 ? 1 : 0;
                if (!first) {
                    int drop_delta = new_dropped_total - prev_dropped_total;
                    int banner_delta = new_had_capped_banner - prev_had_capped_banner;
                    int off_adjust = -drop_delta + banner_delta;
                    if (off_adjust != 0) {
                        off += off_adjust;
                        PDBG("cap adjust load=%d off_adjust=%d drop_delta=%d banner_delta=%d off=%d\n",
                             load_seq, off_adjust, drop_delta, banner_delta, off);
                    }
                }
                if (new_had_capped_banner) {
                    char db[128];
                    snprintf(db, sizeof(db), "  " C_HDM ELL " (+%d older lines capped)" RS, new_dropped_total);
                    L_prepend(&L, db);
                    g_last_capped_lines = new_dropped_total;
                    PDBG("render cap dropped=%d keep=%d\n", new_dropped_total, L.n);
                } else {
                    g_last_capped_lines = 0;
                }
                prev_dropped_total = new_dropped_total;
                prev_had_capped_banner = new_had_capped_banner;
                PDBG("markdown render end load=%d duration=%.2fms lines=%d\n",
                     load_seq, (double)(t_render1 - t_render0) / 1000.0, L.n);
                I_free(&items);
                if (off < 0) off = 0;
                if (off >= L.n) off = L.n > 0 ? (L.n - 1) : 0;
                if (!uscroll) { int b = L.n-(g_crows-1); off = b>0 ? b : 0; }
            }
        } else if (first) {
            cc = 1;
            L_push(&L, C_HDM "(transcript not found)" RS);
        }

        if (queue_load_from_disk()) cc = 1;

        int inp = poll_input(tty_fd, (cc || first) ? 0 : 20, g_input_mode);
        int sc = 0;

        if (inp == INP_CTRL_QUIT) {
            if (control_fd >= 0) {
                char cmd = 'q';
                if (write_all(control_fd, &cmd, 1) == 0) {
                    queue_set_notice("closing TurboDraft session");
                    sc = 1;
                } else {
                    queue_set_notice("session close failed");
                    sc = 1;
                }
            } else {
                queue_set_notice("tip: Ctrl+Q closes a TurboDraft session");
                sc = 1;
            }
            inp = INP_NONE;
        }
        if (inp == INP_QCYCLE_UP || inp == INP_QCYCLE_DOWN) {
            if (queue_cycle_edit(inp == INP_QCYCLE_UP ? -1 : 1)) {
                queue_recalc_rows();
                sc = 1;
            }
            inp = INP_NONE;
        }

        if (inp == INP_MOUSE_MOVE) {
            if (refresh_hover_from_pointer()) sc = 1;
            inp = INP_NONE;
        }

        if (inp == INP_MOUSE_OPEN) {
            int hit_row = link_mouse_row();
            const char *uri = link_map_hit(hit_row, g_mouse_x);
            if (uri && *uri) {
                (void)hover_link_set(uri, hit_row);
                open_uri_async(uri);
                queue_set_notice("opened link");
                sc = 1;
            }
            inp = INP_NONE;
        }

        if (g_input_mode) {
            if (inp == INP_ESC) {
                if (g_edit_index >= 0 && input_restore_draft()) {
                    g_edit_index = -1;
                    queue_set_notice("draft restored");
                } else {
                    input_clear_buffer();
                    input_discard_draft();
                    g_edit_index = -1;
                    queue_set_notice("input cleared");
                }
                input_pending_reset();
                queue_recalc_rows();
                sc = 1;
            } else if (inp == INP_ENTER) {
                if (g_input_len > 0) {
                    if (g_edit_index >= 0 && g_edit_index < g_queue.n) {
                        char *updated = xstrdup(g_input_buf);
                        if (!updated) {
                            queue_set_notice("queue write failed");
                        } else {
                            free(g_queue.d[g_edit_index].prompt);
                            g_queue.d[g_edit_index].prompt = updated;
                            g_queue.d[g_edit_index].encoding_json = 1;
                            int write_rc = queue_write_all_items();
                            if (write_rc == 0) {
                                queue_set_notice("queue updated");
                                (void)queue_load_from_disk();
                                g_queue.selected = g_edit_index;
                            } else if (write_rc == QUEUE_WRITE_CONFLICT) {
                                (void)queue_load_from_disk();
                                queue_set_notice("queue changed on disk; reloaded");
                            } else {
                                queue_set_notice("queue write failed");
                            }
                        }
                    } else {
                        if (queue_append_prompt(g_input_buf) == 0) {
                            queue_set_notice("queued");
                            (void)queue_load_from_disk();
                            if (g_queue.n > 0) g_queue.selected = g_queue.n - 1;
                        } else {
                            queue_set_notice("queue write failed");
                        }
                    }
                } else {
                    queue_set_notice("empty prompt");
                }
                input_clear_buffer();
                input_discard_draft();
                input_pending_reset();
                g_edit_index = -1;
                queue_recalc_rows();
                sc = 1;
            } else if (inp == INP_ATTACH_CLIP) {
                int files = queue_attach_clipboard_file_refs();
                if (files > 0) {
                    if (files == 1) queue_set_notice("clipboard file attached");
                    else queue_set_notice("clipboard files attached");
                } else if (files < 0) {
                    queue_set_notice("input too long for file refs");
                } else {
                    char image_path[PATH_MAX];
                    int rc = queue_attach_clipboard_image(image_path, sizeof(image_path));
                    if (rc == 0) {
                        char ref[PATH_MAX * 2];
                        if (!queue_make_ref_token(image_path, ref, sizeof(ref))) {
                            queue_set_notice("input too long for image ref");
                        } else if (input_append_token(ref)) {
                            queue_set_notice("clipboard image attached");
                        } else {
                            queue_set_notice("input too long for image ref");
                        }
                    } else if (rc == -5) {
                        queue_set_notice("clipboard image too large (20MB max)");
                    } else {
                        queue_set_notice("no clipboard file/image found");
                    }
                }
                sc = 1;
            } else if (inp == INP_NEWLINE) {
                if (input_insert_byte('\n')) {
                    queue_recalc_rows();
                    sc = 1;
                }
            } else if (inp == INP_BACKSPACE) {
                if (input_delete_prev()) {
                    queue_recalc_rows();
                    sc = 1;
                }
            } else if (inp == INP_QDELETE) {
                if (g_queue.n > 0) {
                    queue_clamp_selection();
                    int idx = g_edit_index >= 0 ? g_edit_index : g_queue.selected;
                    if (idx >= 0 && idx < g_queue.n) {
                        queue_item_free(&g_queue.d[idx]);
                        for (int i = idx; i + 1 < g_queue.n; i++) g_queue.d[i] = g_queue.d[i + 1];
                        g_queue.n--;
                        if (g_queue.selected >= g_queue.n) g_queue.selected = g_queue.n - 1;
                        if (g_queue.selected < 0) g_queue.selected = 0;
                        int write_rc = queue_write_all_items();
                        if (write_rc == 0) {
                            (void)queue_load_from_disk();
                            queue_set_notice("queue item removed");
                        } else if (write_rc == QUEUE_WRITE_CONFLICT) {
                            (void)queue_load_from_disk();
                            queue_set_notice("queue changed on disk; reloaded");
                        } else {
                            queue_set_notice("queue write failed");
                        }
                    }
                }
                if (g_queue.n > 0) {
                    queue_clamp_selection();
                    if (g_edit_index >= 0 && g_queue.selected >= 0 && g_queue.selected < g_queue.n) {
                        if (g_queue.selected >= g_queue.n) g_queue.selected = g_queue.n - 1;
                        queue_set_input_from_selected();
                    }
                } else if (input_restore_draft()) {
                    g_edit_index = -1;
                    queue_set_notice("draft restored");
                } else {
                    input_clear_buffer();
                    g_edit_index = -1;
                }
                input_pending_reset();
                queue_recalc_rows();
                sc = 1;
            } else if (inp >= INP_CHAR_BASE && inp < INP_CHAR_BASE + 256) {
                if (input_insert_byte((unsigned char)(inp - INP_CHAR_BASE))) {
                    queue_recalc_rows();
                    sc = 1;
                }
            } else if (inp == INP_LEFT) {
                input_move_cursor_left();
                sc = 1;
            } else if (inp == INP_RIGHT) {
                input_move_cursor_right();
                sc = 1;
            } else if (inp == -1) {
                input_move_cursor_vert(-1);
                sc = 1;
            } else if (inp == 1) {
                input_move_cursor_vert(1);
                sc = 1;
            } else if (inp == INP_HOME) {
                input_move_cursor_home();
                sc = 1;
            } else if (inp == INP_END) {
                input_move_cursor_end();
                sc = 1;
            } else if (inp == INP_MOUSE_IGNORE) {
                inp = INP_NONE;
            } else if (inp == INP_WHEEL_UP || inp == INP_WHEEL_DOWN) {
                off += (inp == INP_WHEEL_UP) ? -1 : 1;
                if (off < 0) off = 0;
                int mx = L.n > 0 ? L.n - 1 : 0;
                if (off > mx) off = mx;
                uscroll = 1;
                hover_link_set("", 0);
                rehit_hover_after_draw = 1;
                sc = 1;
            } else if (inp == -(g_crows - 1) || inp == (g_crows - 1)) {
                off += inp;
                if (off < 0) off = 0;
                int mx = L.n > 0 ? L.n - 1 : 0;
                if (off > mx) off = mx;
                uscroll = 1;
                hover_link_set("", 0);
                rehit_hover_after_draw = 1;
                sc = 1;
            } else if (inp != INP_NONE) {
                off += inp;
                if (off < 0) off = 0;
                int mx = L.n > 0 ? L.n - 1 : 0;
                if (off > mx) off = mx;
                uscroll = 1;
                hover_link_set("", 0);
                rehit_hover_after_draw = 1;
                sc = 1;
            }
        } else {
            if (inp == INP_QUEUE_ADD) {
                if (g_queue_enabled) {
                    g_input_mode = 1;
                    input_clear_buffer();
                    input_discard_draft();
                    input_pending_reset();
                    g_edit_index = -1;
                    queue_set_notice("");
                    queue_recalc_rows();
                    sc = 1;
                } else {
                    queue_set_notice("queue unavailable");
                    sc = 1;
                }
            } else if (inp == INP_QUEUE_UP) {
                if (g_queue.n > 0) {
                    g_queue.selected--;
                    queue_clamp_selection();
                    sc = 1;
                }
            } else if (inp == INP_QUEUE_DOWN) {
                if (g_queue.n > 0) {
                    g_queue.selected++;
                    queue_clamp_selection();
                    sc = 1;
                }
            } else if (inp == INP_HOME) { off=0; uscroll=1; sc=1; }
            else if (inp == INP_END) { int b=L.n-(g_crows-1); off=b>0?b:0; uscroll=0; sc=1; }
            else if (inp == INP_MOUSE_IGNORE) { inp = INP_NONE; }
            else if (inp == INP_WHEEL_UP || inp == INP_WHEEL_DOWN) {
                off += (inp == INP_WHEEL_UP) ? -1 : 1;
                if (off<0) off=0;
                int mx = L.n>0 ? L.n-1 : 0;
                if (off>mx) off=mx;
                uscroll = 1;
                hover_link_set("", 0);
                rehit_hover_after_draw = 1;
                sc = 1;
            }
            else if (inp != INP_NONE) {
                off += inp;
                if (off<0) off=0;
                int mx = L.n>0 ? L.n-1 : 0;
                if (off>mx) off=mx;
                uscroll = 1;
                hover_link_set("", 0);
                rehit_hover_after_draw = 1;
                sc = 1;
            }
        }

        if (cc || sc || first) {
            draw(&L, off, tok, pct, ctx_limit, first);
            if (rehit_hover_after_draw) {
                rehit_hover_after_draw = 0;
                if (refresh_hover_from_pointer()) {
                    draw(&L, off, tok, pct, ctx_limit, 0);
                }
            }
            if (!first_draw_logged && first) {
                PDBG("first draw done off=%d lines=%d\n", off, L.n);
                bench_probe_terminal_ready(tty_fd, "first_draw");
                first_draw_logged = 1;
            }
            first = 0;
            if (g_exit_after_first_draw && first_draw_logged) break;
        }

    }
    term_restore();
    PDBG("run end sync_begin=%d sync_end=%d sync_unwind_end=%d oom=%d\n",
         g_sync_begin_count, g_sync_end_count, g_sync_unwind_end_count, g_oom);
    L_free(&L);
    link_map_clear();
    queue_clear_items();
}

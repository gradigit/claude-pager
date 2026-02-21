/*
 * claude-pager-open — editor shim for Claude Code with built-in pager
 *
 * Works with any GUI editor.  Detects TurboDraft's Unix socket for
 * zero-overhead launch; falls back to CLAUDE_PAGER_EDITOR / VISUAL / EDITOR.
 *
 * Build: cd bin && make
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include "pager.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* ── Debug logging ─────────────────────────────────────────────────────────── */

static FILE *dbg;
static struct timeval t0;
static void dbg_open(void) {
    gettimeofday(&t0, NULL);
    dbg = fopen("/tmp/claude-pager-open.log", "a");
    if (dbg) setbuf(dbg, NULL);
}
static double elapsed_ms(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - t0.tv_sec) * 1000.0 +
           (now.tv_usec - t0.tv_usec) / 1000.0;
}
#define DBG(...) do { if (dbg) { fprintf(dbg, "[%7.2fms] ", elapsed_ms()); fprintf(dbg, __VA_ARGS__); } } while(0)

/* ── Socket helpers ────────────────────────────────────────────────────────── */

static int write_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int send_msg(int fd, const char *json) {
    char header[64];
    size_t body_len = strlen(json);
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %zu\r\n\r\n", body_len);
    if (write_all(fd, header, (size_t)hlen) < 0) return -1;
    if (write_all(fd, json, body_len) < 0) return -1;
    return 0;
}

static char *recv_msg(int fd) {
    char hbuf[256];
    int hpos = 0;
    while (hpos < (int)sizeof(hbuf) - 1) {
        ssize_t n = read(fd, hbuf + hpos, 1);
        if (n <= 0) return NULL;
        hpos++;
        if (hpos >= 4 &&
            hbuf[hpos-4] == '\r' && hbuf[hpos-3] == '\n' &&
            hbuf[hpos-2] == '\r' && hbuf[hpos-1] == '\n') {
            hbuf[hpos] = '\0'; break;
        }
    }
    char *cl = strstr(hbuf, "Content-Length:");
    if (!cl) return NULL;
    size_t body_len = (size_t)atoi(cl + 15);
    if (body_len == 0 || body_len > 4*1024*1024) return NULL;
    char *body = malloc(body_len + 1);
    if (!body) return NULL;
    size_t got = 0;
    while (got < body_len) {
        ssize_t n = read(fd, body + got, body_len - got);
        if (n <= 0) { free(body); return NULL; }
        got += (size_t)n;
    }
    body[body_len] = '\0';
    return body;
}

static int extract_str(const char *json, const char *key,
                       char *out, size_t outlen) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= outlen) return -1;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return 0;
}

static void json_escape_path(const char *src, char *dst, size_t dstlen) {
    size_t i = 0;
    while (*src && i + 3 < dstlen) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') { dst[i++] = '\\'; }
        dst[i++] = (char)c;
    }
    dst[i] = '\0';
}

/* ── Read editor from settings.json ────────────────────────────────────────── */

static int read_settings_editor(const char *home, char *out, size_t outlen) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.claude/settings.json", home);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';

    /* Find "env" object, then "CLAUDE_PAGER_EDITOR" within it */
    const char *env = strstr(buf, "\"env\"");
    if (!env) return -1;
    const char *brace = strchr(env + 4, '{');
    if (!brace) return -1;

    const char *needle = "\"CLAUDE_PAGER_EDITOR\"";
    const char *key = strstr(brace, needle);
    if (!key) return -1;

    /* Find the closing brace of env to make sure key is inside it */
    int depth = 1;
    const char *p = brace + 1;
    while (*p && depth > 0) {
        if (p == key) break;  /* found key before env closes — good */
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        p++;
    }
    if (depth == 0) return -1;  /* key was outside env object */

    /* Extract value */
    const char *colon = strchr(key + strlen(needle), ':');
    if (!colon) return -1;
    const char *quote1 = strchr(colon, '"');
    if (!quote1) return -1;
    quote1++;
    const char *quote2 = strchr(quote1, '"');
    if (!quote2 || (size_t)(quote2 - quote1) >= outlen) return -1;
    memcpy(out, quote1, (size_t)(quote2 - quote1));
    out[quote2 - quote1] = '\0';
    return 0;
}

/* ── Transcript finding (pure C, no process spawns) ────────────────────────── */

static int newest_jsonl(const char *dir, char *out, size_t outlen) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    time_t newest = 0;
    out[0] = '\0';
    while ((e = readdir(d)) != NULL) {
        size_t nlen = strlen(e->d_name);
        if (nlen < 6 || strcmp(e->d_name + nlen - 6, ".jsonl") != 0) continue;
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) < 0) continue;
        if (st.st_mtime > newest) {
            newest = st.st_mtime;
            strncpy(out, full, outlen - 1);
            out[outlen - 1] = '\0';
        }
    }
    closedir(d);
    return out[0] ? 0 : -1;
}

static void find_transcript(const char *home, char *out, size_t outlen) {
    out[0] = '\0';

    /* Strategy 1: tty-keyed file from SessionStart hook */
    char *tty = ttyname(STDIN_FILENO);
    if (tty) {
        const char *key = tty;
        if (strncmp(key, "/dev/", 5) == 0) key += 5;
        char path[256];
        snprintf(path, sizeof(path), "/tmp/claude-transcript-%s", key);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(out, (int)outlen, f)) {
                size_t len = strlen(out);
                if (len > 0 && out[len-1] == '\n') out[len-1] = '\0';
            }
            fclose(f);
            if (out[0] && access(out, R_OK) == 0) return;
            out[0] = '\0';
        }
    }

    /* Strategy 2: PWD-derived project directory */
    const char *pwd = getenv("PWD");
    if (pwd) {
        char project_key[1024];
        size_t ki = 0;
        for (const char *p = pwd; *p && ki + 1 < sizeof(project_key); p++)
            project_key[ki++] = (*p == '/') ? '-' : *p;
        project_key[ki] = '\0';
        char project_dir[2048];
        snprintf(project_dir, sizeof(project_dir),
                 "%s/.claude/projects/%s", home, project_key);
        if (newest_jsonl(project_dir, out, outlen) == 0) return;
    }

    /* Strategy 3: globally most recent */
    char projects_dir[1024];
    snprintf(projects_dir, sizeof(projects_dir), "%s/.claude/projects", home);
    DIR *pd = opendir(projects_dir);
    if (!pd) return;
    struct dirent *pe;
    time_t global_newest = 0;
    while ((pe = readdir(pd)) != NULL) {
        if (pe->d_name[0] == '.') continue;
        char subdir[2048];
        snprintf(subdir, sizeof(subdir), "%s/%s", projects_dir, pe->d_name);
        char candidate[2048];
        if (newest_jsonl(subdir, candidate, sizeof(candidate)) == 0) {
            struct stat st;
            if (stat(candidate, &st) == 0 && st.st_mtime > global_newest) {
                global_newest = st.st_mtime;
                strncpy(out, candidate, outlen - 1);
                out[outlen - 1] = '\0';
            }
        }
    }
    closedir(pd);
}

/* ── Pre-render: instant initial frame ─────────────────────────────────────── */

static void pre_render(int tty_fd) {
    struct winsize ws = {0};
    if (ioctl(tty_fd, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0) {
        ws.ws_col = 100; ws.ws_row = 24;
    }
    int cols = ws.ws_col < 120 ? ws.ws_col : 120;

    char buf[16384];
    int pos = 0;
    pos += snprintf(buf+pos, sizeof(buf)-(size_t)pos, "\033[2J\033[H");
    for (int i = 0; i < cols && pos+4 < (int)sizeof(buf); i++)
        pos += snprintf(buf+pos, sizeof(buf)-(size_t)pos, "\033[38;2;80;80;80m\xe2\x94\x80");
    pos += snprintf(buf+pos, sizeof(buf)-(size_t)pos, "\033[0m\n");
    for (int r = 0; r < (int)ws.ws_row - 4 && pos+2 < (int)sizeof(buf); r++)
        buf[pos++] = '\n';
    for (int i = 0; i < cols && pos+4 < (int)sizeof(buf); i++)
        pos += snprintf(buf+pos, sizeof(buf)-(size_t)pos, "\033[38;2;80;80;80m\xe2\x94\x80");
    pos += snprintf(buf+pos, sizeof(buf)-(size_t)pos, "\033[0m\n");
    pos += snprintf(buf+pos, sizeof(buf)-(size_t)pos,
        "\033[1;33m  Editor open \xe2\x80\x94 edit and close to send\033[0m");
    (void)write(tty_fd, buf, (size_t)pos);
}

/* ── Fork pager child ──────────────────────────────────────────────────────── */

static pid_t fork_pager(int watch_pid) {
    pid_t pid = fork();
    if (pid == 0) {
        int tty_fd = open("/dev/tty", O_RDWR);
        if (tty_fd >= 0) {
            pre_render(tty_fd);
            /* Transcript lookup happens here in the child, overlapping
             * with TurboDraft's window creation in the parent. */
            char transcript[2048] = "";
            const char *home = getenv("HOME");
            if (home) find_transcript(home, transcript, sizeof(transcript));
            run_pager(tty_fd, transcript, watch_pid, 200000);
            close(tty_fd);
        }
        _exit(0);
    }
    return pid;
}

/* ── TurboDraft fast path ──────────────────────────────────────────────────── */

static int turbodraft_path(const char *home, const char *file) {
    char sock_path[512];
    snprintf(sock_path, sizeof(sock_path),
             "%s/Library/Application Support/TurboDraft/turbodraft.sock", home);

    /* Fast bail: if the socket file doesn't exist, TurboDraft isn't installed */
    if (access(sock_path, F_OK) != 0) {
        DBG("turbodraft socket not found: %s\n", sock_path);
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    /* Retry connection — TurboDraft may be restarting after Cmd-Q.
     * LaunchAgent restart can take 3-4 seconds in practice.
     * Only retry on transient errors (ECONNREFUSED); bail immediately
     * on permanent errors (ENOENT = socket disappeared). */
    int connected = 0;
    for (int attempt = 0; attempt < 100; attempt++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            connected = 1;
            break;
        }
        if (errno == ENOENT) {
            DBG("turbodraft socket disappeared\n");
            close(fd);
            return -1;
        }
        if (attempt == 0) {
            DBG("turbodraft socket connect failed: %s (retrying)\n", strerror(errno));
            /* Show pager frame immediately so user sees something
             * while waiting for TurboDraft to restart. */
            int tty = open("/dev/tty", O_RDWR);
            if (tty >= 0) { pre_render(tty); close(tty); }
        }
        usleep(50000); /* 50ms */
        close(fd);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    }
    if (!connected) {
        DBG("turbodraft socket connect failed after retries\n");
        close(fd);
        return -1;
    }
    DBG("turbodraft socket connected\n");

    /* Send session.open */
    char escaped[2048];
    json_escape_path(file, escaped, sizeof(escaped));
    char open_msg[4096];
    snprintf(open_msg, sizeof(open_msg),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"turbodraft.session.open\","
             "\"params\":{\"path\":\"%s\"}}", escaped);

    if (send_msg(fd, open_msg) < 0) { close(fd); return -1; }
    DBG("session.open sent\n");

    /* Fork pager (runs in parallel with TurboDraft's ~120ms open) */
    pid_t pager_pid = fork_pager((int)getpid());
    DBG("pager forked pid=%d\n", (int)pager_pid);

    /* Read session.open response */
    char *resp = recv_msg(fd);
    if (!resp) { kill(pager_pid, SIGTERM); waitpid(pager_pid, NULL, 0); close(fd); return 1; }

    char session_id[256] = "";
    extract_str(resp, "sessionId", session_id, sizeof(session_id));
    DBG("sessionId=%s\n", session_id[0] ? session_id : "(missing)");
    free(resp);

    if (!session_id[0]) { kill(pager_pid, SIGTERM); waitpid(pager_pid, NULL, 0); close(fd); return 1; }

    /* Send session.wait — blocks until TurboDraft session ends */
    char wait_msg[512];
    snprintf(wait_msg, sizeof(wait_msg),
             "{\"jsonrpc\":\"2.0\",\"id\":2,"
             "\"method\":\"turbodraft.session.wait\","
             "\"params\":{\"sessionId\":\"%s\",\"timeoutMs\":86400000}}",
             session_id);

    if (send_msg(fd, wait_msg) < 0) {
        kill(pager_pid, SIGTERM); waitpid(pager_pid, NULL, 0); close(fd); return 0;
    }

    /* --- Close path timing (reset clock to measure from editor close) --- */
    char *resp3 = recv_msg(fd);
    gettimeofday(&t0, NULL);  /* Reset clock: t=0 is now "editor closed" */
    DBG("--- close path start (session.wait returned)\n");
    if (resp3) free(resp3);
    close(fd);

    kill(pager_pid, SIGTERM);
    DBG("pager SIGTERM sent\n");
    waitpid(pager_pid, NULL, 0);
    DBG("pager exited, returning to Claude Code\n");
    return 0;
}

/* ── Self-reference detection ──────────────────────────────────────────────── */

static int is_self(const char *cmd) {
    char tok[256];
    if (sscanf(cmd, "%255s", tok) != 1) return 0;
    const char *base = strrchr(tok, '/');
    base = base ? base + 1 : tok;
    return strstr(base, "claude-pager") != NULL;
}

/* ── Editor command validation ─────────────────────────────────────────────── */

static int editor_exists(const char *cmd) {
    char tok[256];
    if (sscanf(cmd, "%255s", tok) != 1) return 0;

    /* Absolute path — check directly */
    if (tok[0] == '/') return access(tok, X_OK) == 0;

    /* Bare name — search PATH */
    const char *path = getenv("PATH");
    if (!path) return 0;
    char pathbuf[8192];
    strncpy(pathbuf, path, sizeof(pathbuf) - 1);
    pathbuf[sizeof(pathbuf) - 1] = '\0';
    char *saveptr = NULL;
    for (char *dir = strtok_r(pathbuf, ":", &saveptr); dir;
         dir = strtok_r(NULL, ":", &saveptr)) {
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", dir, tok);
        if (access(full, X_OK) == 0) return 1;
    }
    return 0;
}

/* ── Terminal editor detection ─────────────────────────────────────────────── */

static const char *tui_editors[] = {
    "vi", "vim", "nvim", "lvim", "nvi",
    "vim.basic", "vim.tiny", "vim.nox", "vim.gtk", "vim.gtk3",
    "emacs", "nano", "micro",
    "helix", "hx", "kakoune", "kak", "joe", "ed", "ne",
    "mg", "jed", "tilde", "dte", "mcedit", "amp", NULL
};

static int is_terminal_editor(const char *editor) {
    /* Env override: CLAUDE_PAGER_EDITOR_TYPE=tui|gui */
    const char *override = getenv("CLAUDE_PAGER_EDITOR_TYPE");
    if (override) {
        if (strcmp(override, "tui") == 0) return 1;
        if (strcmp(override, "gui") == 0) return 0;
    }

    /* Extract basename of first token */
    char tok[256];
    if (sscanf(editor, "%255s", tok) != 1) return 0;
    const char *base = strrchr(tok, '/');
    base = base ? base + 1 : tok;

    for (int i = 0; tui_editors[i]; i++) {
        if (strcmp(base, tui_editors[i]) == 0) return 1;
    }
    return 0;
}

/* ── Terminal editor path (exec directly, no pager) ────────────────────────── */

static int terminal_editor_path(const char *editor, const char *file) {
    DBG("terminal editor, exec without pager\n");
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "exec %s \"$1\"", editor);
    execl("/bin/sh", "sh", "-c", cmd, "sh", file, (char *)NULL);
    _exit(127);
}

/* ── Generic editor path (GUI editor + pager, with TUI auto-detection) ───── */

static int generic_editor_path(const char *editor, const char *file) {
    /* Fork editor with stdin detached. TUI editors die immediately
     * without a terminal; GUI editors don't read stdin and keep running.
     * This auto-detects unknown TUI editors not in our list. */
    pid_t ed_pid = fork();
    if (ed_pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "exec %s \"$1\"", editor);
        execl("/bin/sh", "sh", "-c", cmd, "sh", file, (char *)NULL);
        _exit(127);
    }
    DBG("probe: editor forked pid=%d (stdin detached)\n", (int)ed_pid);

    /* Grace period: TUI editors without a tty exit within ~30ms.
     * GUI editors stay alive. Check every 10ms for 150ms. */
    int probe_status;
    int is_tui = 0;
    for (int i = 0; i < 15; i++) {
        usleep(10000);
        if (waitpid(ed_pid, &probe_status, WNOHANG) > 0) {
            DBG("probe: editor exited in %dms (status=%d) — TUI detected\n",
                (i + 1) * 10, probe_status);
            is_tui = 1;
            break;
        }
    }

    if (is_tui) {
        /* Re-launch with a real terminal */
        DBG("re-launching as TUI editor (exec with tty)\n");
        return terminal_editor_path(editor, file);
    }

    /* Editor still running after 150ms — GUI confirmed.
     * The probe child IS our editor process. Fork pager alongside. */
    DBG("probe: editor alive after 150ms — GUI confirmed\n");
    pid_t pager_pid = fork_pager((int)ed_pid);
    DBG("pager forked pid=%d\n", (int)pager_pid);

    /* Wait for editor to close */
    int status;
    waitpid(ed_pid, &status, 0);
    DBG("editor exited status=%d\n", status);

    /* Kill pager and wait for terminal restore */
    if (pager_pid > 0) {
        kill(pager_pid, SIGTERM);
        waitpid(pager_pid, NULL, 0);
    }
    return 0;
}

/* ── main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    dbg_open();
    if (argc < 2) {
        fprintf(stderr, "usage: claude-pager-open <file>\n");
        return 1;
    }
    const char *file = argv[1];
    const char *home = getenv("HOME");
    DBG("--- claude-pager-open pid=%d file=%s\n", (int)getpid(), file);

    /* Measure Claude Code's overhead: gap between temp file creation and us */
    if (dbg) {
        struct stat file_st;
        if (stat(file, &file_st) == 0) {
            double file_us = file_st.st_mtimespec.tv_sec * 1e6
                           + file_st.st_mtimespec.tv_nsec / 1e3;
            double start_us = t0.tv_sec * 1e6 + t0.tv_usec;
            DBG("claude-code exec overhead: %.2fms\n", (start_us - file_us) / 1e3);
        }
    }

    if (!home) {
#ifdef __APPLE__
        execlp("open", "open", "-W", "-t", file, (char *)NULL);
#endif
        _exit(1);
    }

    /* Recursion guard: if VISUAL/EDITOR points to our shim, we'd loop.
     * Detect this and open with system default instead. */
    if (getenv("_CLAUDE_PAGER_ACTIVE")) {
        DBG("recursion detected, opening with system default\n");
#ifdef __APPLE__
        execlp("open", "open", "-W", "-t", file, (char *)NULL);
#else
        execlp("xdg-open", "xdg-open", file, (char *)NULL);
#endif
        _exit(1);
    }
    setenv("_CLAUDE_PAGER_ACTIVE", "1", 1);

    /* Resolve editor: CLAUDE_PAGER_EDITOR (env or settings.json) → VISUAL → EDITOR */
    const char *editor = getenv("CLAUDE_PAGER_EDITOR");
    const char *source = "CLAUDE_PAGER_EDITOR";
    static char settings_editor[512];
    if (!editor || !editor[0]) {
        /* Claude Code doesn't export env section to editor process,
         * so read it directly from settings.json */
        if (read_settings_editor(home, settings_editor, sizeof(settings_editor)) == 0) {
            editor = settings_editor;
            source = "settings.json env.CLAUDE_PAGER_EDITOR";
        }
    }
    DBG("env CLAUDE_PAGER_EDITOR=%s\n", editor ? editor : "(null)");
    DBG("env VISUAL=%s\n", getenv("VISUAL") ? getenv("VISUAL") : "(null)");
    DBG("env EDITOR=%s\n", getenv("EDITOR") ? getenv("EDITOR") : "(null)");
    if (editor && (!editor[0] || is_self(editor))) {
        DBG("skipped CLAUDE_PAGER_EDITOR=%s\n", editor);
        editor = NULL;
    }
    if (!editor) {
        editor = getenv("VISUAL");
        source = "VISUAL";
        if (editor && (!editor[0] || is_self(editor))) {
            DBG("skipped VISUAL=%s\n", editor);
            editor = NULL;
        }
    }
    if (!editor) {
        editor = getenv("EDITOR");
        source = "EDITOR";
        if (editor && (!editor[0] || is_self(editor))) {
            DBG("skipped EDITOR=%s\n", editor);
            editor = NULL;
        }
    }

    /* Validate the resolved editor actually exists */
    if (editor && !editor_exists(editor)) {
        fprintf(stderr, "claude-pager: editor not found: %s (from %s)\n", editor, source);
        fprintf(stderr, "  Check the command exists and is in your PATH\n");
        fprintf(stderr, "  Fix CLAUDE_PAGER_EDITOR in ~/.claude/settings.json env section\n");
        DBG("editor not found: %s (from %s)\n", editor, source);
        editor = NULL;
    }

    /* If no editor configured/valid, try TurboDraft before falling back to system default */
    if (!editor) {
        int rc = turbodraft_path(home, file);
        if (rc >= 0) return rc;
        DBG("turbodraft unavailable, using system default\n");
        fprintf(stderr, "claude-pager: no editor configured — using system default\n");
        fprintf(stderr, "  Set CLAUDE_PAGER_EDITOR in ~/.claude/settings.json env section\n");
        editor = "open -W -t";
        source = "system default";
    }
    DBG("resolved editor=%s (from %s)\n", editor, source);

    if (is_terminal_editor(editor))
        return terminal_editor_path(editor, file);
    else
        return generic_editor_path(editor, file);
}

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

static pid_t fork_pager(const char *transcript, int watch_pid) {
    pid_t pid = fork();
    if (pid == 0) {
        int tty_fd = open("/dev/tty", O_RDWR);
        if (tty_fd >= 0) {
            pre_render(tty_fd);
            run_pager(tty_fd, transcript, watch_pid, 200000);
            close(tty_fd);
        }
        _exit(0);
    }
    return pid;
}

/* ── TurboDraft fast path ──────────────────────────────────────────────────── */

static int turbodraft_path(const char *home, const char *file,
                           const char *transcript) {
    char sock_path[512];
    snprintf(sock_path, sizeof(sock_path),
             "%s/Library/Application Support/TurboDraft/turbodraft.sock", home);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        DBG("turbodraft socket connect failed: %s\n", strerror(errno));
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
    signal(SIGCHLD, SIG_IGN);
    pid_t pager_pid = fork_pager(transcript, (int)getpid());
    DBG("pager forked pid=%d\n", (int)pager_pid);

    /* Read session.open response */
    char *resp = recv_msg(fd);
    if (!resp) { kill(pager_pid, SIGTERM); close(fd); return 1; }

    char session_id[256] = "";
    extract_str(resp, "sessionId", session_id, sizeof(session_id));
    DBG("sessionId=%s\n", session_id[0] ? session_id : "(missing)");
    free(resp);

    if (!session_id[0]) { kill(pager_pid, SIGTERM); close(fd); return 1; }

    /* Send session.wait — blocks until TurboDraft session ends */
    char wait_msg[512];
    snprintf(wait_msg, sizeof(wait_msg),
             "{\"jsonrpc\":\"2.0\",\"id\":2,"
             "\"method\":\"turbodraft.session.wait\","
             "\"params\":{\"sessionId\":\"%s\",\"timeoutMs\":86400000}}",
             session_id);

    if (send_msg(fd, wait_msg) < 0) {
        kill(pager_pid, SIGTERM); close(fd); return 0;
    }

    char *resp3 = recv_msg(fd);
    if (resp3) free(resp3);
    close(fd);

    kill(pager_pid, SIGTERM);
    return 0;
}

/* ── Generic editor path ───────────────────────────────────────────────────── */

static int generic_editor_path(const char *file, const char *transcript) {
    /* Resolve editor command */
    const char *editor = getenv("CLAUDE_PAGER_EDITOR");
    if (!editor || !editor[0]) editor = getenv("VISUAL");
    if (!editor || !editor[0]) editor = getenv("EDITOR");

    if (!editor || !editor[0]) {
        DBG("no editor found, using system default\n");
#ifdef __APPLE__
        execlp("open", "open", "-W", "-t", file, (char *)NULL);
#else
        execlp("xdg-open", "xdg-open", file, (char *)NULL);
#endif
        _exit(1);
    }

    DBG("editor=%s\n", editor);

    /* Fork editor as child */
    pid_t ed_pid = fork();
    if (ed_pid == 0) {
        /* Clear recursion guard so the editor doesn't hit it */
        unsetenv("_CLAUDE_PAGER_ACTIVE");
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "exec %s \"$1\"", editor);
        execl("/bin/sh", "sh", "-c", cmd, "sh", file, (char *)NULL);
        _exit(127);
    }
    DBG("editor forked pid=%d\n", (int)ed_pid);

    /* Fork pager — watches editor PID */
    pid_t pager_pid = fork_pager(transcript, (int)ed_pid);
    DBG("pager forked pid=%d\n", (int)pager_pid);

    /* Wait for editor to close */
    int status;
    waitpid(ed_pid, &status, 0);
    DBG("editor exited status=%d\n", status);

    /* Kill pager */
    if (pager_pid > 0) kill(pager_pid, SIGTERM);
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

    /* Pre-compute transcript path */
    char transcript[2048] = "";
    find_transcript(home, transcript, sizeof(transcript));
    DBG("transcript=%s\n", transcript[0] ? transcript : "(none)");

    /* Try TurboDraft fast path first */
    int rc = turbodraft_path(home, file, transcript);
    if (rc >= 0) return rc;

    /* TurboDraft unavailable — use generic editor path */
    DBG("turbodraft unavailable, using generic editor\n");
    return generic_editor_path(file, transcript);
}

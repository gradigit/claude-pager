/*
 * claude-pager-open — zero-overhead editor shim for Claude Code
 *
 * Talks directly to TurboDraft's Unix socket and launches the pager
 * without any shell intermediaries.
 *
 * Bypasses:
 *   - bash shim startup           (~25 ms)
 *   - turbodraft-editor osascript (~234 ms)
 *   - turbodraft-open binary       (~5 ms)
 *   - pager-setup.sh bash          (~25 ms)
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
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "pager.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* ── Helpers ───────────────────────────────────────────────────────────────── */

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

/* Find newest .jsonl in a directory */
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

    /* Strategy 1: tty-keyed file from SessionStart hook.
     * Our tty == Claude Code's tty (we're a direct child). */
    char *tty = ttyname(STDIN_FILENO);
    if (tty) {
        /* ttyname returns "/dev/ttys003", hook uses "ttys003" (from ps -o tty=) */
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
        for (const char *p = pwd; *p && ki + 1 < sizeof(project_key); p++) {
            project_key[ki++] = (*p == '/') ? '-' : *p;
        }
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

/* ── Pre-render: instant initial frame before Python starts ────────────────── */

static void pre_render(int tty_fd) {
    /* Get terminal size */
    struct winsize ws = {0};
    if (ioctl(tty_fd, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0) {
        ws.ws_col = 100;
        ws.ws_row = 24;
    }
    int cols = ws.ws_col < 120 ? ws.ws_col : 120;

    /* Build frame in a buffer for a single write(2) call */
    char buf[16384];
    int pos = 0;

    /* Clear screen + home */
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\033[2J\033[H");

    /* Top separator */
    for (int i = 0; i < cols && pos + 4 < (int)sizeof(buf); i++)
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\033[38;2;80;80;80m\xe2\x94\x80");
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\033[0m\n");

    /* Blank content area */
    for (int r = 0; r < (int)ws.ws_row - 4 && pos + 2 < (int)sizeof(buf); r++)
        buf[pos++] = '\n';

    /* Bottom separator */
    for (int i = 0; i < cols && pos + 4 < (int)sizeof(buf); i++)
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\033[38;2;80;80;80m\xe2\x94\x80");
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\033[0m\n");

    /* Status bar */
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
        "\033[1;33m  Editor open \xe2\x80\x94 edit and close to send\033[0m");

    /* Single write — atomic, appears instantly */
    (void)write(tty_fd, buf, (size_t)pos);
}

/* ── Fallback: exec the bash shim ──────────────────────────────────────────── */

static void get_self_dir(char *out, size_t outlen) {
    out[0] = '\0';
#ifdef __APPLE__
    char buf[1024];
    uint32_t buflen = sizeof(buf);
    if (_NSGetExecutablePath(buf, &buflen) == 0) {
        char resolved[1024];
        if (realpath(buf, resolved)) {
            char *slash = strrchr(resolved, '/');
            if (slash && (size_t)(slash - resolved) < outlen) {
                memcpy(out, resolved, (size_t)(slash - resolved));
                out[slash - resolved] = '\0';
                return;
            }
        }
    }
#endif
    strncpy(out, "/usr/local/bin", outlen - 1);
    out[outlen - 1] = '\0';
}

static void fallback(const char *file) {
    char self_dir[1024];
    get_self_dir(self_dir, sizeof(self_dir));
    char shim[2048];
    snprintf(shim, sizeof(shim), "%s/../shim/claude-pager-shim.sh", self_dir);
    execl("/bin/bash", "bash", shim, file, (char *)NULL);
#ifdef __APPLE__
    execlp("open", "open", "-W", file, (char *)NULL);
#endif
    _exit(1);
}

/* ── main ──────────────────────────────────────────────────────────────────── */

/* ── Debug logging (always writes to /tmp/claude-pager-open.log) ───────── */
#include <sys/time.h>
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

int main(int argc, char *argv[]) {
    dbg_open();
    if (argc < 2) {
        fprintf(stderr, "usage: claude-pager-open <file>\n");
        return 1;
    }
    const char *file = argv[1];
    const char *home = getenv("HOME");
    DBG("--- claude-pager-open pid=%d file=%s\n", (int)getpid(), file);
    if (!home) { DBG("no HOME, fallback\n"); fallback(file); }

    signal(SIGCHLD, SIG_IGN);

    /* ── Pre-compute transcript path (pure syscalls, ~0.1 ms) ──────────── */
    char transcript[2048] = "";
    find_transcript(home, transcript, sizeof(transcript));
    DBG("transcript=%s\n", transcript[0] ? transcript : "(none)");

    /* ── Connect to TurboDraft socket ──────────────────────────────────── */
    char sock_path[512];
    snprintf(sock_path, sizeof(sock_path),
             "%s/Library/Application Support/TurboDraft/turbodraft.sock", home);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) fallback(file);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        DBG("socket connect failed: %s\n", strerror(errno));
        close(fd);
        fallback(file);
    }
    DBG("socket connected\n");

    /* ── Send session.open ─────────────────────────────────────────────── */
    char escaped[2048];
    json_escape_path(file, escaped, sizeof(escaped));
    char open_msg[4096];
    snprintf(open_msg, sizeof(open_msg),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"turbodraft.session.open\","
             "\"params\":{\"path\":\"%s\"}}", escaped);

    if (send_msg(fd, open_msg) < 0) { DBG("send session.open failed\n"); close(fd); fallback(file); }
    DBG("session.open sent\n");

    /* ── Fork pager immediately ────────────────────────────────────────
     * Child runs the C pager directly (no exec, no Python).
     * Runs in parallel with TurboDraft's ~120 ms session.open.          */
    int parent_pid = (int)getpid();

    pid_t pager_pid = fork();
    DBG("fork returned %d\n", (int)pager_pid);
    if (pager_pid == 0) {
        close(fd);
        int tty_fd = open("/dev/tty", O_RDWR);
        if (tty_fd >= 0) {
            /* Pre-render an initial frame (~0.1ms) before full render */
            pre_render(tty_fd);
            DBG("child: run_pager transcript=%s parent=%d\n",
                transcript[0] ? transcript : "(none)", parent_pid);
            run_pager(tty_fd, transcript, parent_pid, 200000);
            close(tty_fd);
        }
        _exit(0);
    }

    /* ── Read session.open response ────────────────────────────────────── */
    char *resp = recv_msg(fd);
    if (!resp) { kill(pager_pid, SIGTERM); close(fd); return 1; }

    char session_id[256] = "";
    extract_str(resp, "sessionId", session_id, sizeof(session_id));
    DBG("session.open resp: sessionId=%s\n", session_id[0] ? session_id : "(missing)");
    DBG("raw resp: %s\n", resp);
    free(resp);

    if (!session_id[0]) {
        DBG("no sessionId, killing pager and exiting\n");
        kill(pager_pid, SIGTERM); close(fd); return 1;
    }

    /* ── Send session.wait ─────────────────────────────────────────────── */
    char wait_msg[512];
    snprintf(wait_msg, sizeof(wait_msg),
             "{\"jsonrpc\":\"2.0\",\"id\":2,"
             "\"method\":\"turbodraft.session.wait\","
             "\"params\":{\"sessionId\":\"%s\",\"timeoutMs\":86400000}}",
             session_id);

    if (send_msg(fd, wait_msg) < 0) {
        if (pager_pid > 0) kill(pager_pid, SIGTERM);
        close(fd);
        return 0;
    }

    /* Blocks until TurboDraft session ends */
    char *resp3 = recv_msg(fd);
    if (resp3) free(resp3);
    close(fd);

    if (pager_pid > 0) kill(pager_pid, SIGTERM);
    return 0;
}

/*
 * claude-pager-open — zero-overhead TurboDraft shim for Claude Code
 *
 * Talks directly to TurboDraft's Unix socket, bypassing:
 *   - bash shim startup           (~25 ms)
 *   - turbodraft-editor osascript (~234 ms)
 *   - turbodraft-open binary       (~5 ms)
 *
 * Usage: set Claude Code's editor to this binary.
 * Falls back to shim/claude-pager-shim.sh if the socket is unavailable.
 *
 * Build: cd bin && make
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <errno.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* ── Fallback: exec the bash shim ──────────────────────────────────────────── */

static void fallback(const char *file) {
    char self_dir[1024] = "";

#ifdef __APPLE__
    char buf[1024];
    uint32_t buflen = sizeof(buf);
    if (_NSGetExecutablePath(buf, &buflen) == 0) {
        char resolved[1024];
        if (realpath(buf, resolved)) {
            char *slash = strrchr(resolved, '/');
            if (slash) {
                size_t dlen = (size_t)(slash - resolved);
                if (dlen < sizeof(self_dir)) {
                    memcpy(self_dir, resolved, dlen);
                    self_dir[dlen] = '\0';
                }
            }
        }
    }
#endif

    /* argv[0] fallback */
    if (!self_dir[0]) {
        strncpy(self_dir, "/usr/local/bin", sizeof(self_dir) - 1);
    }

    char shim[2048];
    snprintf(shim, sizeof(shim), "%s/../shim/claude-pager-shim.sh", self_dir);

    execl("/bin/bash", "bash", shim, file, (char *)NULL);

    /* Last resort: platform open */
#ifdef __APPLE__
    execlp("open", "open", "-W", file, (char *)NULL);
#else
    execlp("xdg-open", "xdg-open", file, (char *)NULL);
#endif
    _exit(1);
}

/* ── Write all bytes to fd ─────────────────────────────────────────────────── */

static int write_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* ── Send a Content-Length framed JSON message ─────────────────────────────── */

static int send_msg(int fd, const char *json) {
    char header[64];
    size_t body_len = strlen(json);
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %zu\r\n\r\n", body_len);
    if (write_all(fd, header, (size_t)hlen) < 0) return -1;
    if (write_all(fd, json, body_len) < 0) return -1;
    return 0;
}

/* ── Read a Content-Length framed response; returns malloc'd body ──────────── */

static char *recv_msg(int fd) {
    /* Read the header byte-by-byte until we see \r\n\r\n */
    char hbuf[256];
    int hpos = 0;

    while (hpos < (int)sizeof(hbuf) - 1) {
        ssize_t n = read(fd, hbuf + hpos, 1);
        if (n <= 0) return NULL;
        hpos++;
        if (hpos >= 4 &&
            hbuf[hpos - 4] == '\r' && hbuf[hpos - 3] == '\n' &&
            hbuf[hpos - 2] == '\r' && hbuf[hpos - 1] == '\n') {
            hbuf[hpos] = '\0';
            break;
        }
    }

    /* Parse Content-Length */
    char *cl = strstr(hbuf, "Content-Length:");
    if (!cl) return NULL;
    size_t body_len = (size_t)atoi(cl + 15);
    if (body_len == 0 || body_len > 4 * 1024 * 1024) return NULL;

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

/* ── Extract a JSON string value for a key ─────────────────────────────────── */

static int extract_str(const char *json, const char *key,
                       char *out, size_t outlen) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= outlen) return -1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* ── JSON-escape a path into dst ───────────────────────────────────────────── */

static void json_escape_path(const char *src, char *dst, size_t dstlen) {
    size_t i = 0;
    while (*src && i + 3 < dstlen) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') {
            dst[i++] = '\\';
            dst[i++] = (char)c;
        } else {
            dst[i++] = (char)c;
        }
    }
    dst[i] = '\0';
}

/* ── Locate pager-setup.sh relative to this binary ────────────────────────── */

static void get_pager_setup(char *out, size_t outlen) {
    char self_dir[1024] = "";

#ifdef __APPLE__
    char buf[1024];
    uint32_t buflen = sizeof(buf);
    if (_NSGetExecutablePath(buf, &buflen) == 0) {
        char resolved[1024];
        if (realpath(buf, resolved)) {
            char *slash = strrchr(resolved, '/');
            if (slash) {
                size_t dlen = (size_t)(slash - resolved);
                if (dlen < sizeof(self_dir)) {
                    memcpy(self_dir, resolved, dlen);
                    self_dir[dlen] = '\0';
                }
            }
        }
    }
#endif

    if (!self_dir[0]) {
        strncpy(self_dir, ".", sizeof(self_dir) - 1);
    }

    snprintf(out, outlen, "%s/../shim/pager-setup.sh", self_dir);
}

/* ── main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: claude-pager-open <file>\n");
        return 1;
    }
    const char *file = argv[1];

    /* Ignore SIGCHLD so the pager child is reaped automatically */
    signal(SIGCHLD, SIG_IGN);

    /* ── Connect to TurboDraft socket ──────────────────────────────────────── */
    const char *home = getenv("HOME");
    if (!home) fallback(file); /* unreachable after exec */

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
        close(fd);
        fallback(file);
    }

    /* ── turbodraft.session.open ─────────────────────────────────────────────
     * turbodraft-open skips hello and goes straight to session.open.
     * hello is optional; skipping it saves one round-trip.               */
    char escaped[2048];
    json_escape_path(file, escaped, sizeof(escaped));

    char open_msg[4096];
    snprintf(open_msg, sizeof(open_msg),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"turbodraft.session.open\","
             "\"params\":{\"path\":\"%s\"}}", escaped);

    if (send_msg(fd, open_msg) < 0) { close(fd); fallback(file); }

    char *resp2 = recv_msg(fd);
    if (!resp2) { close(fd); fallback(file); }

    char session_id[256] = "";
    extract_str(resp2, "sessionId", session_id, sizeof(session_id));
    free(resp2);

    if (!session_id[0]) { close(fd); fallback(file); }

    /* ── Fork pager setup ──────────────────────────────────────────────────── */
    char pager_setup[2048];
    get_pager_setup(pager_setup, sizeof(pager_setup));

    char self_pid_str[32];
    snprintf(self_pid_str, sizeof(self_pid_str), "%d", (int)getpid());

    char parent_pid_str[32];
    snprintf(parent_pid_str, sizeof(parent_pid_str), "%d", (int)getppid());

    pid_t pager_pid = fork();
    if (pager_pid == 0) {
        /* Child: run pager setup then exec the pager */
        execl("/bin/bash", "bash", pager_setup,
              file, self_pid_str, parent_pid_str, (char *)NULL);
        _exit(1);
    }

    /* ── turbodraft.session.wait ───────────────────────────────────────────── */
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

    /* Blocks here until TurboDraft reports the session is done */
    char *resp3 = recv_msg(fd);
    if (resp3) free(resp3);

    close(fd);

    /* Kill the pager now that the editor session is over */
    if (pager_pid > 0) kill(pager_pid, SIGTERM);

    return 0;
}

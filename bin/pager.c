/*
 * pager.c — Pure C scrollable pager for Claude Code transcripts.
 * Zero dependencies beyond libc + POSIX.  Renders in ~3ms.
 */
#define _DARWIN_C_SOURCE
#include "pager.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>

/* ── ANSI ──────────────────────────────────────────────────────────────── */

#define RS      "\033[0m"
#define BO      "\033[1m"
#define DI      "\033[2m"
#define C_HUM   "\033[38;2;255;165;0m"
#define C_AST   "\033[38;2;204;204;204m"
#define C_TOL   "\033[38;2;160;100;255m"
#define C_RES   "\033[38;2;110;110;110m"
#define C_ERR   "\033[38;2;220;80;80m"
#define C_CBG   "\033[48;2;35;35;35m"
#define C_CFG   "\033[38;2;200;230;200m"
#define C_CIN   "\033[38;2;97;175;239m"
#define C_SEP   "\033[38;2;80;80;80m"
#define C_HDM   "\033[38;2;100;100;100m"
#define C_BAN   "\033[1;33m"
#define C_DFG   "\033[38;2;100;220;100m"
#define C_DFR   "\033[38;2;220;80;80m"
#define C_DFC   "\033[38;2;100;150;255m"
#define C_BRG   "\033[38;2;100;220;100m"
#define C_BRY   "\033[38;2;255;165;0m"
#define C_BRR   "\033[38;2;255;80;80m"
#define C_CONN  "\033[38;2;60;60;80m"
#define C_URL   "\033[38;2;255;165;0m"
#define UL_ON   "\033[4m"
#define UL_OFF  "\033[24m"

#define HL  "\xe2\x94\x80"
#define VL  "\xe2\x94\x82"
#define BUL "\xe2\x80\xa2"
#define CHV "\xe2\x9d\xaf"
#define REC "\xe2\x8f\xba"
#define ELL "\xe2\x80\xa6"
#define EMD "\xe2\x80\x94"
#define UAR "\xe2\x86\x91"
#define FBLK "\xe2\x96\x88"
#define EBLK "\xe2\x96\x91"
#define DOT "\xc2\xb7"

#define MOUSE_ON  "\033[?1007h"
#define MOUSE_OFF "\033[?1007l"

/* ── Globals ───────────────────────────────────────────────────────────── */

static int g_cols = 100, g_rows = 24, g_crows = 21;
static int g_fd = -1;
static struct termios g_old;
static volatile sig_atomic_t g_resize = 0, g_quit = 0;
static FILE *g_dbg = NULL;
static long long g_log_t0_us = 0;
static int g_bench_mode = 0;

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

static int env_enabled(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    return strcmp(v, "1") == 0 ||
           strcasecmp(v, "true") == 0 ||
           strcasecmp(v, "yes") == 0 ||
           strcasecmp(v, "on") == 0;
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
        g_cols = ws.ws_col < 120 ? (int)ws.ws_col : 120;
        g_rows = (int)ws.ws_row;
    }
    g_crows = g_rows - 3;
}

static void on_winch(int s) { (void)s; g_resize = 1; }
static void on_term(int s)  { (void)s; g_quit = 1; }

/* ── Output buffer ─────────────────────────────────────────────────────── */

static char g_ob[256*1024];
static int g_ol;

static void ob(const char *s) {
    int n = (int)strlen(s);
    if (g_ol + n < (int)sizeof(g_ob)) { memcpy(g_ob + g_ol, s, n); g_ol += n; }
}

static void obf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_ob + g_ol, sizeof(g_ob) - g_ol, fmt, ap);
    va_end(ap);
    if (n > 0 && g_ol + n < (int)sizeof(g_ob)) g_ol += n;
}

static void ob_flush(void) {
    if (g_ol > 0 && g_fd >= 0) { write(g_fd, g_ob, g_ol); g_ol = 0; }
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

/* ── Dynamic line array ────────────────────────────────────────────────── */

typedef struct { char **d; int n, cap; } Lines;
static void L_init(Lines *l) { memset(l, 0, sizeof(*l)); }
static void L_push(Lines *l, const char *s) {
    if (l->n >= l->cap) { l->cap = l->cap ? l->cap*2 : 128; l->d = realloc(l->d, sizeof(char*)*l->cap); }
    l->d[l->n++] = strdup(s);
}
static void L_free(Lines *l) {
    for (int i=0; i<l->n; i++) free(l->d[i]);
    free(l->d); L_init(l);
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

static int is_pathch(char c) {
    return (c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
           (c>='0'&&c<='9') || c=='_' || c=='.' || c=='-' || c=='/';
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
            while (*p) {
                if (*p=='\a') { LF_CH(*p++); break; }
                if (p[0]=='\033' && p[1]=='\\') { LF_CH(*p++); LF_CH(*p++); break; }
                LF_CH(*p++);
            }
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
        /* Detect URL */
        if (strncmp(p,"http://",7)==0 || strncmp(p,"https://",8)==0) {
            const char *start = p;
            while (*p && is_urlch(*p)) p++;
            while (p>start && (p[-1]=='.'||p[-1]==','||p[-1]==';'||p[-1]==':')) p--;
            int ulen = (int)(p - start);
            if (ulen > 10 && o + ulen + 200 < dstmax) {
                char label[256];
                shorten_url(label, sizeof(label), start, ulen);
                LF_S("\033]8;;");
                for (int i=0; i<ulen; i++) LF_CH(start[i]);
                LF_CH('\a');
                LF_S(C_URL UL_ON);
                LF_S(label);
                LF_S(UL_OFF);
                LF_S("\033]8;;\a");
            } else {
                for (int i=0; i<ulen; i++) LF_CH(start[i]);
            }
            continue;
        }
        /* Detect file path: /segment/segment... or ~/segment... */
        if ((p[0]=='/' && is_pathch(p[1])) ||
            (p[0]=='~' && p[1]=='/' && is_pathch(p[2]))) {
            int tilde = (p[0] == '~');
            const char *start = p;
            const char *sp = tilde ? p + 2 : p + 1;
            while (*sp && is_pathch(*sp)) sp++;
            int has_slash = 0;
            const char *check_from = tilde ? start + 2 : start + 1;
            for (const char *c=check_from; c<sp; c++) { if (*c=='/') { has_slash=1; break; } }
            /* ~/foo is a valid path even without a second slash */
            if ((tilde && (sp-start) >= 3) || (has_slash && (sp-start) >= 3)) {
                p = sp;
                while (p>start+1 && (p[-1]=='.'||p[-1]==',')) p--;
                int fplen = (int)(p - start);
                if (o + fplen + 200 < dstmax) {
                    char label[256];
                    shorten_path(label, sizeof(label), start, fplen);
                    LF_S("\033]8;;file://");
                    if (tilde) {
                        /* Expand ~ to $HOME in the URI */
                        const char *hm = getenv("HOME");
                        if (hm) { LF_S(hm); }
                        /* skip the ~ , emit /rest/of/path */
                        for (int i=1; i<fplen; i++) LF_CH(start[i]);
                    } else {
                        for (int i=0; i<fplen; i++) LF_CH(start[i]);
                    }
                    LF_CH('\a');
                    LF_S(UL_ON);
                    LF_S(label);
                    LF_S(UL_OFF);
                    LF_S("\033]8;;\a");
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
    char lb[32768];
    linkify(lb, sizeof(lb), s);
    L_pushw(l, lb);
}

/* ── Transcript items ──────────────────────────────────────────────────── */

enum { IT_HUM, IT_AST, IT_TU, IT_TR };
typedef struct { int type; char *text; char *label; int is_err; } Item;
typedef struct { Item *d; int n, cap; } Items;

static void I_push(Items *it, int type, char *text, char *label, int err) {
    if (it->n >= it->cap) { it->cap = it->cap ? it->cap*2 : 64; it->d = realloc(it->d, sizeof(Item)*it->cap); }
    Item *e = &it->d[it->n++];
    e->type = type; e->text = text; e->label = label; e->is_err = err;
}

static void I_free(Items *it) {
    for (int i=0; i<it->n; i++) { free(it->d[i].text); free(it->d[i].label); }
    free(it->d); memset(it, 0, sizeof(*it));
}

/* Strip ANSI from text (preserves OSC-8 hyperlinks) */
static char *sanitize(const char *s) {
    int len = (int)strlen(s);
    char *d = malloc(len + 1);
    int j = 0;
    for (int i = 0; i < len; ) {
        if (s[i]=='\033') {
            i++;
            if (i<len && s[i]=='[') { i++; while (i<len && !isalpha((unsigned char)s[i])) i++; if (i<len) i++; }
            else if (i+2<len && s[i]==']' && s[i+1]=='8' && s[i+2]==';') {
                d[j++] = '\033';
                while (i<len) {
                    if (s[i]=='\a') { d[j++]=s[i++]; break; }
                    if (i+1<len && s[i]=='\033' && s[i+1]=='\\') { d[j++]=s[i++]; d[j++]=s[i++]; break; }
                    d[j++] = s[i++];
                }
            }
            else if (i<len && s[i]==']') {
                while (i<len) {
                    if (s[i]=='\a') { i++; break; }
                    if (i+1<len && s[i]=='\033' && s[i+1]=='\\') { i+=2; break; }
                    i++;
                }
            }
            else if (i<len) i++;
        } else { d[j++] = s[i++]; }
    }
    d[j] = '\0'; return d;
}

static int is_systag(const char *s) {
    return strstr(s,"<local-command-caveat") || strstr(s,"<command-name") ||
           strstr(s,"<system-reminder") || strstr(s,"<user-prompt-submit-hook");
}

static const char *lbl_keys[] = {
    "command","file_path","path","pattern","query","url","content","description",NULL
};

/* Extract trimmed, sanitized string from JSON value at p */
static char *extract_text(const char *p, int bufmax) {
    if (!p || *p != '"') return NULL;
    char *buf = malloc(bufmax);
    int n = jstr(p, buf, bufmax);
    if (n <= 0) { free(buf); return NULL; }
    /* Trim */
    char *s = buf;
    while (*s==' '||*s=='\n') s++;
    char *e = s + strlen(s);
    while (e>s && (e[-1]==' '||e[-1]=='\n')) e--;
    *e = '\0';
    if (!*s) { free(buf); return NULL; }
    char *clean = sanitize(s);
    free(buf);
    return clean;
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
                        char *t = extract_text(jfind(el, "text"), (int)len+1);
                        if (t) I_push(items, IT_AST, t, NULL, 0);
                    } else if (jstreq(bt, "tool_use")) {
                        char nm[128] = "?";
                        const char *nv = jfind(el, "name");
                        if (nv) jstr(nv, nm, sizeof(nm));
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
                        if (strlen(lbl)>72) { lbl[69]='.'; lbl[70]='.'; lbl[71]='.'; lbl[72]='\0'; }
                        I_push(items, IT_TU, sanitize(nm), sanitize(lbl), 0);
                    }
                }
                el = jskip(el); el = jws(el); if (*el==',') el=jws(el+1);
            }
        } else if (jstreq(tv, "user")) {
            if (ct && *jws(ct)=='"') {
                char *t = extract_text(ct, (int)len+1);
                if (t && !is_systag(t)) I_push(items, IT_HUM, t, NULL, 0);
                else free(t);
            } else if (ct && *jws(ct)=='[') {
                const char *el = jws(ct);
                if (*el=='[') el=jws(el+1);
                while (el && *el && *el!=']') {
                    if (*el=='{') {
                        const char *bt = jfind(el, "type");
                        if (jstreq(bt, "tool_result")) {
                            const char *rc = jfind(el, "content");
                            char *text = NULL;
                            if (rc && *jws(rc)=='"') {
                                text = extract_text(rc, (int)len+1);
                            } else if (rc && *jws(rc)=='[') {
                                int bmax = (int)len+1;
                                char *buf = malloc(bmax); int bi=0;
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
                                if (*s) { text=sanitize(s); }
                                free(buf);
                            }
                            if (text) {
                                int ie=0;
                                const char *ev = jfind(el, "is_error");
                                if (ev && (*ev=='t'||*ev=='T')) ie=1;
                                I_push(items, IT_TR, text, NULL, ie);
                            }
                        }
                    }
                    el=jskip(el); el=jws(el); if(*el==',') el=jws(el+1);
                }
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

static void render_md(Lines *L, const char *text) {
    int in_code = 0;
    char lb[8192], fb[16384];
    const char *p = text;

    while (*p) {
        const char *eol = strchr(p, '\n');
        int ll = eol ? (int)(eol-p) : (int)strlen(p);
        if (ll >= (int)sizeof(lb)) ll = (int)sizeof(lb)-1;
        memcpy(lb, p, ll); lb[ll]='\0';
        p = eol ? eol+1 : p+ll;

        if (lb[0]=='`' && lb[1]=='`' && lb[2]=='`') { in_code = !in_code; continue; }

        if (in_code) {
            int vl = (int)strlen(lb), pad = g_cols-4-vl;
            if (pad<0) pad=0;
            snprintf(fb, sizeof(fb), C_CBG C_CFG "  %s%*s" RS, lb, pad, "");
            L_pushw_link(L, fb); continue;
        }

        /* Headers */
        if (lb[0]=='#') {
            int lv=0; while(lb[lv]=='#') lv++;
            if (lb[lv]==' ') {
                char *ht = lb+lv+1;
                if (lv==1) {
                    L_push(L, "");
                    snprintf(fb, sizeof(fb), BO C_AST "%s" RS, ht);
                    L_push(L, fb);
                    int ul = (int)strlen(ht)+2; if(ul>g_cols) ul=g_cols;
                    char sep[512]; int si=0;
                    si += snprintf(sep+si, sizeof(sep)-si, "%s", C_SEP);
                    for (int i=0; i<ul && si+4<(int)sizeof(sep); i++) si+=snprintf(sep+si,sizeof(sep)-si,HL);
                    snprintf(sep+si, sizeof(sep)-si, "%s", RS);
                    L_push(L, sep);
                } else if (lv==2) {
                    L_push(L, "");
                    snprintf(fb, sizeof(fb), BO C_AST "%s" RS, ht); L_push(L, fb);
                } else {
                    snprintf(fb, sizeof(fb), BO DI C_AST "%s" RS, ht); L_push(L, fb);
                }
                continue;
            }
        }

        /* Bullets */
        int ind = 0; while(lb[ind]==' ') ind++;
        if ((lb[ind]=='-'||lb[ind]=='*') && lb[ind+1]==' ') {
            fmt_inline(fb, sizeof(fb), lb+ind+2);
            char ob2[16384];
            snprintf(ob2, sizeof(ob2), "%*s" C_AST BUL " %s" RS, ind, "", fb);
            L_pushw_link(L, ob2); continue;
        }

        /* Numbered lists */
        if (isdigit((unsigned char)lb[0])) {
            const char *d = lb; while(isdigit((unsigned char)*d)) d++;
            if (d[0]=='.' && d[1]==' ') {
                int nl = (int)(d-lb); char num[16];
                memcpy(num,lb,nl); num[nl]='\0';
                fmt_inline(fb, sizeof(fb), d+2);
                char ob2[16384];
                snprintf(ob2, sizeof(ob2), C_AST "%s. %s" RS, num, fb);
                L_pushw_link(L, ob2); continue;
            }
        }

        /* Default text */
        if (lb[0]) { fmt_inline(fb, sizeof(fb), lb); L_pushw_link(L, fb); }
        else L_push(L, "");
    }
}

/* ── Item renderer ─────────────────────────────────────────────────────── */

#define MX_HUM 20
#define MX_RES 6

static int is_diff(const char *t) {
    int hp=0, hm=0;
    const char *p = t;
    while (*p) {
        const char *ln = p;
        if (ln[0]=='+' && ln[1]!='+') hp=1;
        if (ln[0]=='-' && ln[1]!='-') hm=1;
        while (*p && *p!='\n') p++;
        if (*p=='\n') p++;
    }
    return hp && hm;
}

static int count_lines(const char *t) {
    int n=1; for (const char *p=t; *p; p++) if (*p=='\n') n++; return n;
}

static void render_items(Lines *L, Items *items) {
    int prev_tu = 0;
    char b[16384];

    for (int i = 0; i < items->n; i++) {
        Item *it = &items->d[i];

        switch (it->type) {
        case IT_HUM: {
            snprintf(b, sizeof(b), "\n" C_HUM BO CHV " you" RS);
            L_push(L, b);
            int nl = count_lines(it->text);
            int show = nl > MX_HUM ? MX_HUM : nl;
            const char *p = it->text;
            for (int ln=0; ln<show; ln++) {
                const char *eol = strchr(p, '\n');
                int ll = eol ? (int)(eol-p) : (int)strlen(p);
                if (ll>(int)sizeof(b)-30) ll=(int)sizeof(b)-31;
                snprintf(b, sizeof(b), DI "%.*s" RS, ll, p);
                L_pushw_link(L, b);
                p = eol ? eol+1 : p+ll;
            }
            if (nl > MX_HUM) {
                snprintf(b, sizeof(b), C_HDM "  " ELL " (%d more lines)" RS, nl-MX_HUM);
                L_push(L, b);
            }
            break;
        }
        case IT_AST:
            L_push(L, "");
            render_md(L, it->text);
            break;

        case IT_TU:
            if (it->label && it->label[0])
                snprintf(b, sizeof(b), C_TOL REC " " BO "%s" RS C_TOL "(%s)" RS, it->text, it->label);
            else
                snprintf(b, sizeof(b), C_TOL REC " " BO "%s" RS, it->text);
            L_pushw_link(L, b);
            break;

        case IT_TR: {
            const char *col = it->is_err ? C_ERR : C_RES;
            const char *conn = prev_tu ? "  " C_CONN VL RS " " : "  ";
            int df = is_diff(it->text);
            int nl = count_lines(it->text);
            int show = nl > MX_RES ? MX_RES : nl;
            const char *p = it->text;
            for (int ln=0; ln<show; ln++) {
                const char *eol = strchr(p, '\n');
                int ll = eol ? (int)(eol-p) : (int)strlen(p);
                if (ll>(int)sizeof(b)-120) ll=(int)sizeof(b)-121;
                const char *lc = col;
                if (df) {
                    if (p[0]=='+'&&p[1]!='+') lc=C_DFG;
                    else if (p[0]=='-'&&p[1]!='-') lc=C_DFR;
                    else if (p[0]=='@'&&p[1]=='@') lc=C_DFC;
                }
                snprintf(b, sizeof(b), "%s%s%.*s" RS, conn, lc, ll, p);
                L_pushw_link(L, b);
                if (!eol) break;
                p = eol+1;
            }
            if (nl > MX_RES) {
                snprintf(b, sizeof(b), "%s" C_HDM ELL " (%d more lines)" RS, conn, nl-MX_RES);
                L_push(L, b);
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
    if (tok <= 0) return;

    int bw = 12, filled = (int)(pct/100.0*bw+0.5);
    if (filled>bw) filled=bw;
    const char *bc = pct<60 ? C_BRG : pct<85 ? C_BRY : C_BRR;

    char ct[64];
    snprintf(ct, sizeof(ct), "%.0f%%  %.0fk/%dk", pct, tok/1000.0, cl/1000);

    int bvl = 38, cvl = bw+1+(int)strlen(ct), svl=5;
    int pad = g_cols - bvl - svl - cvl;
    if (pad<0) pad=0;

    obf("%*s" DI "  " DOT "  " RS, pad, "");
    ob(bc);
    for (int i=0; i<filled; i++) ob(FBLK);
    for (int i=filled; i<bw; i++) ob(EBLK);
    ob(RS DI " "); ob(ct); ob(RS);
}

static void draw(Lines *L, int off, int tok, double pct, int cl, int first) {
    g_ol = 0;

    if (first) ob("\033[?25l\033[2J\033[H");
    else       ob("\033[?25l\033[H");

    draw_sep(); ob("\033[K\n");
    int row = 2;

    if (off > 0) {
        obf(C_HDM "  " UAR " %d lines above  (scroll to view)" RS "\033[K\n", off);
        row++;
    }

    int avail = g_crows - (off>0 ? 1 : 0);
    int end = off + avail;
    if (end > L->n) end = L->n;

    for (int i = off; i < end; i++) { ob(L->d[i]); ob("\033[K\n"); row++; }

    while (row < g_rows - 1) { ob("\033[K\n"); row++; }

    obf("\033[%d;1H", g_rows-1); draw_sep(); ob("\033[K");
    obf("\033[%d;1H", g_rows);   draw_status(tok, pct, cl); ob("\033[K");

    ob_flush();
}

/* ── Terminal ──────────────────────────────────────────────────────────── */

static void term_raw(int fd) {
    struct termios t;
    tcgetattr(fd, &g_old);
    t = g_old;
    t.c_lflag &= ~(unsigned long)(ICANON | ECHO);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
}

static void term_restore(void) {
    if (g_fd < 0) return;
    write(g_fd, MOUSE_OFF "\033[?25h", strlen(MOUSE_OFF)+6);
    tcsetattr(g_fd, TCSANOW, &g_old);
}

/* ── Input ─────────────────────────────────────────────────────────────── */

#define INP_NONE 0
#define INP_HOME 10000
#define INP_END  10001
#define INP_QUIT 10002

static int poll_input(int fd) {
    fd_set fds; struct timeval tv = {0,0};
    FD_ZERO(&fds); FD_SET(fd, &fds);
    if (select(fd+1, &fds, NULL, NULL, &tv) <= 0) return INP_NONE;

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) return INP_NONE;

    int delta = 0;
    for (ssize_t i=0; i<n; i++) {
        if (buf[i]=='\033' && i+2<n && buf[i+1]=='[') {
            char c = buf[i+2];
            if (c=='A') { delta--; i+=2; }
            else if (c=='B') { delta++; i+=2; }
            else if (c=='5' && i+3<n && buf[i+3]=='~') { return -(g_crows-1); }
            else if (c=='6' && i+3<n && buf[i+3]=='~') { return g_crows-1; }
            else if (c=='H') { return INP_HOME; }
            else if (c=='F') { return INP_END; }
            else { i+=2; }
        } else if (buf[i]=='q'||buf[i]=='Q') { return INP_QUIT; }
    }
    return delta;
}

/* ── Main loop ─────────────────────────────────────────────────────────── */

void run_pager(int tty_fd, const char *transcript, int editor_pid, int ctx_limit) {
    g_fd = tty_fd;
    if (ctx_limit <= 0) ctx_limit = 200000;
    g_bench_mode = env_enabled("CLAUDE_PAGER_BENCH");
    dbg_open();
    PDBG("run start transcript=%s editor_pid=%d ctx_limit=%d\n",
         (transcript && transcript[0]) ? transcript : "(none)",
         editor_pid, ctx_limit);
    if (g_bench_mode) PDBG("bench probes enabled\n");

    signal(SIGTERM, on_term);
    signal(SIGWINCH, on_winch);

    geo_update();
    term_raw(tty_fd);
    write(tty_fd, MOUSE_ON, strlen(MOUSE_ON));

    Lines L; L_init(&L);
    int off = 0, uscroll = 0;
    int tok = 0; double pct = 0;
    double last_mt = 0;
    int first = 1;
    int first_draw_logged = 0;
    int load_seq = 0;

    while (!g_quit) {
        if (editor_pid > 0 && kill(editor_pid, 0) != 0) break;

        if (g_resize) {
            g_resize = 0; geo_update();
            first = 1; last_mt = 0;
        }

        int cc = 0;
        if (transcript && transcript[0]) {
            struct stat st;
            if (stat(transcript, &st)==0 && (double)st.st_mtime != last_mt) {
                last_mt = (double)st.st_mtime;
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
                L_push(&L, C_HDM "  " HL HL HL " end of transcript " HL HL HL RS);
                L_push(&L, ""); L_push(&L, "");
                PDBG("markdown render end load=%d duration=%.2fms lines=%d\n",
                     load_seq, (double)(t_render1 - t_render0) / 1000.0, L.n);
                I_free(&items);
                if (!uscroll) { int b = L.n-(g_crows-1); off = b>0 ? b : 0; }
            }
        } else if (first) {
            cc = 1;
            L_push(&L, C_HDM "(transcript not found)" RS);
        }

        int inp = poll_input(tty_fd);
        int sc = 0;

        if (inp == INP_QUIT) break;
        if (inp == INP_HOME) { off=0; uscroll=1; sc=1; }
        else if (inp == INP_END) { int b=L.n-(g_crows-1); off=b>0?b:0; uscroll=0; sc=1; }
        else if (inp != INP_NONE) {
            off += inp;
            if (off<0) off=0;
            int mx = L.n>0 ? L.n-1 : 0;
            if (off>mx) off=mx;
            uscroll = 1; sc = 1;
        }

        if (cc || sc || first) {
            draw(&L, off, tok, pct, ctx_limit, first);
            if (!first_draw_logged && first) {
                PDBG("first draw done off=%d lines=%d\n", off, L.n);
                bench_probe_terminal_ready(tty_fd, "first_draw");
                first_draw_logged = 1;
            }
            first = 0;
        }

        usleep(sc ? 16000 : 50000);
    }

    PDBG("run end\n");
    term_restore();
    L_free(&L);
}

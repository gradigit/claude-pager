/*
 * pager_cli.c — Standalone CLI for the C pager.
 * Usage: claude-pager-c <transcript.jsonl> [editor_pid] [--ctx-limit N]
 *
 * Drop-in replacement for the Python claude-pager CLI.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pager.h"

int main(int argc, char *argv[]) {
    const char *transcript = "";
    int editor_pid = 0;
    int ctx_limit = 200000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ctx-limit") == 0 && i + 1 < argc) {
            ctx_limit = atoi(argv[++i]);
        } else if (!transcript[0]) {
            transcript = argv[i];
        } else if (editor_pid == 0) {
            editor_pid = atoi(argv[i]);
        }
    }

    int tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) { perror("open /dev/tty"); return 1; }

    run_pager(tty_fd, transcript, editor_pid, ctx_limit);
    close(tty_fd);
    return 0;
}

# TODO

## Editor-agnostic C binary

Redesign `bin/claude-pager-open.c` to work with any editor, not just TurboDraft:

1. Read `CLAUDE_PAGER_EDITOR` / `$VISUAL` / `$EDITOR` for the editor
2. TUI editor detection (vim, nvim, nano, helix, emacs, etc.) → `exec` directly, no pager
3. GUI editor → fork to background + fork pager-setup.sh + waitpid + kill pager
4. TurboDraft fast path (optional): if editor looks like turbodraft and socket exists,
   use socket protocol instead of fork+exec (skips osascript overhead)

This makes `claude-pager-open` the single entry point for all users.
The bash shim (`claude-pager-shim.sh`) becomes redundant once this is done.

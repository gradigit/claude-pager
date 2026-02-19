#!/usr/bin/env bash
# claude-pager-shim: launches a GUI editor and renders the conversation
# transcript in the terminal while the editor is open.
#
# In Claude Code settings.json, set:
#   { "editor": "/path/to/claude-pager-shim.sh" }
#
# Specify your GUI editor via env (Claude Code passes env from settings.json):
#   { "env": { "CLAUDE_PAGER_EDITOR": "turbodraft-editor" } }
#
# Falls back to $VISUAL / $EDITOR if CLAUDE_PAGER_EDITOR is unset.
# If no GUI editor is found, opens the file normally (no pager).
set -euo pipefail

# ── Resolve the file path ─────────────────────────────────────────────────────
FILE="${1:-}"

# ── Resolve the GUI editor — fast path first, no process spawns ──────────────

# 1. Env var (zero cost)
REAL_EDITOR="${CLAUDE_PAGER_EDITOR:-}"

# 2. Cache-backed settings.json lookup — only sed on first run or after changes
if [[ -z "$REAL_EDITOR" ]] && [[ -f "$HOME/.claude/settings.json" ]]; then
    _CACHE="${TMPDIR:-/tmp}/.claude-pager-editor"
    if [[ -s "$_CACHE" ]] && [[ ! "$HOME/.claude/settings.json" -nt "$_CACHE" ]]; then
        IFS= read -r REAL_EDITOR < "$_CACHE"   # shell builtin — zero process spawns
    else
        REAL_EDITOR=$(sed -n \
            's/.*"CLAUDE_PAGER_EDITOR"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
            "$HOME/.claude/settings.json" 2>/dev/null | head -1 || true)
        [[ -n "$REAL_EDITOR" ]] && printf '%s\n' "$REAL_EDITOR" > "$_CACHE"
    fi
fi

# 3. $VISUAL / $EDITOR fallback — only reached if CLAUDE_PAGER_EDITOR is unset.
#    Resolve real script path here (lazy — skipped on hot path).
if [[ -z "$REAL_EDITOR" ]]; then
    _link="${BASH_SOURCE[0]}"
    while [[ -L "$_link" ]]; do
        _dir="$(cd "$(dirname "$_link")" && pwd)"
        _link="$(readlink "$_link")"
        [[ "$_link" != /* ]] && _link="$_dir/$_link"
    done
    SELF="$_link"

    for _var in "${VISUAL:-}" "${EDITOR:-}"; do
        [[ -z "$_var" ]] && continue
        _resolved="$(command -v "$_var" 2>/dev/null || echo "$_var")"
        _resolved_real="$(readlink "$_resolved" 2>/dev/null || echo "$_resolved")"
        [[ "$_resolved_real" == "$SELF" || "$_resolved" == "$SELF" ]] && continue
        REAL_EDITOR="$_var"
        break
    done
fi

# No GUI editor found — open file normally, no pager
if [[ -z "$REAL_EDITOR" ]]; then
    if [[ "$(uname)" == "Darwin" ]]; then
        open -W "$FILE"
    else
        xdg-open "$FILE"
    fi
    exit 0
fi

# ── Launch editor immediately — everything above is shell builtins on hot path ─
PARENT_PID=$PPID
$REAL_EDITOR "$FILE" &
EDITOR_PID=$!

# ── Resolve repo path for pager (runs after editor is already launched) ────────
_link="${BASH_SOURCE[0]}"
while [[ -L "$_link" ]]; do
    _dir="$(cd "$(dirname "$_link")" && pwd)"
    _link="$(readlink "$_link")"
    [[ "$_link" != /* ]] && _link="$_dir/$_link"
done
SCRIPT_DIR="$(cd "$(dirname "$_link")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# ── Resolve the pager command ─────────────────────────────────────────────────
# Prefer C pager (fast, zero-dependency) over Python
if command -v claude-pager-c &>/dev/null; then
    PAGER_CMD="claude-pager-c"
elif command -v claude-pager &>/dev/null; then
    PAGER_CMD="claude-pager"
else
    # Running from source — add src/ to Python path
    if [[ -d "$REPO_DIR/src/claude_pager" ]]; then
        export PYTHONPATH="${REPO_DIR}/src${PYTHONPATH:+:$PYTHONPATH}"
    fi
    PAGER_CMD="python3 -m claude_pager"
fi

# ── Find transcript path (fast — a few ps calls) ─────────────────────────────
TRANSCRIPT=""

# Strategy 1: tty-keyed file written by SessionStart hook
pid=$PARENT_PID
for _ in 1 2 3 4 5 6; do
    comm=$(ps -p "$pid" -o comm= 2>/dev/null | tr -d ' ' || true)
    if [[ "$comm" == "claude" || "$comm" == "node" ]]; then
        tty_key=$(ps -p "$pid" -o tty= 2>/dev/null | tr -d ' ' || true)
        if [[ -n "$tty_key" && "$tty_key" != "??" ]]; then
            TRANSCRIPT=$(cat "/tmp/claude-transcript-${tty_key}" 2>/dev/null || true)
        fi
        break
    fi
    ppid=$(ps -p "$pid" -o ppid= 2>/dev/null | tr -d ' ' || true)
    [[ -z "$ppid" || "$ppid" -le 1 ]] && break
    pid=$ppid
done

# Strategy 2: derive from $PWD — use ls -t, no python3
if [[ -z "$TRANSCRIPT" || ! -f "$TRANSCRIPT" ]]; then
    PROJECT_KEY=$(printf '%s' "$PWD" | tr '/' '-')
    PROJECT_DIR="$HOME/.claude/projects/$PROJECT_KEY"
    if [[ -d "$PROJECT_DIR" ]]; then
        TRANSCRIPT=$(ls -t "$PROJECT_DIR"/*.jsonl 2>/dev/null | head -1 || true)
    fi
fi

# Strategy 3: globally most recent (last resort) — use ls -t, no python3
if [[ -z "$TRANSCRIPT" || ! -f "$TRANSCRIPT" ]]; then
    TRANSCRIPT=$(ls -t "$HOME"/.claude/projects/*/*.jsonl 2>/dev/null | head -1 || true)
fi

# ── Start pager ──────────────────────────────────────────────────────────────
LOG_FILE="${CLAUDE_PAGER_LOG:-}"
if [[ -n "$LOG_FILE" ]]; then
    $PAGER_CMD "${TRANSCRIPT:-}" "$EDITOR_PID" >/dev/tty 2>>"$LOG_FILE" &
else
    $PAGER_CMD "${TRANSCRIPT:-}" "$EDITOR_PID" >/dev/tty 2>/dev/null &
fi
RENDER_PID=$!

# ── Wait for editor ──────────────────────────────────────────────────────────
wait "$EDITOR_PID" || true
kill "$RENDER_PID" 2>/dev/null || true
# No terminal restore needed — Claude Code sends \033[?1049l itself
# after the editor exits, restoring the main screen and triggering Ink redraw.

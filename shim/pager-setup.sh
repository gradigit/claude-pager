#!/usr/bin/env bash
# pager-setup.sh — finds transcript and runs the pager.
# Called by claude-pager-open (C binary) after the editor request is sent.
#
# Args: $1=file  $2=editor_pid  $3=parent_pid
set -euo pipefail

FILE="${1:-}"
EDITOR_PID="${2:-}"
PARENT_PID="${3:-$PPID}"

# ── Resolve REPO_DIR ──────────────────────────────────────────────────────────
_link="${BASH_SOURCE[0]}"
while [[ -L "$_link" ]]; do
    _dir="$(cd "$(dirname "$_link")" && pwd)"
    _link="$(readlink "$_link")"
    [[ "$_link" != /* ]] && _link="$_dir/$_link"
done
SCRIPT_DIR="$(cd "$(dirname "$_link")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# ── Resolve the pager command ─────────────────────────────────────────────────
if command -v claude-pager &>/dev/null; then
    PAGER_CMD="claude-pager"
else
    if [[ -d "$REPO_DIR/src/claude_pager" ]]; then
        export PYTHONPATH="${REPO_DIR}/src${PYTHONPATH:+:$PYTHONPATH}"
    fi
    PAGER_CMD="python3 -m claude_pager"
fi

# ── Find transcript path ──────────────────────────────────────────────────────
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

# Strategy 2: derive from $PWD
if [[ -z "$TRANSCRIPT" || ! -f "$TRANSCRIPT" ]]; then
    PROJECT_KEY=$(printf '%s' "$PWD" | tr '/' '-')
    PROJECT_DIR="$HOME/.claude/projects/$PROJECT_KEY"
    if [[ -d "$PROJECT_DIR" ]]; then
        TRANSCRIPT=$(ls -t "$PROJECT_DIR"/*.jsonl 2>/dev/null | head -1 || true)
    fi
fi

# Strategy 3: globally most recent (last resort)
if [[ -z "$TRANSCRIPT" || ! -f "$TRANSCRIPT" ]]; then
    TRANSCRIPT=$(ls -t "$HOME"/.claude/projects/*/*.jsonl 2>/dev/null | head -1 || true)
fi

# ── Run the pager (exec replaces this shell with the pager process) ───────────
LOG_FILE="${CLAUDE_PAGER_LOG:-}"
if [[ -n "$LOG_FILE" ]]; then
    exec $PAGER_CMD "${TRANSCRIPT:-}" "$EDITOR_PID" >/dev/tty 2>>"$LOG_FILE"
else
    exec $PAGER_CMD "${TRANSCRIPT:-}" "$EDITOR_PID" >/dev/tty 2>/dev/null
fi

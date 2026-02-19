#!/usr/bin/env bash
# SessionStart hook: saves transcript_path keyed by Claude's terminal tty.
# This lets the editor shim find the exact transcript for any session,
# even when multiple Claude sessions run from the same directory.
#
# Install: add to Claude Code settings.json under hooks.SessionStart
set -euo pipefail

input=$(cat)
transcript=$(printf '%s' "$input" | jq -r '.transcript_path // empty' 2>/dev/null || true)
[[ -z "$transcript" ]] && exit 0

# Walk up the process tree to find the Claude process and get its tty
pid=$PPID
tty_key=""
for _ in 1 2 3 4 5 6; do
    comm=$(ps -p "$pid" -o comm= 2>/dev/null | tr -d ' ' || true)
    if [[ "$comm" == "claude" || "$comm" == "node" ]]; then
        tty_key=$(ps -p "$pid" -o tty= 2>/dev/null | tr -d ' ' || true)
        break
    fi
    ppid=$(ps -p "$pid" -o ppid= 2>/dev/null | tr -d ' ' || true)
    [[ -z "$ppid" || "$ppid" -le 1 ]] && break
    pid=$ppid
done

[[ -z "$tty_key" || "$tty_key" == "??" ]] && exit 0

printf '%s\n' "$transcript" > "/tmp/claude-transcript-${tty_key}"

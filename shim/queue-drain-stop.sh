#!/usr/bin/env bash
# Stop hook: drains the next queued prompt for the current Claude session.
# Install under hooks.Stop. The pager writes session-scoped queue files to
# ~/.claude/queues/<session-key>.queue; this hook pops the first item and asks
# Claude to continue with it.
set -euo pipefail

input=$(cat)
session_id=$(printf '%s' "$input" | jq -r '.session_id // empty' 2>/dev/null || true)
[[ -z "$session_id" ]] && { printf '{"ok":true}\n'; exit 0; }

queue_key=$(printf '%s' "$session_id" | sed 's/[^[:alnum:]._.-]/_/g')
[[ -z "$queue_key" ]] && queue_key="default"

queue_dir="${HOME}/.claude/queues"
queue_file="${queue_dir}/${queue_key}.queue"
lock_dir="${queue_file}.lockdir"

[[ -f "$queue_file" ]] || { printf '{"ok":true}\n'; exit 0; }

pop_file=$(mktemp "${TMPDIR:-/tmp}/claude-pager-queue-pop.XXXXXX")
tmp_file="${queue_file}.tmp"
cleanup() {
  rm -f "$pop_file" "$tmp_file"
  rmdir "$lock_dir" 2>/dev/null || true
}
trap cleanup EXIT

while ! mkdir "$lock_dir" 2>/dev/null; do
  sleep 0.05
done

head -n 1 "$queue_file" > "$pop_file" 2>/dev/null || true
tail -n +2 "$queue_file" > "$tmp_file" 2>/dev/null || true
if [[ -s "$tmp_file" ]]; then
  mv "$tmp_file" "$queue_file"
else
  rm -f "$queue_file" "$tmp_file"
fi
rmdir "$lock_dir" 2>/dev/null || true

next_line=$(cat "$pop_file" 2>/dev/null || true)
[[ -n "$next_line" ]] || { printf '{"ok":true}\n'; exit 0; }

if printf '%s\n' "$next_line" | jq -e . >/dev/null 2>&1; then
  next_prompt=$(printf '%s\n' "$next_line" | jq -r '.prompt // empty')
else
  next_prompt="$next_line"
fi

[[ -n "$next_prompt" ]] || { printf '{"ok":true}\n'; exit 0; }

jq -n --arg prompt "$next_prompt" '{decision:"block",reason:$prompt}'

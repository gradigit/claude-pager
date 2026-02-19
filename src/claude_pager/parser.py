"""Single-pass JSONL transcript parser with ANSI sanitization."""
from __future__ import annotations

import json
import re

# ── ANSI sanitization ─────────────────────────────────────────────────────────

_ESC_RE = re.compile(
    r"\033(?:"
    r"\[[^a-zA-Z]*[a-zA-Z]"  # CSI sequences  (\033[...m, etc.)
    r"|\][^\a]*\a"            # OSC sequences  (\033]8;...;\a)
    r"|[^\[\]]"               # other ESC+char (\033c, etc.)
    r")"
)

_SYSTEM_RE = re.compile(
    r"<(local-command-caveat|command-name|command-message|"
    r"command-args|local-command-stdout|system-reminder|"
    r"user-prompt-submit-hook)\b"
)


def _sanitize(text: str) -> str:
    """Strip ANSI escape sequences from user-provided transcript text."""
    return _ESC_RE.sub("", text)


# ── Transcript item types ─────────────────────────────────────────────────────
# Items are plain tuples:
#   ('human',       text)
#   ('assistant',   text)
#   ('tool_use',    name, label)
#   ('tool_result', text, is_error)

_TOOL_LABEL_PRIORITY = [
    "command", "file_path", "path", "pattern",
    "query", "url", "content", "description",
]


def parse_transcript(
    path: str,
    ctx_limit: int = 200_000,
) -> tuple[list[tuple[str, ...]], int | None, float | None]:
    """Parse a Claude Code JSONL transcript in a single pass.

    Returns ``(items, total_tokens, pct)`` where *pct* is the percentage
    of *ctx_limit* consumed, or ``None`` if no usage data was found.
    """
    items: list[tuple[str, ...]] = []
    last_usage: dict[str, int] | None = None

    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            raw = raw.strip()
            if not raw:
                continue
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError:
                continue

            mtype: str | None = obj.get("type")
            msg: dict[str, object] = obj.get("message", {})  # type: ignore[assignment]
            content = msg.get("content", [])

            if mtype == "assistant":
                usage = msg.get("usage")
                if isinstance(usage, dict):
                    last_usage = usage  # type: ignore[assignment]
                if isinstance(content, list):
                    for block in content:
                        if not isinstance(block, dict):
                            continue
                        btype = block.get("type", "")
                        if btype == "text":
                            t = _sanitize(str(block.get("text", "")).strip())
                            if t:
                                items.append(("assistant", t))
                        elif btype == "tool_use":
                            name = str(block.get("name", "?"))
                            inp = block.get("input", {})
                            label = ""
                            if isinstance(inp, dict):
                                for key in _TOOL_LABEL_PRIORITY:
                                    if key in inp:
                                        label = str(inp[key])
                                        break
                                if not label and inp:
                                    label = str(next(iter(inp.values())))
                            if len(label) > 72:
                                label = label[:69] + "\u2026"
                            items.append(("tool_use", name, _sanitize(label)))

            elif mtype == "user":
                if isinstance(content, str):
                    text = _sanitize(content.strip())
                    if text and not _SYSTEM_RE.search(text):
                        items.append(("human", text))
                elif isinstance(content, list):
                    for block in content:
                        if not isinstance(block, dict):
                            continue
                        if block.get("type") == "tool_result":
                            rc = block.get("content", "")
                            if isinstance(rc, list):
                                text = "\n".join(
                                    str(b.get("text", ""))
                                    for b in rc
                                    if isinstance(b, dict) and b.get("type") == "text"
                                )
                            else:
                                text = str(rc)
                            text = _sanitize(text.strip())
                            if text:
                                items.append((
                                    "tool_result",
                                    text,
                                    bool(block.get("is_error", False)),
                                ))

    total, pct = _compute_usage(last_usage, ctx_limit)
    return items, total, pct


def _compute_usage(
    usage: dict[str, int] | None,
    ctx_limit: int,
) -> tuple[int | None, float | None]:
    if not usage:
        return None, None
    total = (
        usage.get("input_tokens", 0)
        + usage.get("cache_creation_input_tokens", 0)
        + usage.get("cache_read_input_tokens", 0)
    )
    return total, total / ctx_limit * 100

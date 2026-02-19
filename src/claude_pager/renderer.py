"""Markdown rendering, item rendering, diff coloring, and status bar."""
from __future__ import annotations

import re

from .terminal import (
    ASST_TEXT, B, BANNER, CODE_BG, CODE_FG, CODE_INLINE, DIM,
    HEADER_DIM, HUMAN_ICON, I, R, SEP_COLOR, TOOL_NAME, TOOL_RES,
    _URL_RE, geo, osc8_file, osc8_url,
)

# ── Diff detection / coloring ─────────────────────────────────────────────────

_DIFF_HUNK_RE = re.compile(r"^@@")

DIFF_GREEN = "\033[38;2;100;220;100m"
DIFF_RED = "\033[38;2;220;80;80m"
DIFF_CYAN = "\033[38;2;100;150;255m"


def _is_diff(text: str) -> bool:
    """Heuristic: text looks like a unified diff."""
    lines = text.split("\n")
    has_plus = any(l.startswith("+") and not l.startswith("+++") for l in lines)
    has_minus = any(l.startswith("-") and not l.startswith("---") for l in lines)
    return has_plus and has_minus


def colorize_diff(text: str) -> str:
    """Color +/- diff lines green/red.  Pass-through if not a diff."""
    if not _is_diff(text):
        return text
    out: list[str] = []
    for line in text.split("\n"):
        if line.startswith("+") and not line.startswith("+++"):
            out.append(f"{DIFF_GREEN}{line}{R}")
        elif line.startswith("-") and not line.startswith("---"):
            out.append(f"{DIFF_RED}{line}{R}")
        elif _DIFF_HUNK_RE.match(line):
            out.append(f"{DIFF_CYAN}{line}{R}")
        else:
            out.append(line)
    return "\n".join(out)


# ── Syntax highlighting (optional Pygments) ───────────────────────────────────

def _highlight_code(code: str, lang: str) -> str:
    """Attempt Pygments highlighting; fall back to plain CODE_FG color."""
    try:
        from pygments import highlight  # noqa: PLC0415
        from pygments.formatters import Terminal256Formatter  # noqa: PLC0415
        from pygments.lexers import TextLexer, get_lexer_by_name  # noqa: PLC0415

        try:
            lexer = get_lexer_by_name(lang, stripall=False) if lang else TextLexer()
        except Exception:
            lexer = TextLexer()
        return highlight(code, lexer, Terminal256Formatter(style="monokai")).rstrip("\n")
    except ImportError:
        return f"{CODE_FG}{code}{R}"


# ── Inline markdown ───────────────────────────────────────────────────────────

def inline_md(text: str) -> str:
    """Apply bold, italic, inline code, and URL linking."""
    text = re.sub(r"\*\*(.+?)\*\*", B + r"\1" + R + ASST_TEXT, text)
    text = re.sub(
        r"(?<!\*)\*(?!\*)(.+?)(?<!\*)\*(?!\*)", I + r"\1" + R + ASST_TEXT, text
    )

    def code_replace(m: re.Match[str]) -> str:
        content = m.group(1)
        if content.startswith("/") and len(content) > 3:
            return CODE_INLINE + osc8_file(content) + R + ASST_TEXT
        if content.startswith("http"):
            return CODE_INLINE + osc8_url(content) + R + ASST_TEXT
        return CODE_INLINE + content + R + ASST_TEXT

    text = re.sub(r"`([^`\n]+)`", code_replace, text)
    text = _URL_RE.sub(lambda m: osc8_url(m.group(1)), text)
    return text


# ── Markdown block renderer ──────────────────────────────────────────────────

def render_md(text: str) -> str:
    """Render a block of markdown text to ANSI-colored output."""
    out: list[str] = []
    in_code = False
    code_lines: list[str] = []
    code_lang = ""
    cols = geo.cols

    for line in text.split("\n"):
        if line.startswith("```"):
            if in_code:
                joined = "\n".join(code_lines)
                highlighted = _highlight_code(joined, code_lang)
                for cl in highlighted.split("\n"):
                    out.append(f"{CODE_BG}{CODE_FG}  {cl:<{cols - 4}}{R}")
                code_lines = []
                code_lang = ""
                in_code = False
            else:
                code_lang = line[3:].strip()
                in_code = True
            continue

        if in_code:
            code_lines.append(line)
            continue

        hm = re.match(r"^(#{1,6})\s+(.*)", line)
        if hm:
            level, content = len(hm.group(1)), hm.group(2)
            if level == 1:
                out.append(f"\n{B}{ASST_TEXT}{content}{R}")
                rule = "\u2500" * min(len(content) + 2, cols)
                out.append(f"{SEP_COLOR}{rule}{R}")
            elif level == 2:
                out.append(f"\n{B}{ASST_TEXT}{content}{R}")
            else:
                out.append(f"{B}{DIM}{ASST_TEXT}{content}{R}")
            continue

        bm = re.match(r"^(\s*)[-*]\s+(.*)", line)
        if bm:
            out.append(f"{bm.group(1)}{ASST_TEXT}\u2022 {inline_md(bm.group(2))}{R}")
            continue

        nm = re.match(r"^(\s*)(\d+)\.\s+(.*)", line)
        if nm:
            out.append(
                f"{nm.group(1)}{ASST_TEXT}{nm.group(2)}. {inline_md(nm.group(3))}{R}"
            )
            continue

        out.append(f"{ASST_TEXT}{inline_md(line)}{R}")

    # Unclosed code block — render what we have
    if in_code:
        for cl in code_lines:
            out.append(f"{CODE_BG}{CODE_FG}  {cl}{R}")

    return "\n".join(out)


# ── Item renderer ─────────────────────────────────────────────────────────────

MAX_HUMAN_LINES = 20
MAX_RESULT_LINES = 6

# Vertical connector for tool results indented under tool_use
_CONNECTOR = "\033[38;2;60;60;80m\u2502\033[0m "


def render_items(items: list[tuple[str, ...]]) -> str:
    """Render parsed transcript items to an ANSI string."""
    out: list[str] = []
    prev_kind = ""

    for item in items:
        kind = item[0]

        if kind == "human":
            text = item[1]
            out.append(f"\n{HUMAN_ICON}{B}\u276f you{R}")
            lines = text.split("\n")
            if len(lines) > MAX_HUMAN_LINES:
                out.append(f'{DIM}{chr(10).join(lines[:MAX_HUMAN_LINES])}')
                out.append(
                    f"{HEADER_DIM}  \u2026 ({len(lines) - MAX_HUMAN_LINES} more lines){R}"
                )
            else:
                out.append(f"{DIM}{text}{R}")

        elif kind == "assistant":
            text = item[1]
            out.append("")
            out.append(render_md(text))

        elif kind == "tool_use":
            name, label = item[1], item[2]
            if label.startswith("/"):
                linked = osc8_file(label)
            elif label.startswith("http"):
                linked = osc8_url(label)
            else:
                linked = label
            if linked:
                out.append(f"{TOOL_NAME}\u23fa {B}{name}{R}{TOOL_NAME}({linked}){R}")
            else:
                out.append(f"{TOOL_NAME}\u23fa {B}{name}{R}")

        elif kind == "tool_result":
            text, is_error = item[1], item[2]
            color = "\033[38;2;220;80;80m" if is_error else TOOL_RES

            # Indent under preceding tool_use with a vertical bar connector
            prefix = f"  {_CONNECTOR}" if prev_kind == "tool_use" else "  "

            # Apply diff coloring if the result looks like a diff
            text = colorize_diff(text)

            lines = text.split("\n")
            if len(lines) > MAX_RESULT_LINES:
                shown = "\n".join(prefix + l for l in lines[:MAX_RESULT_LINES])
                out.append(f"{color}{shown}")
                out.append(
                    f"{HEADER_DIM}{prefix}\u2026 ({len(lines) - MAX_RESULT_LINES} more lines){R}"
                )
            else:
                out.append(
                    f"{color}{prefix}{text.replace(chr(10), chr(10) + prefix)}{R}"
                )

        prev_kind = kind

    return "\n".join(out)


# ── Status bar ────────────────────────────────────────────────────────────────

def render_status_bar(
    total: int | None,
    pct: float | None,
    ctx_limit: int = 200_000,
) -> str:
    """Single compact line pinned to bottom: banner left, context bar right."""
    cols = geo.cols
    banner_text = "  Editor open \u2014 edit and close to send"

    if pct is not None and total is not None:
        bar_width = 12
        filled = round(pct / 100 * bar_width)
        bar = "\u2588" * filled + "\u2591" * (bar_width - filled)
        if pct < 60:
            bar_color = "\033[38;2;100;220;100m"
        elif pct < 85:
            bar_color = "\033[38;2;255;165;0m"
        else:
            bar_color = "\033[38;2;255;80;80m"
        limit_k = ctx_limit // 1000
        ctx_text = f"{pct:.0f}%  {total / 1000:.0f}k/{limit_k}k"
        ctx_vis = f"{bar} {ctx_text}"
        ctx_ansi = f"{bar_color}{bar}{R}{DIM} {ctx_text}{R}"
        sep_vis = "  \u00b7  "
        pad = max(0, cols - len(banner_text) - len(sep_vis) - len(ctx_vis))
        return f"{BANNER}{banner_text}{R}{' ' * pad}{DIM}{sep_vis}{R}{DIM}{ctx_ansi}"
    else:
        return f"{BANNER}{banner_text}{R}"

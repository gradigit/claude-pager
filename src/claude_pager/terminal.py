"""Terminal geometry, ANSI constants, and OSC-8 hyperlink helpers."""
from __future__ import annotations

import re
import shutil
from types import SimpleNamespace

# ── Mutable terminal geometry ─────────────────────────────────────────────────
# Updated on SIGWINCH.

geo = SimpleNamespace(cols=0, rows=0, content_rows=0)


def reinit_geometry() -> None:
    """Re-read terminal size and update the global geo namespace."""
    sz = shutil.get_terminal_size((100, 24))
    geo.cols = min(sz.columns, 120)
    geo.rows = sz.lines
    geo.content_rows = geo.rows - 3  # top_sep(1) + bottom_sep(1) + status(1)


reinit_geometry()  # initial read at import time


def sep() -> str:
    """Horizontal separator spanning the full terminal width."""
    return SEP_COLOR + "\u2500" * geo.cols + R


# ── ANSI helpers ──────────────────────────────────────────────────────────────

R = "\033[0m"
B = "\033[1m"
DIM = "\033[2m"
I = "\033[3m"
UL = "\033[4m"
UL_OFF = "\033[24m"

HUMAN_ICON = "\033[38;2;255;165;0m"
ASST_TEXT = "\033[38;2;204;204;204m"
TOOL_NAME = "\033[38;2;160;100;255m"
TOOL_RES = "\033[38;2;110;110;110m"
CODE_INLINE = "\033[38;2;97;175;239m"
CODE_BG = "\033[48;2;35;35;35m"
CODE_FG = "\033[38;2;200;230;200m"
SEP_COLOR = "\033[38;2;80;80;80m"
HEADER_DIM = "\033[38;2;100;100;100m"
BANNER = "\033[1;33m"

# Alternate scroll mode: scroll wheel → arrow key sequences in alt screen.
# Unlike \033[?1000h mouse tracking, this does NOT intercept clicks,
# so OSC-8 links remain Cmd+clickable.
MOUSE_ON = "\033[?1007h"
MOUSE_OFF = "\033[?1007l"


# ── OSC-8 hyperlinks ─────────────────────────────────────────────────────────
# URL color: orange to match Claude Code CLI
URL_COLOR = "\033[38;2;255;165;0m"

def _shorten_url(url: str, max_len: int = 60) -> str:
    """Shorten a URL for display, stripping protocol and truncating."""
    display = re.sub(r"^https?://", "", url)
    if len(display) <= max_len:
        return display
    # domain/start…end
    slash = display.find("/")
    if slash < 0:
        return display[: max_len - 1] + "\u2026"
    domain = display[: slash + 1]
    path = display[slash + 1 :]
    avail = max_len - len(domain) - 1  # 1 for …
    if avail < 8:
        return display[: max_len - 1] + "\u2026"
    tail = min(avail // 3, 20)
    head = avail - tail
    return domain + path[:head] + "\u2026" + path[-tail:]


def _shorten_path(path: str, max_len: int = 50) -> str:
    """Shorten a file path for display, keeping filename and parent."""
    if len(path) <= max_len:
        return path
    parts = path.rsplit("/", 2)
    if len(parts) >= 3:
        short = "\u2026/" + parts[-2] + "/" + parts[-1]
        if len(short) <= max_len:
            return short
    if len(parts) >= 2:
        short = "\u2026/" + parts[-1]
        if len(short) <= max_len:
            return short
    return "\u2026/" + parts[-1][: max_len - 2]


def osc8_file(path: str, label: str | None = None, shorten: bool = True) -> str:
    """Wrap *path* in an OSC-8 file:// hyperlink."""
    from urllib.parse import quote as _urlquote

    encoded = _urlquote(path, safe="/:@")
    if label is None and shorten:
        label = _shorten_path(path)
    return (
        f"\033]8;;file://{encoded}\a"
        f"{UL}{label or path}{UL_OFF}"
        f"\033]8;;\a"
    )


def osc8_url(url: str, label: str | None = None, shorten: bool = True) -> str:
    """Wrap *url* in an OSC-8 hyperlink."""
    if label is None and shorten:
        label = _shorten_url(url)
    return (
        f"\033]8;;{url}\a"
        f"{URL_COLOR}{UL}{label or url}{UL_OFF}"
        f"\033]8;;\a"
    )


_URL_RE = re.compile(r"(https?://[^\s,;'\"()\[\]>\x00-\x1f]+(?<![:.]))")

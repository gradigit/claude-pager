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

def osc8_file(path: str, label: str | None = None) -> str:
    """Wrap *path* in an OSC-8 file:// hyperlink."""
    from urllib.parse import quote as _urlquote

    encoded = _urlquote(path, safe="/:@")
    return f"\033]8;;file://{encoded}\a{label or path}\033]8;;\a"


def osc8_url(url: str, label: str | None = None) -> str:
    """Wrap *url* in an OSC-8 hyperlink."""
    return f"\033]8;;{url}\a{label or url}\033]8;;\a"


_URL_RE = re.compile(r"(https?://[^\s,;:'\"()\[\]>]+)")

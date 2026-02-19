"""Visual line wrapping and viewport drawing."""
from __future__ import annotations

import re
import sys
import textwrap
import unicodedata

from .renderer import render_status_bar
from .terminal import HEADER_DIM, R, geo, sep

# ── Visual width helpers ──────────────────────────────────────────────────────

_ANSI_RE = re.compile(
    r"\033(?:\[[^a-zA-Z]*[a-zA-Z]|\][^\a]*\a|[^\[\]])"
)


def visual_len(s: str) -> int:
    """Column width of *s* with ANSI escape sequences stripped."""
    stripped = _ANSI_RE.sub("", s)
    total = 0
    for ch in stripped:
        if ord(ch) < 32:
            continue
        ea = unicodedata.east_asian_width(ch)
        total += 2 if ea in ("W", "F") else 1
    return total


def wrap_lines(lines: list[str], cols: int) -> list[str]:
    """Expand logical lines into visual rows of at most *cols* columns.

    Plain-text lines are hard-wrapped with :func:`textwrap.wrap`.
    ANSI-rich lines (code blocks, colored output) are kept as-is with
    empty continuation rows appended so pager offset math stays correct.
    """
    result: list[str] = []
    for line in lines:
        vl = visual_len(line)
        if vl <= cols:
            result.append(line)
            continue

        plain = _ANSI_RE.sub("", line)
        if len(plain) == len(line):
            # No ANSI — safe to hard-wrap the plain text
            wrapped = textwrap.wrap(
                line, width=cols, break_long_words=True, break_on_hyphens=False,
            ) or [line]
            result.extend(wrapped)
        else:
            # Has ANSI — keep original, add empty continuation rows
            visual_rows = max(1, (vl + cols - 1) // cols)
            result.append(line)
            result.extend([""] * (visual_rows - 1))
    return result


# ── Viewport drawing ──────────────────────────────────────────────────────────

def draw_viewport(
    pager: object,  # Pager — avoid circular import
    total: int | None,
    pct: float | None,
    *,
    first_render: bool = False,
    ctx_limit: int = 200_000,
) -> None:
    """Draw the current pager viewport to stdout (which is /dev/tty)."""
    from .pager import Pager  # noqa: PLC0415  (deferred to avoid circular import)

    assert isinstance(pager, Pager)
    visible, offset, _total_lines = pager.get_viewport()

    rows = geo.rows
    K = "\033[K"  # clear to end of line

    if first_render:
        buf = ["\033[?25l\033[2J\033[H"]  # hide cursor + full clear + home
    else:
        buf = ["\033[?25l\033[H"]  # hide cursor + home (no clear)

    # Row 1: top separator
    buf.append(f"{sep()}{K}\n")
    row = 2

    if offset > 0:
        buf.append(
            f"{HEADER_DIM}  \u2191 {offset} lines above  (scroll to view){R}{K}\n"
        )
        row += 1

    for line in visible:
        buf.append(f"{line}{K}\n")
        row += 1

    # Clear any leftover rows between content and bottom separator
    while row < rows - 1:
        buf.append(f"{K}\n")
        row += 1

    # Bottom separator + status bar (absolute positioning)
    buf.append(f"\033[{rows - 1};1H{sep()}{K}")
    buf.append(
        f"\033[{rows};1H{render_status_bar(total, pct, ctx_limit=ctx_limit)}{K}"
    )

    sys.stdout.write("".join(buf))
    sys.stdout.flush()

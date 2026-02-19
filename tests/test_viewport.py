"""Tests for claude_pager.viewport."""
from __future__ import annotations

from claude_pager.viewport import visual_len, wrap_lines


# ── visual_len ───────────────────────────────────────────────────────────────

def test_visual_len_plain():
    assert visual_len("hello") == 5


def test_visual_len_strips_ansi():
    assert visual_len("\033[31mred\033[0m") == 3


def test_visual_len_cjk_double_width():
    assert visual_len("\u4e16\u754c") == 4  # 世界 = 2 chars, 4 columns


def test_visual_len_mixed():
    assert visual_len("hi\u4e16") == 4  # 'h'(1) + 'i'(1) + '世'(2) = 4


def test_visual_len_empty():
    assert visual_len("") == 0


def test_visual_len_control_chars():
    # Control chars (ord < 32) should be zero width
    assert visual_len("\t\n") == 0


# ── wrap_lines ───────────────────────────────────────────────────────────────

def test_wrap_short_lines():
    lines = ["short", "also short"]
    assert wrap_lines(lines, 80) == lines


def test_wrap_long_plain_text():
    line = "a " * 50  # 100 chars
    wrapped = wrap_lines([line.strip()], 40)
    assert len(wrapped) > 1
    for w in wrapped:
        assert visual_len(w) <= 40


def test_wrap_ansi_line_adds_continuation():
    # An ANSI-rich line wider than cols should get continuation rows
    ansi_line = "\033[31m" + "x" * 100 + "\033[0m"
    wrapped = wrap_lines([ansi_line], 40)
    assert len(wrapped) > 1
    # First row is the original line
    assert wrapped[0] == ansi_line
    # Continuation rows are empty
    for w in wrapped[1:]:
        assert w == ""


def test_wrap_preserves_order():
    lines = ["first", "second", "third"]
    assert wrap_lines(lines, 80) == lines

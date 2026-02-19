"""Tests for claude_pager.renderer."""
from __future__ import annotations

import re

from claude_pager.renderer import (
    _is_diff,
    colorize_diff,
    inline_md,
    render_items,
    render_md,
    render_status_bar,
)
from claude_pager.terminal import R

_ANSI_RE = re.compile(r"\033(?:\[[^a-zA-Z]*[a-zA-Z]|\][^\a]*\a|[^\[\]])")


def _strip_ansi(s: str) -> str:
    return _ANSI_RE.sub("", s)


# ── Diff detection / coloring ────────────────────────────────────────────────

def test_is_diff_positive():
    diff = "--- a/f\n+++ b/f\n@@ -1 +1 @@\n-old\n+new"
    assert _is_diff(diff)


def test_is_diff_negative():
    assert not _is_diff("hello world")
    assert not _is_diff("--- only minus lines\n--- more")


def test_colorize_diff_non_diff():
    text = "just plain text"
    assert colorize_diff(text) == text


def test_colorize_diff_colors_lines():
    diff = "-old line\n+new line\n@@ -1 +1 @@\n context"
    colored = colorize_diff(diff)
    assert "\033[38;2;220;80;80m" in colored  # red for -
    assert "\033[38;2;100;220;100m" in colored  # green for +
    assert "\033[38;2;100;150;255m" in colored  # cyan for @@


# ── Inline markdown ──────────────────────────────────────────────────────────

def test_inline_md_bold():
    result = inline_md("**bold text**")
    assert "\033[1m" in result
    assert "bold text" in _strip_ansi(result)


def test_inline_md_italic():
    result = inline_md("*italic*")
    assert "\033[3m" in result
    assert "italic" in _strip_ansi(result)


def test_inline_md_code():
    result = inline_md("`code`")
    assert "code" in _strip_ansi(result)


def test_inline_md_url_linking():
    result = inline_md("visit https://example.com please")
    assert "example.com" in _strip_ansi(result)
    # Should have OSC-8 link
    assert "\033]8;;" in result


def test_inline_md_file_path_in_backticks():
    result = inline_md("`/tmp/test.py`")
    # File paths in backticks get OSC-8 file:// links
    assert "file://" in result


# ── render_md ────────────────────────────────────────────────────────────────

def test_render_md_heading_1():
    rendered = render_md("# Main Title")
    plain = _strip_ansi(rendered)
    assert "Main Title" in plain
    assert "\u2500" in rendered  # rule line under h1


def test_render_md_heading_2():
    rendered = render_md("## Section")
    plain = _strip_ansi(rendered)
    assert "Section" in plain


def test_render_md_bullet_list():
    rendered = render_md("- item one\n- item two")
    plain = _strip_ansi(rendered)
    assert "\u2022 item one" in plain
    assert "\u2022 item two" in plain


def test_render_md_numbered_list():
    rendered = render_md("1. first\n2. second")
    plain = _strip_ansi(rendered)
    assert "1. first" in plain
    assert "2. second" in plain


def test_render_md_code_block():
    rendered = render_md("```python\nprint('hi')\n```")
    plain = _strip_ansi(rendered)
    assert "print('hi')" in plain


def test_render_md_unclosed_code_block():
    rendered = render_md("```\nsome code\nno closing")
    plain = _strip_ansi(rendered)
    assert "some code" in plain


# ── render_items ─────────────────────────────────────────────────────────────

def test_render_items_human():
    items = [("human", "Hello there")]
    rendered = render_items(items)
    plain = _strip_ansi(rendered)
    assert "\u276f you" in plain
    assert "Hello there" in plain


def test_render_items_assistant():
    items = [("assistant", "I can help.")]
    rendered = render_items(items)
    assert "I can help." in _strip_ansi(rendered)


def test_render_items_tool_use_with_path():
    items = [("tool_use", "Read", "/tmp/file.py")]
    rendered = render_items(items)
    plain = _strip_ansi(rendered)
    assert "Read" in plain
    assert "file://" in rendered  # OSC-8 link for file path


def test_render_items_tool_use_with_url():
    items = [("tool_use", "WebFetch", "https://example.com")]
    rendered = render_items(items)
    assert "WebFetch" in _strip_ansi(rendered)
    assert "\033]8;;" in rendered  # OSC-8 link


def test_render_items_tool_result():
    items = [("tool_result", "output text", False)]
    rendered = render_items(items)
    assert "output text" in _strip_ansi(rendered)


def test_render_items_tool_result_error():
    items = [("tool_result", "error message", True)]
    rendered = render_items(items)
    assert "error message" in _strip_ansi(rendered)
    assert "\033[38;2;220;80;80m" in rendered  # red for errors


def test_render_items_tool_result_connector():
    items = [("tool_use", "Read", "/tmp/f"), ("tool_result", "content", False)]
    rendered = render_items(items)
    assert "\u2502" in rendered  # vertical bar connector


def test_render_items_long_human_truncated():
    lines = "\n".join(f"line {i}" for i in range(30))
    items = [("human", lines)]
    rendered = render_items(items)
    plain = _strip_ansi(rendered)
    assert "more lines" in plain


# ── render_status_bar ────────────────────────────────────────────────────────

def test_status_bar_no_usage():
    bar = render_status_bar(None, None)
    plain = _strip_ansi(bar)
    assert "PromptPad" in plain


def test_status_bar_with_usage():
    bar = render_status_bar(50000, 25.0, ctx_limit=200_000)
    plain = _strip_ansi(bar)
    assert "25%" in plain
    assert "50k" in plain
    assert "200k" in plain


def test_status_bar_color_green():
    bar = render_status_bar(10000, 5.0)
    assert "\033[38;2;100;220;100m" in bar  # green


def test_status_bar_color_orange():
    bar = render_status_bar(130000, 65.0)
    assert "\033[38;2;255;165;0m" in bar  # orange


def test_status_bar_color_red():
    bar = render_status_bar(180000, 90.0)
    assert "\033[38;2;255;80;80m" in bar  # red

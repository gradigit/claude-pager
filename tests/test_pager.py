"""Tests for claude_pager.pager."""
from __future__ import annotations

from claude_pager.pager import Pager
from claude_pager.terminal import geo


# ── Pager content management ────────────────────────────────────────────────

def test_initial_state():
    p = Pager()
    vis, offset, total = p.get_viewport()
    assert vis == []
    assert offset == 0
    assert total == 0


def test_update_lines():
    p = Pager()
    lines = [f"line {i}" for i in range(10)]
    p.update_lines(lines)
    _, _, total = p.get_viewport()
    assert total == 10


def test_auto_scroll_to_bottom():
    p = Pager()
    # Create more lines than content_rows to ensure scrolling is needed
    lines = [f"line {i}" for i in range(geo.content_rows + 20)]
    p.update_lines(lines)
    _, offset, _ = p.get_viewport()
    # Should be at or near bottom
    expected = len(lines) - (geo.content_rows - 1)
    assert offset == expected


def test_user_scroll_preserves_position():
    p = Pager()
    lines = [f"line {i}" for i in range(geo.content_rows + 20)]
    p.update_lines(lines)

    # Simulate user scroll up
    p._events.put(-5)
    p.process_events()
    _, offset_after_scroll, _ = p.get_viewport()

    # Now update lines (new content arrives)
    more_lines = lines + ["new line"]
    p.update_lines(more_lines)
    _, offset_after_update, _ = p.get_viewport()

    # Position should stay the same (user has scrolled)
    assert offset_after_update == offset_after_scroll


def test_end_key_resets_user_scrolled():
    p = Pager()
    lines = [f"line {i}" for i in range(geo.content_rows + 20)]
    p.update_lines(lines)

    # User scrolls up
    p._events.put(-5)
    p.process_events()
    assert p._user_scrolled is True

    # User hits End
    p._events.put("bottom")
    p.process_events()
    assert p._user_scrolled is False


def test_home_key():
    p = Pager()
    lines = [f"line {i}" for i in range(geo.content_rows + 20)]
    p.update_lines(lines)

    p._events.put("top")
    p.process_events()
    _, offset, _ = p.get_viewport()
    assert offset == 0
    assert p._user_scrolled is True


def test_scroll_clamps():
    p = Pager()
    lines = ["a", "b", "c"]
    p.update_lines(lines)

    # Try to scroll way past bottom
    p._events.put(1000)
    p.process_events()
    _, offset, _ = p.get_viewport()
    assert offset <= len(lines) - 1

    # Try to scroll way past top
    p._events.put(-1000)
    p.process_events()
    _, offset, _ = p.get_viewport()
    assert offset == 0


def test_process_events_returns_changed():
    p = Pager()
    p.update_lines(["a", "b", "c"])

    assert p.process_events() is False  # no events
    p._events.put(1)
    assert p.process_events() is True  # had event


# ── Input parsing ────────────────────────────────────────────────────────────

def test_parse_arrow_up():
    p = Pager()
    remainder = p._parse(b"\x1b[A")
    assert remainder == b""
    event = p._events.get_nowait()
    assert event == -1


def test_parse_arrow_down():
    p = Pager()
    remainder = p._parse(b"\x1b[B")
    assert remainder == b""
    event = p._events.get_nowait()
    assert event == 1


def test_parse_multiple_arrows():
    p = Pager()
    p._parse(b"\x1b[A\x1b[A\x1b[A")
    event = p._events.get_nowait()
    assert event == -3


def test_parse_page_up():
    p = Pager()
    p._parse(b"\x1b[5~")
    event = p._events.get_nowait()
    assert event == -(geo.content_rows - 1)


def test_parse_page_down():
    p = Pager()
    p._parse(b"\x1b[6~")
    event = p._events.get_nowait()
    assert event == geo.content_rows - 1


def test_parse_home():
    p = Pager()
    p._parse(b"\x1b[H")
    event = p._events.get_nowait()
    assert event == "top"


def test_parse_end():
    p = Pager()
    p._parse(b"\x1b[F")
    event = p._events.get_nowait()
    assert event == "bottom"


def test_parse_partial_escape():
    p = Pager()
    remainder = p._parse(b"\x1b[")
    # Should return partial escape as remainder
    assert remainder == b"\x1b["

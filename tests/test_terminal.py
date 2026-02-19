"""Tests for claude_pager.terminal."""
from __future__ import annotations

from claude_pager.terminal import geo, osc8_file, osc8_url, reinit_geometry, sep


def test_geo_has_required_fields():
    assert hasattr(geo, "cols")
    assert hasattr(geo, "rows")
    assert hasattr(geo, "content_rows")
    assert geo.cols > 0
    assert geo.rows > 0


def test_reinit_geometry():
    old_cols = geo.cols
    reinit_geometry()
    # Just check it doesn't crash and sets positive values
    assert geo.cols > 0
    assert geo.rows > 0
    assert geo.content_rows == geo.rows - 3


def test_sep_length():
    s = sep()
    # Should contain exactly geo.cols box-drawing chars
    assert "\u2500" * geo.cols in s


def test_osc8_file_basic():
    result = osc8_file("/tmp/test.py")
    assert "file:///tmp/test.py" in result
    assert "\033]8;;" in result
    assert "\033]8;;\a" in result


def test_osc8_file_url_encodes_spaces():
    result = osc8_file("/tmp/my file.py")
    assert "%20" in result
    assert " " not in result.split("\a")[0]  # no raw spaces in the URI part


def test_osc8_file_preserves_slashes():
    result = osc8_file("/a/b/c")
    assert "file:///a/b/c" in result


def test_osc8_file_custom_label():
    result = osc8_file("/tmp/test.py", label="test.py")
    assert "test.py" in result
    # The label sits between the first \a and the closing \033]8;;
    parts = result.split("\a")
    assert parts[1].startswith("test.py")


def test_osc8_url_basic():
    result = osc8_url("https://example.com")
    assert "https://example.com" in result
    assert "\033]8;;" in result


def test_osc8_url_custom_label():
    result = osc8_url("https://example.com", label="Example")
    parts = result.split("\a")
    assert parts[1].startswith("Example")

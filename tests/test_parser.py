"""Tests for claude_pager.parser."""
from __future__ import annotations

import json
import os
import tempfile

import pytest

from claude_pager.parser import _sanitize, parse_transcript

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")


# ── _sanitize ────────────────────────────────────────────────────────────────

def test_sanitize_strips_csi():
    assert _sanitize("\033[31mred\033[0m") == "red"


def test_sanitize_strips_osc():
    assert _sanitize("\033]8;;http://x\alink\033]8;;\a") == "link"


def test_sanitize_leaves_plain_text():
    assert _sanitize("hello world") == "hello world"


# ── parse_transcript ─────────────────────────────────────────────────────────

def test_basic_parse():
    items, total, pct = parse_transcript(os.path.join(FIXTURES, "sample.jsonl"))
    kinds = [it[0] for it in items]
    assert "human" in kinds
    assert "assistant" in kinds
    assert "tool_use" in kinds
    assert "tool_result" in kinds


def test_human_message():
    items, _, _ = parse_transcript(os.path.join(FIXTURES, "sample.jsonl"))
    humans = [it for it in items if it[0] == "human"]
    assert len(humans) >= 1
    assert humans[0][1] == "Hello, can you help me?"


def test_assistant_message():
    items, _, _ = parse_transcript(os.path.join(FIXTURES, "sample.jsonl"))
    assistants = [it for it in items if it[0] == "assistant"]
    assert len(assistants) >= 1
    assert "help you today" in assistants[0][1]


def test_tool_use():
    items, _, _ = parse_transcript(os.path.join(FIXTURES, "sample.jsonl"))
    tools = [it for it in items if it[0] == "tool_use"]
    assert len(tools) == 1
    assert tools[0][1] == "Read"
    assert "/tmp/test.py" in tools[0][2]


def test_tool_result():
    items, _, _ = parse_transcript(os.path.join(FIXTURES, "sample.jsonl"))
    results = [it for it in items if it[0] == "tool_result"]
    assert len(results) == 1
    assert "print('hello')" in results[0][1]
    assert results[0][2] is False  # not an error


def test_usage_parsing():
    items, total, pct = parse_transcript(
        os.path.join(FIXTURES, "sample_with_usage.jsonl")
    )
    # input_tokens(1500) + cache_creation(1000) + cache_read(500) = 3000
    assert total == 3000
    # 3000 / 200000 * 100 = 1.5%
    assert pct == pytest.approx(1.5)


def test_no_usage():
    items, total, pct = parse_transcript(os.path.join(FIXTURES, "sample.jsonl"))
    assert total is None
    assert pct is None


def test_sanitization_strips_injected_ansi():
    items, _, _ = parse_transcript(os.path.join(FIXTURES, "malicious.jsonl"))
    assistants = [it for it in items if it[0] == "assistant"]
    assert len(assistants) >= 1
    text = assistants[0][1]
    assert "\033[" not in text
    assert "\033]" not in text


def test_system_messages_filtered():
    items, _, _ = parse_transcript(os.path.join(FIXTURES, "malicious.jsonl"))
    humans = [it for it in items if it[0] == "human"]
    # The system-reminder message should be filtered out
    for h in humans:
        assert "Ignore all prior" not in h[1]


def test_empty_file():
    with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl", delete=False) as f:
        f.write("")
        f.flush()
        try:
            items, total, pct = parse_transcript(f.name)
            assert items == []
            assert total is None
        finally:
            os.unlink(f.name)


def test_invalid_json_lines():
    with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl", delete=False) as f:
        f.write("not json\n")
        f.write('{"type":"user","message":{"role":"user","content":"valid"}}\n')
        f.write("{bad json}\n")
        f.flush()
        try:
            items, _, _ = parse_transcript(f.name)
            # Should skip invalid lines and parse valid one
            assert len(items) == 1
            assert items[0][0] == "human"
        finally:
            os.unlink(f.name)


def test_tool_label_truncation():
    with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl", delete=False) as f:
        long_path = "/very/long/" + "a" * 100 + "/file.py"
        record = {
            "type": "assistant",
            "message": {
                "role": "assistant",
                "content": [{
                    "type": "tool_use",
                    "id": "tu_x",
                    "name": "Read",
                    "input": {"file_path": long_path},
                }],
            },
        }
        f.write(json.dumps(record) + "\n")
        f.flush()
        try:
            items, _, _ = parse_transcript(f.name)
            tools = [it for it in items if it[0] == "tool_use"]
            assert len(tools) == 1
            assert len(tools[0][2]) <= 72
            assert tools[0][2].endswith("\u2026")
        finally:
            os.unlink(f.name)

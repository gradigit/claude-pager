"""Main entry point for claude-pager."""
from __future__ import annotations

import argparse
import logging
import os
import signal
import sys
import time

from . import __version__
from .pager import Pager
from .parser import parse_transcript
from .renderer import render_items, render_status_bar
from .terminal import HEADER_DIM, MOUSE_OFF, MOUSE_ON, R, geo, reinit_geometry, sep
from .viewport import draw_viewport, wrap_lines

logger = logging.getLogger(__name__)

BOTTOM_PAD = 2  # empty lines so last text isn't glued to status bar


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="claude-pager",
        description="Scrollable pager for Claude Code session transcripts.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  claude-pager ~/.claude/projects/-Users-me-myproject/abc123.jsonl
  claude-pager transcript.jsonl 12345   # live-follow while PID 12345 alive
  claude-pager transcript.jsonl --ctx-limit 100000
        """,
    )
    p.add_argument(
        "transcript", nargs="?", default="",
        help="Path to .jsonl transcript file",
    )
    p.add_argument(
        "editor_pid", nargs="?", type=int, default=None,
        help="Editor PID to watch (exits when process dies)",
    )
    p.add_argument(
        "--ctx-limit", type=int, default=200_000, metavar="TOKENS",
        help="Context window size for usage bar (default: 200000)",
    )
    p.add_argument(
        "--log-file", metavar="PATH",
        help="Write debug log to PATH",
    )
    p.add_argument(
        "--version", action="version",
        version=f"%(prog)s {__version__}",
    )
    return p


def main(argv: list[str] | None = None) -> None:
    args = _build_parser().parse_args(argv)

    if args.log_file:
        logging.basicConfig(
            filename=args.log_file,
            level=logging.DEBUG,
            format="%(asctime)s %(levelname)s %(message)s",
        )

    path: str = args.transcript
    editor_pid: int | None = args.editor_pid
    ctx_limit: int = args.ctx_limit

    pager = Pager()

    def cleanup_and_exit(*_: object) -> None:
        pager.cleanup()
        try:
            sys.stdout.write(MOUSE_OFF)
            sys.stdout.flush()
        except Exception:
            pass
        sys.exit(0)

    signal.signal(signal.SIGTERM, cleanup_and_exit)

    # ── SIGWINCH: flag for main loop to reinit geometry ───────────────
    resize_pending = False

    def _on_sigwinch(_sig: int, _frame: object) -> None:
        nonlocal resize_pending
        resize_pending = True

    signal.signal(signal.SIGWINCH, _on_sigwinch)

    # No sleep — the C launcher handles timing.  Claude Code's "Save and
    # close editor…" message is already drawn by the time Python starts.

    if not path or not os.path.exists(path):
        sys.stdout.write("\033[2J\033[H")
        sys.stdout.write(f"{HEADER_DIM}(transcript not found){R}\n")
        sys.stdout.write(f"\033[{geo.rows};1H")
        sys.stdout.write(render_status_bar(None, None, ctx_limit=ctx_limit))
        sys.stdout.flush()
        while editor_pid:
            try:
                os.kill(editor_pid, 0)
                time.sleep(2)
            except (ProcessLookupError, OSError):
                break
        cleanup_and_exit()
        return

    pager.start_input()
    sys.stdout.write(MOUSE_ON)
    sys.stdout.flush()

    # Cached state — only recomputed when the JSONL file changes
    cached_total: int | None = None
    cached_pct: float | None = None
    cached_visual_rows: list[str] = []

    first_render = True
    last_mtime = 0.0

    while True:
        if editor_pid:
            try:
                os.kill(editor_pid, 0)
            except (ProcessLookupError, OSError):
                break

        # Handle terminal resize
        if resize_pending:
            resize_pending = False
            reinit_geometry()
            first_render = True
            # Re-wrap cached content with new column width
            if cached_visual_rows:
                try:
                    items, cached_total, cached_pct = parse_transcript(
                        path, ctx_limit=ctx_limit,
                    )
                    logical = render_items(items).split("\n")
                    logical.append(f"{HEADER_DIM}  \u2500\u2500\u2500 end of transcript \u2500\u2500\u2500{R}")
                    logical.extend([""] * BOTTOM_PAD)
                    cached_visual_rows = wrap_lines(logical, geo.cols)
                    pager.update_lines(cached_visual_rows)
                except Exception:
                    logger.exception("resize re-wrap failed")

        try:
            mtime = os.path.getmtime(path)
        except OSError:
            time.sleep(0.016)
            continue

        content_changed = mtime != last_mtime
        scroll_changed = pager.process_events()

        if content_changed:
            last_mtime = mtime
            try:
                items, cached_total, cached_pct = parse_transcript(
                    path, ctx_limit=ctx_limit,
                )
                logical = render_items(items).split("\n")
                logical.append(f"{HEADER_DIM}  \u2500\u2500\u2500 end of transcript \u2500\u2500\u2500{R}")
                logical.extend([""] * BOTTOM_PAD)
                cached_visual_rows = wrap_lines(logical, geo.cols)
                pager.update_lines(cached_visual_rows)
            except Exception:
                logger.exception("content update failed")

        if content_changed or scroll_changed:
            try:
                draw_viewport(
                    pager, cached_total, cached_pct,
                    first_render=first_render, ctx_limit=ctx_limit,
                )
                first_render = False
            except Exception:
                logger.exception("draw failed")

        # 60 fps during active scroll, 20 fps idle
        time.sleep(0.016 if scroll_changed else 0.05)

    cleanup_and_exit()


if __name__ == "__main__":
    main()

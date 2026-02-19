"""Scrollable pager with /dev/tty input thread."""
from __future__ import annotations

import os
import queue
import re
import select
import signal
import threading

from .terminal import geo

# Regex to detect a trailing partial escape sequence
_PARTIAL_ESC = re.compile(rb"\x1b(?:$|\[[^a-zA-Z~]*$)")


class Pager:
    """Manages a viewport offset into a list of visual-row lines."""

    def __init__(self) -> None:
        self._lines: list[str] = []
        self._offset: int = 0
        self._lock = threading.Lock()
        self._events: queue.SimpleQueue[int | str] = queue.SimpleQueue()
        self._tty_fd: int | None = None
        self._tty_old: list[object] | None = None
        self._user_scrolled: bool = False

    # ── Content management ────────────────────────────────────────────────

    def _natural_bottom(self, lines: list[str] | None = None) -> int:
        """Offset that shows the last screenful of content."""
        if lines is None:
            lines = self._lines
        content_rows = geo.content_rows
        if len(lines) <= content_rows:
            return 0
        return len(lines) - (content_rows - 1)

    def update_lines(self, lines: list[str]) -> None:
        """Replace content; auto-scroll to bottom unless user has scrolled."""
        with self._lock:
            self._lines = lines
            if not self._user_scrolled:
                self._offset = self._natural_bottom(lines)
            else:
                max_nav = max(0, len(lines) - 1)
                self._offset = min(self._offset, max_nav)

    # ── Scrolling ─────────────────────────────────────────────────────────

    def _do_scroll(self, event: int | str) -> None:
        with self._lock:
            lines = self._lines
            if event == "top":
                self._offset = 0
                self._user_scrolled = True
            elif event == "bottom":
                self._offset = self._natural_bottom()
                self._user_scrolled = False
            else:
                assert isinstance(event, int)
                self._user_scrolled = True
                max_off = max(0, len(lines) - 1)
                self._offset = max(0, min(max_off, self._offset + event))

    def get_viewport(self) -> tuple[list[str], int, int]:
        """Return ``(visible_lines, offset, total_lines)``."""
        with self._lock:
            content_rows = geo.content_rows
            avail = content_rows - (1 if self._offset > 0 else 0)
            vis = self._lines[self._offset : self._offset + avail]
            return vis, self._offset, len(self._lines)

    def process_events(self) -> bool:
        """Drain pending scroll events.  Returns True if viewport changed."""
        changed = False
        while True:
            try:
                event = self._events.get_nowait()
            except queue.Empty:
                break
            self._do_scroll(event)
            changed = True
        return changed

    # ── Input thread ──────────────────────────────────────────────────────

    def start_input(self) -> None:
        """Open ``/dev/tty`` for reading, set raw mode, start daemon thread."""
        # SIGTTIN must be ignored in the *main* thread (signal.signal is a
        # no-op when called from a non-main thread).
        try:
            signal.signal(signal.SIGTTIN, signal.SIG_IGN)
        except (OSError, ValueError):
            pass

        try:
            import termios  # noqa: PLC0415

            fd = os.open("/dev/tty", os.O_RDWR | os.O_NOCTTY)
            self._tty_fd = fd
            self._tty_old = termios.tcgetattr(fd)
            new = termios.tcgetattr(fd)
            new[3] &= ~(termios.ICANON | termios.ECHO)
            new[6][termios.VMIN] = 0
            new[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, new)
        except Exception:
            return

        t = threading.Thread(target=self._input_loop, daemon=True)
        t.start()

    def _input_loop(self) -> None:
        buf = b""
        while self._tty_fd is not None:
            fd = self._tty_fd  # snapshot — may become None mid-iteration
            if fd is None:
                break
            try:
                r, _, _ = select.select([fd], [], [], 0.05)
                if r:
                    chunk = os.read(fd, 256)
                    if chunk:
                        buf = self._parse(buf + chunk)
            except OSError:
                break

    def _parse(self, data: bytes) -> bytes:
        """Parse escape sequences from *data*.

        Returns any trailing partial sequence to be prepended to the
        next read.
        """
        # Hold back trailing partial escape sequences
        m = _PARTIAL_ESC.search(data)
        if m:
            remainder = data[m.start() :]
            data = data[: m.start()]
        else:
            remainder = b""

        if not data:
            return remainder

        content_rows = geo.content_rows

        # Scroll wheel (via alternate scroll mode) + keyboard arrows
        up_count = data.count(b"\x1b[A")
        down_count = data.count(b"\x1b[B")
        if up_count > 0:
            self._events.put(-up_count)
        elif down_count > 0:
            self._events.put(down_count)
        elif b"\x1b[5~" in data:  # Page Up
            self._events.put(-(content_rows - 1))
        elif b"\x1b[6~" in data:  # Page Down
            self._events.put(content_rows - 1)
        elif b"\x1b[H" in data:  # Home
            self._events.put("top")
        elif b"\x1b[F" in data:  # End
            self._events.put("bottom")

        return remainder

    # ── Cleanup ───────────────────────────────────────────────────────────

    def cleanup(self) -> None:
        """Restore termios and close the tty fd."""
        fd, self._tty_fd = self._tty_fd, None
        if fd is not None:
            if self._tty_old is not None:
                try:
                    import termios  # noqa: PLC0415

                    termios.tcsetattr(fd, termios.TCSANOW, self._tty_old)
                except Exception:
                    pass
            try:
                os.close(fd)
            except Exception:
                pass

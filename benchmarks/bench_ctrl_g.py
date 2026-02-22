#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyobjc-framework-Quartz"]
# ///
"""Automated Ctrl+G benchmark: measures keypress-to-visually-ready latency.

Primary measurement: CGEvent keystroke injection + CGWindowList polling
(~1ms precision). Cross-validated with 60fps screen recording + ffmpeg
scdet frame analysis (16.7ms precision) when --record is used.

Frame analysis measures "time to visual ready" — Ctrl+G to the last frame
of render activity before the screen settles. This is the point where the
app is done painting and ready for interaction.
"""
from __future__ import annotations

import argparse
import json
import math
import platform
import re
import socket
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, List, Optional, Tuple

from Quartz import (
    CGEventCreateKeyboardEvent,
    CGEventPost,
    CGEventSetFlags,
    CGWindowListCopyWindowInfo,
    kCGEventFlagMaskCommand,
    kCGEventFlagMaskControl,
    kCGHIDEventTap,
    kCGNullWindowID,
    kCGWindowListOptionOnScreenOnly,
)

LOG_PATH = Path("/tmp/claude-pager-open.log")

# TurboDraft may register under any of these process names.
TD_NAMES = {"TurboDraft", "turbodraft-app", "turbodraft-app.debug"}

# Ghostty may register as "Ghostty" or "ghostty" depending on build.
GHOSTTY_NAMES = {"Ghostty", "ghostty"}

# Virtual keycodes (macOS HID)
VK_G = 0x05
VK_Q = 0x0C

# ffmpeg warmup: long enough that PTS clock is stable and startup jitter
# is absorbed. After this, perf_counter-to-PTS mapping is a fixed offset.
FFMPEG_WARMUP_S = 3.0

# Frame-analysis defaults.
DEFAULT_MAFD_TRANSITION = 0.3
DEFAULT_MAFD_ACTIVITY_FLOOR = 0.02
DEFAULT_QUIET_FRAMES = 3
DEFAULT_CLOCK_MAX_ERROR_MS = 80.0
DEFAULT_CLOCK_MEDIAN_ERROR_MS = 40.0
DEFAULT_SEARCH_RADIUS_MS = 50.0


# ── CGEvent keystroke delivery ───────────────────────────────────────────────


def post_keystroke(keycode: int, flags: int) -> float:
    """Post key down+up via CGEvent. Returns perf_counter at moment of posting."""
    down = CGEventCreateKeyboardEvent(None, keycode, True)
    CGEventSetFlags(down, flags)
    t = time.perf_counter()
    CGEventPost(kCGHIDEventTap, down)
    up = CGEventCreateKeyboardEvent(None, keycode, False)
    CGEventSetFlags(up, flags)
    CGEventPost(kCGHIDEventTap, up)
    return t


# ── CGWindowList window detection ────────────────────────────────────────────


def poll_for_td_window(timeout: float = 10.0) -> float:
    """Poll until a TurboDraft window appears on screen. Returns timestamp."""
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        if td_window_count() > 0:
            return time.perf_counter()
        time.sleep(0.001)
    raise TimeoutError("TurboDraft window did not appear")


def poll_for_td_gone(timeout: float = 10.0) -> float:
    """Poll until no TurboDraft window is on screen. Returns timestamp."""
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        if td_window_count() == 0:
            return time.perf_counter()
        time.sleep(0.001)
    raise TimeoutError("TurboDraft window did not disappear")


def td_window_count() -> int:
    """Count currently visible TurboDraft-owned windows."""
    windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID
    )
    return sum(1 for w in windows if w.get("kCGWindowOwnerName") in TD_NAMES)


def is_ghostty_frontmost() -> bool:
    """Check if Ghostty is the frontmost normal window."""
    windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID
    )
    # Layer 0 = normal windows, sorted front-to-back by WindowServer
    normal = [w for w in windows if w.get("kCGWindowLayer", -1) == 0]
    if normal and normal[0].get("kCGWindowOwnerName") in GHOSTTY_NAMES:
        return True
    return False


def frontmost_owner() -> Optional[str]:
    """Return owner name of the frontmost normal window."""
    windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID
    )
    normal = [w for w in windows if w.get("kCGWindowLayer", -1) == 0]
    if not normal:
        return None
    owner = normal[0].get("kCGWindowOwnerName")
    return str(owner) if owner else None


def window_owner_snapshot(limit: int = 8) -> List[str]:
    """Capture a short front-to-back window owner snapshot for reproducibility."""
    windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID
    )
    out: List[str] = []
    for w in windows:
        if w.get("kCGWindowLayer", -1) != 0:
            continue
        owner = w.get("kCGWindowOwnerName")
        if not owner:
            continue
        out.append(str(owner))
        if len(out) >= limit:
            break
    return out


# ── Poll overhead calibration ────────────────────────────────────────────────


def measure_poll_overhead(n: int = 100) -> float:
    """Measure median CGWindowList call time in ms."""
    times: List[float] = []
    for _ in range(n):
        t0 = time.perf_counter()
        CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID)
        times.append((time.perf_counter() - t0) * 1000)
    return statistics.median(times)


# ── Statistics ───────────────────────────────────────────────────────────────


def percentile_nearest_rank(samples: List[float], p: float) -> Optional[float]:
    if not samples:
        return None
    xs = sorted(samples)
    idx = max(0, min(len(xs) - 1, math.ceil(max(0.0, min(1.0, p)) * len(xs)) - 1))
    return xs[idx]


def fmt_ms(v: Optional[float]) -> str:
    return f"{v:.0f}ms" if v is not None else "—"


# ── Log parsing ──────────────────────────────────────────────────────────────

# Log line format: [  12.34ms] message text
LOG_LINE_RE = re.compile(r"^\[\s*([\d.]+)ms\]\s+(.+)$")


@dataclass
class LogCycle:
    """Parsed metrics from one open/close cycle in the log."""

    cc_overhead_ms: Optional[float] = None
    open_path_ms: Optional[float] = None  # last open-path timestamp
    close_path_ms: Optional[float] = None  # last close-path timestamp
    pager_prerender_done_ms: Optional[float] = None
    pager_parse_ms: Optional[float] = None  # first parse duration
    pager_render_ms: Optional[float] = None  # first markdown-render duration
    pager_first_draw_done_ms: Optional[float] = None
    pager_termready_tcdrain_ms: Optional[float] = None
    pager_termready_dsr_ms: Optional[float] = None
    pager_termready_total_ms: Optional[float] = None


def parse_log(path: Path) -> List[LogCycle]:
    """Parse claude-pager-open.log into per-cycle metrics."""
    if not path.exists():
        return []

    text = path.read_text(encoding="utf-8", errors="replace")
    cycles: List[LogCycle] = []
    current: Optional[LogCycle] = None
    in_close_path = False

    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue

        # New open cycle starts with "--- claude-pager-open pid=..."
        if "--- claude-pager-open pid=" in line:
            current = LogCycle()
            cycles.append(current)
            in_close_path = False
            continue

        if current is None:
            continue

        m = LOG_LINE_RE.match(line)
        if not m:
            continue

        ms = float(m.group(1))
        msg = m.group(2)

        if "claude-code exec overhead:" in msg:
            overhead_match = re.search(r"([\d.]+)ms", msg)
            if overhead_match:
                current.cc_overhead_ms = float(overhead_match.group(1))

        elif "--- close path start" in msg:
            in_close_path = True

        elif "sessionId=" in msg and not in_close_path:
            current.open_path_ms = ms

        elif "pager exited" in msg and in_close_path:
            current.close_path_ms = ms

        elif "pager pre-render done" in msg and current.pager_prerender_done_ms is None:
            current.pager_prerender_done_ms = ms

        elif "pager: parse end" in msg and current.pager_parse_ms is None:
            dur = re.search(r"duration=([\d.]+)ms", msg)
            if dur:
                current.pager_parse_ms = float(dur.group(1))

        elif "pager: markdown render end" in msg and current.pager_render_ms is None:
            dur = re.search(r"duration=([\d.]+)ms", msg)
            if dur:
                current.pager_render_ms = float(dur.group(1))

        elif "pager: first draw done" in msg and current.pager_first_draw_done_ms is None:
            current.pager_first_draw_done_ms = ms

        elif (
            "pager: bench term-ready" in msg
            and "label=first_draw" in msg
            and current.pager_termready_total_ms is None
        ):
            m_tc = re.search(r"tcdrain=([\d.]+)ms", msg)
            m_ds = re.search(r"dsr=([\d.]+)ms", msg)
            m_tot = re.search(r"total=([\d.]+)ms", msg)
            if m_tc:
                current.pager_termready_tcdrain_ms = float(m_tc.group(1))
            if m_ds:
                current.pager_termready_dsr_ms = float(m_ds.group(1))
            if m_tot:
                current.pager_termready_total_ms = float(m_tot.group(1))

    return cycles


# ── Screen recording ────────────────────────────────────────────────────────


def find_screen_device() -> str:
    """Find the avfoundation screen capture device index."""
    r = subprocess.run(
        ["ffmpeg", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
        capture_output=True,
        text=True,
        timeout=5,
    )
    for line in r.stderr.splitlines():
        if "Capture screen" in line or "capture screen" in line:
            m = re.search(r"\[(\d+)\]\s+[Cc]apture screen", line)
            if m:
                return m.group(1)
    return "2"  # fallback


def start_recording(output_path: Path) -> subprocess.Popen:
    """Start 60fps screen recording via ffmpeg."""
    screen_idx = find_screen_device()
    return subprocess.Popen(
        [
            "ffmpeg",
            "-y",
            "-f",
            "avfoundation",
            "-framerate",
            "60",
            "-capture_cursor",
            "1",
            "-i",
            f"{screen_idx}:none",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-crf",
            "18",
            str(output_path),
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def stop_recording(proc: subprocess.Popen) -> None:
    """Stop ffmpeg recording gracefully."""
    try:
        if proc.stdin and proc.poll() is None:
            proc.stdin.write(b"q")
            proc.stdin.flush()
    except BrokenPipeError:
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def ffmpeg_version() -> Optional[str]:
    """Best-effort ffmpeg version string."""
    try:
        r = subprocess.run(
            ["ffmpeg", "-version"],
            capture_output=True,
            text=True,
            timeout=5,
        )
    except Exception:
        return None
    if r.returncode != 0:
        return None
    line = r.stdout.splitlines()[0].strip() if r.stdout else ""
    return line or None


# ── Frame analysis ───────────────────────────────────────────────────────────

# Metadata lines from ffmpeg scdet filter:
#   frame:0    pts:0        pts_time:0.000000
#   lavfi.scd.mafd=0.000000
#   lavfi.scd.score=0.000000
FRAME_HDR_RE = re.compile(r"^frame:(\d+)\s+pts:\d+\s+pts_time:([\d.]+)")
SCD_MAFD_RE = re.compile(r"^lavfi\.scd\.mafd=([\d.]+)")
SCD_SCORE_RE = re.compile(r"^lavfi\.scd\.score=([\d.]+)")

@dataclass
class FrameAnalysisConfig:
    """Tunable knobs for frame-analysis robustness."""

    transition_threshold: float = DEFAULT_MAFD_TRANSITION
    activity_threshold: Optional[float] = None  # None = auto infer from run noise
    quiet_frames: int = DEFAULT_QUIET_FRAMES
    clock_max_error_ms: float = DEFAULT_CLOCK_MAX_ERROR_MS
    clock_median_error_ms: float = DEFAULT_CLOCK_MEDIAN_ERROR_MS
    search_radius_ms: float = DEFAULT_SEARCH_RADIUS_MS


@dataclass
class ClockCalibration:
    """Clock-offset fit quality for perf_counter ↔ video PTS alignment."""

    offset_s: float = 0.0
    total_cost_s: float = 0.0
    matched_events: int = 0
    max_error_s: float = 0.0
    median_error_s: float = 0.0
    valid: bool = False
    reason: str = ""


@dataclass
class FrameScore:
    """Scene change scores for a single video frame."""

    frame_num: int
    pts_time: float  # seconds from recording start
    mafd: float  # mean absolute frame difference (0-100)
    score: float  # scene change score (0-100)


def analyze_recording(
    video_path: Path,
    transition_threshold: float,
) -> List[FrameScore]:
    """Run ffmpeg scdet filter on recording, return per-frame scores."""
    meta_path = video_path.with_suffix(".scd.txt")

    print("Analyzing recording frames (scdet)...")
    subprocess.run(
        [
            "ffmpeg",
            "-i",
            str(video_path),
            "-vf",
            "scdet=threshold=0,metadata=print:file=" + str(meta_path),
            "-f",
            "null",
            "-",
        ],
        capture_output=True,
        timeout=120,
    )

    if not meta_path.exists():
        print("  WARNING: scdet metadata file not generated")
        return []

    frames: List[FrameScore] = []
    cur_num = -1
    cur_pts = 0.0
    cur_mafd = 0.0
    cur_score = 0.0

    for line in meta_path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue

        m = FRAME_HDR_RE.match(line)
        if m:
            if cur_num >= 0:
                frames.append(FrameScore(cur_num, cur_pts, cur_mafd, cur_score))
            cur_num = int(m.group(1))
            cur_pts = float(m.group(2))
            cur_mafd = 0.0
            cur_score = 0.0
            continue

        m = SCD_MAFD_RE.match(line)
        if m:
            cur_mafd = float(m.group(1))
            continue

        m = SCD_SCORE_RE.match(line)
        if m:
            cur_score = float(m.group(1))

    if cur_num >= 0:
        frames.append(FrameScore(cur_num, cur_pts, cur_mafd, cur_score))

    meta_path.unlink(missing_ok=True)

    n_transitions = sum(1 for f in frames if f.mafd > transition_threshold)
    print(
        f"  {len(frames)} frames analyzed, {n_transitions} transitions "
        f"(MAFD > {transition_threshold:.3f})"
    )

    return frames


@dataclass
class FrameCycleResult:
    """Frame-based timing for one benchmark cycle."""

    # Open path
    open_pts: Optional[float] = None  # first frame TD visible
    open_mafd: float = 0.0
    open_settle_pts: Optional[float] = None  # last frame of open render activity
    # Close path
    close_pts: Optional[float] = None  # first frame TD gone
    close_mafd: float = 0.0
    close_settle_pts: Optional[float] = None  # last frame of close render activity


def _match_monotonic(
    expected: List[float],
    transitions: List[float],
) -> Tuple[float, List[int], List[float]]:
    """Minimum-cost monotonic event matching (expected events → transitions)."""
    n = len(expected)
    m = len(transitions)
    if n == 0 or m == 0 or m < n:
        return float("inf"), [], []

    inf = float("inf")
    dp = [[inf] * (m + 1) for _ in range(n + 1)]
    take = [[False] * (m + 1) for _ in range(n + 1)]

    for j in range(m + 1):
        dp[0][j] = 0.0

    for i in range(1, n + 1):
        for j in range(1, m + 1):
            best = dp[i][j - 1]
            took = False
            use_cost = dp[i - 1][j - 1] + abs(expected[i - 1] - transitions[j - 1])
            if use_cost < best:
                best = use_cost
                took = True
            dp[i][j] = best
            take[i][j] = took

    j_best = min(range(n, m + 1), key=lambda j: dp[n][j])
    total_cost = dp[n][j_best]
    if not math.isfinite(total_cost):
        return float("inf"), [], []

    i = n
    j = j_best
    indices: List[int] = []
    while i > 0 and j > 0:
        if take[i][j]:
            indices.append(j - 1)
            i -= 1
            j -= 1
        else:
            j -= 1
    indices.reverse()

    if len(indices) != n:
        return float("inf"), [], []

    distances = [abs(expected[k] - transitions[idx]) for k, idx in enumerate(indices)]
    return total_cost, indices, distances


def infer_activity_threshold(
    frames: List[FrameScore],
    transition_threshold: float,
) -> float:
    """Infer activity threshold from observed run noise floor."""
    quiet = [f.mafd for f in frames if 0.0 <= f.mafd < transition_threshold]
    if len(quiet) < 30:
        return DEFAULT_MAFD_ACTIVITY_FLOOR

    p90 = percentile_nearest_rank(quiet, 0.90) or DEFAULT_MAFD_ACTIVITY_FLOOR
    p99 = percentile_nearest_rank(quiet, 0.99) or p90
    inferred = max(DEFAULT_MAFD_ACTIVITY_FLOOR, p90 * 1.8, p99 * 1.1)
    return min(inferred, max(DEFAULT_MAFD_ACTIVITY_FLOOR, transition_threshold * 0.5))


def calibrate_clock_offset(
    frames: List[FrameScore],
    cycles: List[CycleResult],
    recording_start: float,
    transition_threshold: float,
    max_error_ms: float,
    median_error_ms: float,
) -> ClockCalibration:
    """Calibrate perf_counter↔PTS offset with monotonic matching + fit checks."""
    cal = ClockCalibration()
    if not frames or not cycles:
        cal.reason = "missing frames or cycles"
        return cal

    transitions = sorted(
        (f for f in frames if f.mafd > transition_threshold),
        key=lambda f: f.pts_time,
    )
    if not transitions:
        cal.reason = "no transitions above threshold"
        return cal

    expected: List[float] = []
    for c in cycles:
        expected.append(c.td_window - recording_start)
        expected.append(c.td_gone - recording_start)
    if not expected:
        cal.reason = "no expected events"
        return cal

    # Limit transition set for fitting: keep strongest candidates and sort by time.
    keep_n = min(len(transitions), max(300, len(expected) * 15))
    strongest = sorted(transitions, key=lambda f: f.mafd, reverse=True)[:keep_n]
    strongest = sorted(strongest, key=lambda f: f.pts_time)
    t_pts = [t.pts_time for t in strongest]

    # Candidate offsets: anchor each expected event to top-M strongest transitions.
    anchor_n = min(len(strongest), max(80, len(expected) * 2))
    anchors = sorted(strongest, key=lambda f: f.mafd, reverse=True)[:anchor_n]
    anchor_pts = [a.pts_time for a in anchors]

    best_cost = float("inf")
    best_offset = 0.0
    best_distances: List[float] = []

    for tp in anchor_pts:
        for ep in expected:
            candidate = tp - ep
            shifted = [e + candidate for e in expected]
            cost, _, distances = _match_monotonic(shifted, t_pts)
            if cost < best_cost:
                best_cost = cost
                best_offset = candidate
                best_distances = distances

    if not best_distances:
        cal.reason = "offset fit failed"
        return cal

    cal.offset_s = best_offset
    cal.total_cost_s = best_cost
    cal.matched_events = len(best_distances)
    cal.max_error_s = max(best_distances)
    cal.median_error_s = statistics.median(best_distances)

    if cal.matched_events != len(expected):
        cal.reason = f"matched {cal.matched_events}/{len(expected)} events"
        return cal

    max_error_s = max_error_ms / 1000.0
    median_error_s = median_error_ms / 1000.0
    if cal.max_error_s > max_error_s:
        cal.reason = (
            f"max alignment error {cal.max_error_s*1000:.1f}ms "
            f"> {max_error_ms:.1f}ms"
        )
        return cal
    if cal.median_error_s > median_error_s:
        cal.reason = (
            f"median alignment error {cal.median_error_s*1000:.1f}ms "
            f"> {median_error_ms:.1f}ms"
        )
        return cal

    cal.valid = True
    cal.reason = "ok"
    return cal


def _find_transition_near(
    frames: List[FrameScore],
    anchor_pts: float,
    transition_threshold: float,
    search_radius: float = 0.050,
) -> Optional[FrameScore]:
    """Find the highest-MAFD frame within ±search_radius of anchor_pts.

    CGWindowList gives sub-ms detection of window appear/disappear.
    We use that as an anchor and find which captured video frame has the
    biggest visual change nearby. This avoids false positives from terminal
    text updates (MAFD 0.3-0.8) that happen far from the actual transition.
    """
    best: Optional[FrameScore] = None
    for f in frames:
        if f.pts_time < anchor_pts - search_radius:
            continue
        if f.pts_time > anchor_pts + search_radius:
            break
        if f.mafd > transition_threshold and (best is None or f.mafd > best.mafd):
            best = f
    return best


def correlate_frames(
    frames: List[FrameScore],
    cycles: List[CycleResult],
    recording_start: float,
    clock_offset: float,
    transition_threshold: float,
    activity_threshold: float,
    quiet_frames: int,
    search_radius_ms: float,
) -> List[FrameCycleResult]:
    """Match frame transitions to benchmark cycles and find settle points.

    Uses CGWindowList timing as anchor (sub-ms precision) to locate
    transitions in the video, then measures render settle from there.
    This avoids false-positive matches on terminal text updates.

    For each cycle:
    1. Open transition: highest MAFD near td_window time (±50ms)
    2. Open settle: last active frame before render goes quiet (→ visual ready)
    3. Close transition: highest MAFD near td_gone time (±50ms)
    4. Close settle: last active frame before Ghostty redraw goes quiet
    """
    results: List[FrameCycleResult] = []
    search_radius = max(0.0, search_radius_ms / 1000.0)

    for ci, cycle in enumerate(cycles):
        fcr = FrameCycleResult()

        # Map perf_counter timestamps to PTS coordinates
        ctrlg_pts = (cycle.ctrlg_sent - recording_start) + clock_offset
        td_window_pts = (cycle.td_window - recording_start) + clock_offset
        cmdq_pts = (cycle.cmdq_sent - recording_start) + clock_offset
        td_gone_pts = (cycle.td_gone - recording_start) + clock_offset

        # Boundary for close settle: next cycle's Ctrl+G or end of video
        if ci + 1 < len(cycles):
            next_ctrlg_pts = (cycles[ci + 1].ctrlg_sent - recording_start) + clock_offset
        else:
            next_ctrlg_pts = frames[-1].pts_time + 1.0 if frames else cmdq_pts + 1.0

        # Open: highest MAFD spike near CGWindowList detection time
        hit = _find_transition_near(
            frames,
            td_window_pts,
            transition_threshold,
            search_radius=search_radius,
        )
        if hit:
            fcr.open_pts = hit.pts_time
            fcr.open_mafd = hit.mafd

        # Open settle: last active frame before quiet, bounded by Cmd+Q
        if fcr.open_pts is not None:
            quiet_count = 0
            last_active_pts = fcr.open_pts
            for f in frames:
                if f.pts_time <= fcr.open_pts:
                    continue
                if f.pts_time > cmdq_pts:
                    break
                if f.mafd > activity_threshold:
                    last_active_pts = f.pts_time
                    quiet_count = 0
                else:
                    quiet_count += 1
                    if quiet_count >= quiet_frames:
                        break
            fcr.open_settle_pts = last_active_pts

        # Close: highest MAFD spike near CGWindowList gone time
        hit = _find_transition_near(
            frames,
            td_gone_pts,
            transition_threshold,
            search_radius=search_radius,
        )
        if hit:
            fcr.close_pts = hit.pts_time
            fcr.close_mafd = hit.mafd

        # Close settle: last active frame before quiet, bounded by next cycle
        if fcr.close_pts is not None:
            quiet_count = 0
            last_active_pts = fcr.close_pts
            for f in frames:
                if f.pts_time <= fcr.close_pts:
                    continue
                if f.pts_time > next_ctrlg_pts:
                    break
                if f.mafd > activity_threshold:
                    last_active_pts = f.pts_time
                    quiet_count = 0
                else:
                    quiet_count += 1
                    if quiet_count >= quiet_frames:
                        break
            fcr.close_settle_pts = last_active_pts

        results.append(fcr)

    return results


# ── Frame extraction for visual analysis ─────────────────────────────────────


def extract_transition_frames(
    video_path: Path,
    frames: List[FrameScore],
    cycles: List[CycleResult],
    frame_cycles: List[FrameCycleResult],
    recording_start: float,
    clock_offset: float,
    transition_threshold: float,
    activity_threshold: float,
) -> Optional[Path]:
    """Extract PNG frames around each detected transition for visual analysis.

    Uses MAFD spikes to identify transition regions, then extracts frames
    from a few frames before the keystroke press through the settle point.
    Each frame is named with its timing relative to the keystroke so an LLM
    can determine exactly when TurboDraft appears/disappears/settles.
    """
    if not frames or not cycles or not frame_cycles:
        return None

    output_dir = video_path.parent / (video_path.stem + "_frames")
    output_dir.mkdir(exist_ok=True)

    PAD_FRAMES = 3  # frames before keystroke / after settle
    FRAME_DT = 1 / 60  # ~16.7ms

    by_num = {f.frame_num: f for f in frames}
    sorted_by_pts = sorted(frames, key=lambda f: f.pts_time)

    # Collect frames to extract: frame_num -> (label, keystroke_pts)
    extract_info: dict = {}

    for ci, cycle in enumerate(cycles):
        if ci >= len(frame_cycles):
            break
        fc = frame_cycles[ci]
        ctrlg_pts = (cycle.ctrlg_sent - recording_start) + clock_offset
        cmdq_pts = (cycle.cmdq_sent - recording_start) + clock_offset

        # Open path: from before keystroke through settle
        if fc.open_pts is not None:
            start = ctrlg_pts - PAD_FRAMES * FRAME_DT
            end = (fc.open_settle_pts or fc.open_pts) + PAD_FRAMES * FRAME_DT
            for f in sorted_by_pts:
                if f.pts_time < start:
                    continue
                if f.pts_time > end:
                    break
                if f.frame_num not in extract_info:
                    extract_info[f.frame_num] = (f"open_c{ci+1:02d}", ctrlg_pts)

        # Close path: from before keystroke through settle
        if fc.close_pts is not None:
            start = cmdq_pts - PAD_FRAMES * FRAME_DT
            end = (fc.close_settle_pts or fc.close_pts) + PAD_FRAMES * FRAME_DT
            for f in sorted_by_pts:
                if f.pts_time < start:
                    continue
                if f.pts_time > end:
                    break
                if f.frame_num not in extract_info:
                    extract_info[f.frame_num] = (f"close_c{ci+1:02d}", cmdq_pts)

    if not extract_info:
        return None

    sorted_nums = sorted(extract_info.keys())
    select_expr = "+".join(f"eq(n\\,{n})" for n in sorted_nums)

    print(f"Extracting {len(sorted_nums)} transition frames...")
    subprocess.run(
        [
            "ffmpeg", "-y",
            "-i", str(video_path),
            "-vf", f"select={select_expr},scale=1280:-1",
            "-vsync", "vfr",
            "-q:v", "2",
            str(output_dir / "tmp_%04d.png"),
        ],
        capture_output=True,
        timeout=60,
    )

    # Rename with timing info and build manifest
    manifest = [
        "# Transition Frames — Visual Verification",
        "#",
        f"# {'Filename':45s}  {'PTS':>8s}  {'MAFD':>7s}  Notes",
    ]

    for idx, fn in enumerate(sorted_nums):
        tmp = output_dir / f"tmp_{idx+1:04d}.png"
        if not tmp.exists():
            continue

        f = by_num[fn]
        label, keystroke_pts = extract_info[fn]
        delta_ms = (f.pts_time - keystroke_pts) * 1000
        sign = "+" if delta_ms >= 0 else "-"
        abs_ms = abs(delta_ms)

        fname = f"{label}_{sign}{abs_ms:03.0f}ms_f{fn:04d}.png"
        tmp.rename(output_dir / fname)

        note = ""
        if f.mafd > transition_threshold:
            note = "TRANSITION"
        elif f.mafd > activity_threshold:
            note = "activity"

        manifest.append(
            f"  {fname:45s}  {f.pts_time:>7.3f}s  {f.mafd:>6.3f}  {note}"
        )

    (output_dir / "manifest.txt").write_text("\n".join(manifest) + "\n")
    print(f"  {output_dir}/")

    return output_dir


# ── Main benchmark ───────────────────────────────────────────────────────────


@dataclass
class CycleResult:
    """Timing data for one open/close cycle."""

    ctrlg_sent: float = 0.0
    td_window: float = 0.0
    cmdq_sent: float = 0.0
    td_gone: float = 0.0


@dataclass
class BenchResult:
    """Full benchmark results."""

    cycles: List[CycleResult] = field(default_factory=list)
    log_cycles: List[LogCycle] = field(default_factory=list)
    poll_resolution_ms: float = 0.0
    errors: List[str] = field(default_factory=list)
    # Frame analysis (only with --record)
    frame_scores: List[FrameScore] = field(default_factory=list)
    frame_cycles: List[FrameCycleResult] = field(default_factory=list)
    recording_start: float = 0.0
    clock_offset: float = 0.0
    calibration: Optional[ClockCalibration] = None
    transition_threshold: float = DEFAULT_MAFD_TRANSITION
    activity_threshold: float = DEFAULT_MAFD_ACTIVITY_FLOOR
    quiet_frames: int = DEFAULT_QUIET_FRAMES
    search_radius_ms: float = DEFAULT_SEARCH_RADIUS_MS
    recording_path: Optional[Path] = None
    frame_dir: Optional[Path] = None
    metadata: dict[str, Any] = field(default_factory=dict)


def collect_metadata(n_cycles: int, dwell_ms: int, record: bool) -> dict[str, Any]:
    """Capture reproducibility metadata for this run."""
    return {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "platform": platform.platform(),
        "mac_ver": platform.mac_ver()[0],
        "python_version": sys.version.split()[0],
        "hostname": socket.gethostname(),
        "ffmpeg_version": ffmpeg_version(),
        "requested_cycles": n_cycles,
        "requested_dwell_ms": dwell_ms,
        "record": record,
        "frontmost_owner_start": frontmost_owner(),
        "window_owner_snapshot_start": window_owner_snapshot(),
        "turbodraft_owner_names": sorted(TD_NAMES),
        "ghostty_owner_names": sorted(GHOSTTY_NAMES),
    }


def run_benchmark(
    n_cycles: int,
    dwell_ms: int,
    record: bool,
    cfg: FrameAnalysisConfig,
    start_delay_s: float = 0.0,
    open_timeout_s: float = 10.0,
    close_timeout_s: float = 10.0,
    cycle_retries: int = 1,
    inter_cycle_delay_ms: int = 120,
) -> BenchResult:
    result = BenchResult()
    result.metadata = collect_metadata(n_cycles, dwell_ms, record)
    result.metadata["start_delay_s"] = start_delay_s
    result.metadata["open_timeout_s"] = open_timeout_s
    result.metadata["close_timeout_s"] = close_timeout_s
    result.metadata["cycle_retries"] = cycle_retries
    result.metadata["inter_cycle_delay_ms"] = inter_cycle_delay_ms
    result.transition_threshold = cfg.transition_threshold
    result.quiet_frames = cfg.quiet_frames
    result.search_radius_ms = cfg.search_radius_ms

    # Step 0: Calibrate poll overhead
    print("Calibrating CGWindowList poll overhead...")
    result.poll_resolution_ms = measure_poll_overhead(100)
    print(f"  poll resolution: {result.poll_resolution_ms:.2f}ms")

    if start_delay_s > 0:
        print(f"Delaying start for {start_delay_s:.1f}s so you can switch windows...")
        time.sleep(start_delay_s)

    # Step 1: Activate Ghostty (one-time osascript, outside measurement)
    print("Activating Ghostty...")
    subprocess.run(
        ["osascript", "-e", 'tell application "ghostty" to activate'],
        capture_output=True,
        timeout=5,
    )
    time.sleep(0.5)

    # Step 2: Verify Ghostty is frontmost
    if not is_ghostty_frontmost():
        print("WARNING: Ghostty does not appear to be the frontmost window.")
        print("Please focus Ghostty and press Enter to continue...")
        input()
        if not is_ghostty_frontmost():
            print("Ghostty still not frontmost. Proceeding anyway.", file=sys.stderr)
    result.metadata["frontmost_owner_after_activation"] = frontmost_owner()
    result.metadata["window_owner_snapshot_after_activation"] = window_owner_snapshot()

    # Step 3: Truncate log
    LOG_PATH.write_text("")

    # Step 4: Start recording if requested
    recorder = None
    if record:
        rec_path = Path(f"/tmp/ctrl_g_bench_{int(time.time())}.mov")
        result.recording_path = rec_path
        print(f"Recording to {rec_path}")
        recorder = start_recording(rec_path)
        result.recording_start = time.perf_counter()
        print(f"  ffmpeg warmup ({FFMPEG_WARMUP_S:.0f}s)...")
        time.sleep(FFMPEG_WARMUP_S)

    # Step 5: Run cycles
    try:
        for i in range(n_cycles):
            ok = False
            last_err: Optional[Exception] = None
            for attempt in range(cycle_retries + 1):
                cycle = CycleResult()
                try:
                    # Recovery: close stale TurboDraft window before starting.
                    if td_window_count() > 0:
                        _ = post_keystroke(VK_Q, kCGEventFlagMaskCommand)
                        try:
                            poll_for_td_gone(timeout=min(2.0, close_timeout_s))
                        except TimeoutError:
                            pass
                        time.sleep(0.1)

                    # Verify Ghostty is frontmost
                    if not is_ghostty_frontmost():
                        subprocess.run(
                            ["osascript", "-e", 'tell application "ghostty" to activate'],
                            capture_output=True,
                            timeout=5,
                        )
                        time.sleep(0.3)

                    time.sleep(max(0.01, inter_cycle_delay_ms / 1000.0))

                    # Send Ctrl+G
                    cycle.ctrlg_sent = post_keystroke(VK_G, kCGEventFlagMaskControl)

                    # Poll for TurboDraft window
                    cycle.td_window = poll_for_td_window(timeout=open_timeout_s)

                    # Dwell
                    time.sleep(dwell_ms / 1000.0)

                    # Send Cmd+Q to close TurboDraft
                    cycle.cmdq_sent = post_keystroke(VK_Q, kCGEventFlagMaskCommand)

                    # Poll for TurboDraft gone
                    cycle.td_gone = poll_for_td_gone(timeout=close_timeout_s)

                    result.cycles.append(cycle)
                    open_ms = (cycle.td_window - cycle.ctrlg_sent) * 1000
                    close_ms = (cycle.td_gone - cycle.cmdq_sent) * 1000
                    msg = (
                        f"  cycle {i + 1}/{n_cycles}: open={open_ms:.0f}ms  close={close_ms:.0f}ms"
                    )
                    if attempt > 0:
                        msg += f"  (retry {attempt}/{cycle_retries})"
                    print(msg)
                    ok = True
                    break

                except (TimeoutError, RuntimeError) as e:
                    last_err = e
                    # Recovery: try focus reset and close any lingering TD window.
                    try:
                        subprocess.run(
                            ["osascript", "-e", 'tell application "ghostty" to activate'],
                            capture_output=True,
                            timeout=5,
                        )
                    except Exception:
                        pass
                    if td_window_count() > 0:
                        _ = post_keystroke(VK_Q, kCGEventFlagMaskCommand)
                        time.sleep(0.1)
                    if attempt < cycle_retries:
                        print(
                            f"  cycle {i + 1}/{n_cycles}: retry {attempt + 1}/{cycle_retries} "
                            f"after error: {e}"
                        )
                        time.sleep(0.6)

            if not ok and last_err is not None:
                result.errors.append(f"cycle {i + 1}: {last_err}")
                print(f"  cycle {i + 1}/{n_cycles}: ERROR — {last_err}")
    finally:
        if recorder:
            stop_recording(recorder)

    # Step 6: Let final log entries flush
    time.sleep(0.2)
    result.metadata["successful_cycles"] = len(result.cycles)
    result.metadata["error_count"] = len(result.errors)
    result.metadata["window_owner_snapshot_end"] = window_owner_snapshot()

    # Step 7: Parse log
    result.log_cycles = parse_log(LOG_PATH)

    # Step 8: Frame analysis
    if record and result.recording_path and result.recording_path.exists() and result.cycles:
        result.frame_scores = analyze_recording(
            result.recording_path,
            transition_threshold=cfg.transition_threshold,
        )
        if result.frame_scores:
            result.calibration = calibrate_clock_offset(
                result.frame_scores,
                result.cycles,
                result.recording_start,
                transition_threshold=cfg.transition_threshold,
                max_error_ms=cfg.clock_max_error_ms,
                median_error_ms=cfg.clock_median_error_ms,
            )
            result.clock_offset = result.calibration.offset_s
            print(f"  clock offset: {result.clock_offset * 1000:.0f}ms")
            print(
                "  alignment fit: "
                f"median={result.calibration.median_error_s*1000:.1f}ms "
                f"max={result.calibration.max_error_s*1000:.1f}ms "
                f"status={'ok' if result.calibration.valid else 'invalid'}"
            )

            if cfg.activity_threshold is None:
                result.activity_threshold = infer_activity_threshold(
                    result.frame_scores,
                    cfg.transition_threshold,
                )
            else:
                result.activity_threshold = cfg.activity_threshold

            print(
                f"  activity threshold: {result.activity_threshold:.3f} "
                f"({'auto' if cfg.activity_threshold is None else 'fixed'})"
            )

            if result.calibration.valid:
                result.frame_cycles = correlate_frames(
                    result.frame_scores,
                    result.cycles,
                    result.recording_start,
                    result.clock_offset,
                    transition_threshold=cfg.transition_threshold,
                    activity_threshold=result.activity_threshold,
                    quiet_frames=cfg.quiet_frames,
                    search_radius_ms=cfg.search_radius_ms,
                )

                # Step 9: Extract transition frames for visual analysis
                if result.frame_cycles:
                    result.frame_dir = extract_transition_frames(
                        result.recording_path,
                        result.frame_scores,
                        result.cycles,
                        result.frame_cycles,
                        result.recording_start,
                        result.clock_offset,
                        transition_threshold=cfg.transition_threshold,
                        activity_threshold=result.activity_threshold,
                    )
            else:
                msg = (
                    "frame analysis disabled: "
                    f"clock fit invalid ({result.calibration.reason})"
                )
                print(f"  WARNING: {msg}")
                result.errors.append(msg)

    return result


def save_json(
    result: BenchResult,
    n_cycles: int,
    dwell_ms: int,
    path: Path,
    warmup_cycles: int = 1,
) -> None:
    """Save raw per-cycle data as JSON."""
    origin = result.cycles[0].ctrlg_sent if result.cycles else 0.0

    cycles_data = []
    for i, cycle in enumerate(result.cycles):
        entry = {
            "ctrlg_sent": (cycle.ctrlg_sent - origin) * 1000,
            "td_window": (cycle.td_window - origin) * 1000,
            "cmdq_sent": (cycle.cmdq_sent - origin) * 1000,
            "td_gone": (cycle.td_gone - origin) * 1000,
        }
        if i < len(result.log_cycles):
            lc = result.log_cycles[i]
            if lc.cc_overhead_ms is not None:
                entry["cc_overhead_ms"] = lc.cc_overhead_ms
            if lc.open_path_ms is not None:
                entry["pager_open_ms"] = lc.open_path_ms
            if lc.close_path_ms is not None:
                entry["pager_close_ms"] = lc.close_path_ms
            if lc.pager_prerender_done_ms is not None:
                entry["pager_prerender_done_ms"] = lc.pager_prerender_done_ms
            if lc.pager_parse_ms is not None:
                entry["pager_parse_ms"] = lc.pager_parse_ms
            if lc.pager_render_ms is not None:
                entry["pager_render_ms"] = lc.pager_render_ms
            if lc.pager_first_draw_done_ms is not None:
                entry["pager_first_draw_done_ms"] = lc.pager_first_draw_done_ms
            if lc.pager_termready_tcdrain_ms is not None:
                entry["pager_termready_tcdrain_ms"] = lc.pager_termready_tcdrain_ms
            if lc.pager_termready_dsr_ms is not None:
                entry["pager_termready_dsr_ms"] = lc.pager_termready_dsr_ms
            if lc.pager_termready_total_ms is not None:
                entry["pager_termready_total_ms"] = lc.pager_termready_total_ms
        if i < len(result.frame_cycles):
            fc = result.frame_cycles[i]
            if fc.open_pts is not None:
                entry["frame_open_pts_ms"] = round(fc.open_pts * 1000, 1)
            if fc.open_settle_pts is not None:
                entry["frame_open_settle_pts_ms"] = round(fc.open_settle_pts * 1000, 1)
            if fc.close_pts is not None:
                entry["frame_close_pts_ms"] = round(fc.close_pts * 1000, 1)
            if fc.close_settle_pts is not None:
                entry["frame_close_settle_pts_ms"] = round(fc.close_settle_pts * 1000, 1)
        cycles_data.append(entry)

    data = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "cycles": n_cycles,
        "dwell_ms": dwell_ms,
        "warmup_cycles": warmup_cycles,
        "successful_cycles": len(result.cycles),
        "errors": result.errors,
        "poll_resolution_ms": round(result.poll_resolution_ms, 3),
        "recorded": bool(result.recording_path),
        "clock_offset_ms": round(result.clock_offset * 1000, 1) if result.recording_path else None,
        "analysis": {
            "transition_threshold": round(result.transition_threshold, 6),
            "activity_threshold": round(result.activity_threshold, 6),
            "quiet_frames": result.quiet_frames,
            "search_radius_ms": round(result.search_radius_ms, 3),
            "pager_bench_probe_seen": any(
                lc.pager_termready_total_ms is not None for lc in result.log_cycles
            ),
        },
        "results": cycles_data,
        "metadata": result.metadata,
    }
    if result.calibration:
        data["clock_fit"] = {
            "valid": result.calibration.valid,
            "reason": result.calibration.reason,
            "matched_events": result.calibration.matched_events,
            "median_error_ms": round(result.calibration.median_error_s * 1000, 3),
            "max_error_ms": round(result.calibration.max_error_s * 1000, 3),
            "total_cost_ms": round(result.calibration.total_cost_s * 1000, 3),
        }
    if result.recording_path:
        data["recording"] = str(result.recording_path)
    if result.frame_dir:
        data["frames"] = str(result.frame_dir)

    path.write_text(json.dumps(data, indent=2) + "\n")


def print_report(
    result: BenchResult,
    n_cycles: int,
    dwell_ms: int,
    warmup_cycles: int = 1,
) -> None:
    n = len(result.cycles)
    if n == 0:
        print("\nNo successful cycles. Cannot generate report.")
        return

    warmup_eff = warmup_cycles
    if warmup_eff >= n:
        print(
            f"\nWARNING: warmup_cycles={warmup_eff} >= successful cycles={n}; "
            "using all cycles for report."
        )
        warmup_eff = 0
    measured_n = n - warmup_eff

    # Compute per-cycle metrics
    open_totals: List[float] = []
    close_totals: List[float] = []
    cc_overheads: List[float] = []
    pager_opens: List[float] = []
    td_renders: List[float] = []
    pager_closes: List[float] = []
    td_cc_redraws: List[float] = []
    pager_prerender_done: List[float] = []
    pager_parse: List[float] = []
    pager_markdown_render: List[float] = []
    pager_first_draw_done: List[float] = []
    pager_termready_tcdrain: List[float] = []
    pager_termready_dsr: List[float] = []
    pager_termready_total: List[float] = []

    log_matched = len(result.log_cycles) >= n

    for i, cycle in enumerate(result.cycles):
        if i < warmup_eff:
            continue
        open_ms = (cycle.td_window - cycle.ctrlg_sent) * 1000
        close_ms = (cycle.td_gone - cycle.cmdq_sent) * 1000
        open_totals.append(open_ms)
        close_totals.append(close_ms)

        if log_matched and i < len(result.log_cycles):
            lc = result.log_cycles[i]

            if lc.cc_overhead_ms is not None:
                cc_overheads.append(lc.cc_overhead_ms)

            if lc.open_path_ms is not None:
                pager_opens.append(lc.open_path_ms)

            # TurboDraft render = total open - CC overhead - pager open
            cc = lc.cc_overhead_ms or 0
            po = lc.open_path_ms or 0
            remainder = open_ms - cc - po
            if remainder >= 0:
                td_renders.append(remainder)

            if lc.close_path_ms is not None:
                pager_closes.append(lc.close_path_ms)

            # TurboDraft hide + CC redraw = total close - pager close
            pc = lc.close_path_ms or 0
            close_remainder = close_ms - pc
            if close_remainder >= 0:
                td_cc_redraws.append(close_remainder)

            if lc.pager_prerender_done_ms is not None:
                pager_prerender_done.append(lc.pager_prerender_done_ms)
            if lc.pager_parse_ms is not None:
                pager_parse.append(lc.pager_parse_ms)
            if lc.pager_render_ms is not None:
                pager_markdown_render.append(lc.pager_render_ms)
            if lc.pager_first_draw_done_ms is not None:
                pager_first_draw_done.append(lc.pager_first_draw_done_ms)
            if lc.pager_termready_tcdrain_ms is not None:
                pager_termready_tcdrain.append(lc.pager_termready_tcdrain_ms)
            if lc.pager_termready_dsr_ms is not None:
                pager_termready_dsr.append(lc.pager_termready_dsr_ms)
            if lc.pager_termready_total_ms is not None:
                pager_termready_total.append(lc.pager_termready_total_ms)

    # Header
    print(f"\nCtrl+G Benchmark — {n} cycles, {dwell_ms}ms dwell")
    print(
        f"headline metrics: cycles {warmup_eff + 1}..{n} "
        f"(excluded warmup={warmup_eff}, measured={measured_n})"
    )
    print(f"poll resolution: {result.poll_resolution_ms:.1f}ms")
    print(
        "frame config: "
        f"transition>{result.transition_threshold:.3f}, "
        f"activity>{result.activity_threshold:.3f}, "
        f"quiet_frames={result.quiet_frames}, "
        f"search_radius={result.search_radius_ms:.0f}ms"
    )
    if result.errors:
        print(f"errors: {len(result.errors)} (see below)")

    # Open path table
    print("\n=== OPEN PATH (Ctrl+G → TurboDraft on screen) ===")
    print(f"{'':30s} {'Min':>7s}  {'Median':>7s}  {'Max':>7s}  {'p95':>7s}")
    _print_row("Total (keypress→window)", open_totals)
    if cc_overheads:
        _print_row("  CC exec overhead", cc_overheads)
    if pager_opens:
        _print_row("  claude-pager-open", pager_opens)
    if td_renders:
        _print_row("  TurboDraft render", td_renders)

    # Close path table
    print("\n=== CLOSE PATH (Cmd+Q → TurboDraft gone) ===")
    print(f"{'':30s} {'Min':>7s}  {'Median':>7s}  {'Max':>7s}  {'p95':>7s}")
    _print_row("Total (keypress→gone)", close_totals)
    if pager_closes:
        _print_row("  claude-pager-close", pager_closes)
    if td_cc_redraws:
        _print_row("  TurboDraft hide + CC", td_cc_redraws)

    if (
        pager_prerender_done
        or pager_parse
        or pager_markdown_render
        or pager_first_draw_done
    ):
        print("\n=== CLAUDE-PAGER RENDER STAGES (C instrumentation) ===")
        print(f"{'':30s} {'Min':>7s}  {'Median':>7s}  {'Max':>7s}  {'p95':>7s}")
        if pager_prerender_done:
            _print_row("Pre-render done (timestamp)", pager_prerender_done)
        if pager_parse:
            _print_row("Parse transcript (duration)", pager_parse)
        if pager_markdown_render:
            _print_row("Markdown render (duration)", pager_markdown_render)
        if pager_first_draw_done:
            _print_row("First draw done (timestamp)", pager_first_draw_done)
        if pager_termready_tcdrain:
            _print_row("tcdrain (duration)", pager_termready_tcdrain)
        if pager_termready_dsr:
            _print_row("DSR reply wait (duration)", pager_termready_dsr)
        if pager_termready_total:
            _print_row("Terminal ready probe total", pager_termready_total)
        elif pager_first_draw_done:
            print("  note: terminal-ready probe disabled (set env.CLAUDE_PAGER_BENCH=1 in ~/.claude/settings.json)")

    # Frame analysis + TTFTC
    if result.frame_cycles:
        _print_frame_analysis(result, warmup_cycles=warmup_eff)

    if not log_matched:
        print(f"\nWARNING: Log had {len(result.log_cycles)} cycles but script ran {n}.")
        print("Component breakdown may be inaccurate.")

    if result.errors:
        print(f"\n=== ERRORS ({len(result.errors)}) ===")
        for err in result.errors:
            print(f"  {err}")


def _print_frame_analysis(result: BenchResult, warmup_cycles: int = 1) -> None:
    """Print frame analysis section with settle times for both paths."""
    n_total = len(result.cycles)
    if n_total == 0:
        return
    warmup_eff = min(max(0, warmup_cycles), max(0, n_total - 1))
    n = n_total - warmup_eff
    fc = result.frame_cycles

    frame_open: List[float] = []
    open_settle: List[float] = []  # first paint → settled
    visual_ready: List[float] = []  # Ctrl+G → settled
    frame_close: List[float] = []
    close_settle: List[float] = []  # first close frame → settled
    close_ready: List[float] = []  # Cmd+Q → settled

    for i, cycle in enumerate(result.cycles):
        if i < warmup_eff:
            continue
        if i >= len(fc):
            break
        f = fc[i]
        ctrlg_pts = (cycle.ctrlg_sent - result.recording_start) + result.clock_offset
        cmdq_pts = (cycle.cmdq_sent - result.recording_start) + result.clock_offset

        if f.open_pts is not None:
            frame_open_ms = (f.open_pts - ctrlg_pts) * 1000
            if frame_open_ms >= 0:
                frame_open.append(frame_open_ms)

        if f.open_settle_pts is not None and f.open_pts is not None:
            vr_ms = (f.open_settle_pts - ctrlg_pts) * 1000
            if vr_ms >= 0:
                visual_ready.append(vr_ms)
            settle_ms = (f.open_settle_pts - f.open_pts) * 1000
            if settle_ms >= 0:
                open_settle.append(settle_ms)

        if f.close_pts is not None:
            frame_close_ms = (f.close_pts - cmdq_pts) * 1000
            if frame_close_ms >= 0:
                frame_close.append(frame_close_ms)

        if f.close_settle_pts is not None and f.close_pts is not None:
            cr_ms = (f.close_settle_pts - cmdq_pts) * 1000
            if cr_ms >= 0:
                close_ready.append(cr_ms)
            cs_ms = (f.close_settle_pts - f.close_pts) * 1000
            if cs_ms >= 0:
                close_settle.append(cs_ms)

    open_matched = len(frame_open)
    close_matched = len(frame_close)

    print(f"\n=== FRAME ANALYSIS (60fps, {16.7:.1f}ms precision) ===")
    print(f"clock offset: {result.clock_offset * 1000:.0f}ms")
    if result.calibration:
        print(
            "clock fit: "
            f"median={result.calibration.median_error_s*1000:.1f}ms "
            f"max={result.calibration.max_error_s*1000:.1f}ms "
            f"status={'ok' if result.calibration.valid else 'invalid'}"
        )
    print(f"transitions matched: {open_matched}/{n} open, {close_matched}/{n} close")

    hdr = f"{'':30s} {'Min':>7s}  {'Median':>7s}  {'Max':>7s}  {'p95':>7s}"

    if frame_open or visual_ready:
        print(f"\n{hdr}")
    if frame_open:
        _print_row("First paint (Ctrl+G→frame)", frame_open)
    if open_settle:
        _print_row("  Render settle", open_settle)
    if visual_ready:
        _print_row("Visual ready (Ctrl+G→settled)", visual_ready)

    if frame_close or close_ready:
        if not frame_open and not visual_ready:
            print(f"\n{hdr}")
    if frame_close:
        _print_row("First close (Cmd+Q→frame)", frame_close)
    if close_settle:
        _print_row("  Redraw settle", close_settle)
    if close_ready:
        _print_row("Ghostty ready (Cmd+Q→settled)", close_ready)

    if result.recording_path:
        print(f"\nRecording: {result.recording_path}")
    if result.frame_dir:
        print(f"Frames: {result.frame_dir}/")


def _print_row(label: str, samples: List[float]) -> None:
    if not samples:
        return
    mn = min(samples)
    md = statistics.median(samples)
    mx = max(samples)
    p95 = percentile_nearest_rank(samples, 0.95)
    print(
        f"{label:30s} {fmt_ms(mn):>7s}  {fmt_ms(md):>7s}  {fmt_ms(mx):>7s}  {fmt_ms(p95):>7s}"
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Automated Ctrl+G latency benchmark for claude-pager + TurboDraft.",
    )
    ap.add_argument(
        "--cycles", type=int, default=20, help="Number of open/close cycles (default: 20)"
    )
    ap.add_argument(
        "--dwell", type=int, default=150, help="Ms to wait in TurboDraft before closing (default: 150)"
    )
    ap.add_argument(
        "--record", action="store_true", help="Record 60fps screen capture and run frame analysis"
    )
    ap.add_argument(
        "--json",
        type=str,
        default=None,
        help="Path to save raw JSON results (default: /tmp/ctrl_g_bench_<ts>.json)",
    )
    ap.add_argument(
        "--start-delay",
        type=float,
        default=0.0,
        help="Seconds to wait before benchmark actions begin (default: 0)",
    )
    ap.add_argument(
        "--open-timeout",
        type=float,
        default=10.0,
        help="Seconds to wait for TurboDraft window to appear (default: 10)",
    )
    ap.add_argument(
        "--close-timeout",
        type=float,
        default=10.0,
        help="Seconds to wait for TurboDraft window to disappear (default: 10)",
    )
    ap.add_argument(
        "--cycle-retries",
        type=int,
        default=1,
        help="Retries per cycle on transient failure (default: 1)",
    )
    ap.add_argument(
        "--inter-cycle-delay",
        type=int,
        default=120,
        help="Ms settle delay before each cycle keypress (default: 120)",
    )
    ap.add_argument(
        "--warmup-cycles",
        type=int,
        default=1,
        help="Exclude first N successful cycles from headline metrics (default: 1)",
    )
    ap.add_argument(
        "--transition-threshold",
        type=float,
        default=DEFAULT_MAFD_TRANSITION,
        help=f"MAFD threshold for transition detection (default: {DEFAULT_MAFD_TRANSITION})",
    )
    ap.add_argument(
        "--activity-threshold",
        type=float,
        default=None,
        help=(
            "MAFD threshold for settle/activity detection "
            "(default: auto-infer from noise floor)"
        ),
    )
    ap.add_argument(
        "--quiet-frames",
        type=int,
        default=DEFAULT_QUIET_FRAMES,
        help=f"Consecutive quiet frames to declare settled (default: {DEFAULT_QUIET_FRAMES})",
    )
    ap.add_argument(
        "--search-radius-ms",
        type=float,
        default=DEFAULT_SEARCH_RADIUS_MS,
        help=f"Anchor search radius around CGWindowList events (default: {DEFAULT_SEARCH_RADIUS_MS:.0f})",
    )
    ap.add_argument(
        "--clock-max-error-ms",
        type=float,
        default=DEFAULT_CLOCK_MAX_ERROR_MS,
        help=f"Reject frame analysis if clock-fit max error exceeds this (default: {DEFAULT_CLOCK_MAX_ERROR_MS:.0f})",
    )
    ap.add_argument(
        "--clock-median-error-ms",
        type=float,
        default=DEFAULT_CLOCK_MEDIAN_ERROR_MS,
        help=f"Reject frame analysis if clock-fit median error exceeds this (default: {DEFAULT_CLOCK_MEDIAN_ERROR_MS:.0f})",
    )
    args = ap.parse_args()

    if args.cycles < 1:
        print("--cycles must be >= 1", file=sys.stderr)
        return 1
    if args.dwell < 0:
        print("--dwell must be >= 0", file=sys.stderr)
        return 1
    if args.start_delay < 0:
        print("--start-delay must be >= 0", file=sys.stderr)
        return 1
    if args.open_timeout <= 0 or args.close_timeout <= 0:
        print("--open-timeout and --close-timeout must be > 0", file=sys.stderr)
        return 1
    if args.cycle_retries < 0:
        print("--cycle-retries must be >= 0", file=sys.stderr)
        return 1
    if args.inter_cycle_delay < 0:
        print("--inter-cycle-delay must be >= 0", file=sys.stderr)
        return 1
    if args.warmup_cycles < 0:
        print("--warmup-cycles must be >= 0", file=sys.stderr)
        return 1
    if args.transition_threshold <= 0:
        print("--transition-threshold must be > 0", file=sys.stderr)
        return 1
    if args.activity_threshold is not None and args.activity_threshold <= 0:
        print("--activity-threshold must be > 0", file=sys.stderr)
        return 1
    if args.quiet_frames < 1:
        print("--quiet-frames must be >= 1", file=sys.stderr)
        return 1
    if args.search_radius_ms <= 0:
        print("--search-radius-ms must be > 0", file=sys.stderr)
        return 1
    if args.clock_max_error_ms <= 0 or args.clock_median_error_ms <= 0:
        print("--clock-*-error-ms values must be > 0", file=sys.stderr)
        return 1

    cfg = FrameAnalysisConfig(
        transition_threshold=args.transition_threshold,
        activity_threshold=args.activity_threshold,
        quiet_frames=args.quiet_frames,
        search_radius_ms=args.search_radius_ms,
        clock_max_error_ms=args.clock_max_error_ms,
        clock_median_error_ms=args.clock_median_error_ms,
    )

    print(f"Running {args.cycles} cycles with {args.dwell}ms dwell...")
    result = run_benchmark(
        args.cycles,
        args.dwell,
        args.record,
        cfg=cfg,
        start_delay_s=args.start_delay,
        open_timeout_s=args.open_timeout,
        close_timeout_s=args.close_timeout,
        cycle_retries=args.cycle_retries,
        inter_cycle_delay_ms=args.inter_cycle_delay,
    )
    result.metadata["warmup_cycles"] = args.warmup_cycles
    print_report(result, args.cycles, args.dwell, warmup_cycles=args.warmup_cycles)

    # Save JSON
    if result.cycles:
        json_path = Path(args.json) if args.json else Path(f"/tmp/ctrl_g_bench_{int(time.time())}.json")
        save_json(
            result,
            args.cycles,
            args.dwell,
            json_path,
            warmup_cycles=args.warmup_cycles,
        )
        print(f"\nRaw data saved to {json_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

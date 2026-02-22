# Automated Ctrl+G Benchmark — 2026-02-20

## Setup

- MacBook Air 13" M4, macOS, Ghostty terminal
- Claude Code + claude-pager-open (C) + TurboDraft ultraFast mode
- `benchmarks/bench_ctrl_g.py` — sends keypresses via `osascript`, polls for app activation via `osascript`, cross-references with instrumented log
- Two runs: **clean** (50 cycles, no recording) and **recording** (50 cycles, 60fps ffmpeg)
- osascript overhead: 161ms clean, 166ms recording (median of 10 calibration calls)

## Why automated?

The [manual benchmark](2026-02-20-ctrl-g-latency.md) couldn't separate user dwell time from system latency — the "remainder" column mixed human reaction time with TurboDraft render time. This script eliminates user dwell entirely: keypresses are programmatic with precise timestamps.

## Clean run (no recording)

Primary data. No CPU contention from ffmpeg.

### Open path (Ctrl+G → TurboDraft visible)

|                        | Min   | Median | Max   | p95   |
|------------------------|-------|--------|-------|-------|
| **Total**              | 286ms | 335ms  | 379ms | 374ms |
| CC exec overhead       | 21ms  | 32ms   | 37ms  | 37ms  |
| claude-pager-open      | 6ms   | 9ms    | 23ms  | 15ms  |
| TurboDraft render*     | 95ms  | 133ms  | 165ms | 162ms |

### Close path (Cmd+Q → Ghostty visible)

|                        | Min   | Median | Max   | p95   |
|------------------------|-------|--------|-------|-------|
| **Total**              | 291ms | 325ms  | 404ms | 357ms |
| claude-pager-close     | 1ms   | 1ms    | 3ms   | 2ms   |
| TD + CC redraw*        | 130ms | 164ms  | 242ms | 196ms |

\* = derived: total minus instrumented components minus osascript overhead (161ms)

### What the user perceives

Subtracting osascript measurement overhead (which is an artifact of the benchmark, not part of the user experience):

- **Open: 174ms median** (335 - 161)
- **Close: 164ms median** (325 - 161)

### Where the time goes (open)

| Component          | Median | Notes |
|--------------------|--------|-------|
| CC exec overhead   | 32ms   | Upstream — writing temp file + exec() |
| claude-pager-open  | 9ms    | Socket + session.open + sessionId |
| TurboDraft render  | 133ms  | Window creation + first paint — **newly measured** |

### Where the time goes (close)

| Component          | Median | Notes |
|--------------------|--------|-------|
| claude-pager-close | 1ms    | SIGTERM + waitpid — at the floor |
| TD + CC redraw     | 164ms  | TurboDraft hide + Claude Code terminal repaint |

## Recording run (with ffmpeg)

50 cycles with 60fps screen recording active. Confirms CPU contention effect.

### Open path

|                        | Min   | Median | Max   | p95   |
|------------------------|-------|--------|-------|-------|
| **Total**              | 489ms | 676ms  | 998ms | 888ms |
| CC exec overhead       | 30ms  | 39ms   | 111ms | 60ms  |
| claude-pager-open      | 7ms   | 12ms   | 34ms  | 30ms  |
| TurboDraft render*     | 275ms | 455ms  | 774ms | 668ms |

### Close path

|                        | Min   | Median | Max    | p95    |
|------------------------|-------|--------|--------|--------|
| **Total**              | 623ms | 975ms  | 1389ms | 1291ms |
| claude-pager-close     | 1ms   | 5ms    | 19ms   | 17ms   |
| TD + CC redraw*        | 457ms | 804ms  | 1206ms | 1123ms |

\* = derived: total minus instrumented components minus osascript overhead (166ms)

### CPU contention multiplier

| Component          | Clean  | Recording | Multiplier |
|--------------------|--------|-----------|------------|
| Total open         | 335ms  | 676ms     | 2.0x       |
| Total close        | 325ms  | 975ms     | 3.0x       |
| CC exec overhead   | 32ms   | 39ms      | 1.2x       |
| claude-pager-open  | 9ms    | 12ms      | 1.3x       |
| TurboDraft render  | 133ms  | 455ms     | 3.4x       |
| TD + CC redraw     | 164ms  | 804ms     | 4.9x       |

TurboDraft is disproportionately affected by CPU contention — its GPU/compositor work competes with ffmpeg for resources. CC exec overhead and claude-pager-open (pure CPU, no rendering) are barely affected.

## Video cross-validation

60fps screen recording analyzed with `ffmpeg -vf "select='gt(scene,0.05)',showinfo"`.

- **101 scene changes** detected (expected 100 for 50 cycles + 1 trailing open)
- Brightness cleanly separates: **terminal ≤42**, **TurboDraft ≥48**
- All 50 cycles completed without error in both runs
- Video confirms every programmatic cycle corresponded to a real screen transition

### Video-derived timing

| Metric                 | Min    | Median  | Max     |
|------------------------|--------|---------|---------|
| TD visible (incl dwell)| 383ms  | 2125ms  | 2717ms  |
| Terminal visible       | 933ms  | 1142ms  | 1600ms  |
| Full cycle             | 1317ms | 3308ms  | 4317ms  |

These video times include osascript measurement overhead (the script uses osascript for both sending keystrokes and polling for app activation). They are not directly comparable to the programmatic measurements, which subtract osascript overhead. The video serves as a sanity check that real screen transitions occurred, not as a primary timing source.

## Comparison with manual benchmark

| Metric (median)   | Manual (video) | Automated (clean) | Notes |
|--------------------|----------------|-------------------|-------|
| CC exec overhead   | 113ms*         | 32ms              | Manual had ffmpeg running |
| claude-pager-open  | 12.5ms         | 9ms               | Consistent |
| claude-pager-close | 5.9ms          | 1ms               | Automated has no pager scroll state |
| TurboDraft render  | unknown        | **133ms**          | Previously buried in "remainder" |

\* Manual benchmark was recorded with ffmpeg — clean CC overhead is ~40ms.

## Key findings

1. **TurboDraft render = 133ms median** — the previously unknown "remainder" from the manual benchmark. This is now the dominant open-path latency after CC exec overhead.
2. **CC exec overhead = 32ms clean** — confirmed the ~40ms estimate from the manual benchmark.
3. **claude-pager-open is at the floor** — 9ms open, 1ms close. No optimization possible.
4. **Close path is dominated by TurboDraft + CC redraw** (164ms) — the 1ms pager close is negligible.
5. **ffmpeg CPU contention hits TurboDraft hardest** — 3.4x on open, 4.9x on close. Rendering/compositor work is the bottleneck under load.

## Methodology

- **Keystroke delivery**: `osascript` sends keystrokes to the frontmost process. `time.perf_counter()` records timestamp after `osascript` returns (keystroke confirmed delivered).
- **App detection**: `osascript` polls `get name of first application process whose frontmost is true` every 10ms. Polling adds up to one osascript round-trip (~166ms) of detection latency.
- **Instrumented log**: claude-pager-open writes per-step `gettimeofday` deltas to `/tmp/claude-pager-open.log`. CC exec overhead measured via `stat()` of temp file `st_mtimespec` vs process start.
- **TurboDraft render (derived)**: total open − CC overhead − pager open − osascript overhead.
- **osascript overhead**: median of 10 no-op `get frontmost of process` round-trips, subtracted from derived metrics.
- **Video**: 60fps screen recording at 2940x1912, scene detection threshold 0.05. Used for cycle count validation only.

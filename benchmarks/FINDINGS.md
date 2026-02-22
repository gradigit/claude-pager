# Ctrl+G Benchmark Findings

Keypress-to-visually-ready latency for the Ctrl+G shortcut that opens TurboDraft (an external editor) via claude-pager.

## Methodology

### Two measurement systems

**1. CGEvent + CGWindowList (primary, ~1ms precision)**

Native macOS APIs via `pyobjc-framework-Quartz`. `CGEventPost` injects keystrokes at the HID event tap in sub-microsecond time. `CGWindowListCopyWindowInfo` polls at ~0.5ms/call (~2000 Hz) to detect when TurboDraft's window appears on or disappears from screen. The poll loop runs continuously with no sleep, so detection latency is bounded by a single `CGWindowListCopyWindowInfo` call time (calibrated at startup over 100 iterations).

**2. 60fps screen recording + ffmpeg scdet frame analysis (cross-validation, 16.7ms precision)**

Records the screen via avfoundation at 60fps, then runs ffmpeg's `scdet` filter (threshold=0) to compute per-frame MAFD (Mean Absolute Frame Difference) on a 0-100 scale. MAFD quantifies how much the screen changed between consecutive frames. This identifies visual transitions and measures "render settle" -- the point when the app stops painting and is visually ready for interaction.

### Clock synchronization

The ffmpeg PTS clock and Python's `perf_counter` run at the same rate after a 3-second warmup period that absorbs ffmpeg startup jitter. The only unknown is a fixed offset between the two clocks.

Multi-event clock offset calibration resolves this:

1. Collect all MAFD transitions above threshold (0.3) from the video
2. Collect all expected CGWindowList event times (window appear + window disappear for each cycle) mapped to perf_counter-relative coordinates
3. Try every (MAFD transition, expected event) pair as a candidate offset anchor
4. For each candidate, compute a **monotonic** expected→transition match cost (order-preserving dynamic programming)
5. Pick the offset that minimizes total alignment error across all events
6. Validate fit quality (max and median alignment error bounds); if invalid, skip frame timing output

With N cycles this gives 2N anchor points that must all agree, making the calibration robust against cursor blink, clock updates, and other terminal noise that produces spurious MAFD spikes.

### Frame extraction for visual verification

MAFD spikes identify transition regions in the video. Frames are extracted as PNGs around each transition, covering from a few frames before the keystroke through the settle point. Each frame is named with keystroke-relative timing:

```
open_c01_+057ms_f0168.png    # 57ms after Ctrl+G, frame 168
close_c01_+042ms_f0192.png   # 42ms after Cmd+Q, frame 192
```

A `manifest.txt` is generated with PTS, MAFD, and transition/activity annotations for every extracted frame. LLM visual analysis of these frames confirms automated detection.

## Key findings

### Latency breakdown (frozen production run: 52 total, 2 warmup excluded, 50 measured; 200ms dwell)

#### Open path (Ctrl+G -> TurboDraft on screen)

| Component           | Min   | Median | Max   | p95   |
|---------------------|-------|--------|-------|-------|
| **Total**           | 48ms  | 60ms   | 77ms  | 76ms  |
| CC exec overhead    | 6ms   | 6ms    | 7ms   | 7ms   |
| claude-pager first draw | 2ms | 3ms  | 4ms   | 3ms   |
| Terminal-ready probe | 0.01ms | 0.04ms | 0.12ms | 0.08ms |

Most of the remaining open-path latency is outside claude-pager (TurboDraft + window/render path).

#### Close path (Cmd+Q -> TurboDraft gone)

| Component                  | Min   | Median | Max   | p95   |
|----------------------------|-------|--------|-------|-------|
| **Total**                  | 43ms  | 53ms   | 63ms  | 61ms  |

Benchmark quality checks for this run:
- 0 errors across all 52 cycles
- Poll resolution: 0.425ms
- Benchmark probe coverage: present in all 50 measured cycles

### Frame analysis (validation mode, recorded run)

| Metric                             | Range         |
|------------------------------------|---------------|
| Visual ready (Ctrl+G -> settled)   | 190-222ms     |
| Render settle beyond first paint   | 100-133ms     |
| Ghostty ready (Cmd+Q -> settled)   | 149-180ms     |
| Ghostty redraw settle              | ~117ms        |

Frame analysis reveals significant latency *after* CGWindowList detection. The window is registered in the window list but still painting its UI.

### Important discoveries

**1. CGWindowList detects before visual completion.** In recorded validation runs, the window appears in the window list well before full visual settle. API/window readiness and perceptual settle are different metrics.

**2. Terminal text causes false MAFD positives.** Script output between cycles (printing `cycle N/20: open=XXms close=XXms`) produces MAFD spikes of 0.3-0.8, which overlaps with real first-paint MAFD values (0.7-1.3). This was fixed by anchoring frame search to CGWindowList timing (a +/-50ms window around the detection time) instead of searching blindly after the keystroke. The function `_find_transition_near` implements this anchored search.

**3. Render settle is the user-visible metric.** CGWindowList measurements represent API-level readiness, not perceptual settle. True visual settle requires activity dropping below threshold for consecutive quiet frames. By default, activity threshold is auto-inferred from each run's noise floor (with a floor of 0.02), improving stability across machines.

**4. Late Ghostty redraw spikes.** After TurboDraft closes, Ghostty has a secondary redraw spike ~80-100ms later (MAFD 0.3-0.6), likely from restoring terminal content that was obscured by the TurboDraft window. The close settle measurement captures this secondary redraw.

## Replaced approach (v1 -- osascript)

The original benchmark used `osascript` for both keystroke delivery (~160ms/call) and window polling (~160ms/poll). Measurement overhead was larger than the latency being measured. The "133ms" TurboDraft render number from v1 was derived by subtracting noisy calibration values and was unreliable.

v2 replaced osascript with native CGEvent/CGWindowList, reducing measurement overhead from ~320ms to ~1ms per measurement. This made sub-component timing (CC exec overhead, pager open, TurboDraft render) directly observable from log timestamps rather than requiring statistical subtraction.

## Files

- `benchmarks/bench_ctrl_g.py` -- the benchmark script (requires `pyobjc-framework-Quartz`, auto-installed by uv via PEP 723 inline metadata)

### Running

Frozen command (non-record, production headline metrics):

```
uv run benchmarks/bench_ctrl_g.py \
  --cycles 52 \
  --dwell 200 \
  --warmup-cycles 2 \
  --start-delay 2 \
  --inter-cycle-delay 200 \
  --cycle-retries 0 \
  --open-timeout 10 \
  --close-timeout 10
```

Optional validation run with frame analysis:

```
uv run benchmarks/bench_ctrl_g.py \
  --cycles 52 \
  --dwell 200 \
  --warmup-cycles 2 \
  --start-delay 2 \
  --inter-cycle-delay 200 \
  --cycle-retries 0 \
  --open-timeout 10 \
  --close-timeout 10 \
  --record
```

| Flag       | Default | Description                                           |
|------------|---------|-------------------------------------------------------|
| `--cycles` | 20      | Number of open/close cycles                           |
| `--dwell`  | 150     | Milliseconds to wait in TurboDraft before closing     |
| `--record` | off     | Enable 60fps screen capture and frame analysis        |
| `--json`   | auto    | Path for raw JSON results (default: `/tmp/ctrl_g_bench_<ts>.json`) |
| `--start-delay` | 0 | Delay benchmark start so you can switch to Claude Code window |
| `--open-timeout` | 10 | Wait timeout for TurboDraft open detection |
| `--close-timeout` | 10 | Wait timeout for TurboDraft close detection |
| `--cycle-retries` | 1 | Retries per cycle after transient timeout/focus failures |
| `--inter-cycle-delay` | 120 | Settle delay before each cycle keypress |
| `--warmup-cycles` | 1 | Exclude first N successful cycles from headline metrics |
| `--transition-threshold` | 0.3 | MAFD threshold for transition detection |
| `--activity-threshold` | auto | Override settle/activity threshold (default auto-inferred) |
| `--quiet-frames` | 3 | Consecutive quiet frames before settle is declared |
| `--search-radius-ms` | 50 | Anchor search radius around CGWindowList events |
| `--clock-max-error-ms` | 80 | Reject frame analysis if max clock-fit error is too large |
| `--clock-median-error-ms` | 40 | Reject frame analysis if median clock-fit error is too large |

### Prerequisites

- Ghostty terminal must be the frontmost window at benchmark start
- TurboDraft must be configured as the Ctrl+G target in Claude Code
- Accessibility permissions for CGEvent injection (System Settings > Privacy > Accessibility)
- ffmpeg installed (only needed with `--record`)

### Output

- Console report with min/median/max/p95 tables for open and close paths
- JSON file with per-cycle raw timestamps and frame analysis data
- With `--record`: extracted PNG frames around each transition with a `manifest.txt`

### claude-pager render-stage instrumentation

`claude-pager-open` and `pager.c` now emit explicit render-stage timings into `/tmp/claude-pager-open.log`. The benchmark parser surfaces these in a dedicated **CLAUDE-PAGER RENDER STAGES** section:

- `pager pre-render done` (timestamp)
- `pager: parse end ... duration=...ms`
- `pager: markdown render end ... duration=...ms`
- `pager: first draw done` (timestamp)

This gives code-path-level latency for claude-pager itself, separate from TurboDraft visual paint timing.

For benchmark-only terminal-completion probes (`tcdrain + DSR reply`), enable:

```json
{
  "env": {
    "CLAUDE_PAGER_BENCH": "1"
  }
}
```

in `~/.claude/settings.json`.

## Known issues / future work

- Frame extraction at 50 measured cycles produces a large artifact set; visual verification should be selective (spot-check representative cycles rather than reviewing every extracted frame).
- The 3-second ffmpeg warmup adds latency to benchmark startup. This is necessary for PTS clock stability but could potentially be reduced with further investigation.
- Render settle measurement still depends on threshold tuning; the default auto-threshold is more robust than a fixed value, but atypical display noise can still require manual override via `--activity-threshold`.
- The benchmark assumes TurboDraft registers under one of a fixed set of process names (`TurboDraft`, `turbodraft-app`, `turbodraft-app.debug`). A different build configuration could cause detection failure.

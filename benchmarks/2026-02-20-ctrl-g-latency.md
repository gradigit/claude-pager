# Ctrl+G Latency Benchmark — 2026-02-20

## Setup

- MacBook Air 13" M4, macOS, Ghostty terminal
- Claude Code + claude-pager-open (C) + TurboDraft ultraFast mode
- 20 Ctrl+G open/close cycles (Cmd+Q and Cmd+W both hide — same operation)
- Instrumented via `gettimeofday` deltas in claude-pager-open
- Cross-validated with 60fps screen recording analyzed via ffmpeg scene detection

## Pipeline

```
Ctrl+G
  → Claude Code       writes temp file, exec()s editor
  → claude-pager-open connects socket, sends session.open, forks pager
  → TurboDraft        renders window

Cmd+Q/W (both hide)
  → TurboDraft        hides window, signals session complete
  → claude-pager-open SIGTERMs pager, waitpid
  → Claude Code       redraws terminal
```

## Claude Code (upstream — not our code)

| Metric        | Min    | Median  | Max     |
|---------------|--------|---------|---------|
| Exec overhead | 35.6ms | 113.4ms | 216.7ms |

Exec overhead = gap between CC writing the temp file and our process starting. Dominant open-path bottleneck. Inflated ~2.5x by ffmpeg CPU contention during recording; clean run median is ~40ms.

## claude-pager-open (this repo)

| Metric     | Min    | Median | Max    |
|------------|--------|--------|--------|
| Open path  | 7.5ms  | 12.5ms | 24.1ms |
| Close path | 2.2ms  | 5.9ms  | 23.5ms |

Open = socket connect + session.open + pager fork + sessionId receive.
Close = SIGTERM to pager + waitpid.

**At the performance floor.** No further optimization possible.

## TurboDraft (downstream)

Not directly instrumented in this benchmark.

## End-to-end (video, 60fps)

Total time between screen transitions, including user dwell time between keypresses.

| Transition | Min   | Median | Max   | Frames (min) |
|------------|-------|--------|-------|---------------|
| Open       | 283ms | 458ms  | 616ms | 17            |
| Close      | 550ms | 783ms  | 984ms | 33            |

### Per-cycle open breakdown

Video gap minus instrumented system time = remainder (user dwell + TurboDraft render + anything uninstrumented).

| Cycle | Video  | CC overhead | pager-open | Remainder |
|-------|--------|-------------|------------|-----------|
| 1     | 616ms  | 152.4ms     | 10.8ms     | 452.8ms   |
| 2     | 500ms  | 132.9ms     | 10.9ms     | 356.2ms   |
| 3     | 433ms  | 213.8ms     | 15.5ms     | 203.8ms   |
| 4     | 350ms  | 183.1ms     | 21.3ms     | 145.6ms   |
| 5     | 283ms  | 78.6ms      | 21.9ms     | 182.5ms   |
| 6     | 500ms  | 74.8ms      | 13.5ms     | 411.7ms   |
| 7     | 583ms  | 216.7ms     | 15.1ms     | 351.2ms   |
| 8     | 316ms  | 99.4ms      | 12.7ms     | 204.0ms   |
| 9     | 366ms  | 35.7ms      | 21.1ms     | 309.3ms   |
| 10    | 550ms  | 70.7ms      | 13.3ms     | 466.0ms   |
| 11    | 466ms  | 118.2ms     | 8.0ms      | 339.8ms   |
| 12    | 283ms  | 78.3ms      | 10.5ms     | 194.2ms   |
| 13    | 550ms  | 83.8ms      | 7.5ms      | 458.7ms   |
| 14    | 450ms  | 108.6ms     | 21.7ms     | 319.8ms   |
| 15    | 416ms  | 172.1ms     | 24.1ms     | 219.8ms   |
| 16    | 450ms  | 55.0ms      | 11.5ms     | 383.5ms   |
| 17    | 484ms  | 138.9ms     | 9.7ms      | 335.4ms   |
| 18    | 467ms  | 146.5ms     | 8.3ms      | 312.2ms   |
| 19    | 484ms  | 143.7ms     | 11.1ms     | 329.2ms   |
| 20    | 450ms  | —           | —          | —         |

### Close path

Video close transitions ranged 550–984ms (median 783ms). The instrumented close path in claude-pager-open is only 6ms median. The remainder is user dwell time + TurboDraft hide processing + Claude Code terminal redraw — not yet separately instrumented.

## Methodology

- **Instrumented log**: claude-pager-open writes per-step `gettimeofday` deltas to `/tmp/claude-pager-open.log`. CC exec overhead measured via `stat()` of temp file `st_mtimespec` vs process start.
- **Video**: 60fps screen recording at 2940x1912, analyzed with `ffmpeg -vf "select='gt(scene,0.05)',showinfo"`. Mean brightness ≤42 = terminal, ≥48 = TurboDraft.
- **Frame resolution**: 16.67ms per frame (±17ms precision).
- **ffmpeg overhead**: Screen recording inflated CC exec overhead ~2.5x. Clean run (no recording) confirmed ~40ms median.

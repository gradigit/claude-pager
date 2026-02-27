# V2 Regression & Visual Comparison Plan (Ghostty-first)

## Baseline
Use current `main` (v1) as baseline and collect one fresh run with the frozen command.

## Frozen benchmark command

```sh
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

## Required regression checks

- Open median (Ctrl-G -> window visible)
- Open p95
- Close median (Cmd-Q -> gone)
- Close p95
- Probe coverage (`pager_bench_probe_seen=true`)
- Poll resolution sanity (`< 1ms`)

## Performance guardrail (default)

V2 should stay within **+10%** of v1 medians/p95 unless an intentional tradeoff is documented.

## Visual comparison protocol

For each parity milestone:

1. Capture a "before" screenshot from v1
2. Capture an "after" screenshot from v2 branch with same terminal size/content
3. Store side-by-side comparison under `assets/readme/` or `assets/parity/`
4. Note differences in:
   - diff readability (hunks/gutters)
   - tool output fidelity
   - statusline parity
   - link/path click behavior

## Merge gate for V2

- [ ] Ghostty parity goals demoed (diff/tool/statusline)
- [ ] No critical regression in benchmark guardrail
- [ ] README updated with V2 parity visuals
- [ ] Final summary posted with benchmark deltas and screenshots

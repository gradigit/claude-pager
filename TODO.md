# TODO

## Benchmark suite publication

Benchmark command and methodology are now stabilized (frozen command + production metrics captured). Remaining follow-ups:

- Optional: add a repeatable gate script/check target for regression testing

## README / launch assets

- Capture polished screenshots showing transcript rendering + clickable wrapped links/file paths
- Record a short demo video showing Ctrl-G open/close speed and smooth flow
- Add screenshot/video assets to README and reuse in launch/social posts

## UX parity

- Achieve 1:1 complete visual parity with Claude Code (including diff views, tool outputs, and statusline)

## V2 experimental track (Ghostty-first)

- [ ] Build a Ghostty parity mode for richer diff rendering (file headers, hunk headers, old/new line gutters)
- [ ] Add hunk-level file anchors with displayed line references for quick jump/open
- [ ] Improve tool output fidelity (less aggressive truncation, better block framing)
- [ ] Polish statusline/theme to match Claude Code feel
- [ ] Capture before/after visual comparison screenshots for each parity milestone
- [ ] Run regression latency benchmarks against v1 frozen command and compare medians/p95
- [ ] Publish V2 parity + performance comparison report before merge decision

## Completed cleanup

- Hardened benchmark suite and froze benchmark command defaults
- Removed legacy Python pager code (`src/claude_pager/`)
- Removed legacy shell shims (`shim/claude-pager-shim.sh`, `shim/pager-setup.sh`)
- Removed Python packaging/test scaffolding (`pyproject.toml`, `tests/`)

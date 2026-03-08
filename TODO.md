# TODO

## Benchmark suite publication

Benchmark command and methodology are now stabilized (frozen command + production metrics captured). Remaining follow-ups:

- Optional: add a repeatable gate script/check target for regression testing

## README / launch assets

- Capture polished screenshots showing transcript rendering + clickable wrapped links/file paths
- Record a short demo video showing Ctrl-G open/close speed and smooth flow
- Add screenshot/video assets to README and reuse in launch/social posts

## UX parity

- Achieve 1:1 visual clarity parity with Claude Code for transcript content (diff views, tool outputs, spacing, colors; statusline excluded)

## V2 experimental track (Ghostty-first)

- [ ] Restore simultaneous wheel scrolling + clickable links in Ghostty pager mode without sacrificing prompt-editor arrow-key behavior
- [x] Build a Ghostty parity mode for richer diff rendering (file headers, hunk headers, old/new line gutters)
- [x] Add hunk-level file anchors with displayed line references for quick jump/open
- [x] Improve tool output fidelity (less aggressive truncation, better block framing)
- [x] Render `toolUseResult.structuredPatch` in Claude-like style (Update/Create headers + Added/Removed summary + single-gutter patch rows)
- [ ] Polish transcript theme cadence (statusline excluded)
- [ ] Capture before/after visual comparison screenshots for each parity milestone
- [ ] Run regression latency benchmarks against v1 frozen command and compare medians/p95
- [ ] Publish V2 parity + performance comparison report before merge decision

## Completed cleanup

- Landed TurboDraft fast-path queue metadata handoff (`source`, `queuePath`, `queueKey`, `queueFormatVersion`) plus safer session-scoped queue writes
- Hardened benchmark suite and froze benchmark command defaults
- Removed legacy Python pager code (`src/claude_pager/`)
- Removed legacy shell shims (`shim/claude-pager-shim.sh`, `shim/pager-setup.sh`)
- Removed Python packaging/test scaffolding (`pyproject.toml`, `tests/`)

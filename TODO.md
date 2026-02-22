# TODO

## Benchmark suite publication

Benchmark command and methodology are now stabilized (frozen command + production metrics captured). Remaining follow-ups:

- Optional: add a repeatable gate script/check target for regression testing

## README / launch assets

- Capture polished screenshots showing transcript rendering + clickable wrapped links/file paths
- Record a short demo video showing Ctrl-G open/close speed and smooth flow
- Add screenshot/video assets to README and reuse in launch/social posts

## Completed cleanup

- Hardened benchmark suite and froze benchmark command defaults
- Removed legacy Python pager code (`src/claude_pager/`)
- Removed legacy shell shims (`shim/claude-pager-shim.sh`, `shim/pager-setup.sh`)
- Removed Python packaging/test scaffolding (`pyproject.toml`, `tests/`)

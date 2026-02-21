# TODO

## Deprecate Python pager and bash shims

The C binary now handles everything end-to-end: editor resolution, TUI/GUI detection,
pager rendering, and transcript finding. The following are dead code:

- `src/claude_pager/` — Python pager (replaced by `bin/pager.c`)
- `shim/claude-pager-shim.sh` — bash editor shim (replaced by `bin/claude-pager-open.c`)
- `shim/pager-setup.sh` — bash pager launcher (replaced by `fork_pager()` in C)
- `pyproject.toml` — Python packaging config
- `tests/` — Python tests

Remove in a future PR after confirming no one depends on the Python entry point.

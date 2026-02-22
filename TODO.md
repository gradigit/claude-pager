# TODO

## Benchmark suite publication

Benchmark command and methodology are now stabilized (frozen command + production metrics captured). Remaining publication tasks:

- Decide which benchmark files in `benchmarks/` should be committed vs kept local-only
- Commit benchmark suite/docs once final public write-up language is approved
- Optional: add a repeatable gate script/check target for regression testing

## Completed cleanup

- Hardened benchmark suite and froze benchmark command defaults
- Removed legacy Python pager code (`src/claude_pager/`)
- Removed legacy shell shims (`shim/claude-pager-shim.sh`, `shim/pager-setup.sh`)
- Removed Python packaging/test scaffolding (`pyproject.toml`, `tests/`)

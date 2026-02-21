# TODO

## Finish benchmark suite

Benchmark work is in progress in `benchmarks/` and should remain untracked until complete.

Before committing benchmark artifacts:

- Define final benchmark scenarios and acceptance thresholds
- Run repeatable Ctrl-G latency benchmarks (cold/warm) and collect outputs
- Summarize findings and recommended defaults in `benchmarks/FINDINGS.md`
- Decide which benchmark files should be committed vs kept local-only

## Completed cleanup

- Removed legacy Python pager code (`src/claude_pager/`)
- Removed legacy shell shims (`shim/claude-pager-shim.sh`, `shim/pager-setup.sh`)
- Removed Python packaging/test scaffolding (`pyproject.toml`, `tests/`)

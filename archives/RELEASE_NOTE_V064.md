# Release Note - satria v0.6.4

v0.6.4 is a performance and tooling release: incremental builds parse and
write less, `satria init` becomes a guided scaffold, and the trace used by
`satri bench` is now built in.

## Faster incremental builds

The cache layer was the main CPU cost on large workspaces. This release
removes most of the repeated work without changing any cache format or
build decision:

- **Cache files are read once and parsed in place.** `cache_store` no
  longer allocates a buffer per line; nodes and payloads come from a
  per-store arena, and the decimal parser replaces millions of `strtoll`
  calls per run.
- **The `opts.cache` router memo uses a file stamp** (path, mtime in ns,
  size, project) instead of hashing multi-megabyte cache files 15-16 times
  per run. Large `csOpen` parses dropped by roughly half.
- **`sources.cache` is loaded once** and split into lines in memory, so
  no-op validation no longer pays `fgets`/`strlen` per line for tens of
  thousands of entries.
- **One stat instead of two-to-three** for pack entries, link targets and
  source resolution (`fsStatDirMTimeNs`).
- **Libraries build static and shared in parallel**, with O(N log N)
  de-duplication, and the MSVC link command is assembled in O(N).
- **Executables link through the project's static archive**
  (`--whole-archive`, `-all_load` on macOS) when one exists; `ld` reads an
  indexed archive instead of opening every object.
- **No clock calls on the save-walk path unless `SATRIA_PROFILE` is set**,
  matching the documented contract.

CI `Workspace Benchmark` at this release (AMD EPYC runner, median of 3):

| Scenario | 1k cmake | 1k satria | 5k cmake | 5k satria |
| --- | ---: | ---: | ---: | ---: |
| cold | 10.883 ms | **10.434 ms** | 60.198 ms | 60.882 ms |
| leaf | 690 | **587** | 4.139 | 4.352 |
| bade | 695 | **595** | 4.160 | 4.376 |
| hedit | 743 | **673** | 4.420 | 4.916 |
| noop | 43 | **25** | 232 | **146** |
| touch / libtouch | 798 / 776 | **36 / 36** | 4.577 / 4.362 | **213 / 208** |

Rates are only comparable inside one run (runner hardware differs between
runs). At 1k satria wins every rebuild scenario; at 5k the leaf gap is
about 5% and hedit about 11%. Cold builds remain a different kind of
measurement (see `satria bench` report notes).

## `satria init` interactive scaffold

`satria init -p` now shows a summary before writing anything and asks for
confirmation. On a terminal the scaffold is built and run immediately.

- `--here` puts a single-project scaffold in the current directory instead
  of `<name>/` (workspace mode still uses project folders).
- `--no-run` skips the automatic build+run.
- Nothing is written until the summary is confirmed; Ctrl+C or EOF cancels.

## Trace and benchmark

- `satria trace` is the default trace backend for `satria bench` on
  POSIX: it uses `LD_PRELOAD`, works without ptrace, and records
  **per-call durations** (total seconds, percent, and µs/call in the CSV).
  `strace=true` remains available as an explicit opt-in.
- Workspace trace now configures cmake/meson after resetting the build
  directory (without tracing the configure step) and traces both `cold`
  and `leaf`, with a `scenario` column in `workspace-syscalls.csv`.
- Leaf trace no longer follows children (`-f`), so the number reflects
  satria's own walk instead of being dominated by gcc/ld waiting.
- The single-project benchmark generator caps the generated header include
  chain at 64. CI runs with large `headers` values (e.g. 1000) no longer
  exceed the compiler's include-depth limit.
- Bench sanitizer labels are valid C identifiers.

## Fixes

- The trace summary test now matches the top list ordered by total time.
- The router-centric scheduler plan is on by default (`SATRIA_PLAN=1`); set
  `SATRIA_PLAN=0` to use the legacy path. `SATRIA_WS_PLAN` remains a legacy
  alias.

## Known limitations

- Router-centric workspace orchestration (all cache decisions and writes
  owned by the `opts.cache` router) is still experimental and not part of
  this release.
- `tup` participates in the single-project bench but is still rejected by
  the CI workflow input until verified there.
- `docs/` still reflects the v0.6.0 snapshot (with `init` updated).

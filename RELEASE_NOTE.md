# Release Note - satria v0.6.2

v0.6.2 moves the benchmark into the satria binary itself — no bash in the
measurement path — and fixes workspace state handling around it.

## Benchmark in C: `satria bench`

```bash
satria bench                                        # single project, all default tools
satria bench -f tools=ninja,cmake,satria -f source=80 -f header=20 -f runs=5
satria bench --workspace -f tools=cmake,satria      # graph: rupamod, core, bade
satria bench --help                                 # full flag list
```

- **Modes**: single project (N sources chained by H headers) and
  `--workspace` (3-project graph). Scenarios: single `cold, noop, touch,
  edit, editrel, compdb`; workspace `cold, noop, leaf, bade, mod, touch,
  hedit, sel, dep, libtouch, libhdr, rel`.
- **Tools**: satria, cmake, ninja, make, bear, meson, xmake, tup, plus a
  `gcc-floor` baseline for edit/editrel. Missing tools are skipped, every
  requested tool's actual work is validated against expectations
  (`Detailed Validation Log` + `Success Rate`), and the benchmark exits
  non-zero when validation fails.
- **Measurement**: rolling A/B per repetition (`-f rolling=true`, default),
  median report with bars, untimed prime per tool (build + snapshot +
  restore), artifact CSVs (`results.csv`, `view.csv`), work log, stderr
  log, and optional syscall profiling (`-f trace=true` via a `gcc`/`ld`
  shim under `RB_BENCH_SHIM`).
- **Honest cold comparison**: the report states explicitly that cold-build
  times are not apples-to-apples — each tool pays a different
  configuration cost (ninja reads a static graph, satria parses the
  Buildfile and rebuilds startup state, cmake/meson run a full configure);
  the note is skipped when satria is the only tool.
- **Workspace scenario isolation**: the prime snapshot is restored before
  every measured run, so scenario mutations never accumulate (previously
  `leaf` followed by `sel` appended the same function name and the rest of
  the run failed with a redefinition error). gcc-floor is now set up even
  when `warmup=false`.
- Spec: `design/bench_c.md`; the bash predecessor (`bench/ws`) stays for
  reference but is no longer used by CI.

## CI: benchmark workflows run `bin/satria bench`

`bench-compare.yml` and `bench-ws.yml` no longer call `bench/compare.sh`
or `bench/ws.sh`. Inputs map to `-f` flags directly (`tools`, `source`,
`header`, `runs`, `warmup`, `trace`, `artifact`); the audit toggle and
delay knobs are gone because validation is always on and the C bench is
deterministic. The workspace workflow still restricts tools to
cmake/satria (workspace mode).

## Fixes & performance

- **`use workspace c` survives the cache**: `startup.state` now
  serializes the workspace language (`SATRIA-STARTUP-3`). Previously the
  restored model silently reverted to hybrid detection, so the source
  walk kept picking up `.cpp` files on every run after the first parse.
  Old caches load with the previous behavior and re-parse once on the
  next WsBuildfile change.
- **Selective no-op runs are faster**: the selective-divergence warning
  is only computed after the fast-state no-op gate misses, removing a
  full source/header stat sweep from every no-op `-w core` run.
- Bench validation fixes as listed above (scenario isolation, gcc-floor
  setup).

## Known limitations

- `tup` participates in the single-project bench but is still rejected by
  the CI workflow input until verified there.
- `docs/` still reflects the v0.6.0 snapshot.

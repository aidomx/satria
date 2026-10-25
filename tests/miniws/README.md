# miniws

Small standalone workspace fixture covering the *active* features of a
larger workspace: aliases, tar archive, embed, shared/static libraries,
two-pass linking, and reciprocal `cdeps`.

Run from `tests/miniws` with `../../build/bin/satria` after bootstrap.
Expected programs: `bin/rupa` prints `core=8 bade=7`, and `bin/bade`
prints `bade=7 core=8`.

Regression checks: cold, no-op, touch `core/src/core.c`, edit
`bade/src/bade.c`, add/remove a `.c` file under `core/src`, and verify
`.satria/{startup.state,opts.cache,sources.cache,report.blob,artifact.cache}`
plus `caches/`.
Touch-only does NOT recompile: workspace validates via content-hash
(fingerprint PATCH) and single-project via the FAST5 state hash stamp —
both keep no-op (design/startup.md §4).

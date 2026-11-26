# Release Note - satria v0.6.8

v0.6.8 is a workspace performance release: project batches run in parallel by
default, link steps overlap, and incremental planning stops paying for stat
calls that were already answered. It also adds `<project>.extension` for
projects whose content is not C source.

## Parallel workspace builds by default

The dependency-ready scheduler is now the default for workspace batches with
more than one project and `-j` above one. A single `cc`/`ld` invocation costs
roughly 0.25–0.37 s and cannot be split across cores, so overlapping project
links removes most of the serial tail on rebuild scenarios such as `hedit`
and `libhdr`.

Output stays readable through three policies:

- **Terminal (TTY):** serial runner, so per-object progress is realtime.
- **Regular file:** parallel runner, printed in worklist order, so CI logs and
  unit tests are deterministic.
- **Pipe or socket:** parallel runner with live, line-buffered output
  (`satria -w | tee build.log`, wrappers, and nested `satria | satria -w`).

`SATRIA_WS_PAR=0` forces the serial runner; `SATRIA_WS_PAR=1` forces parallel
with live output.

The TTY check now reads the **original** stdout saved by the report relay
(`reportBlobStdoutIsTty`). During a build `STDOUT_FILENO` is a tee pipe, so
`isatty(STDOUT_FILENO)` was always false; terminal builds silently chose the
parallel runner and progress stalled per project. Pipe detection uses the same
saved descriptor.

## Overlapped links

- In the workspace library phase only `lib<name>.a` is built. The shared
  variant (`.so`) is deferred to the binary phase, where its link starts just
  before the binary link. Total time becomes `max(.so, binary)` instead of
  `.so + binary`.
- Independent project links overlap through the ready-dependency scheduler.
- Static archives are rewritten incrementally: symbols of all changed objects
  are resolved with a single `nm -A` call instead of one `nm` process per
  object.

## Faster planning and no-op validation

- `statPlanBuild` reuses the stat memo filled by the workspace no-op gate
  (`cmdsStatMemoPeek`). Sources and objects already verified in the same run
  are no longer stat'ed a second time.
- Object-path lookup uses a hash map instead of the previous O(n²) string
  scan. On a 5,000-source fixture the plan phase dropped from roughly
  4.5–5.0 s to 1.0–1.8 s; the quadratic path went from 2,546 ms to 24 ms.
- The gate fills the memo for **all** pre-stat entries before validation can
  early-exit, so a miss near the front no longer forces the plan to re-stat
  the rest.
- Library fast-state validation skips paths already in the memo and evaluates
  entries flagged stale by the gate first.
- Forked children invalidate inherited plan verdicts once any project has
  finished, because a completed project may have rewritten shared inputs
  (embedded tarballs, archives, libraries).

## `<project>.extension`

Projects whose content is not C source can declare the extension that
identifies their inputs:

```text
mod.extension = .rp
```

- Freshness walks and `sources.cache` records only touch files with that
  extension, instead of every file under `archive.src` / `embedded.<n>.src`.
- The project value is inherited by every `embedded.<n>` entry that does not
  set its own `.extension`.
- It affects tracking only. `archive.pattern`, `embedded.<n>.pattern`, and
  `exclude` still decide what goes into the archive, so metadata such as
  `module.json` stays in the tarball.
- The archive configuration hash includes the field, and `startup.state` was
  bumped to format 17, so the cache is regenerated once after upgrading.

## Fixes

- The workspace benchmark buildfile now mirrors the CMake fixture: `bade`
  sources only `src`, and `main.c` is excluded from the library. The old file
  named a `manager` directory that the fixture never creates.
- A deferred `.so` is no longer recorded as a missing artifact in library
  fast-state, which previously made the next library phase miss.
- Added `.extension` to the archive config hash so changing it rebuilds the
  archive state exactly once.
- Fixed windows clock time
- fixed `src/test/watch.c` replace POSIX-only APIs with portable wrappers

## Known limitations

- `docs/` still reflects the v0.6.0 snapshot, with incremental changes for
  newer features.
- `tup` participates in the single-project bench but is still rejected by the
  CI workflow input until verified there.
- The CI `Workspace Benchmark` numbers for this release are attached to the
  tag; local smoke runs are for diagnosis only.

# Release Note - satria v0.6.1 

v0.6.1 adds two ways to use satria outside the full Buildfile lifecycle —
ad-hoc compilation and a development mode — and cleans up static archive
membership, project language selection, and the unit-test tooling.

## Ad-hoc compile (`-o` / `-b` / `-F`)

```bash
satria src/x.c -o build/x.o            # compile only
satria src/x.c -o build/x.o -b bin/x   # compile + link
satria src/a.c src/b.c -o build -b bin/app
satria src/x.c -F quick.conf           # flags/headers/output from a light config
```

- `-o` is an object path for one source or an object directory for several
  sources (`<dir>/<basename>.o` per source, mirroring gcc); `-b` links a
  binary and requires `-o`; `-F` takes `flags`, `headers`, `defines`, `std`
  and `o.*` from a lightweight config file (release, cdeps and multi-project
  logic are skipped).
- No side effects: no `.satria`, cache or fingerprint is written, so this mode
  never no-ops; compiler failures pass through unchanged.
- `-jN` behaves like a normal build: without `-j` the job count comes from
  `SATRIA_JOBS` or the CPU core count, `-j1` forces serial; the link step
  always runs once at the end.
- The compiler is auto-detected (`cc`, `gcc`, `clang`, `tcc`, `cl`) and can be
  overridden with `compiler =` in the `-F` file.
- Rejected combinations come with clear messages: sources without
  `-o`/`-b`/`-F`/`-f`, `-b` without `-o`, `-F` together with `-o`/`-b`, and
  sources together with `-f`/`-w`.
- Spec: `design/compile.md` §2–§3; reference: `docs/reference/cli.md`.

## `--dev` — build without the release/pack phase

```bash
satria --dev            # pack.* ignored for this run
satria -w --dev         # workspace release pass skipped
```

Compile + link only, useful for measuring iteration cost. The report shows
`* Release : skipped (--dev)`; workspace state, `opts.cache` and artifacts are
left untouched, so the next run without `--dev` behaves normally. Rejected
(exit code 2) when combined with `release` or a `-- key=value` selector.

## Project language: `use project c|cpp|c++`

- `use project c` stops the source walk from picking up `.cpp` files — a pure-C
  project is no longer "contaminated" by a stray C++ file.
- `use project cpp` (alias `c++`) enables the C++ defaults (`std`, compiler,
  linker) just like a `.cpp` source would.
- Plain `use project` keeps the historical auto-detect behavior. An unknown
  language token is reported instead of silently ignored. The same rules apply
  in `WsBuildfile`.

## Static library archives

- **Unique members**: `ar` stores basenames, so two sources sharing a basename
  (`src/install.c` and `src/manager/install.c`) are staged to unique names
  (`install.o` + `manager_install.o`, using the project tag and a `_2`, `_3`
  safety suffix) before `ar rcs`. An archive no longer holds two members with
  the same name.
- **Stale archives heal themselves**: `ar rcs` never removes obsolete members,
  and an archive whose mtime is newer than its inputs used to be skipped as
  up-to-date. When staging occurs and the archive content does not match (count
  and member names checked via `ar t`), the library phase forces a rebuild and
  removes the old archive first. Without a basename collision there is no extra
  cost and the result stays `cached`.
- Verified: `libbade.a` that previously had 39 members (3 duplicates) became 36
  unique members after one build, and the second run reported `cached`.

## Unit tests

- **`satria test <target>`** delegates to `scripts/unit.sh`:
  `satria test miniws`, `-t <name>` for a single test, `-j N`, `--clean`, and
  `-w` to watch a run in progress. The running satria binary is inserted
  automatically; the runner's exit code becomes satria's exit code.
- **`.test` DSL** (M0–M3): parser, expressions/ternary, `@if`/`@for`/`@while`,
  `run.bg/stop/wait`, `run.tool`, `stage`, and `text.*`; 38 `.test`/`.t` files
  run through `tests/unit/lib/dsl.sh`, with the conformance kit in
  `tests/unit/dsl/`. Porting the engine to C (M4) is still open.
- New documentation page: `docs/reference/unit-test.md`.

## Workspace file: `WsBuildfile`

The workspace file is now **`WsBuildfile`**, without an extension: `.ws` is
also the Witcher Script extension, so editors and tools misread `Buildfile.ws`.

- `satria init -w` writes `WsBuildfile`.
- `Buildfile.ws` (and the older `Buildfile.workspace`) is still read during the
transition, printing a one-time deprecation warning that suggests the rename.
  Detection order when several exist: `WsBuildfile`, `Buildfile.ws`,
  `Buildfile.workspace`.
- The build log/cache records the detected filename, so renaming invalidates
  the old workspace cache once and then behaves normally.
- Vim filetype detection now covers `WsBuildfile` while keeping the legacy
  name.
- Both legacy names will be removed once the transition window ends.

## Workspace & CLI

- `--with-cdeps` (opt-in, `-w` only) widens the selective closure to `cdeps`
  providers transitively; without the flag the old contract is unchanged. A
  provider outside the closure whose library is missing is reported by a
  pre-flight warning.
- CLI reference refreshed: `--with-cdeps`, `satria watch`/`-w watch`,
  `-g compdb`, `-xf`/`-xcf` conversion, `satria test`, and the ad-hoc mode.

## Fixes & performance

- Embedded artifacts in parallel builds no longer lose results.
- `--dev` and ad-hoc mode leave state/cache untouched; artifacts that are not
  rewritten are still reported `cached`.
- `opts.cache`: one shared store handle per process, commits deferred to phase
  boundaries, content-keyed memo for the router scan, memoized cache-group
  presence; the workspace save walk reuses the no-op gate stat memo.
- Bench: edit+relink and serial scenarios are part of the summary.
- fix: fixed bug windows getpid
- Multi-line syntax for `release.files` in `Buildfile` or `WsBuildfile` configurations is now supported.

## Known limitations

- Ad-hoc mode has no dedicated unit tests (manual verification recorded in
  `design/compile.md` §3.1) and has not been tested on Windows/MSVC.
- Unit-test DSL M4 (native C parser/evaluator/runner, retiring `unit.sh`) is
  not done; `tests/unit/common` is still a mix of `.test` and `.sh`.
- The `design/` documents are reconstructions and some `docs/guide/` pages
  still date from the v0.2.4 era; the code remains the source of truth.

## Verification for this release

- `bin/satria test miniws`: **PASS=371 FAIL=0 PRE=0** (31 tests).
- `bin/satria version` still reports `satria v0.5.0` — `.satria-version` has
  not been bumped ; the self-host build succeeds.
- Ad-hoc smoke: `satria x.c -o build/x.o -b bin/x` → rc 0, object and binary
  produced; `satria x.c` without a destination → "Output destination not
  specified" error.
- Static archive test in a workspace copy: duplicated archive with 39 members
  → 36 unique members, second run `cached`.

# Profiling

`satria profile <context>` measures the real cost of satria's operations without
performing a normal build. It is a diagnostic tool, not an alternative build
mode: it does not require Buildfile overrides, does not touch fast-state caches,
fingerprints, `Buildfile`, or build artifacts, and cleans up its temporary data
afterwards — so profiling never changes the result of your next incremental build.

For the full design rationale see [design/profile.md](../../design/profile.md).

## CLI

```bash
satria profile <context> [options]
```

The context is selected with a `name=value` expression; everything after
`profile` belongs to the context and is not parsed as global satria options.

Available contexts:

| Context | Measures | Options |
|---|---|---|
| `archive=<dir>` | tar + compression of a directory | `with=tar\|none\|tar,gz\|tar,xz\|tar,bz2` |
| `embed=<dir\|file>` | embed probe + object generation | — |
| `library=<dir>` | `.o` collection + `ar rcs` / `cc -shared` | `with=static\|shared\|static,shared` (default `static`) |
| `binary=<dir>` | full cold compile + link in isolation | `jobs=N`, `--report <file>` |
| `project=<dir>` | incremental cycle: config, fingerprint, decide, compile, link | `jobs=N` |
| `workspace=<dir>` | workspace model: parse, dependency (topo), library checks | — |

## Archive

Archive profiling works directly on any directory — no Buildfile changes
needed. This makes it easy to compare compression backends on the same input:

```bash
satria profile archive=rupamod
satria profile archive=rupamod with=tar        # tar only, no compression
satria profile archive=rupamod with=tar,gz     # tar + gzip
satria profile archive=rupamod with=tar,xz     # tar + xz
satria profile archive=rupamod with=tar,bz2    # tar + bzip2
```

`with=gz` without `tar` is rejected — a compression backend only makes sense
on top of tar. Measured phases appear separately, so you can see tar and the
compressor as distinct costs:

```text
Profile : archive=rupamod with=tar,gz

tar        90.7 ms   in 18,442,193 B   out 18,721,024 B
gzip       42.7 ms   in 18,721,024 B   out  4,382,912 B
total     133.4 ms   dominant gzip
```

## Embed

```bash
satria profile embed=rupamod
satria profile embed=rupamod/dist/modules.tar.gz
```

Measures the embed pipeline on a directory or a single file, including the
GNU ld probe (which runs without touching the ld.cache used by normal builds).

## Library

```bash
satria profile library=rupamod
satria profile library=rupamod with=static
satria profile library=rupamod with=shared
satria profile library=rupamod with=static,shared
```

Collects the project's `.o` files and measures `ar rcs` (static) and/or
`cc -shared` (shared). The target needs existing build output for the
`static` backend; the `shared` backend needs a Buildfile in the target so the
C++ toolchain decision (`-lstdc++`) can be made.

## Binary

```bash
satria profile binary=.
satria profile binary=tests/mini jobs=4
satria profile binary=. --report profile.md
```

Measures a complete compile + link of the target project in isolation:
configuration, source gathering, compilation (`jobs=N`, default = CPU core
count), and linking. Output directories are redirected to an isolated
temporary location, so the project's real `bin/`, `build/`, and caches stay
untouched. Embedded projects and `output.binary = false` are rejected.

`binary=` measures **cold** build cost: objects are never reused, every
source compiles fresh in the sandbox. It works both with and without a
Buildfile — for pure-convention projects (`src/` folder, no Buildfile) the
standard conventions apply.

## Project

```bash
satria profile project=.
satria profile project=tests/mini jobs=4
```

Measures the **incremental** project cycle: configuration, fingerprint
(hashing the build configuration), decide (mtime + header content check
via the dependency engine), compile (only stale sources), link. Combine
with `--repeat 2`: the second run keeps the sandbox (deps cache lives)
and shows the warm path — `decide` costs something, `compile` is 0.0 ms.

Dependency state lives in the isolated temporary directory, so the
project's real `.satria/` is untouched.

## Workspace

```bash
satria profile workspace=.
```

Measures the workspace *model* cost — parsing `Buildfile.ws`, dependency
ordering (topological sort of `depends_on`), and validation of every
member (each `<name>/` project directory exists). Projects are enumerated
and validated but **not built**: building member projects writes state at
their roots, which the non-mutating rule forbids, so those phases are not
fabricated. The `projects` field in the detailed report shows the member
count; use `binary=`/`project=` per member to measure their build cost.

## Repeat

```bash
satria profile archive=. --repeat 5
```

Repeats the measured run N times (1–100). The sandbox is kept between
runs — caches live inside it, so repetition never touches real build
state (design rule) while still showing warm-path behavior. Phase
timings aggregate over all runs in both the summary and the report.

## Report

Terminal output is a short summary answering "what is expensive?". When piped
(or redirected) the same command prints the detailed report instead — so
`> profile.md` is enough:

```bash
satria profile archive=rupamod with=tar,gz > profile.md
```

For an explicit report file that keeps the summary on the terminal:

```bash
satria profile binary=. --report profile.md
```

The report includes the context, environment (satria version, platform,
compiler), input statistics, per-phase timing with input/output byte counts,
and a summary with the dominant phase. Reports are never written
automatically — automatic files would add I/O to the measurement itself.

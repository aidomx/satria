# satria

**A simple builder for you.**

satria is a declarative build tool for C (and C++) projects. A project is
described by a `Buildfile`; satria handles compilation, dependency
tracking, incremental builds, libraries, workspaces, embedded assets,
compile_commands, and packaging — with no cmake/make layer of your own.

```bash
mkdir -p hello/src
printf 'int main(void){return 0;}\n' > hello/src/main.c
cd hello && satria init && satria   # produces bin/hello
```

## Highlights

- **Workspace builds** — one `WsBuildfile` builds every member project in
  dependency order; `satria -w` (workspace), `satria -w NAME` (one project
  plus its closure), `satria -w release` (centralized `dist/`).
- **Releases** — tarball + optional `.deb` (built without dpkg-deb),
  SHA-256 sidecars, reproducible-detection for GNU tar,
  `release.format = deb` adds the deb beside the tarball.
- **Fastest compression option** — `release.optimize = true` builds the
  tarball with the compressor's lowest (fastest) level; you own the size
  trade-off.
- **Trace** — `satria trace` prints strace-like `op(args) = result` lines
  (with named errnos) for diagnosing why a build did (or did not) happen.
- **No-op is real** — content fingerprints (global PATCH hash) let repeated
  builds stop at the gate: 0 compiled, 0 linked, still correct.
- **Ad-hoc compile** — `satria src/x.c -o build/x.o -b bin/x` (or `-F
  quick.conf`) behaves like the gcc command you would have typed yourself:
  no Buildfile, no `.satria` state.
- **`satria test`** — runs the unit-test suite for a fixture (`satria test
  miniws`, `-t <name>`, `-j N`, `-w` to watch); tests are written in the
  `.test` DSL, not shell.

## Documentation

Note: the guide and reference pages are synced with the current code
(v0.6.0); version numbers inside examples are illustrative. Newer features
(ad-hoc compile, `--dev`, unit-test DSL) have their own pages. The newest
specs always live in the repository's `design/` folder
(`design/packaging.md`, `design/README.md` and friends).

### Guide

- [Getting Started](guide/getting-started)
- [Buildfile](guide/buildfile)
- [Incremental Builds and Dependency Tracking](guide/incremental)
- [Workspace](guide/workspace)
- [Releases](guide/releases)
- [Packaging](guide/packaging)
- [Archives and Embedded Assets](guide/embedded-and-archives)
- [Platforms and Bootstrap](guide/platforms)
- [Profiling](guide/profile)

### Reference

- [CLI Reference](reference/cli)
- [Buildfile Field Reference](reference/buildfile-fields)
- [Examples](reference/examples)
- [Errors and Interrupts](reference/errors-and-interrupts)
- [Compilation Database](reference/compdb)
- [Ninja Buildfile Import](reference/satria-ninja-import)
- [Unit Tests](reference/unit-test)
- [Cheatsheet](cheatsheet)

### Design & Benchmark

- [Design Philosophy](design/philosophy)
- [Architecture Notes](design/architecture)
- [Configuration Design](design/configuration)
- [Direction Notes](design/roadmap-notes)
- [Benchmark Methodology](benchmark/methodology)
- [Field Notes from satria Development](benchmark/field-notes)

## Version scope

This snapshot documents **satria v0.6.0 (pre-release)**: the v0.5.0
feature set plus
ad-hoc compile (`-o`/`-b`/`-F`), `--dev`, project language selection
(`use project c|cpp`), the unit-test DSL and `satria test`, unique members
in static-library archives, and the refreshed CLI reference. Features
still in flux are documented in the repository `design/` folder. Source,
issues, and the latest design notes live on
[GitHub - aidomx/satria](https://github.com/aidomx/satria).

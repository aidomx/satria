# CLI Reference

## Build

```bash
satria
```

Build the current project from `Buildfile`.

## Alternate Buildfile

```bash
satria -f FILE
```

Use another Buildfile.

## Initialize project

```bash
satria init
```

Create a project Buildfile — for a standard project this is just `use project`.

## Interactive init

```bash
satria init -p [--here] [--no-run]
```

Interactive scaffold. Enter accepts the default at every question; the
scaffold is written into a new project folder, so it can be run from
anywhere. Before writing, a summary of the answers is shown for
confirmation. On a terminal, the scaffold is then built and run
automatically; `--no-run` skips that step. `--here` puts a single-project
scaffold in the current directory instead of `<name>/` (a workspace still
keeps its project folders):

| Question | Default (Enter) |
|---|---|
| `Project name?` | random name (`satria-xxxxxx`) — also the scaffold folder, unless `--here` is used |
| `language?` | `c` — answering `cpp` scaffolds `src/main.cpp` (iostream) and writes `std = c++17` |
| `std?` | none written — convention (`gnu11`, or `c++17` for cpp) |
| `compiler?` | none written — auto-detect |
| `flags?` | none written — defaults |
| `Create include/?` | `N` |
| `Uses workspace?` | `N` |
| `Workspace projects?` (workspace only) | `app,tool` |
| `depends_on?` (workspace only) | none — format `<project> depends <project>` |

Non-default answers are written explicitly into the Buildfile (`std = ...`,
`compiler = ...`, `flags = ...`); everything else stays convention-based.
The scaffold builds and runs out of the box:

```text
<name>/
├── Buildfile        # single-project only; workspace projects need none
├── include/<name>.h # when include/ is requested
└── src/main.c       # src/main.cpp when language = cpp
```

A workspace scaffold writes `WsBuildfile` (`projects = ...`, optional
`<p>.depends_on = <p>` lines) plus a `src/main.c` per member — no per-project
Buildfiles; satria synthesizes them. After a workspace build, the run step
offers the primary project or another member by number. `init -p` never
overwrites: if any target file already exists, it aborts before writing
anything. Ctrl+C or EOF on a question cancels the scaffold.

## Initialize workspace

```bash
satria init -w
```

Create a workspace Buildfile.

## Workspace build

```bash
satria -w
satria -w NAME
```

Build all projects or one named project and its dependencies.

```bash
satria -w NAME --with-cdeps
```

Opt-in: the selective closure also pulls in `cdeps` providers (transitively,
including cyclic cdeps) — for example `satria -w bade --with-cdeps` builds
`core` when bade links its library through cdeps. Without the flag the old
contract holds: cdeps never widens the closure. The flag is only valid with
`-w` (otherwise exit code 2). When a cdeps provider outside the closure has
no library on disk yet, a pre-flight warning names it and suggests
`--with-cdeps`.

## Dev mode

```bash
satria --dev
satria -w --dev
```

Development build: the release/packaging phase is ignored for this run —
compile + link only. In a workspace this skips the release pass (the
build report shows `* Release : skipped (--dev)`); in a single project
`pack.*` is treated as absent (pure packaging projects still build their
embedded archives, since other projects depend on them). Useful to
measure compile+link cost while iterating. Per-run only: no Buildfile or
state mutation. Rejected combinations: `-w release --dev` and `--dev`
together with a `--` selector.

## Workspace release

```bash
satria -w
satria -w release -- key=value[,key=value...]
```

Build the workspace and package ONE release artifact into the workspace
`dist/` (when a `release.*` section exists in `WsBuildfile`). With `--`,
each `key=value` pair overrides a release setting for this run (`name`,
`version`, `format`, `target` — accepted as a legacy alias of `format`).
See [Releases](../guide/releases.md).

## Clean

```bash
satria clean
```

Remove generated build artifacts according to the active project/workspace configuration.

## Watch a background build

```bash
satria watch
satria watch --full
satria -w watch [--full]
```

With `background = true` in `Buildfile` (or as a workspace-global line in
`WsBuildfile`), a build starts detached and returns immediately; output goes
to `.satria/background.log`. `satria watch` replays progress, the summary and
the final status, then exits; `--full` streams the log live. The watcher never
starts a build of its own.

## Compilation database only

```bash
satria -g compdb
```

Write/refresh `compile_commands.json` without building. See
[Compilation Database](compdb.md).

## Convert Ninja or Makefile

```bash
satria -xf build.ninja     # convert for this run only
satria -xcf Makefile       # convert and save as Buildfile
```

See [Ninja Buildfile Import](satria-ninja-import.md).

## Jobs

```bash
satria -j
satria -j4
satria -j1
satria --jobs=4
```

Control parallel build jobs. Flexible default: without `-j`, the job count
comes from the `SATRIA_JOBS` environment variable when set, otherwise from
the CPU core count. `-j1` forces serial. Applies to normal builds and to
ad-hoc compiles alike.

## Ad-hoc compile

```bash
satria src/x.c -o build/x.o
satria src/x.c -o build/x.o -b bin/app
satria src/a.c src/b.c -o build -b bin/app
satria src/x.c -F quick.conf
```

Compile without a Buildfile lifecycle: no project config, no `.satria`
state, behaves like the gcc command you would have typed yourself.

- `-o <path>`: object path (one source) or object directory (multi-source —
  one `<basename>.o` per source, mirroring gcc).
- `-b <path>`: link a binary; requires `-o`. With multi-source it links
  every object produced from `-o`.
- `-F <file>`: compile + link using a lightweight config file — `flags`,
  `headers`, `defines`, `std` and `o.*` paths come from `<file>`; release,
  cdeps and multi-project logic is skipped. Relative paths resolve against
  the directory of `<file>`. Built-in aliases `output as o`, `clean as c`
  and `library as lib` work in this mode.
- `-jN` works here too (see [Jobs](#jobs)); the link step always runs once
  at the end.
- Mixing `-F` with `-o`/`-b` is an error; sources together with `-f` or
  `-w` is an error. No `.satria` state is written, so ad-hoc runs never
  no-op; compiler failures pass through unchanged. Compiler flags such as `-O2` or `-I` are not accepted
  on the CLI — put them in the `-F` config (`flags`, `headers`, `defines`).
- The compiler is auto-detected (`cc`, `gcc`, `clang`, `tcc`, `cl` — first
  on `PATH` wins); a `compiler =` key in the `-F` config overrides it.

See `satria help adhoc` and [design/compile.md](../../design/compile.md).

## Unit tests

```bash
satria test miniws                 # full sweep for the miniws fixture
satria test miniws -t cache_noop   # one test only
satria test complexws -j 2         # whole suite, 2 jobs
satria test -w                     # watch an active run
```

Delegate to `scripts/unit.sh`: `<target>` is a fixture name under `tests/`,
not a path. The exit code of the runner becomes the exit code of satria.
Tests are `.test`/`.t` DSL files under `tests/unit/common/` (classification
in `tests/unit/classes.conf`); see [Unit Tests](unit-test) for running and
writing them.

## Profile

```bash
satria profile <context> [options]
```

Measure the cost of satria operations without performing a normal build. Does
not touch build state (`.satria/`, `Buildfile`, artifacts); temporary data is
isolated and removed afterwards. All arguments after `profile` belong to the
context — global satria options are not parsed there.

```bash
satria profile archive=DIR [with=tar|none|tar,gz|tar,xz|tar,bz2]
satria profile embed=DIR|FILE
satria profile library=DIR [with=static|shared|static,shared]
satria profile binary=DIR [jobs=N] [--report FILE]
satria profile project=DIR [jobs=N]
satria profile workspace=DIR
```

- `with=` is context-specific; an option that a context does not support is
  rejected with `satria: profile option 'with' is not valid for context '<ctx>'`.
- Terminal output is a short summary; piped output is the detailed report —
  `satria profile archive=DIR > profile.md` is enough.
- `--report FILE` writes the detailed report to a file and keeps the summary
  on the terminal.
- `--repeat N` (1–100) repeats the run; the sandbox is kept between runs
  (caches live inside it), so later runs measure the warm path; timings
  aggregate over all runs.
- Paths accept `~/...` (expanded by satria — shells only expand `~` at word
  positions).

See [Profiling](../guide/profile.md) for examples and measured phases.

## Version and help

```bash
satria version
satria help
```


## Self-upgrade

```bash
satria self-upgrade
```

Check the latest release of satria through the official GitHub Releases API
and use the official installer to upgrade satria itself. This does not update
project dependencies or system packages. If satria is running from a development
checkout or an unrecognized custom installation path, automatic replacement
is refused and manual instructions are shown. If the version check fails, the
local installation is left unchanged. On Windows, automatic upgrade is limited
to the official per-user installation at `%LOCALAPPDATA%\satria\satria.exe`.
Because Windows locks the running executable, satria launches a temporary
PowerShell helper in a separate console; the helper waits for satria to exit,
checks GitHub Releases, and invokes `install.ps1` from the validated release tag.

## Self-uninstall

```bash
satria self-uninstall
satria self-uninstall --yes
```

Remove satria's own installation and globally managed state (`$SATRIA_HOME`,
default `~/.satria/`). On an interactive terminal, satria asks for confirmation.
In non-interactive mode, `--yes` (or `-y`) is required. The command must not
remove project build state (`.satria/`), project source dependencies (such as
`third/`), system packages, or files whose ownership by satria cannot be
verified. A binary is removed only when its installation path is recognized
and safe to remove.

## Ninja conversion

The tree also contains Ninja Buildfile conversion support. The exact conversion flags are implementation-specific; `satria -h` is authoritative for the installed binary.

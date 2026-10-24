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
satria init -p
```

Interactive scaffold. Enter accepts the default at every question; the
scaffold is written into a new project folder, so it can be run from
anywhere:

| Question | Default (Enter) |
|---|---|
| `Project name?` | random name (`satria-xxxxxx`) — the name is always the scaffold folder |
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

A workspace scaffold writes `Buildfile.ws` (`projects = ...`, optional
`<p>.depends_on = <p>` lines) plus a `src/main.c` per member — no per-project
Buildfiles; satria synthesizes them. `init -p` never overwrites: if any target
file already exists, it aborts before writing anything. Ctrl+C or EOF on a
question cancels the scaffold.

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

## Workspace release

```bash
satria -w
satria -w release -- key=value[,key=value...]
```

Build the workspace and package ONE release artifact into the workspace
`dist/` (when a `release.*` section exists in `Buildfile.ws`). With `--`,
each `key=value` pair overrides a release setting for this run (`name`,
`version`, `format`, `target` — accepted as a legacy alias of `format`).
See [Releases](../guide/releases.md).

## Clean

```bash
satria clean
```

Remove generated build artifacts according to the active project/workspace configuration.

## Jobs

```bash
satria -j
satria -j4
satria -j1
satria --jobs=4
```

Control parallel build jobs.

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

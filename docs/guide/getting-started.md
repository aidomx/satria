# Getting Started

## The basic idea

satria is a declarative build tool for C projects. A project is described by a `Buildfile`; satria handles compilation, dependency tracking, incremental builds, libraries, workspaces, embedded assets, compile commands, and packaging.

The intended user experience is simple: a conventional project should require very little configuration, while unusual projects can opt into explicit settings.

## Minimal project

A standard project — `src/` for sources, optionally `include/` for public
headers — needs only one line in its Buildfile:

```text
use project
```

Everything else follows satria conventions: `sources` defaults to `src`,
`headers` to `include` when that folder exists, the binary is named after
the project folder, and `std = gnu11`, `bin/`, `build/` fill in the rest.
C++ works the same way with zero declarations — `src/main.cpp` instead of
`src/main.c` gives you `std = c++17` and the `g++`/`clang++` toolchain
automatically.
A quick test:

```bash
mkdir -p hello/src
printf 'int main(void){return 0;}\n' > hello/src/main.c
cd hello && satria init && satria   # produces bin/hello
```

Non-standard layouts stay free: any field declared explicitly wins over
the convention (see [Buildfile](buildfile.md)).

## Initialize

```bash
satria init
```

Creates the minimal Buildfile above — just `use project`.

For a guided walkthrough — project name, optional `include/`, workspace
with member projects and `depends_on` — use the interactive scaffold:

```bash
satria init -p
```

Enter accepts the default at every question (a random name for an empty
answer). It scaffolds a new project folder that builds and runs
immediately; see [CLI](../reference/cli.md) for the full question list.

For a workspace:

```bash
satria init -w
```

creates a workspace Buildfile listing projects:

```text
use workspace

projects = app
```

Each project follows the same standard convention: a folder `app/`
containing `src/` (and optionally `include/`), built with `satria -w`
from the workspace root.

## Build

```bash
satria
```

Without arguments satria searches for `Buildfile` and builds the project.

## Other common commands

```bash
satria -f FILE
satria clean
satria version
satria help
satria -j4
satria -j1
satria watch
```

`-j` enables the normal parallel build behavior; `-jN` chooses an explicit job
count. `satria watch` monitors a background build (`background = true`, see
[Buildfile](buildfile.md#background-build)) until it finishes, prints the
build summary and exits.

## Version

The project version lives in `.satria-version` at the project root and is
embedded into the binary at build time:

```bash
printf 'v0.2.0\n' > .satria-version
satria
satria version
```

Version changes trigger the rebuilds that need them. C code can read the
embedded version through `build/version.h`:

```c
#include "version.h"
printf("%s\n", SATRIA_VERSION_EMBEDDED);
```

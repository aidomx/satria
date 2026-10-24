# Examples

## Minimal project

Desired simple form:

```text
use project
```

## Explicit C project

```text
use project

compiler = gcc, clang
flags = Wall, Wextra, O2, MMD, MP
headers = include, I.
sources = src

output.binary = true
output.binaryName = app
output.binaryDir = bin
output.buildDir = build
output.compileCommands = auto
```

## Library project

```text
use project

sources = src
headers = include
compiler = gcc, clang
flags = Wall, Wextra, O2, MMD, MP

output.binary = false
output.libraryName = core
output.libDir = lib
output.libraryShared = true
```

## Workspace

```text
use workspace

projects.core as core
projects.app as app

core.sources = src
core.output.binary = false
core.output.libraryName = core

app.sources = src
app.depends_on = core
app.output.binaryName = app
```

The exact alias/field combinations should be checked against the installed satria version when using a configuration that goes beyond the documented examples.

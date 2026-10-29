# Workspace

A workspace groups multiple satria projects under one top-level Buildfile.

## Initialization

```bash
satria init -w
```

## Basic form

A minimal workspace only lists its projects:

```text
use workspace

projects = app, tool
```

Each project follows the standard project convention (see
[Buildfile](buildfile.md)): a folder `<name>/` under the workspace root
containing `src/` and optionally `include/`. The projects need no
Buildfile of their own — satria synthesizes one per project from the
workspace settings, with the binary named after the project. Common
settings (`flags`, `std`, ...) declared at the workspace level apply to
every project.

Non-standard projects stay free: declare per-project settings explicitly
in the workspace file.

```text
projects.rupamod as mod
projects.rupa as rupa
projects.ruka as ruka
```

A workspace can define common compiler settings and project-specific settings.

## Root and output locations

Every project builds with the project folder as its working directory, so
`root` is a **per-project** setting. A workspace-level `root = .` is
**ignored with a warning** — it does not aggregate outputs into the
workspace folder. Each project writes its own `bin/`, `lib/`, `build/`
inside its folder.

To aggregate outputs at the workspace root, point each project's output
folders there with relative paths (`..` = workspace root):

```text
app.output.binaryDir = ../bin
app.output.libDir = ../lib
mod.archive.dir = ../modules
```

Packaging (archive) projects such as `mod` above run once during the
library phase (as `Fase : archive`) and are skipped silently in the
binary phase — their artifacts (archives, packages) are produced there
and are not rebuilt.

## Build commands

```bash
satria -w
```

Build all projects in the workspace.

```bash
satria -w rupa
```

Build a named project and the dependencies needed by it.

```bash
satria -w release -- name=rupa
cd ../ruka && satria -w clean
```

Release a single project (see [Releases](releases.md)); `clean` works per
project root like build.

With `background = true` in `Buildfile.ws`, `satria -w` returns to the
prompt immediately and the build runs detached; watch it with:

```bash
satria -w watch            # progress until done, then the summary
satria -w watch --full     # stream .satria/background.log live
```

`clean` is blocked while a background build is running. See
[Buildfile](buildfile.md#background-build) for details.

## Dependencies

A project can declare another workspace project as a dependency:

```text
ruka.depends_on = rupamod
```

This is intended to make project ordering explicit while keeping each project independently buildable.

## Library-first builds

Workspace projects can be configured to produce libraries before final binaries. This is useful when multiple projects depend on each other's libraries or when the workspace has a two-stage relationship between libraries and executables.

The important model is:

1. establish required libraries;
2. link the final binaries once their library dependencies exist.

This avoids forcing users to manually orchestrate intermediate library builds.

Cross-project library references (`<n>.library.<os> = <project>`) resolve to
the other project's library artifact. Cyclic references (`rupa <-> ruka`) are
allowed: the two-phase build resolves them, and selective builds pull the
referenced projects transitively.

### cdeps — compile-time dependencies in one key

When a project consumes another project's headers **and** its library, the
per-OS spellings add up. `cdeps` replaces them with a single key:

```text
# before
ruka.headers = ../rupa/include
rukalib.linux = rupa
rukalib.macos = rupa
rukalib.windows = rupa

# after
ruka.cdeps = rupa
```

Synthesis expands `cdeps` internally into the dependency's library artifact
(`library = ../rupa/lib/librupa.a`) and its public include folders (the
folders the dependency itself declares as `headers`; `include` by default).
`cdeps` cycles are allowed exactly like `library.<os>` cycles — the two-phase
build resolves them — and selective builds (`-w <name>`) pull `cdeps`
projects transitively. The expanded lines win over nothing: any explicit
`headers`/`library` you also write stays valid and is not duplicated.

Packaging a project's files can reference another project's build outputs;
such references do NOT order the build by themselves. Declare the order with
`<project>.depends_on = <other>` (the list order of `projects` is not enough when a
referenced output must exist first). `depends_on` is kept on purpose: it also guards
the order when the `projects` list is arranged differently.

## Shared internal headers

A folder of internal headers shared by several projects (for example a `library/`
folder included as `"../../library/intl.h"`) is not a project and must not be a
header search path. Adding it to `headers` puts it on `-I`, where its files can
shadow system headers (for example `library/stdlib.h` hides `<stdlib.h>`). Declare
it with `headers.internal` instead:

```text
headers = include, build, I.
headers.internal = ../library
```

satria then tracks files in that folder as build inputs (editing one rebuilds every
object that includes it, and a touch with unchanged content rebuilds nothing), but
does not add the folder to the compiler's include path.

## Releases

A workspace can also declare distribution units — see
[Releases](releases.md). In short: `releases = ...` names the projects whose
artifacts get packaged into the workspace-level `dist/release/`, and
`satria -w release -- name=<project>` packages a release straight from the CLI.

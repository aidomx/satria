# Workspace

A workspace groups multiple satria projects under one top-level Buildfile.

## Filename

The workspace file is called **`WsBuildfile`** (no extension — `.ws` belongs
to Witcher Script, which confused editors and tools). `satria init -w` writes
that name.

`Buildfile.ws` (and the older `Buildfile.workspace`) is still read during the
transition window, printing a one-time deprecation warning that asks you to
rename it; both legacy names will be removed once the transition window ends.
When more than
one candidate exists, detection order is `WsBuildfile`, `Buildfile.ws`,
`Buildfile.workspace`.

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
library phase and are skipped silently in the binary phase — their artifacts
(archives, packages) are produced there and are not rebuilt. The build report
lists them under `# Archive` / the execution summary.

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
satria -w rupa --with-cdeps
```

A selective build normally stays inside the dependency closure. `--with-cdeps`
(opt-in, only with `-w`) also pulls the project's `cdeps` providers —
transitively and across cdeps cycles — so the compile-time dependency's library
is available too. Without the flag, a provider outside the closure is not built;
if its library is missing on disk a pre-flight warning names it and suggests
this flag (the link would otherwise fail).

`satria --dev` / `satria -w --dev` skips the release or packaging phase for one
run (compile + link only); see [CLI Reference](../reference/cli.md#dev-mode).

```bash
satria -w release -- name=rupa
cd ../ruka && satria -w clean
```

`-- name=rupa` renames the release package for this run (it does not select a
project); a selective `-w <project>` never packages a release. `clean` works
per project root like build. See [Releases](releases.md).

With `background = true` in `WsBuildfile`, `satria -w` returns to the
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
build resolves them — but a selective build (`-w <name>`) does **not** widen
its closure through `cdeps` unless you pass `--with-cdeps`, which pulls the
providers transitively (across cdeps cycles too). The expanded lines win over
nothing: any explicit
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

A workspace can also declare one distribution unit — see
[Releases](releases.md). In short: a single `release.*` section (same keys as
`pack.*`) describes the package, `satria -w` builds and packages it into the
workspace-level `dist/`, and `satria -w release -- key=value` overrides
settings for one run. The old per-project `releases = a, b` syntax is removed.

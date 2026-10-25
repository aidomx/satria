# Buildfile Field Reference

Reference for the Buildfile keys that satria v0.2.4 (alpha) actually accepts.
Keys are written as `section.subkey = value`. A few keys have per-platform
variants (`library.<platform>`, `flags.<platform>`) and workspace-only forms
(`<project>.<key>`); those are noted in place.

For concepts and layout, see the [Buildfile guide](../guide/buildfile.md) and
the [workspace guide](../guide/workspace.md).

## Mode

| Key | Purpose |
|---|---|
| `use project` | Single-project mode. |
| `use workspace` | Workspace mode (requires a `Buildfile.ws` at the workspace root). |
| `name as alias` | Declares an alias. `clean as c`, then `c.build = false`. |

## Sources and headers

| Key | Purpose |
|---|---|
| `sources` | Source directories, relative to the project root. Several roots may be listed. |
| `exclude` | Sources left out of the library (see below). |
| `headers` | Public include directories, passed as `-I`. |
| `headers.internal` | Directories tracked as build inputs but **not** passed as `-I`. Use for shared internal headers included by relative path. |
| `headers.each` | Workspace only. Applies a `headers` value to every project, or to the projects named in it (see below). |
| `defines` | Preprocessor definitions. |
| `flags` | Compiler flags. `flags.<platform>` applies on that platform only. |
| `std` | Language standard, for example `gnu11` or `c++17`. |
| `compiler` | Compiler candidates in order, for example `gcc, clang`. |
| `foreground` | `true` or `false`. Controls the child process group and SIGINT behaviour. |
| `log` | `true` or `false` for enable log |

### `exclude`

A value matches a source when it equals the source path relative to the
**project** root, the source path as written, or the basename. A value ending in
`/` excludes everything under that directory.

```text
bade.exclude = manager/main.c   # relative to the bade/ folder
core.exclude = main.c           # basename
```

### `headers.each` (workspace)

```text
headers.each = I.                     # every project: <project>/include
headers.each = bade, I.               # only bade receives "I."
headers.each = auto                   # include/ of each project, when it exists
```

Tokens that match a project name filter the rest of the list. `I.` means the
project's own `include/`. `auto` means `I.` only when that folder exists.

### Include path

- The workspace root `include/` is added as a default when it exists.
- A project's default `include/` is added when it exists and no `headers` is set.
- The project's **output directory** (`build/` in project mode, `build/<project>/`
  in workspace mode) is always on the include path. Generated headers such as
  `embedded.h` and `version.h` are found there without any `headers` line.

## Libraries and linking

| Key | Purpose |
|---|---|
| `library` | Libraries used when linking. |
| `library.<platform>` | Libraries used on that platform only. |
| `<project>.cdeps` | Workspace only. Depends on other projects by name. Adds the dependency's library and public headers to this project. |
| `<project>.depends_on` | Workspace only. Builds the named projects first. |

## Outputs

| Key | Default | Purpose |
|---|---|---|
| `output.binary` | `true` | Build the binary artifact. |
| `output.binaryName` | project folder name | File name of the binary. |
| `output.binaryDir` | `bin` | Binary output directory. |
| `output.buildDir` | `build` | Object and dependency directory. Workspace: `build/<project>`. |
| `output.libraryName` | — | Library name. Setting it requests a library. |
| `output.libraryStatic` | `true` when a library is requested | Build `lib<name>.a`. |
| `output.libraryShared` | `false` | Build `lib<name>.so`. |
| `output.libDir` | `lib` | Library output directory. |
| `output.compileCommands` | `auto` | `auto` and `true` write `compile_commands.json`. `false` disables it. |

In workspace mode, `bin/`, `lib/`, and `build/<project>/` are created at the
workspace root.

## Clean

| Key | Default | Purpose |
|---|---|---|
| `clean.build` | `true` | Remove the build directory on `clean`. |
| `clean.compdb` | `false` | Remove `compile_commands.json` on `clean`. |

## Progress

| Key | Default | Purpose |
|---|---|---|
| `progress.bar` | `true` | Show the progress bar. |
| `progress.error` | `always` | `always` shows every error block. Any other value shows them only once. |

## Embedded data

Embedded data is declared per project with a named entry:

```text
bade.embedded.modules as bademod

bademod.file = modules/rupa_modules.tar
bademod.variable = MODULES
bademod.extract = /tmp/satria-system
bademod.pattern = .rp
```

| Subkey | Purpose |
|---|---|
| `file` | Use an existing archive. Relative to the workspace root in workspace mode. |
| `src` | Archive the contents of this folder instead of using `file`. |
| `key` | Access key for `getEmbed("<key>")`. Default: the entry name. |
| `type` | Label in the `type` field of `Embedded`. Default: `archive` when `.file`/`.src` is set, otherwise `asset`. |
| `variable` | Name used in the generated compatibility macros, `EMBED_<VARIABLE>_*`. |
| `extract` | Extraction target, exposed as the `extract` field (and `EMBED_<VARIABLE>_EXTRACT_DIR`). |
| `pattern` | Only files matching this pattern are archived. |
| `exclude` | Files and folders left out of the archive. |
| `dir` | Folder where the archive is written. |
| `tar`, `ext` | Archive format, for example `tar` with `ext = gz`. |
| `enable` | `false` disables the entry. |

Satria generates `embedded.c` and `embedded.h` in the output directory.
Include the header as `#include <embedded.h>`; the directory is on the
include path (see above). The `.c` file holds the byte arrays and is built
like any other source (no `ld -r -b binary` is involved).

Access the data through one API:

```c
#include <embedded.h>

const Embedded *e = getEmbed("modules"); /* NULL when the key is unknown */
if (e && e->data) {
  printf("%s: %zu bytes (extract: %s)\n", e->name, e->size, e->extract);
  fwrite(e->data, 1, e->size, stdout);
}

for (size_t i = 0; (e = getEmbedAt(i)) != NULL; i++)
  printf("%s\n", e->key);
```

`Embedded` is a flat struct: `key`, `type`, `name`, `extract`, `size`,
`data`. The legacy `EMBED_<VARIABLE>_*` macros still work for existing
consumers during the transition.

## Archive (workspace)

Archive-only projects produce an archive and no binary:

```text
mod.output.binary = false
mod.archive.src = .
mod.archive.name = rupa_modules
mod.archive.with = tar
mod.archive.exclude = build, dist, LICENSE, README.md
mod.archive.dir = modules
```

`with = tar` and `with.tar = true` select the tar format. `with.ext` selects the
compression (`gz`, `xz`, and so on).

## Packaging and release

| Key | Purpose |
|---|---|
| `pack.name`, `pack.version` | Package name and version. |
| `pack.output` | Package output path or template. |
| `pack.files` | Files to include, optionally as `src:dst`. |
| `pack.format` | `tar` or `deb`. |
| `pack.checksum` | `sha256` (writes a `.sha256` file) or `none`. Any other value prints a warning. |
| `pack.exclude` | Paths left out of the package. |
| `pack.compress` | Compression for tar output, for example `gz` or `xz`. |
| `pack.deb.install_prefix`, `pack.deb.maintainer`, `pack.deb.description`, `pack.deb.architecture` | Debian package metadata. |
| `release.*` | Same keys as `pack.*`, applied to a whole workspace or project. Setting any `release.*` key enables the release phase. |

In workspace mode, `pack.*` set on an individual project is ignored with a
warning. Use `release.*` at the workspace level instead.

## Workspace-only keys

| Key | Purpose |
|---|---|
| `projects = a, b` | Declares the member projects. |
| `projects.<name> as <alias>` | Declares a member project with an alias. |
| `<project>.root` | The project folder, relative to the workspace root. |
| `log` | Build log setting. |

Workspace-level `root` is ignored with a warning. Set `root` per project.

## Defaults for standard projects

A standard project (folders `src/` and optional `include/`) needs only
`use project`. These values apply when a key is not declared:

| Key | Default |
|---|---|
| `sources` | `src` |
| `headers` | `include`, when that folder exists |
| `output.binaryName` | project folder name, sanitized (fallback `satria`) |
| `std` | `gnu11`, or `c++17` when the sources contain `.cpp` (the compiler then defaults to `g++`/`clang++`) |
| `output.binaryDir` | `bin` |
| `output.buildDir` | `build` |
| `output.compileCommands` | `auto` |
| `output.libDir` | `lib` |
| `progress.bar` | `true` |
| `foreground` | `true` |

Explicit declarations always win over these defaults.

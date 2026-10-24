# Buildfile Field Reference

The following fields are part of the current configuration model (v0.2.4) or are exercised by the project/workspace examples.

| Field | Purpose |
|---|---|
| `use project` | project mode |
| `use workspace` | workspace mode |
| `root` | project root |
| `sources` | source directories |
| `headers` | include/header directories (passed as `-I`) |
| `headers.internal` | header directories tracked as build inputs but NOT passed as `-I`; use for shared internal headers included by relative path, e.g. `headers.internal = ../library` |
| `exclude` | sources excluded from a target |
| `flags` | compiler flags |
| `std` | C language standard |
| `compiler` | compiler candidates |
| `foreground` | child/process group and SIGINT behavior |
| `library` | libraries used during linking |
| `library.<platform>` | platform-specific link libraries |
| `output.binary` | enable/disable binary artifact |
| `output.binaryName` | binary filename |
| `output.binaryDir` | binary output directory |
| `output.buildDir` | build/object directory |
| `output.compileCommands` | compile database generation mode |
| `output.libraryName` | library name |
| `output.libDir` | library output directory |
| `output.libraryShared` | shared library generation |
| `progress.bar` | progress UI |
| `progress.error` | error display policy |
| `pack.name` | package name |
| `pack.version` | package version |
| `pack.output` | package output path/template |
| `pack.files` | package file list/mapping |
| `pack.format` | package format |
| `pack.checksum` | checksum generation |
| `pack.merge` | merge package files from another project |
| `pack.deb.install_prefix` | Debian install prefix |
| `pack.deb.maintainer` | Debian maintainer |
| `pack.deb.description` | Debian description |
| `pack.deb.architecture` | Debian architecture |
| `release.*` | single workspace/project release (distribution unit; keys mirror `pack.*` — see Releases guide) |
| `release.files` | release content: file/folder entries, optional `src:dst` mapping |
| `release.exclude` | paths/basenames/dirs ignored while walking folder entries |
| `release.format` | release packaging format: `deb` or `tar` |

Aliases can be introduced with `name as alias`, for example:

```text
clean as c
output as o
```

and then referenced as `c.*` or `o.*`.

## Defaults for standard projects

Undeclared fields follow satria conventions, so a standard project (folders
`src/` and optional `include/`) needs only `use project`:

| Field | Default when undeclared |
|---|---|
| `sources` | `src` |
| `headers` | `include` (only when the folder exists) |
| `output.binaryName` | project folder name, sanitized (fallback `satria`) |
| `std` | `gnu11` — or `c++17` when sources contain `.cpp` (compiler then defaults to `g++`/`clang++`) |
| `output.binaryDir` | `bin` |
| `output.buildDir` | `build` |
| `output.compileCommands` | `auto` |
| `output.libDir` | `lib` |

Explicit declarations always win over these defaults; see
[Buildfile](../guide/buildfile.md) for details.

# Buildfile

satria uses a declarative `Buildfile` with `key = value` assignments and `use` declarations.

A representative Buildfile is:

```text
use project

clean as c
library as lib
output as o

root = .
sources = src
headers = include, I., build
flags = Wall, Wextra, O2, MMD, MP
std = gnu11
compiler = cl, gcc, clang

c.build = false
c.compdb = false

o.binaryName = satria
o.binaryDir = bin
o.buildDir = build
o.compileCommands = auto
```

## Minimal form: standard projects

For a standard layout — `src/` for sources, optionally `include/` for
public headers — the whole Buildfile can be a single line:

```text
use project
```

The representative Buildfile above stays valid, but nothing in it is
required: defaults fill only the fields you leave out, and every explicit
declaration wins over the convention. Non-standard layouts therefore stay
free to declare `sources`, `headers`, and output names explicitly.

### Implied defaults

| Field | Default when undeclared |
|---|---|
| `sources` | `src` |
| `headers` | `include` — only when an `include/` folder exists |
| `output.binaryName` | the project folder name (see below) |
| `std` | `gnu11` — or `c++17` when sources contain `.cpp` |
| `output.binaryDir` | `bin` |
| `output.buildDir` | `build` |
| `output.compileCommands` | `auto` |
| `output.libDir` | `lib` |

Notes:

- The binary name is the cwd folder's base name, sanitized: characters
  outside letters, digits, `_`, `-`, `.` become `_` (fallback `satria` if
  the name is unusable).
- `-Iinclude` reaches the compiler and `compile_commands.json` through
  the `headers` convention.
- With the default `sources` but no `.c` files in `src/`, the build fails
  with a guided error: put sources under `src/` or set `sources = ...`
  for a different layout.

## Core fields

### `use project`

Selects project mode.

### `root`

Project root used as the base for project paths.

### `sources`

Source directories scanned for C sources.

### `headers`

Include/header directories. Multiple paths can be comma-separated.

### `flags`

Compiler flags. For example:

```text
flags = Wall, Wextra, O2, MMD, MP
```

`MMD`/`MP` are useful with GNU/Clang dependency files.

### `std`

C language standard, for example:

```text
std = gnu11
```

### `compiler`

Compiler candidates, for example:

```text
compiler = cl, gcc, clang
```

The platform/compiler implementation chooses an available compiler.

## Aliases

The configuration supports aliases:

```text
clean as c
library as lib
output as o
```

Then the corresponding settings can be addressed through the alias:

```text
o.binaryName = app
o.binaryDir = bin
o.buildDir = build
```

## Output

A typical executable configuration:

```text
output as o

o.binaryName = app
o.binaryDir = bin
o.buildDir = build
o.compileCommands = auto
```

Library output can be configured through the output/library fields when the project needs static or shared artifacts.

## Boolean binary switch

`output.binary` is used as a boolean switch in the current configuration model. In particular:

```text
output.binary = false
```

means that the binary artifact is skipped while library/artifact work can still occur.

This distinction is important for workspace projects that exist primarily to create a library or archive.

## Libraries

Platform-specific libraries can be expressed as:

```text
library.default = ssl, crypto
library.linux = m, pthread
library.macos = m
library.windows = ws2_32
```

## Platform & compiler suffixes

`flags`, `library`, `headers`, `defines` and `std` accept a target suffix:

```text
<property> = <global value>
<property>.<target> = <specific value>
```

Supported targets — platform: `linux` (includes Android/Termux), `android`,
`macos`, `windows`, `freebsd`, `unix` (POSIX fallback when no platform
suffix matches); compiler: `gcc`, `clang`, `cl`.

The global value is applied first, matching suffix values are appended
after it (duplicates removed). `std` is scalar, so `std.<target>`
overrides the global `std`. Example on Linux with GCC:

```text
flags = Wall, Wextra
flags.linux = D_REENTRANT
flags.gcc = O2
```

resolves to `-Wall -Wextra -D_REENTRANT -O2`.

## Defines

Preprocessor macros are declared without any `-D`/`/D` prefix — satria
translates them for the active compiler (GCC/Clang: `-DDEBUG`, MSVC:
`/DDEBUG`):

```text
defines = DEBUG, VERSION=1.0.0
defines.linux = _POSIX_C_SOURCE=200809L
defines.windows = UNICODE, WIN32_LEAN_AND_MEAN
```

## Process behavior

`foreground` controls process-group/SIGINT behavior. satria can own SIGINT handling and forward interruption to child build processes.

## Progress

The build UI can be configured with progress settings such as:

```text
progress.bar = true
progress.error = always
```

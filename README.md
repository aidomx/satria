<div align="center">
 <img src="satria_logo.webp" alt="satria logo" width="auto" height="auto">
</div>

# satria
**A simple builder for you.**

`satria` is a simple build tool for C projects, driven by a declarative `Buildfile`. No Makefile or CMake project is required — define the project once and let satria handle compilation, dependencies, libraries, workspace projects, embedded assets, packaging, and releases.

Cross-platform: Linux, macOS, and Windows (MSVC + MinGW).

Full documentation lives in [docs/](./docs/README.md): guides ([getting started](./docs/guide/getting-started.md), [Buildfile](./docs/guide/buildfile.md), [workspace](./docs/guide/workspace.md), [releases](./docs/guide/releases.md), [packaging](./docs/guide/packaging.md), [profiling](./docs/guide/profile.md)) and references ([CLI](./docs/reference/cli.md), [Buildfile fields](./docs/reference/buildfile-fields.md)).

## Installation

### Linux/macOS
```bash
curl -fsSL https://raw.githubusercontent.com/aidomx/satria/main/install.sh | sh
```
The script installs the prebuilt `satria` binary from the repository.

### Bootstrap from source
```bash
./install.sh --dev
```
Requires a C compiler such as `gcc` or `clang`. The resulting binary is `build/bin/satria`.

### Windows
From a source tree, use MSVC (`cl.exe`) or MinGW (`gcc`) without CMake:
```powershell
.\install.ps1 --dev
```
The resulting binary is `build\bin\satria.exe`.

## Usage
```bash
satria            # build project from Buildfile
satria -f FILE    # use a different Buildfile
satria init       # create a new Buildfile
satria init -w    # create a new WsBuildfile
satria -w         # build all projects in the workspace
satria -w NAME    # build project NAME and its dependencies
satria -w release -- name=NAME    # release a single project via CLI
satria clean      # clean build artifacts
satria profile archive=DIR [with=tar,gz|tar,xz|tar,bz2]  # measure operations
satria profile library=DIR [with=static|shared|static,shared]
satria profile binary=DIR [jobs=N] [--report FILE]
satria version    # display satria version
satria help       # display help
```
Without arguments, `satria` looks for a `Buildfile`. At the workspace root, `WsBuildfile` can be used to manage multiple projects (`Buildfile.ws` is still read, with a deprecation warning — rename it to `WsBuildfile`).

## Buildfile
For a standard project — a `src/` folder (and an optional `include/` folder) — a single line is enough; the rest follows satria conventions:
```text
use project
```
Default conventions: `sources = src`, `headers = include` (if the folder exists), binary name = project folder name, `std = gnu11`, output in `bin/` and `build/`. C++ projects need no extra declaration: when sources contain `.cpp`, `std` defaults to `c++17` and the toolchain switches to `g++`/`clang++` automatically. Non-standard projects are still free to declare everything explicitly — explicit fields always override conventions:
```text
use alias
clean as c
output as o
root = .
sources = src
headers = include, I.
flags = Wall, Wextra, O2, MMD, MP
std = gnu11
compiler = gcc, clang
o.binaryName = app
o.binaryDir = bin
o.buildDir = build
o.compileCommands = auto
```
The old Buildfile format is still supported. The complete list of fields and defaults is available at [docs/reference/buildfile-fields.md](./docs/reference/buildfile-fields.md).

## Multi-project Workspace
`WsBuildfile` manages multiple projects at once. A minimal workspace just lists the projects — each project follows the standard convention (a `<name>/` folder containing `src/`), without needing its own Buildfile:
```text
use workspace
projects = rupamod, rupa, ruka
```
Per-project settings, dependencies, and cross-project library references can still be declared for non-standard layouts:
```text
projects.rupamod as mod
projects.rupa as rupa
ruka.depends_on = rupamod
rukalib.linux = rupa
rupalib.linux = ruka
```
Commands:
```bash
satria -w          # build + release all projects
satria -w rupa     # build rupa and its dependencies
satria -w clean    # clean the entire workspace
```
satria builds the workspace in two phases — the library pass creates `lib<name>.a`/`.so` without linking, and the binary pass links the final binaries after all libraries are available — so cross-library dependencies (`rupa <-> ruka`) can still be built. Details in [docs/guide/workspace.md](./docs/guide/workspace.md).

## Release
The workspace separates `projects` (build units) and `release` (distribution units):
```text
release.name = rupa
release.target = deb
```
```bash
satria -w                          # build + release all -> dist/release/
satria -w release -- name=rupa     # release a single project via CLI
```
Releases can be fully triggered from the CLI without declaration in `WsBuildfile`. Details in [docs/guide/releases.md](./docs/guide/releases.md).

## Other Features
- **Incremental build** — two-layer header tracking (mtime + content hash), dependency/fingerprint cache; see [docs/guide/incremental.md](./docs/guide/incremental.md).
- **Parallel build** — `-j[N]`, defaults to the number of CPU cores.
- **compile_commands.json** — `o.compileCommands = auto` for clangd; see [docs/reference/compdb.md](./docs/reference/compdb.md).
- **Archive** — `archive.*` for archive-packaged projects; see [docs/guide/embedded-and-archives.md](./docs/guide/embedded-and-archives.md).
- **Embedded files** — archives become binary objects and are embedded into binaries/libraries; see [docs/guide/embedded-and-archives.md](./docs/guide/embedded-and-archives.md).
- **Packaging** — `pack.*`: tarball, `.deb` (file mapping, merging across projects, sha256 checksum); see [docs/guide/packaging.md](./docs/guide/packaging.md).
- **Library output** — static + shared from the same objects (`output.libraryName`, `output.libraryShared`).
- **build.ninja conversion** — `satria -xf build.ninja` (temporary) or `-xcf` (kept); see [docs/reference/satria-ninja-import.md](./docs/reference/satria-ninja-import.md).
- **Interrupt (Ctrl+C)** — SIGINT is forwarded to compiler jobs; a canceled build does not record a success snapshot. Details in [docs/reference/errors-and-interrupts.md](./docs/reference/errors-and-interrupts.md).
- **Profiling** — `satria profile` measures archive/library/binary costs without touching build state; see [docs/guide/profile.md](./docs/guide/profile.md).

## License
MIT — see [LICENSE](./LICENSE).

# Releases

> Scope: revision 2 of `design/release.md` (October 2026). See
> `docs/README.md` for the versioning convention.

A workspace separates two concepts:

```text
workspace
├── projects   = build units (can be many)
└── release    = distribution unit (one)
```

Revision 1 modeled releases as a list of distribution units, each bound to
a project (`releases = a, b` + `releases.<n>.name = <project>`). That
syntax is REMOVED: release is now a single workspace-level package whose
content you choose freely.

## Declaring release

The release configuration lives in one `release.*` section of
`Buildfile.ws` (workspace mode) — and equally in a project's `Buildfile`
(single-project mode). Keys are the same as `pack.*` because the release
phase runs the pack engine:

```text
release.name = rupa
release.version = 0.2.2
release.files = bin/rupa, ../bade/bin/bade, share/rupa/cmd.txt
release.exclude = .git, .satria, build, node_modules
release.output = dist/{name}-v{version}.tar.gz
release.format = deb
release.compress = gzip
release.checksum = sha256
release.deb.install_prefix = /usr/local
```

- `release.name` / `release.version` / `release.output` — template output
  (tokens: `{name}`, `{version}`, `{os}`, `{arch}`); default
  `dist/{name}-v{version}.tar.gz`, computed from the workspace root (or
  project root in single-project mode).
- `release.files` — file/folder entries with optional mapping `src:dst`.
  Plain paths are relative to the workspace root; `../<project>/...`
  points at another project's output (the leading `../` is stripped).
  Folders are walked recursively.
- `release.exclude` — entries applied while walking folder entries:
  exact relative path, basename, or directory prefix. Never applied to
  explicit file entries.
- `release.format` — `tar` (default) or `deb`; `release.compress`,
  `release.checksum` (sha256), `release.deb.*` work exactly like `pack.*`.
- All keys mirror the `pack` engine (see packaging guide for `deb.*`
  metadata).

**Default content** — without `release.files`, the package contains the
build artifacts of every project in the workspace: each binary project
contributes `<project>/bin/<binaryName>` and each library project
contributes `<project>/lib/lib<libraryName>.a` and `.so`. Archive/pack-only
projects contribute nothing.

## CLI

```bash
satria -w                                   # build + release (when release.* exists)
satria -w clean                             # clean, no release
satria -w <project>                         # build closure; release stays full-workspace
satria -w release                           # same as satria -w (release name), kept for clarity
satria -w release -- version=0.2.3          # override release settings for this run
satria -w release -- format=tar,version=1.0 # multiple overrides, comma separated
```

`-- key=value` pairs override release settings for the run (`name`,
`version`, `format`, `target` — accepted as a legacy alias of `format` —
plus any other pack key). The revision-1 selector semantics
(`name=<project>` picks a release) are gone; errors are reserved for
unknown keys and invalid names.

## Output

One centralized folder: `dist/` at the workspace root (or project root in
single-project mode). Per-project `pack.*` output still goes to each
project's own `dist/` and is untouched by release.

```text
dist/
├── rupa-v0.2.2.tar.gz
├── rupa-v0.2.2.tar.gz.sha256
```

`release.format = deb` **adds** a `.deb` beside the tarball (both artifacts
are built, each with its own `.sha256`); it does not replace it:

```text
dist/
├── rupa-v0.2.2.tar.gz
├── rupa-v0.2.2.tar.gz.sha256
├── rupa-v0.2.2.deb
└── rupa-v0.2.2.deb.sha256
```

Deb-only mode is explicit: make the output template end in `.deb`
(`release.output = dist/{name}-v{version}.deb`) and the tarball is skipped.

## Pipeline

Building a workspace runs three phases:

```text
1. library pass   — compile sources, package lib<name>.a/.so (no binary link)
2. binary pass    — link binaries once all libraries exist
3. release pass   — package ONE artifact from the workspace into workspace dist/
```

If no `release.*` section exists at all, the release phase is skipped and
the workspace behaves exactly as before.

## Relationship with pack

`pack.*` stays the project-level packaging engine; release only
orchestrates it at the workspace (or project) level with a wider file
scope. No second packaging implementation exists — deb metadata, tar
compression, staging, and checksums are the pack engine's own machinery,
reused wholesale.

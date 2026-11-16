# Packaging

satria has built-in packaging support through the `pack.*` namespace.

## Source archive

```text
pack.name = satria
pack.version = 0.2.0
pack.output = dist/{name}-v{version}.tar.gz
pack.files = bin/satria:bin/satria, README.md:share/satria/README.md
```

A mapping uses:

```text
source:path/in/package
```

Without a mapping, the source path is preserved.

Binary entries may omit the `.exe` suffix: when the exact source is absent,
the packer falls back to `<source>.exe` (a Windows-linked binary is
`bin/satria.exe` while the Buildfile says `bin/satria`). The packaged path keeps
the destination as configured.

## Package formats

A package can select a format:

```text
pack.format = deb
```

and optionally a checksum:

```text
pack.checksum = sha256
```

## Faster packaging

```text
pack.optimize = true
```

Builds the tarball in two steps — `tar` first, then the compressor at its
lowest (fastest) level (`gzip -1`, likewise `-1` for `xz` and `bzip2`) — which
trades archive size for build time. Default `false` keeps the single-step
`tar czf` behavior. The same key works as `release.optimize` for workspace
releases.

## Debian

Example:

```text
pack.deb.install_prefix = /usr/local
pack.deb.maintainer = "Aidomx <aidomxdev@gmail.com>"
pack.deb.description = "A simple builder for you"
pack.deb.architecture = arm64
```

The install prefix is combined with package destinations when constructing the Debian filesystem tree. The `.deb` is assembled by satria itself from the staged files (ar + tar members), so `dpkg-deb` is not required.

`pack.format = deb` adds the `.deb` beside the tarball; make `pack.output` end
in `.deb` to skip the tarball and produce a deb-only package.

## Package merge

Workspace package definitions can merge files from another project while keeping the target project's package metadata and output identity.

```text
rupa.pack.merge = ruka
```

This is useful when a release artifact contains the outputs of several workspace projects.

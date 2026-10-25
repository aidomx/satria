# Archives and Embedded Assets

satria can build archives as workspace artifacts and can embed generated files into another binary/library.

## Archive source

An archive project can use:

```text
mod.archive.src = .
```

or `./` as its source root.

This is useful when the archive represents a module tree rather than a conventional `src/` directory.

## Archive example

```text
mod.output.binary = false
mod.archive.src = .
mod.archive.name = rupa_modules
mod.archive.with = tar, gz
mod.archive.exclude = build, dist, LICENSE, README.md, self_archive
mod.archive.dir = ../ruka/modules
```

The source root defines what is considered for archiving. Exclusion rules determine what is omitted.

`.git` is excluded from source archives by default.

## Embedded files

A generated archive can be embedded into a binary/library:

```text
ruka.embedded.modules as rukamod

rukamod.file = ../rupamod/dist/rupa_modules.tar.gz
rukamod.variable = MODULES
rukamod.extract = /tmp/rupa-system
rukamod.pattern = .rp
```

Satria generates `embedded.c` and `embedded.h` in the project output
directory. The header defines a flat `Embedded` struct and the accessor API:

```c
#include <embedded.h>

const Embedded *e = getEmbed("modules"); /* NULL when the key is unknown */
if (e && e->data) {
  fwrite(e->data, 1, e->size, stdout);
}
```

`e->key` defaults to the entry name (`<alias>.key` overrides it), `e->type`
is `archive` or `asset` unless `<alias>.type` sets a label, `e->name` is the
archive file name, and `e->extract` is the `.extract` field. Iterate with
`getEmbedAt(i)` until it returns NULL.

The legacy `EMBED_*` macros (archive name, extraction directory, symbol,
end symbol, and symbol length) remain available for compatibility; their
symbols point at the arrays in the generated `embedded.c`. The file is a
normal source: it joins the fingerprint, the parallel compile, and the
library archive, so a static-library consumer reaches the embedded object
without any special linker handling (no `ld -r -b binary` is involved).

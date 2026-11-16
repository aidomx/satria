use workspace

# Fixture minimal: alias, archive, embed, library, dan dependensi sirkular
projects.rupamod as mod
projects.core as core

projects = bade, core, rupamod

compiler = gcc, clang
flags = Wall, Wextra, MMD, MP
headers.each = I.
std = gnu11

mod.output.binary = false
mod.archive.src = .                            
mod.archive.name = rupa_modules
mod.archive.with = tar
mod.archive.exclude = build, dist, LICENSE, README.md, .git, self_archive
mod.archive.dir = modules

core.sources = src
core.exclude = main.c
core.output as coreout
core.library as corelib                               
coreout.binaryName = rupa
coreout.libraryName = rupa
coreout.libDir = lib
coreout.libraryShared = true                          
bade.sources = manager, src
# manager/main.c: milik binary, tidak dikemas ke library.
# src/cli.c: CLI bade memanggil simbol core (core_cli_notes, core_value,
# thisCore). Shared library Windows (bade.dll) menuntut SEMUA simbol
# terdefinisi saat link - tidak seperti .so Linux yang menunda resolusi ke
# runtime - sehingga cli.c dikeluarkan dari library (tetap dikompilasi dan
# di-link ke bin/bade, simbol core terjawab di link binary via cdeps
# `library = core/lib/librupa.a`). Pola yang sama sudah diterapkan sisi
# core: core_value() nilai tetap, bukan memanggil bade_value().
bade.exclude = manager/main.c, src/cli.c

bade.output as badeout
bade.library as badelib

badeout.binaryName = bade
badeout.libraryName = bade
badeout.libDir = lib
badeout.libraryShared = true

bade.depends_on = rupamod

bade.embedded.modules as bademod

bademod.file = modules/rupa_modules.tar
bademod.variable = MODULES
bademod.extract = /tmp/satria-miniws-system
bademod.pattern = .rp

# Relasi dua arah bade <-> core (grup cdeps level atas, design/cdeps_group.md):
#   cdeps = {bade,core}
# == badelib.cdeps = core + corelib.cdeps = bade
cdeps = {bade,core}

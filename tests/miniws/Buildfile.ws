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
bade.exclude = manager/main.c

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

badelib.cdeps = core
corelib.cdeps = bade

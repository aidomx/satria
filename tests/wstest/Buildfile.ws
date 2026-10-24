use workspace

# alias singkat
projects.rupamod as mod
projects.rupa as rupa

projects = rupamod, rupa, ruka

compiler = gcc, clang
flags = Wall, Wextra, MMD, MP
headers = rupaapi, I.
headers.each = I.
std = gnu11

# A. rupamod — proyek kemasan (archive)
mod.output.binary = false
mod.archive.src = .
mod.archive.name = rupa_modules
mod.archive.with = tar
mod.archive.exclude = build, dist, LICENSE, README.md, .git, self_archive
mod.archive.dir = modules

# B. rupa
rupa.sources = src
rupa.exclude = main.c

rupa.output as rupaout
rupa.library as rupalib

rupaout.binaryName = rupa
rupaout.libraryName = rupa
rupaout.libDir = lib
rupaout.libraryShared = true

# C. ruka — embed arsip jadi dari rupamod
ruka.sources = src
ruka.exclude = main.c
ruka.output as rukaout
ruka.library as rukalib

rukaout.binaryName = ruka
rukaout.libraryName = ruka
rukaout.libDir = lib
rukaout.libraryShared = true

ruka.embedded.modules as rukamod

ruka.depends_on = rupamod

rukamod.file = modules/rupa_modules.tar
rukamod.variable = MODULES
rukamod.extract = /tmp/rupa-system
rukamod.pattern = .rp

rukalib.cdeps = rupa
rupalib.cdeps = ruka

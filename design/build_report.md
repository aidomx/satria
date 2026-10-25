# Build report

## No-op report
```
> Project   : rupamod  (archive pass)

> Summary
Archive  : ../modules/rupa_modules.tar
Status   : Success

> Project   : bade  (library pass)
> Library   : lib/libbade.a (up-to-date)
> Library   : lib/libbade.so (up-to-date)

> Summary
Library  : lib/libbade.a (432.9KB)
Library  : lib/libbade.so (271.4KB)
Compiled : 0
Skipped  : 36
Status   : Success

> Project   : core  (library pass)
> Library   : lib/libcore.a (up-to-date)
> Library   : lib/libcore.so (up-to-date)

> Summary                                       Library  : lib/libcore.a (1168.4KB)
Library  : lib/libcore.so (713.9KB)             Compiled : 0
Skipped  : 176
Status   : Success
                                                > Project   : rupamod  (skip: selesai di fase library)

> Project   : bade                              > Build with cached state (fingerprint unchanged)
> Linking   : bin/bade (up-to-date)             > Library   : lib/libbade.a (up-to-date)
> Library   : lib/libbade.so (up-to-date)
> Summary                                       Target   : bin/bade
Size     : 626.3KB                              Library  : lib/libbade.a (432.9KB)
Library  : lib/libbade.so (271.4KB)
Compiled : 0
Skipped  : 36
Status   : Success
                                                > Project   : core
> Build with cached state (fingerprint unchanged)
> Linking   : bin/rupa (up-to-date)             > Library   : lib/libcore.a (up-to-date)
> Library   : lib/libcore.so (up-to-date)
                                                > Summary
Target   : bin/rupa                             Size     : 838.2KB
Library  : lib/libcore.a (1168.4KB)             Library  : lib/libcore.so (713.9KB)
Compiled : 0                                
Skipped  : 176
Status   : Success
> Release    : rupa (workspace, format deb)     > Package   : dist/rupa-v0.2.2.tar.gz (up-to-date)
> Checksum  : dist/rupa-v0.2.2.tar.gz.sha256 (up-to-date)
> Package   : dist/rupa-v0.2.2.deb (up-to-date)
> Checksum  : dist/rupa-v0.2.2.deb.sha256 (up-to-date)
```

Design report di atas memang sudah cukup bagus, tapi kalau kurang jeli/teliti memang gampang salah paham.

Design baru menggunakan symbol | (unicode) fallback > kalau tidak mendukung.

```
# cold build
| Project : rupamod
| Fase    : archive
| Summary :
    Archive  : ../modules/rupa_modules.tar
    Skipped  : true # hanya muncul di no-op
    Status   : Success
```

# Next version

```text
Build Report
============
Toolchain : gcc -Wall -Wextra -MMD -MP -std=gnu11
CompDB : compile_commands.json refreshed (workspace)

Archive
-------
- rupamod : modules/rupa_modules.tar (12.4 KB) [cached]

Libraries
---------
- bade
Embedded : config changed -> recompiling all sources
[1/3] OK 0.56s bade/manager/main.c
[2/3] OK 0.72s bade/src/bade.c
WARN bade/src/bade.c:9:13: 'sayHelloWorld' defined but not used
[3/3] OK 0.88s bade/src/embed_use.c
Embedded : modules/rupa_modules.tar
Output : lib/libbade.a, lib/libbade.so
Status : Success (3 compiled, 0 skipped, 1 warning)

- core
[1/2] OK 1.15s core/src/core.c
[2/2] OK 1.29s core/src/main.c
Output : lib/librupa.a, lib/librupa.so
Status : Success (2 compiled, 0 skipped)

Binaries
--------
- bade : bin/bade (79.8 KB)
- core : bin/rupa (69.2 KB)

Summary
-------
Projects : 3
Compiled : 5
Skipped : 0
Warnings : 1
Status : Success (with warning)
```

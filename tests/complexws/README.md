# complexws

Fixture workspace kecil tapi **nyata**: tiga proyek yang saling bergantung
(alias, arsip tar, embed, library shared/static, linking dua tahap, dan
`cdeps` dua arah) yang menghasilkan dua program CLI yang benar-benar
menjalankan sesuatu — bukan sekadar berkas yang dikompilasi.

Dipakai sebagai target cepat untuk asersi tingkat workspace, pengganti proyek
nyata (`tests/rupaws`) yang build-nya butuh puluhan menit.

## Program

| Binary | Sumber | Perilaku |
| --- | --- | --- |
| `bin/rupa` | `core/src/main.c` + `core` (library) | CLI catatan: `notes add/list/count/clear`, `version`, `help`. Tanpa argumen: `this bade` lalu `core=8 bade=7` |
| `bin/bade` | `bade/manager/main.c` + `bade` (library) | `extract` (bongkar modul tertanam), `embed` (ringkas entri embed), `notes …`, `version`, `help`. Tanpa argumen: `this core`, ekstraksi modul, lalu `bade=7 core=8` |

Store catatan memakai direktori `$RUPA_HOME` (default `.rupa`) dan satu
catatan per baris di `notes.txt`, jadi tes bisa memeriksa efek sampingnya di
disk. Keluaran tanpa argumen sengaja dipertahankan: `tests/unit/common/embed_getembed.test`
memeriksa `bade=7` dan `tests/miniws` memakai keluaran yang sama.

Modul tersimpan di `modules/rupa_modules.tar` (diproduksi `rupamod`, arsip
direktori ini dengan `exclude` di `Buildfile.ws`) dan ditanam ke `bin/bade`
lewat `bade.embedded.modules`. `bade` mengekstraknya ke `bademod.extract`
seperti loader lama: tulis arsip, lalu `tar xf`.

## Menjalankan

```
cd tests/complexws
../../build/bin/satria -w          # build cold
./bin/bade                         # bade=7 core=8
./bin/bade embed                   # embed: rupa_modules.tar <n> bytes -> <extract>
./bin/rupa notes add "halo"        # added: halo
./bin/rupa notes list              #  1  halo
```

Lewat runner unit test:

```
bash scripts/unit.sh bin/satria complexws -j 2
```

Regression checks yang sama seperti fixture lain: cold, no-op, touch
`core/src/core.c`, edit `bade/src/bade.c`, tambah/hapus `.c` di `core/src`,
dan status cache di `.satria/` (`startup.state`, `opts.cache`, `sources.cache`,
`report.blob`, `artifact.cache`). Touch-only tidak merekompilasi: workspace
memakai content-hash (PATCH fingerprint), proyek tunggal memakai stamp FAST5
(design/startup.md §4).

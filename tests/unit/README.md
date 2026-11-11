# tests/unit — suite unit test

Satu suite datar; **fixture adalah input runner, bukan pemilik tes**
(design/unit_test.md §1). Karena itu tidak ada subfolder `miniws/` atau
`rupaws/`: tes yang butuh struktur tertentu mendeklarasikan prasyarat dan
berakhir sebagai PRECONDITION bila fixture tidak memenuhinya.

| Berkas | Isi |
| --- | --- |
| `common/` | Semua tes (`*.sh`), dijalankan untuk target apa pun |
| `lib/layout.sh` | Helper fixture-aware (sumber proyek, binary, arsip, PRECONDITION) |
| `classes.conf` | Klasifikasi tiap tes: `[cold]` atau `[snap]` — wajib, runner menolak tes yang tidak terdaftar |

## Menjalankan

```
bash scripts/unit.sh bin/satria <target> [-b] [-j N] [-o <log>] [-t <tes>] [--clean]
bash scripts/unit.sh -w [target]
```

`<target>` adalah nama fixture di `tests/<target>` (mis. `miniws`, `complexws`,
`rupaws`), bukan path. Runner menyalin fixture ke
`/tmp/satria-unit/targets/<target>/{fixture,snap,work,t}` dan menulis log ke
`.../log/`. Satu run melayani satu target.

```
bash scripts/unit.sh bin/satria complexws -j 2                 # seluruh suite
bash scripts/unit.sh bin/satria complexws -j 2 -t cache_noop   # satu tes
bash scripts/unit.sh -w                                        # watcher: satu-satunya run aktif
bash scripts/unit.sh -w complexws                              # watcher satu target (Ctrl-C)
bash scripts/unit.sh bin/satria rupaws --clean                 # hapus staging target
```

`-w` **hanya memantau kemajuan run**, tidak menjalankan atau memicu run. Sumber
statusnya `targets/<target>/log/` di staging: `run.pid` (masih hidup?), `run.env`
(total tes, waktu mulai/selesai), `plan.tsv` (urutan tes), `timing.tsv` (tes
selesai + PASS/FAIL/PRE), dan log run (tes yang sedang berjalan). Status mengalir
per tes (design §9):

```
# Watch unit test
* PID      : 5339
* Staging  : /tmp/satria-unit/targets
* Interval : 1s, Ctrl-C untuk berhenti
* Target   : complexws - 5150

[13/24] build_log
* PASS: 4 FAIL: 0 PRE: 0
[14/24] cache_noop
* process: 00:00:02 41%
```

Saat run berakhir (`RUN_ENDED`), `# Summary` dicetak sekali. Run yang mati tanpa
`RUN_ENDED` tidak ditandai selesai dan tidak mendapat summary. Tanpa argumen,
watcher memilih target satu-satunya run aktif — jadi selama cuma satu run
berjalan, `-w` cukup tanpa nama target. Bila ada beberapa run aktif maupun nol
run aktif, `-w` berhenti dan menyebut kandidatnya: sebut salah satu, mis.
`-w complexws`. Hanya satu watcher per staging (lock `$UNIT_STAGE/watch.pid`);
Ctrl-C menghapus lock. Interval poll: `UNIT_WATCH_INTERVAL` (detik, default 1).
Watcher berjalan di foreground dan tidak menerima `<binary>`, `-b`, `--clean`,
atau `-t`. Di non-TTY (mis. output dialihkan ke berkas) hanya entri selesai dan
summary yang ditulis.

Contoh memantau run background, lalu mengembangkan watcher sambil run jalan:

```
bash scripts/unit.sh bin/satria complexws -j 2 -b -o log/unit.log   # run di latar
bash scripts/unit_dev.sh -w                                        # pantau (satu run)
```

Run memegang salinan `unit_dev.sh` miliknya sendiri, jadi mengedit atau
menjalankan `scripts/unit_dev.sh` langsung tidak mengganggu run yang berjalan.

Pilih target sesuai kebutuhan: `complexws` adalah workspace tiga proyek yang
menghasilkan program CLI nyata (build hitungan detik), `rupaws` adalah proyek
nyata yang lambat, `miniws` contoh minimal.

## Aturan menulis tes

1. **Jangan hardcode nama berkas fixture.** Ambil dari discovery runner
   (`$UNIT_DISCOVERY/sources.tsv`) lewat `lib/layout.sh`
   (`unit_project_source`, `unit_project_sources`, `unit_project_binary`,
   `unit_archive_project`). `miniws/core/src/core.c` tidak ada di rupaws
   (`core/src/main.c`), dan membuat berkas baru saat "touch" memicu build ulang
   sehingga asersi no-op gagal.
2. **PRECONDITION, bukan FAIL,** bila fixture memang tidak menyediakan
   kebutuhan tes (mis. `embed_getembed` butuh `bademod.extract` + konsumen
   `getEmbed()`). Pakai `unit_precondition "alasan"` — runner menghitungnya
   sebagai kategori `PRE`.
3. **Daftarkan tes di `classes.conf`.** Tes `[snap]` tidak boleh menghapus
   `.satria`, `build`, `bin`, `lib` (runner menolaknya); tes yang butuh build
   dari nol masuk `[cold]`.
4. Setiap skrip menerima `$1` = binary (wrapper `-j`), `$2` = salinan fixture,
   dan memakai `$UNIT_TMP/<nama>` sebagai folder kerja yang disediakan runner.
5. **Jangan mengandalkan konfigurasi yang hanya ada di fixture.** Buildfile
   proyek nyata bisa sudah punya `release.*` (rupaws memakai `release.format =
   deb`), jadi tes yang menambah blok `release.*` sendiri harus menetapkan
   semua kunci yang menentukan hasilnya (`release.format`, `release.output`).
   Hal yang sama berlaku untuk `scripts/` milik repo Satria: pakai root repo
   (`$UNIT_REPO`), bukan root fixture.
6. Root repo jangan ditebak dari `$0`: runner mengekspor `$UNIT_REPO`. Bila
   skrip dijalankan manual dari `common/`, root repo =
   `$(dirname "$0")/../../..` (common → unit → tests → repo) dan fixture
   `tests/<nama>` = `$(dirname "$0")/../../../tests/<nama>`.

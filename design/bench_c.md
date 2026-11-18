# Benchmark di C (`satria bench`)

Status: implementasi awal dua mode, menggantikan jalur ukur
`bench/compare.sh` + `bench/compare/*.sh` (satu proyek) dan `bench/ws.sh` +
`bench/ws/*.sh` (workspace). Skrip bash lama tetap disimpan sebagai referensi
sejarah; angka yang dipakai untuk regresi berasal dari perintah C ini.

## Mode

| perintah | graf | skenario |
| --- | --- | --- |
| `satria bench` | satu proyek (N source, H header) | cold, noop, touch, edit, editrel, compdb, + gcc-floor |
| `satria bench --workspace` | rupamod + core + bade | cold, noop, leaf, bade, mod, touch, hedit, sel, dep, libtouch, libhdr, rel |

Mode adalah pilihan struktural (generator dan harapan kerja berbeda), jadi
dipilih lewat flag `--workspace`, bukan `-f case=...`. `--compare` hanya
pengubah tampilan dan berlaku di kedua mode; `-f` tetap untuk tunable.

## Alasan

`bench/ws.sh` bergantung pada Bash 5 (`EPOCHREALTIME`), `getopts`, `awk`, `sort`,
`stat`, `cp -a`, dan shim `gcc`/`ld` berupa skrip shell. Setiap lapisan itu
menambah ketidakpastian pada pengukuran (parsing argumen, format locale,
overflow timestamp, `pipefail` + SIGPIPE) dan pada bukti kerja (log shim bisa
berbeda perilaku dengan tool asli). Porting ke C membuat satu binary menjadi:

- parser opsi,
- generator project sintetis,
- mutator skenario,
- penjadwal + pengukur (jam monotonic),
- shim `gcc`/`ld` (symlink ke binary satria sendiri),
- pembaca sinyal artefak (`stat` langsung),
- pelapor (median, validation log, success rate, CSV).

## Fase (mengikuti design/bootstrap.md)

1. **Parse** — `argv` → `Bench`. Murni: belum ada filesystem. Format opsi
   `-f key=value` (mudah diperluas tanpa huruf pendek yang bertabrakan).
2. **Prepare** — direktori kerja, `.wrap/gcc` + `.wrap/ld` (symlink ke
   `/proc/self/exe`), `PATH`, env shim, kandidat tool (A/B via `-f bin=`).
3. **Execute** — skenario berurutan; tiap run: snapshot artefak → truncate log
   shim → spawn (fork/exec, tanpa shell) → snapshot → delta → runtime check →
   validasi harapan → rekam.
4. **Report** — median per skenario, `[Detailed Validation Log]`, Success Rate,
   artefak (`results.csv`, `view.csv`, `work.log`, `strace` bila aktif).

## Pemetaan opsi bash → `-f`

| bash | C |
| --- | --- |
| `-n` | `source` |
| `-H` | `header` |
| `-m` | `module` |
| `-r` | `runs` |
| `-t` | `tools` |
| `-j` | `jobs` |
| `-c` | `compiler` |
| `-o` | `out` |
| `-k` | `keep=true` |
| `-s` | `scenario` |
| `-a` | `satria="args"` |
| `-w` (audit) | `warmup` (prime + gate) |
| `-artifacts` | `artifact` |
| `-strace` | `trace` |
| `-O abba/grouped` | `rolling=true/false` |
| (baru) | `bin=a,b` untuk A/B binary satria |

## Shim tanpa shell

`.wrap/gcc` dan `.wrap/ld` adalah symlink ke binary satria. `main()` mendeteksi
`argv[0]` basename `gcc`/`ld` + env `RB_BENCH_SHIM`, lalu `benchShimMain`
mencatat `C|obj`, `L|bin`, `S|shared`, `D|ld`, `E|embed` ke `RB_BENCH_LOG` dan
`execv` tool asli (`RB_BENCH_REAL_GCC` / `RB_BENCH_REAL_LD`). `argv[0]` diganti
path tool asli sebelum exec agar gcc menemukan `cc1`/`as` dari prefix-nya
sendiri. Ini menghilangkan `bash -c` dan menjaga bukti kerja identik dengan
proses yang benar-benar berjalan.

## Prime dan mtime

Prime = build penuh + satu run hangat + runtime gate, lalu salinan pohon ke
`<out>/.prime/<label>`. Salinan mempertahankan **mtime** (seperti `cp -a`);
tanpa itu archive `rupa_modules.tar` terlihat lebih tua daripada `.rp` dan
skenario `noop`/`touch` memicu rebuild palsu.

## Generator embed (CMake)

CMake tidak memakai skrip shell: target `embedded.c` dibuat lewat
`satria bench --embed-source <arsip> <out.c> <symbol>` (array C dengan simbol
`<sym>_start` / `<sym>_size`), lalu dikompilasi sebagai source biasa — pekerjaan
yang sama dengan codegen embed satria (`design/embedded.md`).

## Backend single project

Satu pohon proyek dilengkapi semua file build sekaligus (Buildfile,
`build.ninja`, Makefile, CMakeLists.txt, meson.build, xmake.lua, Tupfile), jadi
setiap tool membangun isi yang sama:

| tool | cold | inkremental | output |
| --- | --- | --- | --- |
| satria | `satria -jN` | `satria -jN` | `bin/app` |
| ninja | tulis graph + `ninja -jN` | `ninja -jN` | `bin/app` |
| make | `make -jN` | `make -jN` | `bin/app` |
| bear | `bear -- make -jN` | `bear -- make -jN` | `bin/app` |
| cmake | configure + `ninja -C build-cmake` | `ninja -C build-cmake` | `build-cmake/bin/app` |
| meson | `meson setup build-meson` + `ninja -C build-meson` | `ninja -C build-meson` | `build-meson/app` |
| xmake | `xmake -y -jN` | `xmake -y -jN` | tool-dependent (tidak di-hard-check) |
| tup | `tup` (setelah `tup init`) | `tup` | `bin/app` |
| gcc-floor | — (objek disiapkan lewat make) | `gcc -c src/t1.c` + `gcc build/*.o -o bin/app` | `bin/app` |

Skenario `compdb` hanya berjalan untuk tool yang punya compdb native (satria,
ninja, bear, CMake, Meson, XMake); `make` dan `tup` dilewati. Hasilnya
divalidasi dengan memastikan `compile_commands.json` ada dan berisi lebih dari
2 byte. Bear memakai build bersih tiap run (compile_commands hanya merekam
compile yang benar-benar jalan), sedangkan satria menyalakan
`o.compileCommands = auto` sementara lalu mengembalikannya ke `false`.

Ketersediaan dicek lewat PATH; tool yang diminta tapi tidak ada dilewati dengan
pesan. `--workspace` tetap hanya mendukung satria dan CMake.

## Harapan per skenario — single project

`obj` dihitung dari file `.o` yang berubah, `bin` dari binary hasil build.

| skenario | semua tool | catatan |
| --- | --- | --- |
| cold | obj N+1, bin 1 | cc N untuk satria; cc tidak dipatok untuk tool lain (configure/probe) |
| noop | obj 0 | bin 0 kecuali xmake yang menyalin ulang target |
| touch | tidak dipatok | satria: obj/cc/bin 0 (content-hash); tool lain boleh rebuild |
| edit (komentar) | obj 1 | satria/floor cc 1; satria boleh tanpa relink |
| editrel (fungsi baru) | obj 1, bin 1 | satria/floor cc 1; xmake bin tidak dipatok |
| compdb | tidak dipatok | `compile_commands.json` harus ada dan >2 byte |

## Lantai gcc (`gcc-floor`)

`edit` mengubah komentar: satria boleh melewati relink, sehingga membandingkan
"satria vs make" di sana bukan pembanding link yang adil. Karena itu benchmark
selalu mengukur baris `gcc-floor` untuk `edit` dan `editrel`:

- objek disiapkan lewat `make -jN` saat prime (tidak dihitung);
- satu run = `gcc -O2 -std=gnu11 -Iinclude -MMD -MP -c src/t1.c -o build/t1.o`
  lalu `gcc build/*.o -o bin/app`, tanpa build tool;
- `edit` menyembunyikan baris lantai di tabel (tetap ada di CSV) dan menunjuk ke
  `editrel`; `editrel` menampilkan lantai sebagai pembanding link yang jujur,
  sehingga selisih tool terhadap lantai = overhead tool.

## Harapan per skenario — workspace

| skenario | satria | CMake |
| --- | --- | --- |
| cold | obj penuh, arc 1, lib 4, bin 2, cc penuh+embed | sama + probe configure |
| noop | semua 0 | semua 0 |
| leaf/bade | obj 1, cc 1, lib 2, bin 2 | sama |
| mod | obj 1, arc 1, cc 1, lib 2, bin 2 | sama |
| touch | semua 0 (content-hash) | obj/cc 6, lib 4, bin 2 (mtime) |
| hedit | obj/cc 6, lib 4, bin 2 | sama |
| sel (`-w core`) | obj 1, lib 2, bin 1 | bin 0 (target core saja) |
| dep (`-w bade`) | obj 1, arc 1, lib 2, bin 1 | sama |
| libtouch | semua 0 | obj/cc 2 |
| libhdr | obj/cc 2, lib 4, bin 2 | sama |
| rel | obj/cc 0 + checksum valid | — |

Runtime check kedua binary setelah setiap run memastikan graph circular nyata
dan tidak ada object basi yang lolos (nilai header mengalir ke kedua `main`).

## Batasan

- POSIX (fork/exec, symlink, strace). Windows belum didukung.
- `BENCH_EMBED=ld` (eksperimen lama) tidak diporting; jalur yang diukur adalah
  codegen carray, sama untuk satria dan CMake.
- Progress bar interaktif diganti baris per run; laporan tetap memuat median,
  validation log, dan success rate.
- `rel` memakai `sha256sum -c` (bukan shell) untuk verifikasi checksum.
- `tup` membutuhkan FUSE; di lingkungan tanpa `/dev/fuse` (mis. container
  unprivileged) prime gagal dan run dilaporkan gagal, bukan dilewati diam-diam.
- Output `xmake` bergantung versi/platform, jadi runtime check-nya dilewati
  (sama seperti skrip bash lama).

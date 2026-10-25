# Buildfile — semantik konfigurasi

> REKONSTRUKSI (3 Okt 2026) — lihat catatan provenance di
> [README.md](README.md).

## Dua format, satu parser

1. **Legacy** — section/indentasi (mis. `embedded:`), tanpa baris `use alias`.
2. **Format baru** — diawali `use alias`; flat `key = value` dengan
   kualifikasi titik; alias `X as Y` (nama kanonik tetap berlaku, bisa
   berantai); key tanpa titik mewarisi prefix section terakhir; komentar `//`
   didukung (`#` didukung di semua format).

`satria` membaca `Buildfile` otomatis; `-f <file>` (atau rapat `-f<file>`) untuk
nama lain. Path `-f` yang memuat direktori → chdir dulu (gaya `make -C`),
sources/headers/output tetap relatif terhadap lokasi Buildfile.

## Katalog key (format baru)

- **build**: `sources` (direktori, scan rekursif `.c`), `headers` (→ `-I`;
  `I.` shorthand), `flags` (Wall, Wextra, O2, MMD, MP), `std`, `compiler`
  (gcc, clang), `target` (x86_64/arm64/riscv64 → flag arsitektur otomatis),
  `library` (+ per-OS `library.linux` dll.), `exclude` (nama file / path /
  prefix direktori).
- **output**: `o.binaryName/binaryDir/buildDir`, `o.compileCommands`
  (`auto|false`), `o.libraryName` (statis `lib<name>.a`), `o.libraryShared`
  (dinamis `.so/.dylib/.dll` + `-fPIC` otomatis), `o.libDir`.
- **lain**: `clean as c` + `c.build/c.compdb`, `foreground` (kendali Ctrl+C),
  `progress.bar`, `progress.error`.
- **embedded**: `embedded.<nama>.src/pattern/extract/variable/key/type/file`,
  `embedded.<nama>.archive.dir/name/with.tar/with.ext`. Hasil:
  `build/embedded.c` + `build/embedded.h` — codegen tunggal (array byte +
  tabel `getEmbed()`; jalur `ld -r -b binary` sudah dihapus — design/
  embedded.md). Freshness arsip otomatis, regenerasi content-based.
- **versi**: `.satria-version` → `build/version.h` (SATRIA_VERSION_EMBEDDED);
  mengubah isi memicu kompilasi ulang; `satria version` selalu benar karena
  versi ter-embed saat build.

## Workspace (`use workspace`)

- `projects = a, b, c`; alias `projects.a as x`; key per proyek `x.<key>`;
  `x.depends_on = a` (urutan build & invalidasi antar proyek); embedded lintas
  proyek via `file = ../<proj>/dist/...`.
- `<n>.root = .` mengubah root proyek. **Fix 2 Okt 2026**: `wsRecordLine`
  kini meng-intercept `root` → `p->root` (sebelumnya hanya terdokumentasi,
  tak pernah dihormati wsExecOne).
- Sintesis: tiap proyek disintesis menjadi Buildfile dengan prefix
  `projects.<n>.` di-strip; `projects.<n>.pack as pack` menjadi section
  `pack` mandiri di Buildfile sintetis.
- Eksekusi: `satria -w` (semua, hormati depends_on) / `satria -w <nama>`
  (selektif, tetap menarik dependensi).

## Lintas konfigurasi (`-xf` / `-xcf`)

`build.ninja` (hasil CMake Ninja atau tulisan tangan) dikonversi menjadi
Buildfile format baru oleh `nc/`:

- edge compile (`-c ... -o <obj>.o`) → source; `-D/-O/-W/-f` diteruskan;
  `-std=...` → `std`; `-I` → `headers`;
- edge link → menebak `o.binaryName`;
- path absolut di bawah cwd di-relatif-kan; template CMake (`$FLAGS`,
  `$INCLUDES`, `${...}`) diekspansi; custom command diabaikan.

`-xf` menghapus Buildfile sementara setelah command; `-xcf` menyimpan hasilnya
(titik awal migrasi).

## Cache konfigurasi (`cfg/config_cache.c`, format v6)

- Cache hit bila Buildfile & kondisi terkait tak berubah → skip parse penuh;
  seluruh Config diserialisasi — sejak v6 termasuk PackConfig.
- `packSetStr` melepas kutip ganda pembuka/penutup.
- Routing `configApply`: bare key top-level
  `name/version/output/compress/checksum/format/files` → packApply; section
  `deb`, atau section `pack` sub `deb` → packApplyDeb; listForSection `pack`
  → pack.files.
- `configFinalizeEntry` kini berada di `embed.c` (dideklarasi `embed.h`):
  proyek tanpa sources otomatis `output.binary=false`.

## pack.* (ringkas)

Detail penuh di [packaging.md](packaging.md). Section `pack` mandiri di
Buildfile ATAU via alias workspace; `pack.files`/`pack.output` menandai pack
aktif (`pack.requested`).

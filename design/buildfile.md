# Buildfile — semantik konfigurasi

> REKONSTRUKSI (3 Okt 2026) — lihat catatan provenance di
> [README.md](README.md). Diperbarui 15 Nov 2026 untuk satria v0.5.0:
> `use project c|cpp|c++`, `use workspace … | hybrid`, `<proyek>.lang`,
> section `release.*` (revisi 2), `headers.each`, dan cache konfigurasi v15.

## Dua format, satu parser

1. **Legacy** — section/indentasi (mis. `embedded:`), tanpa baris `use alias`.
2. **Format baru** — diawali `use alias`; flat `key = value` dengan
   kualifikasi titik; alias `X as Y` (nama kanonik tetap berlaku, bisa
   berantai); key tanpa titik mewarisi prefix section terakhir; komentar `//`
   didukung (`#` didukung di semua format).

`satria` membaca `Buildfile` otomatis; `-f <file>` (atau rapat `-f<file>`) untuk
nama lain. Path `-f` yang memuat direktori → chdir dulu (gaya `make -C`),
sources/headers/output tetap relatif terhadap lokasi Buildfile.

Setiap key yang tidak dikenal tetap diabaikan dengan aman (kecuali yang
memang divalidasi, lihat `lang` di bawah).

## Bahasa (C / C++)

Tiga bentuk, dievaluasi sekali di `configFinalize` sehingga walk sumber,
std default, dan driver kompilasi memakai nilai yang sama:

```text
use project c            # hanya .c, driver C
use project cpp          # mode C++ (walk .c + .cpp), std default c++17
use project c++          # alias cpp
use project              # tanpa argumen = deteksi otomatis (perilaku lama)

use workspace c | cpp | c++ | hybrid     # aturan default semua proyek
base.lang = c                           # override per proyek
core.lang = cpp
```

- `hybrid` adalah default (dan juga perilaku `use workspace` tanpa argumen):
  setiap proyek dideteksi dari sumbernya sendiri — ada `.cpp` → C++, murni
  `.c` → C. Workspace campuran C + C++ tidak berubah perilakunya.
- `use workspace c` memaksa **semua** proyek hanya memindai `.c`;
  `cpp`/`c++` memaksa mode C++ untuk semua proyek.
- `<proyek>.lang` menimpa aturan workspace untuk satu proyek; nilainya boleh
  `c`, `cpp`, `c++`, atau `hybrid`.
- Nilai tak dikenal **tidak** diabaikan diam-diam: `use workspace foo` dan
  `base.lang = rust` mencetak peringatan, lalu memakai default workspace.
- Bahasa ikut masuk hash konfigurasi (`wsLogCfgHash`) dan label fast-state
  (`Buildfile.ws:<proyek>:<bahasa>`), jadi mengganti `c` ↔ `cpp` membatalkan
  state lama dan benar-benar mengompilasi ulang — daftar sumber bisa tampak
  sama padahal std/driver berubah.

Catatan: pada mode C++ berkas `.c` tetap dikompilasi sebagai C++ (perilaku
lama); gcc lalu mengeluh `-std=c++17 is valid for C++/ObjC++ but not for C`.

## Katalog key (format baru)

- **build**: `sources` (direktori, scan rekursif `.c`/`.cpp` sesuai bahasa),
  `exclude` (nama file / path / prefix direktori), `flags` (Wall, Wextra, O2,
  MMD, MP) dan `flags.<platform>`, `defines`, `std` (+ `std.<target>`),
  `compiler` (gcc, clang), `target` (x86_64/arm64/riscv64 → flag arsitektur
  otomatis), `library` (+ per-OS `library.linux` dll.), `foreground`
  (kendali Ctrl+C), `log`, `background` (build terpisah, design/background.md).
- **headers**:
  - `headers` — direktori publik, masuk `-I`; `I.` = `include/` proyek.
  - `headers.internal` — folder yang **dilacak sebagai input build tetapi
    tidak** masuk `-I`. Dipakai untuk shared header yang di-include lewat
    path relatif (`"../../library/pad.h"`), supaya folder itu tidak
    menaungi header sistem. Di workspace nilainya relatif root workspace.
    **Ini bukan sinonim `headers.each`** — `headers.internal` tidak pernah
    menjadi `-I`, sedangkan `headers.each` selalu berakhir sebagai `headers`
    (publik) tiap proyek.
  - `headers.<platform>` — header publik pada platform tertentu.
  - `headers.each` — **khusus workspace**: menerapkan nilai `headers` ke
    setiap proyek (relatif root tiap proyek), dengan token nama proyek
    sebagai penyaring dan `auto` = `I.` bila `include/` ada. Di mode proyek
    tunggal key ini jatuh ke daftar header publik biasa.
- **output**: `output.binary` (`false` untuk proyek library/archive),
  `output.binaryName/binaryDir/buildDir`,
  `output.compileCommands` (`auto|true|false`),
  `output.libraryName` (statis `lib<name>.a`), `output.libraryStatic`,
  `output.libraryShared` (dinamis `.so/.dylib/.dll` + `-fPIC` otomatis),
  `output.libDir`.
- **lain**: `clean as c` + `c.build/c.compdb`, `progress.bar`,
  `progress.error`.
- **embedded**: `embedded.<nama>.src/pattern/extract/variable/key/type/file`,
  `embedded.<nama>.archive.dir/name/with.tar/with.ext`. Hasil:
  `build/embedded.c` + `build/embedded.h` — codegen tunggal (array byte +
  tabel `getEmbed()`; jalur `ld -r -b binary` sudah dihapus — design/
  embedded.md). Freshness arsip otomatis, regenerasi content-based.
  `build/` proyek selalu ada di include path, jadi `#include <embedded.h>`
  tidak butuh baris `headers`.
- **archive**: `archive.src/dir/name/with/with.ext/exclude` untuk proyek
  arsip-saja (`o.binary = false`).
- **pack**: `pack.name/version/output/files/format/compress/optimize/
  checksum/exclude` + `pack.deb.*` (lihat [packaging.md](packaging.md)).
  Bare key `name/version/output/compress/optimize/checksum/format/files`
  juga dirutekan ke pack.
- **versi**: `.satria-version` → `build/version.h` (SATRIA_VERSION_EMBEDDED);
  mengubah isi memicu kompilasi ulang; `satria version` selalu benar karena
  versi ter-embed saat build.

## Workspace (`use workspace`)

- `projects = a, b, c`; alias `projects.a as x`; key per proyek `x.<key>`;
  `x.depends_on = a` (urutan build & invalidasi antar proyek); `x.cdeps = y`
  memperluas dependensi menjadi library + header publiknya.
- `<n>.root = .` mengubah root proyek (root di level workspace ditolak
  dengan peringatan; output tetap root-centric: `bin/`, `build/<proyek>/`,
  `lib/` di root workspace).
- `<n>.lang` — bahasa proyek (lihat bagian Bahasa); `headers.each` dan
  `headers.internal` lintas proyek.
- Konfigurasi tiap proyek **disusun di memori per pass** (`wsBuildConfig`);
  tidak ada Buildfile sintetis di disk. Baris `<proyek>.<key>` dipetakan
  dengan skoping path: kunci konten-proyek (sumber, headers, exclude,
  output) relatif root proyek; kunci lintas-proyek (`headers.internal`,
  `cdeps`, `embedded.*.file`, `archive.dir`, sisi source `release.files`)
  relatif root workspace.
- Eksekusi: `satria -w` (semua, hormati depends_on) / `satria -w <nama>`
  (selektif, tetap menarik dependensi).
- Paket distribusi: SATU paket workspace lewat section `release.*`
  (key identik `pack.*`). Sintaks revisi 1 (`releases = a, b`) sudah
  dihapus dan ditolak dengan pesan migrasi. Lihat [release.md](release.md).

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

## Cache startup & konfigurasi

Isi `.satria/` (root workspace pada mode workspace):

| Berkas | Isi |
|---|---|
| `opts.cache` | router: mode, proyek, dan kunci cache yang dipakai run ini |
| `startup.state` | model workspace hasil parse (proyek, alias, lang, release) |
| `sources.cache` | indeks walk sumber (`SATRIASRC2`): `R <dir> <ext>` + entri `D/S/H/O/W` berisi hash isi, mtime, ukuran, path |
| `caches/state.cache` | fast-state per proyek (stamp input + target, snapshot daftar sumber) |
| `caches/deps.cache` | dependensi header per unit |
| `caches/lib.cache` | fase library (meta + stamp) |
| `compile_commands.cache/.blob` | sidik CompDB agar isi identik tidak ditulis ulang |
| `report.blob` | data laporan build terakhir |

- `cfg/config_cache.c` memakai **versi 15** (terakhir: `langDeclared`).
  Cache hit bila Buildfile & kondisi terkait tak berubah → skip parse penuh;
  seluruh `Config` diserialisasi (sejak v6 termasuk `PackConfig`).
- `sources.cache` sengaja tidak menyimpan pemilik proyek: pemetaan
  proyek→sumber ada di `caches/state.cache` (snapshot `src`) yang diisi fase
  startup tipis bersama CompDB. Karena itu tidak ada berkas `sources.src`
  terpisah — informasinya sudah lengkap dari walk startup + state.
- `packSetStr` melepas kutip ganda pembuka/penutup.
- Routing `configApply`: bare key top-level
  `name/version/output/compress/optimize/checksum/format/files` → packApply;
  section `deb`, atau section `pack` sub `deb` → packApplyDeb; listForSection
  `pack` → pack.files.
- `configFinalizeEntry` berada di `embed.c` (dideklarasi `embed.h`): proyek
  tanpa sources otomatis `output.binary=false`.

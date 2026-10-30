# Embedded Assets & Archives

Dokumen ini merinci desain fitur *embedding* di Satria. Tujuannya adalah menyederhanakan cara developer menyisipkan file eksternal (arsip, gambar, teks, dll.) ke dalam binary, serta menjamin portabilitas lintas platform (termasuk MSVC di Windows) dan kemudahan penggunaan.

## Masalah Sebelumnya

Implementasi lama memiliki dua jalur, dan keduanya mengekspos permukaan API yang rumit ke kode konsumen:

- **Jalur utama: trik linker** `ld -r -b binary` (GNU ld) — tidak portabel (gagal di MSVC) dan mengekspos simbol mentah (`_binary_data_tar_start` / `_end`) yang harus diakses lewat `extern` + pointer arithmetic.
- **Jalur fallback C-array sudah ada** (MSVC / tanpa ld / `SATRIA_NO_LD=1`), tetapi konsumen tetap berurusan dengan makro per-enumerasi di `build/embedded.h` (`EMBED_<VAR>_SYMBOL`, `_SYMBOL_END`, `_ARCHIVE_NAME`, `_EXTRACT_DIR`, ...).

Akibatnya kode konsumen diisi plumbing sebelum logika bisnis — lihat pemakaian nyata di repo `tests/miniws/bade/src/embed_use.c` (contoh konsumen dengan makro `EMBED_*`): `#if __has_include("build/embedded.h")`, `extern` weak-symbol untuk dunia make/CMake alternatif (bahkan `tests/miniws/CMakeLists.txt` menulis `embedded.h` tiruan), dan ukuran dihitung dari selisih pointer. Salah satu penamaan makro = link error, dan `.variable` harus dijaga sinkron manual dengan nama simbol.

## Solusi Desain: Code Generation & Single API

Alih-alih memaksa linker memproses file biner mentah, Satria **hanya** menghasilkan file C standar — `embedded.c` dan `embedded.h` di output-dir proyek — yang berisi data sebagai array `static const uint8_t`. File `.c` dikompilasi sebagai source biasa bersama source proyek; **jalur `ld -r -b binary` dihapus**. Inilah inti rancangan ini: bukan sekadar fallback baru (itu sudah ada), melainkan *codegen sebagai satu-satunya mekanisme* **dan** satu API tunggal untuk semua tipe aset.

Satria mengekspos satu fungsi (`getEmbed`) untuk mengakses semua data, sehingga fitur ini tidak terkunci hanya untuk arsip, tetapi bisa digunakan untuk aset apa pun.

## API & Pengalaman Pengguna (User Experience)

```c
/* Satria menambah output-dir proyek ke include path; ejaan include
   dipatok di bagian Pipeline di bawah (kompatibel dengan ejaan lama). */
#include "embedded.h"

void test(void) {
    /* key didefinisikan di Buildfile (default: nama entri) */
    const Embedded *e = getEmbed("modules");

    /* Akses aman: NULL bila key tidak dikenal / tipe tidak diisi. */
    if (e && e->data) {
        printf("Archive : %s\n", e->name);          /* "rupa_modules.tar" */
        printf("Size    : %zu bytes\n", e->size);
        printf("Extract : %s\n", e->extract);        /* field dari .extract */
        fwrite(e->data, 1, e->size, stdout);
    }
}
```

## Definisi Struct (`embedded.h`)

Satu struct datar untuk semua tipe — tanpa flag `has_*` terpisah dan tanpa duplikasi struct per tipe. Satu-satunya pemeriksaan adalah null-check terhadap pointer hasil `getEmbed()`:

```c
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *key;      /* kunci: "modules" (default = nama entri)      */
    const char *type;     /* "archive" | "asset" | label lain (opsional)  */
    const char *name;     /* nama file sumber: "rupa_modules.tar"         */
    const char *extract;  /* field .extract, "" bila tidak diset          */
    size_t size;          /* jumlah byte data                             */
    const uint8_t *data;  /* isi file (static const, di .c yang digenerate)*/
} Embedded;

/* NULL bila key tidak dikenal. Iterasi tersedia via getEmbedAt(i). */
const Embedded *getEmbed(const char *key);
```

Menambah tipe baru di masa depan (json, teks, font, ...) = menambah nilai `type`, **bukan** menambah struct + pasangan flag baru — klaim "fleksibel" dipenuhi oleh struktur datanya sendiri.

## Pipeline Build di Satria

Satria menangani embedded dalam 4 langkah yang terintegrasi dengan pipeline yang sudah ada:

1. **Fase Parse & Generate:** saat membaca `Buildfile` dan entri embedded aktif, satria men-generate `embedded.c` (payload + tabel `getEmbed()`; payload lewat `.incbin` untuk GCC/Clang, array byte untuk MSVC  lihat *Mode Codegen*) dan `embedded.h`. **Regenerasi berbasis konten, bukan mtime** — lihat seksi *Regenerasi Content-Based* di bawah.
2. **Fase Kompilasi:** `embedded.c` diterbitkan **sebelum kompilasi dimulai** (sama seperti penerbitan `embedded.h`/`version.h` hari ini di `commands.c`) lalu diperlakukan persis seperti source biasa: masuk daftar source, ikut fingerprint, menghasilkan object dengan compiler yang sama (GCC/Clang/MSVC).
3. **Fase Linking:** object hasil codegen di-link bersama object lain. Pada fase library, object ini ikut dikemas ke `lib*.a` — kasus *"object harus tetap reachable oleh linker"* yang tercatat di `docs/guide/embedded-and-archives.md` **selesai dengan sendirinya** karena object adalah bagian normal dari build.
4. **Fase Akses:** developer memanggil `getEmbed("key")`.

### Path & include (dipatok)

- File digenerate berada di `outBuildDir` proyek seperti sekarang (`embed.c`): mode single → `build/`, mode workspace → output-dir proyek (root-centric, lihat `design/workspace.md`).
- Ejaan include **satu** untuk kedua mode, dan output-dir selalu masuk include path. Ejaan lama `#include "build/embedded.h"` (dipakai `wstest/ruka/src/main.c` dan pola `__has_include` di konsumen luar repo) **tetap resolve selama masa transisi** — pastikan saat implementasi diverifikasi terhadap konsumen nyata, jangan ditebak.

## Regenerasi Content-Based

Aturan ini wajib agar tidak melanggar filosofi touch-proof satria (lihat `design/startup.md`; bug serupa pernah terjadi dan diperbaiki di commit `9331330` — "archive always appears as new"):

1. Generate ke `embedded.c.tmp` (dan `embedded.h.tmp`).
2. Bandingkan byte dengan file yang sudah ada.
3. **Identik → buang file temp, jangan sentuh file lama** (mtime tetap).
4. Berbeda → rename atomik (`fsRenameReplace`), seperti pola `reportBlob`/`wsCacheSave`.

Akibatnya `touch data.tar` (isi sama) tidak memicu compile ulang maupun relink, dan run berikutnya tetap no-op. Perubahan isi file sumber tetap terekam lewat stamp/fingerprint seperti input biasa.

5. **Stamp (`embedded.stamp`)**: codegen dilewati bila stamp cocok  versi `2`, `gen` (waktu mulai codegen sebelumnya, ns), `mode`, dan per entri: path, ukuran, mtime (ns), key/type/name/extract/sym. Stamp hanya mempercepat keputusan; isi tetap diverifikasi saat codegen dijalankan.
6. **Aturan racy:** codegen dilewati hanya bila mtime setiap arsip **lebih awal** dari `gen`. Arsip yang berubah dalam detik yang sama dengan codegen tidak boleh dipercaya, jadi codegen diulang. `gen` diambil dari mtime file `.tmp` setelah header ditulis, sehingga domainnya sama dengan mtime arsip.

## Konfigurasi Buildfile

Contoh penulisan (mode workspace — `embedded.*.file` relatif **root workspace**, tanpa `../`):

```text
# Definisi embedded untuk proyek 'bade'; key default = nama entri "modules"
bade.embedded.modules as bademod

# File sumber yang akan di-embed (relatif root workspace)
bademod.file = modules/rupa_modules.tar

# Ekstraksi tujuan, diekspos sebagai field .extract di struct
bademod.extract = /tmp/rupa-system
```

### Tabel field

| Field | Wajib | Default | Keterangan |
|---|---|---|---|
| `<proyek>.embedded.<nama> as <alias>` | ya | — | deklarasi entri; `<nama>` jadi key default |
| `<alias>.file` | ya\* | — | arsip jadi, lewati tahap tar (relatif root workspace di mode ws) |
| `<alias>.src` | ya\* | — | alternatif: sumber yang diarsipkan (konten proyek) |
| `<alias>.key` | tidak | nama entri `<nama>` | kunci `getEmbed("<key>")`; cukup satu nama untuk entri & akses |
| `<alias>.type` | tidak | disimpulkan: `archive` bila `.file`/aturan arsip ada, selain itu `asset` | label `type` di struct |
| `<alias>.extract` | tidak | `""` | diekspos sebagai field `extract` (dibutuhkan `embed_use.c`) |
| `<alias>.variable` | tidak | UPPERCASE nama entri | prefix makro kompat `EMBED_<VAR>_*` (lihat Migrasi) |
| `<alias>.pattern`, `<alias>.exclude`, `archive.*` | — | — | aturan penyaringan/arsip tetap seperti implementasi sekarang |

\* salah satu dari `.file` atau `.src`.

Tiga nama untuk satu entri (alias, nama entri, key) pada rancangan sebelumnya diganti default: **key = nama entri**, `.key` hanya override bila memang perlu.

## Integrasi Incremental & State (checklist implementasi)

- `embedded.c` diterbitkan **sebelum** fase compile agar daftar source/fingerprint mengikutinya (preseden: penerbitan `embedded.h` + `version.h` di `commands.c`).
- Header mengikuti state dependensi seperti header lain (closure `.d`, blob `hdr/*` di `caches/state.cache`) sehingga perubahan key/type me-recompile konsumen, sedangkan regenerasi identik tidak.
- Gate no-op: stamp `embedded.*.file`/`.src` tetap input proyek (sudah dicatat hari ini di fast-state, `cmds_state.c`) — file embed berubah isi → no-op batal; sentuh-saja → tetap no-op (aturan content-based di atas).
- Mode workspace: resolusi path mengikuti aturan kunci ws (`ws_config.c`): `.file/.dir` relatif root workspace, `.src` konten proyek.

## Keuntungan Desain Ini

1. **Portabilitas Mutlak:** tidak ada ketergantungan pada fitur linker eksotis. MSVC mengompilasi array `uint8_t` standar; GCC/Clang memakai `.incbin` (lihat *Mode Codegen*) dengan fallback array untuk MSVC.
2. **Kemudahan Penggunaan (DX):** tanpa simbol linker aneh, tanpa hutan makro — cukup `getEmbed("key")` dengan null-check.
3. **Fleksibel:** tidak terkunci untuk archive; aset lain cukup menambah nilai `type`.
4. **Aman (Safe):** pointer `const` + NULL-check mencegah akses data yang tidak tersedia; ukuran eksplisit (`size`) menggantikan pointer arithmetic.
5. **Debugging Mudah:** developer bisa membuka `embedded.c` hasil generate — tidak ada *black box*.
6. **Integrasi lebih rapi:** object generated jadi source biasa → kasus static-library *"harus tetap reachable"* selesai natural; konsumen kehilangan seluruh blok `__has_include`/weak-symbol.

## Catatan Skala & Portabilitas

- Array besar (ratusan KB s.d. beberapa MB) aman di GCC/Clang; **MSVC perlu diverifikasi** pada batas ukuran initializer — uji dengan arsip berukuran representatif (bukan hanya fixture KB).
- Array C berukuran besar mahal: payload 6 MB  ~7,8 s kompilasi GCC; 60 MB  teks C ~300 MB dan gcc kehabisan memori di mesin uji. Untuk GCC/Clang ini ditangani oleh mode `incbin` (lihat *Mode Codegen*).
- Bila dibutuhkan kelak: strategi chunking (pecahan array per ~64KB) atau fast path `#embed` (C23) dengan fallback ke array generated. Tidak masuk scope v1, tetapi format `Embedded` (pointer + size) sudah mengakomodasi keduanya tanpa mengubah API.

## Mode Codegen

Dua mode menghasilkan API yang sama (`getEmbed`, `getEmbedAt`, struct `Embedded`, simbol `<sym>_start/_end/_len`); konsumen tidak melihat perbedaannya.

| Mode | Dipakai untuk | Payload masuk ke object lewat |
|---|---|---|
| `incbin` | GCC dan Clang (default) | assembler: `.incbin` di blok `__asm__` |
| `carray` | MSVC (wajib), atau bila diminta | array `static const uint8_t` di C |

### Kenapa `.incbin`

Biaya utama array C ada di **kompilasi**, bukan di codegen: gcc harus mem-parse teks C sebesar payload. `.incbin` menyuruh assembler menyalin byte file langsung ke section `.rodata`, tanpa parsing C sama sekali.

| Payload | Mode | Waktu |
|---|---|---|
| 6 MB | carray | 7 753 ms |
| 6 MB | incbin | 24 ms |
| 60 MB | incbin | 94 ms (objek 60 MB) |
| 60 MB | carray | ±77 s (estimasi), teks C ~300 MB, gcc OOM di mesin uji |

Shared library tidak menyelesaikan masalah ini: `.so` yang dibangun dari array C punya biaya kompilasi yang sama. Tidak ada shared library dalam desain ini; payload tetap statis di binary.

### Bentuk `embedded.c` (mode incbin)

- Prelude makro: `SATRIA_PFX` (awalan label dari `__USER_LABEL_PREFIX__`, `_` di Mach-O), `SATRIA_SECT` (`.section .rodata`, atau `__TEXT,__const` di Apple), `SATRIA_LONG_DIR` (`.quad` bila `long` 8 byte, `.long` selain itu).
- Per entri: satu blok `__asm__` berisi `_start:` `.incbin "<path absolut>"` `_pend:` `_end: .byte 0` `_len: .quad _pend - _start`. Nama simbol **sama** dengan mode carray, jadi `embedded.h` tidak berubah.
- Tabel `embed_table` dibuat non-const dan ukurannya diisi oleh `__attribute__((constructor))` dari `_len`. Dengan begitu ukuran selalu cocok dengan byte yang benar-benar di-assemble, walaupun arsip berubah di antara codegen dan kompilasi.
- Komentar per entri memuat `size` dan **hash FNV-1a 64 isi arsip**, bukan mtime. Ini menjaga regenerasi berbasis konten (lihat *Regenerasi Content-Based*): `touch` atau pemulihan arsip dengan isi sama tidak mengubah `embedded.c`, sehingga tidak ada kompilasi ulang.
- Path arsip diubah menjadi absolut (cwd + path relatif, backslash jadi slash). Path yang mengandung `"` atau newline tidak bisa masuk string assembler; dalam kasus itu satria memakai mode carray untuk seluruh proyek.

### Pemilihan mode

1. `compilerIsMSVC(c)` -> `carray` (MSVC tidak punya `.incbin`).
2. Env `SATRIA_EMBED_MODE=carray` -> `carray`; `=incbin` -> `incbin` (non-MSVC).
3. Path arsip tidak bisa dijadikan literal assembler -> `carray`.
4. Selain itu -> `incbin`.

Mode ikut tercatat di stamp (`mode incbin|carray`). Stamp versi `1` (sebelum mode ada) dianggap tidak cocok, sehingga codegen diulang sekali setelah upgrade.

**Catatan env override:** `SATRIA_EMBED_MODE` bukan bagian dari fingerprint. Build tanpa perubahan lewat fast path no-op tidak menjalankan codegen, sehingga mode tidak berubah. Untuk mengganti mode, hapus `build/<proyek>/embedded.*` atau jalankan `clean`. Ini knob pengujian, bukan pengaturan proyek.

### Batasan yang diterima

- Arsip yang berubah **selama** build (antara codegen dan assembler) menghasilkan object dari isi terbaru, dan ukuran di tabel tetap benar karena dari `_len`. Komentar hash di `embedded.c` bisa tertinggal dari isi itu; run berikutnya mendeteksi mismatch lewat stamp dan meregenerasi. Tidak ada keadaan salah yang bertahan.
- Build MSVC belum diuji di mesin ini (jalur carray tidak berubah).

### Verifikasi

- Dumper yang sama (`getEmbed` -> ukuran + hash isi + sentinel) menghasilkan output identik untuk mode carray dan incbin.
- Payload 60 MB: cold build sukses, byte yang dibaca identik dengan arsip, perubahan isi (ukuran berbeda) menghasilkan ukuran baru di tabel.
- `touch` arsip (isi sama): 0 compile.
- Path dengan spasi: sukses.
- `tests/unit/embed_stamp_skip.sh` dan seluruh tes unit lulus.

## Migrasi & Kompatibilitas

Konsumen nyata yang harus tetap hijau: `tests/miniws/bade/src/embed_use.c`, `tests/wstest/ruka/src/main.c`, fixture `tests/miniws`, `bench/ws/gen.sh` + `bench/workspace*.sh` (menulis `.file/.variable/.extract/.pattern`), `tests/miniws/verify-fpic.sh`, `tests/miniws/CMakeLists.txt` (embedded.h tiruan), dan penghasil `embedded.*` di `src/nc/nc_convert.c` (konverter Makefile).

Strategi:

1. **Fase 1 — kompat:** codegen baru (`getEmbed` + struct datar) digenerasi **bersama** makro lama `EMBED_<VAR>_ARCHIVE_NAME/_EXTRACT_DIR/_SYMBOL/_SYMBOL_END` (termasuk `.variable` yang lama tetap diterima). Ejaan include lama tetap resolve. Jalur `ld -r -b binary` dimatikan, tetapi simbol-simbol makro lama kini menunjuk array hasil codegen sehingga konsumen lama tanpa `getEmbed` tetap jalan — termasuk fallback `__has_include` pada konsumen lama.
2. **Fase 2 — migrasi konsumen:** `embed_use.c` (dan konsumen lain) pindah ke `getEmbed()`; komentar sisa rebranding ikut diperbaiki.
3. **Fase 3 — pensiun makro:** makro lama diberi catatan deprecated di `embedded.h`, dihapus setelah satu siklus rilis.
4. Konfigurasi lama (`variable`, key tanpa `.key`, `pattern`, `extract`) tidak berubah artinya — tidak ada migrasi Buildfile pengguna.

## Definition of Done

- `sh scripts/unit.sh` — seluruh suite (177 pemeriksaan) hijau, termasuk skenario embed/no-op.
- `bench/ws.sh` 12/12 *in accordance* (skenario `rel` dan `mod` menyentuh embed).
- `touch` file embed (isi sama) tetap no-op; ubah isi → konsumen direcompile + relink.
- `embed_use.c` tetap terbangun dan berjalan (fase 1), lalu dimigrasi ke `getEmbed()` (fase 2).
- Build MSVC/GCC/Clang dengan arsip berukuran representatif menghasilkan binary yang berfungsi (GCC/Clang: mode incbin; MSVC: mode carray).

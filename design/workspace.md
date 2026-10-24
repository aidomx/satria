# Workspace

Dokumen ini mencatat aturan, desain, dan target implementasi workspace `satria` v0.2.4+. Workspace adalah jalur build tersendiri; perubahan di sini tidak mengubah perilaku maupun performa single-project.

---

## Filosofi

Workspace di `satria` dirancang dengan prinsip:
- **Convention over Configuration** — proyek anggota tidak butuh `Buildfile` lokal kecuali memang ingin berdiri sendiri.
- **Root-Centric Output** — semua artefak build (biner, objek, library, arsip, state, CompDB) dipusatkan di root workspace. Direktori anggota hanya berisi sumber.
- **Single Source of Truth** — `Buildfile.ws` di root adalah satu-satunya konfigurasi untuk seluruh workspace.
- **Two-Phase Build** — library pass dulu (tanpa link binary), baru binary pass. Ini memungkinkan dependensi siklik (`core ↔ bade`) diselesaikan secara native.

---

## Aturan Pengguna

1. **Build hanya dari root workspace.** Pemanggilan `satria` atau `satria -g compdb` dari direktori proyek anggota ditolak dengan pesan yang menunjuk ke root workspace dan `satria -w <project>`.
2. **Build selektif** menggunakan `satria -w <project>` dan membangun dependency closure yang diperlukan. Build penuh tetap `satria -w`.
3. **Editor sebaiknya membuka root workspace** agar source seluruh proyek dan satu `compile_commands.json` dapat diakses bersama.
4. **Direktori anggota adalah penyedia source/config**, bukan tempat output build.
5. **Deteksi anggota workspace** harus memvalidasi keanggotaan pada `Buildfile.ws` induk; keberadaan file workspace pada parent saja tidak cukup. Jalur single-project di luar workspace tidak berubah.
6. **Proyek anggota tidak perlu `Buildfile` lokal.** Seluruh konfigurasi didefinisikan di `Buildfile.ws`. Pengecualian: proyek yang ingin bisa di-build secara standalone boleh punya `Buildfile` sendiri, dan diintegrasikan ke workspace via **symlink**.

---

## Struktur Direktori

```text
workspace/
 ├── Buildfile.ws                 # Satu-satunya file konfigurasi
 ├── compile_commands.json        # CompDB global (seluruh workspace)
 ├── library/                     # Shared headers (nama bebas, tidak harus 'include')
 │   └── ext.h
 ├── .satria/                       # State & cache terpusat
 │   ├── startup.state             # Config cache (parse Buildfile.ws)
 │   ├── sources.cache             # Peta path -> hash isi + stamp (fast path no-op)
 │   ├── opts.cache                # Route/opsi + stamp grup per proyek
 │   ├── artifact.cache            # Validasi artefak release/embed
 │   ├── report.blob               # Laporan terkondensasi (diputar saat no-op)
 │   ├── ld.cache                  # Cache verdict linker
 │   └── caches/
 │       ├── fingerprint.cache     # Fingerprint per proyek + fast state + metadata grup P
 │       ├── state.cache           # Blob per proyek (ws.state, snapshot src, hdr/*)
 │       ├── deps.cache            # Dependensi .d per proyek
 │       ├── lib.cache             # Stamp fase library per proyek
 │       └── embed.cache           # Cache arsip embedded
 ├── build/                       # Objek (.o) dan dependency files (.d)
 │   ├── rupa/
 │   └── bade/
 ├── bin/                         # Biner final (FILE, bukan direktori per proyek)
 │   ├── rupa                     # bin/rupa, bukan bin/rupa/rupa
 │   └── bade
 ├── lib/                         # Static/shared libraries
 │   ├── librupa.a
 │   └── libbade.a
 ├── dist/                        # Release
 │   └── rupa_v0.2.2.deb
 ├── modules/                     # Arsip sumber (archive.dir, nama bebas)
 │   └── rupa_modules.tar
 ├── rupa/                        # Source proyek (BERSIH, tanpa .satria)
 ├── bade/                        # Source proyek (BERSIH)
 └── rupamod/                     # Source proyek (BERSIH)
```

**Aturan penting:**
- Binary di root adalah **file**: `bin/bade`, bukan `bin/bade/bade`. Object memakai `build/<project>/...` agar nama source antar-proyek tidak bertabrakan.
- Nama output binary/library/archive yang bentrok **harus dideteksi**, bukan ditimpa diam-diam.
- Jika `satria` mendeteksi folder `.satria/` di dalam direktori proyek anggota saat mode workspace aktif, dia harus mengabaikannya atau memberikan peringatan, dan tetap menggunakan state di root workspace.
- Nama folder shared headers bebas (tidak harus 'include'). Inilah alasan `headers.internal = library` — karena nama folder yang dipilih user adalah `library`.

---

## Konfigurasi Buildfile.ws

### Sintaks Dasar

```text
use workspace

# Alias singkat (opsional, untuk mengurangi pengetikan)
projects.rupamod as mod
projects.core as core

# Daftar proyek — root default <root>/<nama>, bisa dioverride per proyek
projects = bade, core, rupamod
projects.bade.root = path/khusus    # opsional; dipakai bila folder ≠ <root>/<nama>

# Pengaturan global (berlaku untuk semua proyek, kecuali ditimpa).
# Catatan skoping path: kunci konten-proyek (sumber, headers, exclude,
# output) relatif ROOT PROYEK masing-masing; kunci lintas-proyek/workspace
# (headers.internal, cdeps, embed, archive.dir, sisi source release.files)
# relatif ROOT WORKSPACE.
compiler = gcc, clang
flags = Wall, Wextra, MMD, MP
headers = include, I.       # per-proyek: relatif root tiap anggota
library.default = ssl, crypto
library.linux = m, pthread
library.macos = m
library.windows = ws2_32
std = gnu11
```

### Platform-Specific Flags & Libraries

Properti yang mendukung sufiks spesifik: `flags`, `library`, `headers`, `defines`, `std`.

```text
# Flags global
flags = Wall, Wextra

# Flags spesifik platform (di-append ke global, deduplikasi otomatis)
flags.linux = D_POSIX_C_SOURCE=200809L
flags.macos = D_DARWIN_C_SOURCE
flags.windows = D_CRT_SECURE_NO_WARNINGS, DWIN32_LEAN_AND_MEAN

# Flags spesifik compiler
flags.clang = fcolor-diagnostics
flags.gcc = fdiagnostics-color=always

# Defines (satria otomatis translasi -D untuk GCC/Clang, /D untuk MSVC)
defines = DEBUG, VERSION=\\\"1.0.0\\\"
defines.linux = _POSIX_C_SOURCE=200809L
defines.windows = UNICODE, _UNICODE

# Library
library.linux = pthread, m, dl
library.windows = ws2_32, advapi32, userenv

# Headers lintas-proyek: relatif ROOT WORKSPACE (nama folder bebas)
headers.internal = library
```

**Aturan Resolusi:**
- Nilai global dimuat pertama, lalu nilai spesifik platform/compiler di-append.
- Untuk array (`flags`, `library`), duplikat dihapus otomatis.
- User tidak perlu menulis prefix `-D` atau `/D` di `defines`; `satria` menerjemahkan berdasarkan compiler yang terdeteksi.
- Fallback `unix` berlaku untuk semua OS berbasis POSIX jika tidak ada definisi spesifik.

### Proyek Anggota

```text
# A. rupamod — proyek kemasan (archive)
mod.output.binary = false
mod.archive.src = .                     # relatif root proyek
mod.archive.name = rupa_modules
mod.archive.with = tar
mod.archive.exclude = build, dist, LICENSE, README.md, self_archive
mod.archive.dir = modules                # lintas-proyek: relatif root workspace

# B. Core engine
core.sources = src
core.exclude = main.c
core.output as coreout
core.library as corelib
coreout.binaryName = rupa
coreout.libraryName = core
coreout.libDir = lib
coreout.libraryShared = true

# C. bade — embed arsip dari rupamod
bade.sources = manager, src
bade.exclude = manager/main.c
bade.output as badeout
bade.library as badelib
badeout.binaryName = bade
badeout.libraryName = bade
badeout.libDir = lib
badeout.libraryShared = true
bade.embedded.modules as bademod
bade.depends_on = rupamod
bademod.file = modules/rupa_modules.tar    # lintas-proyek: relatif root workspace
bademod.variable = MODULES
bademod.extract = /tmp/rupa-system
bademod.pattern = .rp

# Dependensi siklik (core ↔ bade) — ditangani Two-Phase Build
badelib.cdeps = core
corelib.cdeps = bade
```

### Release & Packaging

```text
# Release — SATU paket distribusi workspace (semua path relatif root workspace)
release.name = rupa
release.version = 0.2.2
release.files = bin/rupa:bin/rupa, bin/bade:bin/bade, core/src/prompt/cmd.txt:share/rupa/cmd.txt
release.exclude = build, dist
release.output = dist/{name}-v{version}.tar.gz
release.format = deb
release.checksum = sha256
release.deb.install_prefix = /usr/local
```

Release bisa dipicu murni dari CLI tanpa deklarasi:
```bash
satria -w release -- name=rupamod target=deb
```

**`pack.*` per proyek di luar scope v1:** jalur workspace hanya mendukung **build + release workspace** (`release.*`). Properti `pack.*` per proyek diabaikan dengan peringatan singkat (tidak menggagalkan build); dukungan menyusul di iterasi terpisah.

---

## Resolusi Path & State

### Path

1. **CWD Tetap di Root:** `satria` tidak pernah mengubah CWD ke direktori proyek anggota saat build. CWD selalu di root workspace; `fsSetCwd()` per-proyek dilarang di jalur workspace.
2. **Root Proyek:** default `<root>/<nama>`; override eksplisit `projects.<nama>.root`. Semua path yang ditafsirkan relatif proyek memakai root ini.
3. **Default Output Path (Root-Centric):**
   - Binary: `<root>/bin/<binaryName>` — **file**, bukan direktori; default `binaryName` = nama proyek sehingga menghasilkan `bin/bade` (bukan `bin/bade/bade`).
   - Build (Object/Dep): `<root>/build/<nama_proyek>/`
   - Library: `<root>/lib/`
   - Release/Dist: `<root>/dist/`
4. **Source Path (kunci konten-proyek):** `sources`, `exclude`, `headers` (tanpa `.internal`), `archive.src`, dan lokasi file proyek lain ditafsirkan relatif terhadap root proyek; hasil kompilasi dikirim ke `<root>/build/<nama_proyek>/`.
5. **Path Lintas-Proyek & Workspace (kunci ws):** `headers.internal`, `cdeps`, `embedded.*.file`, `archive.dir`, dan sisi source `release.files` ditafsirkan relatif terhadap root workspace — misal `headers.internal = library` = `<root>/library`.

### State & Cache

1. **Global State:** seluruh cache berada di root workspace — `caches/fingerprint.cache` (fingerprint per proyek, snapshot fast state, metadata grup P), `caches/state.cache` (blob per proyek: `ws.state`, snapshot `src`, salinan header `hdr/*`), `caches/lib.cache`, `caches/deps.cache`, `sources.cache`, `opts.cache`, `artifact.cache`, `startup.state`, dan `compile_commands.json`.
2. **Per-Project State:** tidak ada lagi file per proyek (`rupa.ws.state`, `rupa.build.fingerprint`, ...) — semuanya menjadi record/entri berprefix nama proyek di file cache terpusat di atas. Sisa format lama (`workspace.state`, `workspace_log`, `.satria/workspace/*`) di-sweep otomatis saat run.
3. **Implementasi C:** baca/tulis state lewat API cache (`stateBlob*`, `csOpen`/`csCommit`, ...) dengan path root workspace; tidak ada penulisan state ke direktori anggota dan tidak ada chdir.
4. **No-Op Check:** gerbang no-op (`wsCacheNoopValid`, design/startup.md §4) menilai `sources.cache` (fast path metadata P — N stat tanpa membuka `fingerprint.cache`), lalu `caches/fingerprint.cache` + `artifact.cache`; pesan no-op diseragamkan (`> Cache valid: ...`) dan laporan tersimpan diputar dari `.satria/report.blob`.

---

## Pipeline `wsCmdBuild()`

1. **Muat model dan `WsConfig` workspace**, lalu validasi proyek serta konflik output.
2. **Discovery seluruh workspace sekali:** satu traversal per direktori, kumpulkan seluruh ekstensi source yang didukung, terapkan `exclude`, dan buat indeks source → proyek. Hindari `opendir()` ganda dan `stat()` yang tidak perlu.
3. **Hitung dependency closure** untuk target selektif dan rencana pekerjaan dari indeks yang sama, tanpa discovery ulang per proyek.
4. **Periksa incremental state**, source/header dependencies, command/fingerprint, dan output.
5. **Compile secara paralel** ke `build/<project>/...` serta hasilkan dependency files (path `.d` dibuat relatif root — aman karena CWD tidak berubah).
6. **Kerjakan library, archive/embed, dan linking** sesuai dependensi nyata, termasuk dependensi siklik yang didukung. Binary di `bin/<name>` (file), library di `lib/`, arsip member ke `archive.dir` (mis. `modules/`), release ke `dist/`. Fase library memakai **penjadwal siap-dep** (anak fork diluncurkan segera setelah `depends_on` selesai; jalur serial `-j1`/`SATRIA_WS_PAR=0` bertingkat dependensi), fase binary menyusul setelah seluruh library tersedia.
7. **Perbarui cache state** (`caches/*`, `sources.cache`, `opts.cache`) dan CompDB bila memang berubah — tulis atomik sekali di akhir run sukses.

### Pemisahan Jalur Build

- `cmdBuildEx()` hanya menjadi referensi alur untuk implementasi workspace, **bukan dipanggil** dari build workspace.
- Implementasi workspace berada di `src/workspace/` dengan `wsCmdBuild()` dan `WsConfig` sendiri. `WsConfig` adalah model workspace (parse `Buildfile.ws`, resolusi path per-kunci, closure) yang dimiliki penuh oleh modul workspace — tanpa sintesis sebagai konfigurasi yang bisa di-edit user, tanpa `loadConfig()` sebagai *jalur utama*, dan tanpa pemanggilan `cmdBuildEx()`. Mesin mekanis yang sudah matang (decide mtime + deps `.d`, parallel jobs, link, kemasan library, fingerprint, fast-state) dipakai ulang dengan `Config` per proyek yang **disusun/di-rebase di memori** sebagai pembungkus path hasil resolusi `WsConfig` — chdir tidak pernah terjadi, dan state/CompDB/output tetap mengikuti aturan root-centric di atas. (Amendasi hybrid: ketergantungan yang dilarang adalah *alur* Config+loadConfig+chdir, bukan pemakaian tipe `Config` sebagai wadah path.)
- Jalur workspace **tidak mengubah CWD** ke direktori proyek (`fsSetCwd()` per-proyek dilarang).
- Komponen bersama yang independen dari `Config` (filesystem, process runner) boleh dipakai tanpa mengubah perilakunya. Jangan menambah overhead workspace ke jalur single-project.
- Setelah `wsCmdBuild()` lengkap dan lolos regresi, bersihkan cabang workspace yang sudah tidak diperlukan di `cmdBuildEx()`.

---

## No-op dan CompDB

- **Indeks no-op & CompDB** kini berupa entri per proyek di `caches/state.cache` (leaf `compdb-index-v1`; berkas `workspace_log` lama hanya objek sweep). Isi per proyek:
  1. nama proyek,
  2. hash konfigurasinya dari `Buildfile.ws` (baris yang memengaruhinya),
  3. fingerprint daftar source terakhir (dari snapshot `src`),
  4. hash isi `compile_commands.json` terakhir.

  Indeks ini hanya dipakai untuk dua keputusan: **(a) no-op cepat** — proyek dengan (2)+(3) identik dilewati tanpa re-scan source-nya; **(b) skip-write CompDB** — `compile_commands.json` hanya ditulis ulang bila hash-nya berbeda dari (4). Indeks **tidak** menyimpan mtime/size/artefak — domain itu milik `caches/fingerprint.cache` (fingerprint + fast state) dan `artifact.cache`; jangan menduplikasinya.
- `compile_commands.json` **hanya ada di root workspace** dan selalu merepresentasikan seluruh workspace, sekalipun build terakhir selektif.
- `satria -g compdb` dari root workspace bersifat **query-only**: tidak menjalankan build atau mengubah build state.
- Jangan tulis ulang CompDB yang identik. Perubahan daftar source atau command harus terdeteksi meski `Buildfile.ws` tidak berubah.
- No-op tidak boleh memicu kompilasi, link, archive, atau penulisan artefak yang tidak diperlukan.

---

## Ide Masa Depan: Workspace Flatten (Cold Build Optimization)

*Catatan: Ini adalah desain konseptual, tidak dieksekusi secara default.*

Untuk mempercepat cold build, `satria` dapat menerapkan **Source Flattening & Batch Compilation**:

1. **Fase 0 (Flatten):** `satria` membaca `Buildfile.ws` sekali, meratakan semua daftar source dari semua proyek menjadi satu daftar raksasa di memori.
2. **Fase 1 (Batch Compile):** Antrian raksasa dilempar ke compiler pool secara bersamaan. Tidak ada sekat antar proyek. Kecepatan setara Ninja.
3. **Fase 2 (Reverse Map):** Setelah `.o` terbentuk, `satria` melakukan pencocokan path (`src_a/a.c` → `build/a.o`) untuk mengembalikan object files ke proyek masing-masing.
4. **Fase 3 (Library & Binary Pass):** Berjalan seperti biasa menggunakan object files yang sudah siap.

**Dampak:** Mengurangi overhead konteks antar-proyek saat fase kompilasi, membuat cold build jauh lebih cepat tanpa mengubah arsitektur fase build.

---

## Kriteria Selesai

- [ ] Bootstrap berhasil; build penuh dan `satria -w <project>` berhasil dari root workspace.
- [ ] **Tidak ada build state, object, binary, atau CompDB baru di direktori anggota proyek.** (Semua harus di root workspace).
- [ ] Compile, link, archive/embed, dan dependensi lintas proyek benar-benar dijalankan; tidak ada fase yang dilewati hanya untuk mempercepat benchmark.
- [ ] No-op dan incremental (edit source/header, penambahan/penghapusan source, perubahan config) diuji; CompDB lengkap dan valid tanpa rewrite pada no-op.
- [ ] Pemanggilan dari anggota workspace ditolak; proyek standalone tetap dapat dibangun seperti sebelumnya.
- [ ] `cmdBuildEx()` tidak dipanggil pada jalur workspace dan tidak mengalami regresi fungsional/performa.
- [ ] Dependensi siklik (`core ↔ bade`) diselesaikan dengan Two-Phase Build tanpa error `dependency cycle`.
- [ ] Shared headers di root (nama bebas) dikenali dan ditambahkan ke `-I` secara otomatis.
- [ ] Platform-specific flags (`flags.linux`, `defines.windows`, dll.) di-resolve dan di-append dengan benar.
- [ ] Fixture workspace (`tests/rupaws/Buildfile.ws`) dan generator bench dimigrasi ke path root-centric; seluruh regresi lulus (self-host, 12 skenario `bench/ws.sh`, rupaws).
- [ ] `pack.*` per proyek diabaikan dengan peringatan di mode workspace (di luar scope v1, tidak menggagalkan build).

---

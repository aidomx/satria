# satria v0.2.4 — Cheatsheet

Referensi cepat untuk penggunaan sehari-hari. Berdasarkan dokumentasi `satria-v0.2.4`; source dipakai untuk memeriksa detail implementasi yang relevan.

---

## 1. CLI

### Build project saat ini

```bash
satria
satria -f FILE
```

`-f` memakai Buildfile alternatif.

### Init

```bash
satria init
satria init -p
satria init -w
```

- `init` → Buildfile project minimal.
- `init -p` → interactive scaffold.
- `init -w` → `Buildfile.ws` workspace.

`init -p` tidak pernah overwrite: jika target sudah ada, proses dibatalkan sebelum menulis.

### Jobs

```bash
satria -j
satria -j4
satria -j1
satria --jobs=4
```

### Lainnya

```bash
satria clean
satria version
satria help
```

### Workspace

```bash
satria -w
satria -w NAME
satria -w clean
```

- `satria -w` → build workspace dan release yang dideklarasikan.
- `satria -w NAME` → build project `NAME` beserta dependency closure; hanya release milik target yang dirilis.

### Release CLI

```bash
satria -w release -- name=rupa
satria -w release -- name=rupamod target=deb
```

`name=` wajib dan project harus ada. `target=` dapat berupa `tar` atau `deb` dan hanya berlaku untuk run tersebut.

### Ninja conversion

Tree v0.2.x memiliki dukungan konversi Ninja, tetapi flag persisnya implementation-specific. Gunakan:

```bash
satria -h
```

sebagai sumber kebenaran untuk binary yang terpasang.

### Profile

```bash
satria profile archive=rupamod
satria profile archive=rupamod with=tar,gz      # atau tar,xz / tar,bz2 / tar / none
satria profile embed=rupamod                    # dir atau file
satria profile library=rupamod with=static,shared
satria profile binary=. jobs=4 --report profile.md
```

- Mengukur biaya operasi tanpa build normal; tidak menyentuh build state.
- `with=` spesifik per context; opsi tak dikenal ditolak dengan pesan jelas.
- TTY: ringkasan singkat. Pipe (`> profile.md`): laporan rinci.
- `--report FILE`: laporan rinci ke file, ringkasan tetap di terminal.

### Background build & watch

Aktifkan di `Buildfile` (atau baris global di `Buildfile.ws`):

```text
background = true
```

`satria` / `satria -w` lalu menjalankan build di child terpisah dan langsung
kembali ke prompt (exit code 0 = "berhasil memulai"); output build masuk ke
`.satria/background.log`:

```bash
satria                     # build berjalan di background
satria -w
satria watch               # pantau: progress -> summary -> keluar
satria watch --full        # stream background.log live (tail -f)
satria -w watch [--full]   # sama untuk workspace
```

- Run yang sudah no-op (tidak ada yang dikerjakan) dijawab langsung di
  terminal oleh jalur router/fast-state — tanpa child, tanpa output kosong;
  exit 0 = "berhasil memulai" hanya saat build benar-benar jalan di background.
- Watcher keluar `0` = success, `1` = failed; saat selesai otomatis cetak
  Build Summary (pola `> Summary :`).
- Build ganda ditolak: `Error: Background build is already running (PID: N).
  Use 'satria watch' to monitor.`
- Ctrl+C di watch hanya melepas watcher — build tetap jalan (bisa attach
  ulang kapan saja).
- `clean` diblokir selama build background berjalan.

---

# 2. Project Buildfile

Minimal project standar:

```text
use project
```

Layout standar:

```text
project/
├── Buildfile
├── include/       # opsional
└── src/
```

Dengan layout standar, satria mengisi default secara otomatis.

### Default utama

| Field | Default |
|---|---|
| `sources` | `src` |
| `headers` | `include` jika folder ada |
| `output.binaryName` | nama folder project, disanitasi |
| `std` | `gnu11`, atau `c++17` jika ada `.cpp` |
| `compiler` untuk C++ | `g++` / `clang++` saat `.cpp` terdeteksi |
| `output.binaryDir` | `bin` |
| `output.buildDir` | `build` |
| `output.compileCommands` | `auto` |
| `output.libDir` | `lib` |

Deklarasi eksplisit selalu mengalahkan default.

---

# 3. Konfigurasi dasar

```text
use project

root = .
sources = src
headers = include, I., build
flags = Wall, Wextra, O2, MMD, MP
std = gnu11
compiler = gcc, clang
```

Field utama:

```text
root       # project root
sources    # source directories
headers    # include/header directories
exclude    # source yang dikecualikan
aflags     # compiler flags
std        # language standard
compiler   # compiler candidates
```

> `flags` adalah field yang benar; bukan `aflags`.

Contoh lengkap output:

```text
output.binary = true
output.binaryName = app
output.binaryDir = bin
output.buildDir = build
output.compileCommands = auto
```

---

# 4. C++

Jika source mengandung `.cpp`, satria otomatis memakai mode C++ dengan default:

```text
std = c++17
```

dan compiler C++ yang tersedia (`g++` / `clang++`).

Override bila diperlukan:

```text
std = c++20
compiler = g++, clang++
```

Contoh:

```text
use project

sources = src
headers = include
std = c++20
compiler = clang++
```

---

# 5. Alias

Konfigurasi dapat diberi alias:

```text
clean as c
library as lib
output as o
```

Kemudian:

```text
o.binaryName = app
o.binaryDir = bin
o.buildDir = build
```

Alias juga digunakan di workspace untuk project/release/embedded/archive.

---

# 6. Library

Library link biasa:

```text
library.default = ssl, crypto
```

Platform-specific:

```text
library.linux = m, pthread
library.macos = m
library.windows = ws2_32
```

Output library:

```text
output.binary = false
output.libraryName = core
output.libDir = lib
output.libraryShared = true
```

> `lib.cdeps` / `*.cdeps` kini field resmi: dependensi lintas-proyek mode workspace (mis. `badelib.cdeps = core`), diselesaikan lewat graph `cdeps`.

---

# 7. Compile database

```text
output.compileCommands = auto
```

Menghasilkan/memperbarui:

```text
compile_commands.json
```

`auto` berusaha menghindari rewrite yang tidak diperlukan ketika konfigurasi relevan tidak berubah.

---

# 8. Clean & process behavior

Clean dikontrol melalui `clean`/alias:

```text
clean as c

c.build = false
c.compdb = false
```

Process/SIGINT:

```text
foreground = true
```

Background build — `background = true` (lihat §1 *Background build & watch*);
`clean` diblokir selama build background berjalan:

```text
background = true
```

Progress:

```text
progress.bar = true
progress.error = always
```

---

# 9. Workspace

Minimal:

```text
use workspace

projects = app, tool
```

Project standar tidak membutuhkan Buildfile sendiri. satria mensintesis konfigurasi project dari `Buildfile.ws`.

Contoh per-project:

```text
projects.rupamod as mod
projects.rupa as rupa
projects.ruka as ruka
```

Common setting berlaku untuk semua project:

```text
compiler = gcc, clang
flags = Wall, Wextra, MMD, MP
headers = include, build, I.
std = gnu11
```

Per-project:

```text
rupa.sources = src
rupa.exclude = main.c

ruka.sources = manager, src
ruka.exclude = manager/main.c
```

---

# 10. Workspace root & output

Setiap project dibangun dengan folder project sebagai working directory.

Jadi:

```text
root = .
```

di level workspace **tidak menggabungkan output** ke workspace root dan akan diabaikan dengan warning.

Default output tetap berada di masing-masing project:

```text
rupa/bin/
rupa/lib/
rupa/build/

ruka/bin/
ruka/lib/
ruka/build/
```

Jika ingin agregasi ke workspace root, arahkan output secara eksplisit:

```text
app.output.binaryDir = ../bin
app.output.libDir = ../lib
mod.archive.dir = ../modules
```

---

# 11. Workspace dependency

```text
ruka.depends_on = rupamod
```

Ini menentukan ordering/dependency antar project.

Build satu project + dependency:

```bash
satria -w ruka
```

---

# 12. Library-first workspace

Workspace dapat melakukan build dalam dua tahap:

```text
1. library pass
2. binary pass
```

Konsepnya:

```text
compile sources
     ↓
build lib<name>.a / lib<name>.so
     ↓
semua library tersedia
     ↓
link binary
```

Cross-project library:

```text
ruka.library.linux = rupa
rupalib.library.linux = ruka
```

Dalam bentuk alias yang umum pada Buildfile workspace:

```text
rukaout ...
rukalib ...
```

Circular library references diperbolehkan dan diselesaikan melalui two-phase build.

---

# 13. Archive

Project archive dapat memakai:

```text
mod.output.binary = false
mod.archive.src = .
mod.archive.name = rupa_modules
mod.archive.with = tar, gz
mod.archive.exclude = build, dist, LICENSE, README.md, self_archive
mod.archive.dir = ../ruka/modules
```

`.git` dikecualikan dari source archive secara default.

Archive project berjalan pada library phase dan tidak perlu membuat binary.

---

# 14. Embedded archive

Deklarasi embedded:

```text
ruka.embedded.modules as rukamod

rukamod.file = ../rupamod/dist/rupa_modules.tar.gz
rukamod.variable = MODULES
rukamod.extract = /tmp/rupa-system
rukamod.pattern = .rp
```

Archive dapat menjadi input build project lain.

Generated embedded output (`embedded.c` + `embedded.h`) menyediakan API `getEmbed("key")` / `getEmbedAt(i)` dengan struct datar `Embedded` (key, type, name, extract, size, data); macro lama `EMBED_*` (archive, extraction directory, symbol, end symbol, symbol length) tetap tersedia untuk kompatibilitas.

---

# 15. Packaging

Namespace packaging:

```text
pack.name = satria
pack.version = 0.2.2
pack.output = dist/{name}-v{version}.tar.gz
pack.files = bin/satria:bin/satria, README.md:share/satria/README.md
```

Format mapping:

```text
source:path/in/package
```

Tanpa mapping, source path dipertahankan.

Format:

```text
pack.format = deb
```

Checksum:

```text
pack.checksum = sha256
```

---

# 16. Debian package

```text
pack.format = deb
pack.deb.install_prefix = /usr/local
pack.deb.maintainer = "Aidomx <aidomxdev@gmail.com>"
pack.deb.description = "A simple builder for you"
pack.deb.architecture = arm64
```

---

# 17. Package merge

Workspace dapat memasukkan output project lain ke package:

```text
rupa.pack.merge = ruka
```

`pack.merge` membuat hubungan build dependency otomatis terhadap output project yang dirujuk.

---

# 18. Release

Workspace memisahkan:

```text
projects = build units (banyak)
release  = distribution unit (TUNGGAL)
```

Deklarasi (key identik `pack.*` karena fase release memakai pack engine):

```text
release.name = rupa
release.version = 0.2.2
release.files = bin/rupa, ../bade/bin/bade, rupadocs
release.exclude = .git, .satria, build, node_modules
release.output = dist/{name}-v{version}.tar.gz
release.format = deb
release.checksum = sha256
```

Tanpa `release.files`: default = artefak build semua proyek
(`bin/<binaryName>` + `lib/lib<libraryName>.a/.so` per proyek).
`release.exclude` berlaku saat walk folder (path/basename/prefix dir).

Override run via CLI:

```bash
satria -w release -- version=0.2.3,format=tar
```

Default format release adalah `tar`.

---

# 19. Release output

Full workspace:

```bash
satria -w
```

→

```text
dist/
```

Satu paket terpusat di root workspace (`<name>-v<version>.tar.gz` /
`.deb` + `.sha256`).

---

# 20. Workspace pipeline

```text
library pass
    ↓
binary pass
    ↓
release pass
```

`satria -w` menjalankan seluruh pipeline untuk release yang dideklarasikan.

`satria -w NAME` membangun closure project target dan hanya merilis release yang dimiliki target tersebut.

---

# 21. Interactive init

```bash
satria init -p
```

Pertanyaan utama:

```text
Project name?        default: random satria-xxxxxx
language?            default: c
std?                 default: tidak ditulis
compiler?            default: auto-detect
flags?               default: tidak ditulis
Create include/?     default: N
Uses workspace?      default: N
```

Untuk C++:

```text
language = cpp
```

menghasilkan `src/main.cpp` dan menulis:

```text
std = c++17
```

Pada mode workspace, init juga menanyakan project dan dependency.

Enter menerima default. Ctrl+C/EOF membatalkan scaffold.

---

# 22. Pola project paling sederhana

```text
use project
```

Struktur:

```text
myapp/
├── Buildfile
├── src/
│   └── main.c
└── include/
```

Build:

```bash
satria
```

Binary default:

```text
bin/myapp
```

---

# 23. Project C eksplisit

```text
use project

compiler = gcc, clang
flags = Wall, Wextra, O2, MMD, MP
headers = include, I.
sources = src

output.binary = true
output.binaryName = app
output.binaryDir = bin
output.buildDir = build
output.compileCommands = auto
```

---

# 24. Project library

```text
use project

sources = src
headers = include
compiler = gcc, clang
flags = Wall, Wextra, O2, MMD, MP

output.binary = false
output.libraryName = core
output.libDir = lib
output.libraryShared = true
```

---

# 25. Mental model

```text
project
├── sources / headers
├── compiler / flags / std
├── binary
├── library
├── archive / embedded
└── pack

workspace
├── projects
│   ├── depends_on
│   ├── cross-project libraries
│   └── library → binary phases
└── release
    └── distribution unit (satu)
```

Secara konsep:

```text
project  = unit build
library  = build/link artifact
archive  = kumpulan file/module
embedded = archive/file menjadi input artifact
pack     = packaging
workspace = kumpulan project
release  = unit distribusi
```

---

# 26. Catatan versi

Sebagian halaman dokumentasi masih berbaseline v0.2.0/v0.2.2, sementara
release ini adalah v0.2.4. Untuk fitur yang ditambahkan setelah baseline
tersebut—workspace release, `init -p`, no-op cache (aktif default;
`SATRIA_CACHE_NOOP=0` menonaktifkan), validasi header semantik
(`SATRIA_HDR_SEM`), `headers` berskop workspace/proyek, dan dependensi
lintas-proyek `cdeps`—gunakan perilaku binary dan dokumen `design/` sebagai
acuan.

Ide yang masih dikembangkan **bukan konfigurasi resmi**; periksa `satria -h`
dan `design/` sebelum memakai syntax baru.

Jika konfigurasi lama masih diterima satria, kompatibilitas lama tetap
boleh digunakan; cheatsheet ini tidak mengubahnya menjadi syntax baru.

---

# 27. Environment variables

```text
SATRIA_CACHE_NOOP   no-op cache gate (default aktif; =0 menonaktifkan)
SATRIA_CACHE_TRACE  jejak keputusan cache (fast path / gate no-op)
SATRIA_HDR_SEM      validasi header semantik (default: workspace; 1=selalu, 0=nonaktif)
SATRIA_PROFILE      cetak penanda fase (ms) ke stderr
SATRIA_STATUS       cetak status gate/cold build
SATRIA_WS_PAR       =0 menjalankan anak workspace serial (default: penjadwal siap-dep)
SATRIA_NO_COLOR     matikan warna (juga NO_COLOR)
SATRIA_HOME         lokasi state global (default ~/.satria)
SATRIA_CMD          path cmd.txt untuk bantuan CLI
```

---

**Version:** satria v0.2.4
**Purpose:** quick reference, bukan spesifikasi lengkap Buildfile.

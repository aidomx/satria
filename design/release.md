# Rencana Release Workspace satria

Tanggal: 5 Oktober 2026
Revisi 2 (10 Oktober 2026): release = SATU paket workspace, bukan daftar
terikat proyek.

## 1. Tujuan

Menambahkan mekanisme "release" pada satria untuk menghasilkan artefak
distribusi siap pakai.

"release" menggunakan kemampuan packaging yang sudah dimiliki "pack",
tetapi bekerja pada scope yang lebih luas: seluruh workspace (mode -w)
atau seluruh proyek (mode satu-proyek).

### Workspace memiliki dua child utama:

```
workspace
├── projects
└── release        (tunggal — satu produk distribusi)
```

"projects" merepresentasikan project yang dapat dibangun.

"release" merepresentasikan produk tunggal yang akan didistribusikan:
hasil build seluruh project + file/folder tambahan apa pun dari root
workspace, dikemas menjadi SATU paket di dist/.

---

## 2. Model

Revisi 1 memodelkan release sebagai daftar unit distribusi yang masing-
masing terikat satu project (releases = a, b; releases.a.name = a).
Model itu kaku:

- name wajib nama project — paket lintas-project tidak mungkin;
- file yang dikemas terkurung pack.files milik satu project;
- workspace yang menghasilkan SATU produk (mis. distribution tarball
  berisi bin/, lib/, share/) harus mendeklarasikan release palsu.

Revisi 2 membalik modelnya:

- release TUNGGAL di level workspace (section release.* di Buildfile.ws)
  dan juga tersedia di mode satu-proyek (release.* di Buildfile);
- isi paket bebas: release.files bisa menunjuk file/folder proyek mana
  pun (prefix ../<n>/...), artefak build, sumber, atau dokumen;
- release.exclude mengeluarkan path/pola dari folder yang di-walk;
- semua fitur pack ikut: name, version, output, format (tar|deb),
  compress, optimize, checksum, deb.*;
- pack.* project TETAP berjalan seperti biasa (kemasan per-proyek ke
  dist/ project sendiri) — tidak diubah, tidakcampuradukkan.

---

## 3. Konfigurasi

Section release.* di Buildfile.ws (workspace) maupun Buildfile (proyek
tunggal) — key-nya sama dengan pack.*:

```yaml
release.name = rupa                    # nama produk (default: folder cwd)
release.version = 0.2.2                # default 0.0.0
release.files = bin/rupa, bade/bin/bade, share/rupa/cmd.txt
release.exclude = build, .git, dist    # diabaikan saat walk folder
release.output = dist/rupa-v{version}-{os}-{arch}.tar.gz
release.format = deb                   # tar (default) atau deb
release.compress = gzip
release.optimize = true                  # kompresor level ter-FASTEST (gzip -1); default false
release.checksum = sha256
release.deb.maintainer = "Nama <email>"
release.deb.description = ...
release.deb.install_prefix = /usr/local
release.deb.architecture = amd64
```

Aturan penting:

- TANPA release.files, default isi = artefak build semua project
  workspace: bin/<binaryName> tiap proyek binary + lib/<libraryName>.a
  dan .so tiap proyek library (proyek archive/kemasan tidak menyumbang
  file). Di mode satu-proyek default = output binary + library proyek.
- path release.files tanpa prefix ../ dihitung relatif root workspace
  (mode -w) atau root proyek (mode tunggal); prefix ../<folder>/...
  menunjuk folder proyek lain.
- release.exclude berlaku pada entri release.files bentuk FOLDER yang
  di-walk rekursif (suffix path, pencocokan segmen); tidak berlaku pada
  entri file eksplisit.
- release.output default: dist/{name}-v{version}.tar.gz — selalu
  dihitung dari root workspace/root proyek, BUKAN folder project.
- release.optimize = true: kompresor memakai level ter-FASTEST —
  gzip -1 / xz -1 / bzip2 -1 lewat dua langkah (tar flat dulu, lalu
  compressor). Arsip lebih besar (user yang memilih), biaya kompresi
  jauh lebih murah. Default false: perilaku satu langkah tar czf/cJf/cjf
  TIDAK berubah. Memutar nilai false↔true tidak membatalkan no-op —
  hapus artefak (atau touch input) untuk rebuild. Catatan run asal:
  `tarDetFlags` membuat isi tar deterministik (mtime/owner tetap), jadi
  rebuild yang berakhir identik menghasilkan arsip byte-sama. Cakupan:
  hanya tarball — staging internal .deb tetap `tar czf`
  (lihat design/packaging.md §Deb).
- release tanpa release.* sama sekali TIDAK dijalankan (back-compat:
  workspace tanpa section release berperilaku seperti sebelumnya).

---

## 4. CLI

```bash
satria -w                       # build + release (bila release.* ada)
satria -w clean                 # bersih, tidak ada release
satria -w <proyek>              # build closure; release tetap workspace penuh
satria --release                # paksa release meski build selektif
satria release                  # mode tunggal: build + release proyek
satria release -- name=x        # OVERRIDDEN name untuk run ini
```

Selektor `name=<project>` revisi 1 DIHAPUS — tidak ada lagi daftar
release. `--` di mode workspace hanya menerima key=value yang menimpa
setting release (name, version, target->format, ...).

---

## 5. Output

```
dist/
├── rupa-v0.2.2.tar.gz
├── rupa-v0.2.2.tar.gz.sha256
└── (atau .deb bila release.format = deb)
```

Satu folder output terpusat: dist/ di root workspace (mode -w) atau
root proyek (mode tunggal). project/dist/ (pack per-proyek) tidak
disentuh release.

---

## 6. Pipeline

```
workspace
    ↓
projects (build dua fase: library pass -> binary pass)
    ↓
release (pack sekali pada root, isi semua project)
    ↓
dist/
```

pack per-proyek tetap iont di fase binary masing-masing project,
seperti saat ini.

---

## 7. Prinsip Desain

### 7.1 Project dan release adalah dua konsep berbeda

projects = build units (bisa banyak)
release = distribution unit (TUNGGAL)

### 7.2 Release = pack dengan scope lebih luas

Tidak ada engine kedua. release.* dijalankan oleh pack engine yang sama; Buildfile sintesis fase release hanyalah Buildfile project + override section release.

### 7.3 Tidak semua project menyumbang file

Project dependency boleh menyumbang file via release.files eksplisit (mis. ../compiler/bin/cc) — tidak ada paksaan.

### 7.4 Back-compat

- Tanpa section release.*: perilaku lama utuh.
- pack.* project: tetap berjalan dan output ke project/dist/.
- Sintaks revisi 1 (releases = ..., releases.<n>.name) DIHAPUS; parser workspace menolaknya dengan pesan migrasi yang jelas.

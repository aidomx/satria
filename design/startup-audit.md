# Audit `design/startup.md` vs implementasi

Tanggal: 2026-10-20. Diperbarui setelah perbaikan clean (caches/* dan ld.cache dipertahankan) dan penyesuaian desain. Sumber: `satria-v0_2_4_opts_tar.gz`. Build bersih (0 error, 0 warning).

## Ringkasan

Sebagian besar klaim `[x]` di Daftar Implementasi terbukti. Ada dua hal yang
tertinggal di checklist padahal sudah ada di kode, satu deviasi dari §6, dan satu
persyaratan Windows yang belum ada sama sekali.

## Yang terbukti (sesuai klaim)

| Klaim | Bukti |
|---|---|
| Startup: cold banner, no-op diam, Buildfile berubah diam, clean menyisakan `startup.state`, `rm -rf .satria` memulai cold | Uji TTY workspace |
| `sources.cache` format `SATRIASRC2`, self-heal dari `SATRIASRC1` | Uji header sebelum & sesudah |
| Route `sources` di `opts.cache` = `st_mtime_ns` `sources.cache` | Perbandingan nilai persis |
| Sinkronisasi dua arah | `opts_sources_sync.sh` 26/26 |
| Cache no-op | `cache_noop_miniws.sh` 24/24 |
| Router | `router_noop_miniws.sh` 12/12 |
| Benchmark `bench/ws.sh` (cache off & on) | satria 12/12, pembanding 11/11 |
| `paths.cache` dihapus saat save sukses | `ws_cache_save.c` + uji build |
| Build log (`log = true`, `satria log`) | `build_log` diuji dengan target yang ada |
| `report.blob` diputar ulang pada no-op (§10) | 69/69 baris laporan muncul di output |
| `artifact.cache` (§9) dan validasi release | Lihat di bawah |

### Release (§9) terbukti penuh

Fixture release (internal, hanya untuk uji): cold → no-op memakai cache dengan release aktif;
artefak `.deb` diubah → no-op batal dan fase release berjalan lagi; file input
`core/src/prompt/cmd.txt` diubah → no-op batal dan release berjalan lagi; file dipulihkan → cache valid lagi.

## Checklist yang tidak sinkron dengan kode

- **Gerbang release no-op** masih tertulis `[ ]`, padahal gerbang lama sudah dihapus dan
  validasi release lewat `artifact.cache` berjalan (terbukti di atas). Ubah ke `[x]`.
- **`artifact.cache` (§9) dan `report.blob` (§10)** tidak ada di checklist. Keduanya ada dan
  bekerja. Tambahkan sebagai `[x]`.
- **Touch** masih tertulis `[ ]`. Dengan cache aktif (default), touch tanpa perubahan isi tidak
  memicu build (`cache valid=1`). Hanya jalur incremental lama (cache dimatikan) yang tetap
  mengompilasi ulang. Pisahkan item ini: default `[x]`, jalur lama `[ ]`.
- **README `tests/miniws`** masih menulis bahwa touch memicu recompile. Perbarui sesuai perilaku
  di atas.
- **`paths.cache` "tetap sementara"** di §4 sudah tidak akurat: file itu dihapus saat save
  sukses. Ubah kalimatnya.

## Deviasi dari desain

1. **Rename Windows (PENTING).** §4 (catatan PATCH) mewajibkan `MoveFileEx` dengan
   `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` atau fallback copy+delete. Ada 14
   pemanggilan `rename()` di `src/`, dan tidak ada pembungkus Windows. Repo punya `install.ps1`,
   jadi Windows adalah target yang didukung. Tidak tercatat di checklist.
2. **Clean menghapus `caches/deps.cache`** (`cmds_state.c`, blok `depsLeaf`). **Diselesaikan:** file
   dipertahankan dan isinya dikosongkan, sesuai keputusan. Patch: `clean-keep-caches.patch`.
3. **Format `opts.cache` pada §4** ditulis `<OP> <alamat_utama> <alamat_caches>` (3 kolom).
   Baris nyata: `PUT <proyek> <file> <content> <time> <size> <payload>` (6 kolom + OP). Contoh
   di dokumen sudah diberi catatan "ilustrasi", tapi bentuk kolomnya perlu disamakan.
4. **§9 `artifact.cache`** ditulis "metadata dari archive yang tidak punya kode c". Implementasi
   juga mencatat output release dan input pack. Perluas kalimat §9.

## Belum selesai (benar terbuka di checklist)

- Audit penghapusan state lama.

## Diselesaikan setelah audit (2026-10-20)

- Rename `wsWalkDirCached` → `stateWalkDirCached` (dan `wsWalkDirStamps` →
  `stateWalkDirStamps`); keduanya dipakai juga oleh single-project.
- Metadata `P <alamat> <content> <time> <size>` + optimasi no-op (dua lapis):
  peta P di `sources.cache` bermetadata -> fast path no-op tanpa membuka
  fingerprint.cache (sentuh -> PATCH + segarkan P + sync route); metadata grup P
  di `fingerprint.cache`/`opts.cache` (stamp konfigurasi + daftar sumber per
  proyek) -> gate jalur penuh menolak perubahan per proyek sebelum membuka
  entri. Kompatibel mundur. `p_meta_noop.sh` 32/32.
- Flag `log` single-project kini dipulihkan dari `startup.state` (`cfgCacheLoad`); pembaca
  `Buildfile` langsung `buildLogEnabled` dihapus dari `src/log/build_log.c`. Terverifikasi: cold
  `log=true` membuat `build.log`, `log=false` berhenti, cache hit tanpa membuka Buildfile tetap
  mengaktifkan relay.
- CRUD per-source (opsi minimal: create/update sentuh-PATCH/delete) di kedua mode;
  `crud_touch_delete.sh` 19/19.

## Kebersihan repo (tidak mengubah perilaku)

- `startup.md` dan `19_10_2026_startup.md` ada di root arsip, bukan di `problems/`. Duplikasi
  dokumen; tentukan satu sumber.
- File `*.orig` tertinggal di `src/` (contoh `src/cmds/cmds_wsstate.c.orig`).
- `tests/unit/build_log_miniws.sh` dahulu gagal (rc=1, tanpa pesan karena `set -eu`) karena menulis
  `#warning` ke `a/src/main.c`, padahal `a` tidak ada di `projects` `Buildfile.ws` fixture,
  sehingga warning tidak pernah dikompilasi. **Sudah diperbaiki** (target `core/src/core.c`);
  skrip lulus penuh.
- `tests/unit/test_state_blob.c` tidak bisa dikompilasi dengan perintah standalone karena
  `state_blob.c` memanggil `fsMakeDir` dari util. Build satria tidak terpengaruh.
- `bench/ws.sh` memakai bash dan tool `cninja` (bukan `ninja`/`make`).

## Tidak diuji dalam audit ini

- Benchmark skala besar (`scale*.sh`), `compare.sh`, `graph.sh`, `jobs.sh`.
- Build Windows (rename dan `install.ps1`).
- Single-project no-op dengan cache (hanya cold + no-op legacy).
- Collision hash `sources.cache` (hanya dicek lewat unit, bukan dengan tabrakan nyata).

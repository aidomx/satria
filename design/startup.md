# Satria Startup Design

> Persistent Workspace Configuration for Fast and Reliable Builds  
> **Satria v0.2.4+ — Rancangan startup**

## 1. Konsep Utama

### Mengapa startup?

Menggabungkan **mtime**, **content hash**, dan **fingerprint** memberi tingkat akurasi yang efektif untuk mendeteksi perubahan. Namun, pada skala besar (ratusan proyek), jumlah file dalam `.satria` bisa bertambah banyak dan berpotensi menjadi bottleneck.

### Solusi: Persistent Startup

Seperti CMake yang menghasilkan konfigurasi siap pakai, Satria menerjemahkan `Buildfile` atau `Buildfile.ws` menjadi konfigurasi persisten di `.satria` tanpa membutuhkan perintah konfigurasi terpisah.

```sh
satria -w
```

Untuk membersihkan hasil build tanpa menghapus konfigurasi startup:

```sh
satria -w clean
```

**Manfaat:**

- Startup penuh hanya dijalankan saat diperlukan.
- Clean tidak menghapus konfigurasi startup.
- Mendukung workspace skala besar.
- Mengurangi overhead pada no-op dan incremental.

## 2. Alur Kerja Startup

```text
satria -w
    │
    ▼
bootstrapParse()        Parse CLI arguments
    │
    ▼
bootstrapApply()        Setup environment
    │
    ▼
startUp()              Cek .satria/startup.state
    │
    ├── Ada & cocok ───────► Pulihkan model workspace dari startup.state (diam)
    │
    ├── Ada, Buildfile ────► Parse Buildfile.ws, perbarui startup.state (diam)
    │   berubah
    │
    └── Tidak ada ─────────► Startup penuh (clean cold):
                             - siapkan skeleton direktori (sekali)
                             - parse, resolve, tulis startup.state
                             - cetak fase startup (tetap terlihat)
    │
    ▼
workspaceRun()         Validate build state & execute
```

**Aturan:**

- Startup penuh hanya terjadi saat `startup.state` belum ada: clean pertama, atau `rm -rf .satria`.
- Run no-op tidak mencetak output startup dan tidak membuat direktori.
- Buildfile.ws yang berubah cukup memperbarui `startup.state`; startup penuh tidak dijalankan ulang.
- `satria -w clean` mendahulukan pengecekan `.satria` sebelum startup apa pun (lihat §6). Tanpa `.satria`, startup tidak dijalankan sama sekali. Dengan `.satria`, `startup.state` disinkronkan lalu clean berjalan.

> **Status (diverifikasi 2026-10-20):** alur di atas berjalan untuk cold, no-op, perubahan Buildfile.ws, dan clean, pada mode workspace dan single-project (cold dan no-op single terverifikasi di TTY). CRUD per-source dan flag `log` single-project kini selesai (lihat *Daftar Implementasi*). Yang **belum** selesai: penulisan atomik untuk Windows (lihat *Daftar Implementasi*).

## 3. Struktur `.satria`

```text
.satria/
├── artifact.cache
├── startup.state
├── sources.cache
├── opts.cache
├── caches/
│   ├── deps.cache
│   ├── embed.cache
│   ├── fingerprint.cache
│   └── lib.cache
├── compile_commands.blob
├── compile_commands.cache
├── ld.cache
├── report.blob
└── build.log
```

| File | Fungsi |
|---|---|
| `artifact.cache` | Cache release. |
| `startup.state` | Konfigurasi lengkap hasil penerjemahan Buildfile / Buildfile.ws, termasuk alamat sumber terkait. |
| `sources.cache` | Daftar seluruh sumber terkait workspace, menggantikan `ws.src`. |
| `opts.cache` | Router cache: indeks semua entri (alamat utama, file cache, proyek, content, time, size). No-op membaca ini lebih dulu. |
| `caches/deps.cache` | Cache dependency. |
| `caches/embed.cache` | Cache embed. |
| `caches/fingerprint.cache` | Cache fingerprint. |
| `caches/lib.cache` | Cache library. |
| `compile_commands.blob` | Data kompilasi dalam bentuk blob biner. |
| `compile_commands.cache` | Cache untuk `compile_commands.json`. |
| `ld.cache` | Verdict deteksi GNU `ld`: path, mtime ns, dan ukuran binary `ld` yang di-resolve, beserta hasil probe (GNU atau bukan). Divalidasi ulang setiap run. |
| `report.blob` | Pesan no-op. |
| `build.log` | Log progres build. |

> **Jumlah file tetap:** `caches/` hanya berisi empat file (`deps`, `embed`, `fingerprint`, `lib`), dan tidak ada file yang dibuat per proyek. Setiap file memuat baris untuk seluruh proyek, sehingga jumlah file di `.satria/` sama berapa pun jumlah proyeknya.

> **Catatan:** `compile_commands.blob` dan `compile_commands.cache` tidak dibuat oleh startup. Keduanya dibuat `compdb` saat dibutuhkan, supaya startup tetap ringan.

### build.log

`log = false` adalah default global.

> **Status: sebagian diimplementasikan.**
> - Workspace: `log = true|false` dipulihkan dari `startup.state` (`commonLines`) dan dipakai untuk memutuskan relay. Terverifikasi: `log = true` + warning sungguhan membuat `.satria/build.log`, dan `satria --log` menampilkannya.
> - Single-project: `log = true|false` dipulihkan dari `startup.state` lewat cache konfigurasi (`cfgCacheLoad`, validasi mtime+size Buildfile; Buildfile tidak dibuka pada cache hit). Pembacaan `Buildfile` langsung (`buildLogEnabled`) sudah dihapus. Terverifikasi: cold `log=true` membuat `build.log`, `log=false` berhenti menulis, dan recompile tanpa mengubah Buildfile tetap ter-log (flag dari cache).
> - `build.log` hanya dibuat saat ada baris diagnostik (warning, error, failed). Build sukses tanpa diagnostik tidak menghasilkan file. Ini perilaku yang diharapkan, dan sekarang tertulis di sini.

Berfungsi untuk menyimpan informasi seperti warning, error, dan Kegagalan dari proses build.

Dan tetap bisa diakses dengan :
```bash
satria log
# atau
satria --log
```

## 4. Isi File Startup

### `startup.state`

**Header & Version**

```text
Magic          : SATRIA-STARTUP-1
Buildfile Hash : 64-bit FNV-1a
Buildfile Path : Buildfile.ws
Timestamp      : ...
```

**Workspace Configuration**

- Project count
- Release configuration
- Common lines
- Release lines

**Project Data**

- Project name & root
- Project type
- Project configuration
- Dependency graph
- Library dependencies
- Topological order

### `sources.cache`

Daftar seluruh sumber workspace. Tempat resmi untuk memetakan alamat ke path.

```text
<alamat> <type> <size> <mtime> <path>
```

- `<alamat>` = hash FNV-1a 64 dari `<path>`, ditulis di file. Stabil antar run dan tidak bergantung urutan sumber.
- Pointer tidak ditulis ke disk. Saat `sources.cache` dibaca, pointer dibentuk di memori dari `<alamat>`.
- `<path>` disimpan lengkap. Dua path berbeda dengan `<alamat>` yang sama (tabrakan hash) dideteksi saat dibaca, dan kedua entrinya ditandai invalid sehingga dihitung ulang.
- `<type>`: `S` (file sumber) atau `D` (direktori).
- `<size>`, `<mtime>` dipakai fast path validasi (lihat `opts.cache`).

> **Status implementasi: format baru aktif (`SATRIASRC2`).**
> - Build menulis dan membaca format `<alamat> <type> <size> <mtime> <path>` di atas. Header file `SATRIASRC2 <cwd>`; record `R <folder> <ext>` tetap dipakai untuk indeks discovery. File format lama `SATRIASRC1` ditolak saat dibaca → di-walk ulang dan ditulis ulang sebagai `SATRIASRC2` (self-heal), tanpa migrasi manual.
> - `<alamat>` diverifikasi terhadap `csHash(path)` saat dibaca. Tabrakan hash (alamat sama, path beda) membuat kedua entri invalid → dihitung ulang.
> - Tabel alamat -> path untuk cache no-op sekarang ada di `sources.cache` (satu sumber kebenaran). `caches/paths.cache` sudah tidak ditulis: file itu dihapus saat save build sukses.
> - Cache no-op aktif secara default; nonaktifkan dengan `SATRIA_CACHE_NOOP=0`. Terverifikasi: `cache_noop_miniws.sh` 24/24, `router_noop_miniws.sh` 12/12, `opts_sources_sync.sh` 26/26, `cache_store` unit 5096 pengecekan.
> - **Baris P kini bermetadata** (2026-10-20): `P <alamat> <tipe> <content> <time> <size> <path>` — inilah basis **fast path no-op**: semua baris P + stat cocok (non-racy) -> no-op sah TANPA membuka `fingerprint.cache` (satu baca sources.cache + N stat; trace `P-fast hit` via `SATRIA_CACHE_TRACE`). Sentuh -> fast path menolak; jalur penuh memvalidasi (hash isi), PATCH fingerprint, menyegarkan baris P + menyinkronkan route sources di opts.cache; run berikutnya kembali P-fast. Format lama tanpa metadata: fast path ditolak (jalur penuh menilai), di-upgrade otomatis pada no-op valid berikutnya. Terverifikasi: `p_meta_noop.sh` 32/32; 30 run pada workspace uji: P-fast 2.68s vs jalur penuh 5.24s per no-op; fingerprint.cache 42KB tidak dibaca sama sekali pada fast path.

### `caches/*`

> Empat file tetap. Baris `P <alamat_proyek>` mengelompokkan file milik satu proyek dan memuat metadata grup (lihat butir metadata di bawah). Setiap entri membawa alamat lengkap (proyek dan file) agar bisa langsung dicari:

```text
P 12345
<OP> <alamat_proyek> <alamat_file> <content> <time> <size>
...
```

Contoh:

```text
P 12345                       # alamat_proyek (hash FNV-1a 64; angka dipendekkan untuk contoh)
GET 12345 678910 91b151cd599dd53a 1792314999281163016 1034
PATCH 12345 678910 91b151cd599dd53a 1792314999290000000 1034
```

- `<alamat_proyek>` dan `<alamat_file>` adalah hash FNV-1a 64 (proyek dan path file). `<alamat_file>` sama dengan alamat di `sources.cache`.
- **Metadata grup `P <alamat_proyek> <content> <time> <size>`** (diimplementasikan 2026-10-20, `csProjMetaSet`): `content` = stamp proyek (hash konfigurasi Buildfile.ws + daftar sumber kanonik, `wsProjStamp`); `time` = stempel ns save; `size` = jumlah entri grup. Grup workspace (`fingerprint`/`workspace`) memakai hash Buildfile.ws dari entri `W`. Ditulis di `fingerprint.cache` DAN `opts.cache` saat save. Gate no-op jalur penuh membandingkan stamp dengan model pulih: konfigurasi/daftar sumber satu proyek berubah -> miss SEBELUM membuka seluruh entri, jejak (`SATRIA_CACHE_TRACE`) menyebut nama proyeknya. Grup tanpa metadata (cache versi lama) dilewati — kompatibel mundur; simpan berikutnya menulis stamp baru. Terverifikasi: `p_meta_noop.sh` 32/32.
- Saat dibaca, setiap entri dimasukkan ke indeks hash di memori dengan kunci `(alamat_proyek, alamat_file)`. Pencarian menjadi lookup langsung, tanpa menelusuri ulang file.
- Nama file menentukan jenis cache (`deps`, `embed`, `fingerprint`, `lib`).
- `PATCH` pada file teks berarti menulis ulang file. Perubahan dikumpulkan selama run, lalu setiap file ditulis sekali secara atomik (tmp lalu rename).

**Penulisan atomik (rename)**
Di POSIX, rename() adalah atomic. Tapi di Windows:
Jika file tujuan sedang dibuka (misalnya oleh antivirus atau indexer),rename() akan gagal dengan `ERROR_ACCESS_DENIED`.
Solusi: Gunakan MoveFileEx() dengan flag `MOVEFILE_REPLACE_EXISTING` | `MOVEFILE_WRITE_THROUGH`, atau fallback ke copy + delete.

> **Status: belum diimplementasikan.** Kode memakai `rename()` di 14 tempat tanpa pembungkus Windows. Target Windows (`install.ps1`) perlu dijadikan item tersendiri.

**Contoh alur `core/src/bootstrap/loader.c`:**

| Kondisi | Operasi |
|---|---|
| `GET` cocok (time dan content sama) | Tidak ada yang ditulis. |
| `time` beda, `content` sama (mis. `touch`) | `PATCH` memperbarui `time` saja. Tidak ada rebuild. |
| `content` beda (mis. edit) | `PATCH` memperbarui `content`, `time`, `size`. Entri ditandai berubah, dan proyek yang bergantung padanya dibangun ulang lewat `deps.cache`. |

### `opts.cache`

> **Status: tersambung ke `sources.cache` (dua arah); CRUD per-source selesai (opsi minimal).** `opts.cache` adalah *router* cache. Yang sudah berjalan: operasi create/read/update/delete (`opts_create/read/update/delete.c`) untuk entri per proyek dan route workspace (`csHash("workspace")` -> `fingerprint`); no-op membaca route ini lebih dulu. **Sambungan dua arah:**
> - **opts -> sources:** setiap kunci (workspace + per proyek) menyimpan route `sources` dengan stempel `mtime` **nanodetik** + ukuran `sources.cache`. Validasi (`optsRouterValidate`/`wsRouterValidate`) mewajibkan route ada dan cocok selama file indeks ada; route menunjuk file hilang juga menolak. Route opsional hanya untuk proyek yang belum dirutekan / tidak punya indeks.
> - **sources -> opts:** `optsSourcesRouteSync()` dipanggil di akhir setiap run workspace yang SUKSES dan menstempel ulang SEMUA route `sources` (kunci workspace + semua proyek) ke berkas terkini — perlu karena re-walk `srcSave` dan merge `P` (`wsCacheSave`) menulis ulang `sources.cache` SETELAH route proyek tercatat. Run gagal sengaja tidak disinkronkan: route basi menolak no-op sampai build berhasil.
> - **Belum:** — CRUD per-source melalui router sudah berjalan (lihat Daftar Implementasi).
> - **Route `fingerprint` di single-project** (2026-10-22): single-project merekam route `fingerprint` (hash nama `fingerprint`) seperti workspace, sehingga snapshot fast state dan build fingerprint di `fingerprint.cache` ikut divalidasi lewat `opts.cache`. No-op single: `opts.cache` -> `fingerprint.cache`/`deps.cache` sesuai route -> keluar. Route basi (file hilang) menolak no-op. Terverifikasi: `opts_sources_sync` 26/26, `crud_touch_delete` 21/21, `p_meta_noop` 38/38, dan kasus file hilang/edit/binary/clean.
> - **Metadata grup P di kedua jalur** (2026-10-20): workspace menulis stamp via `wsOptsMetaSave` (save); single-project via `optsProjMetaStamp` setelah `optsRouterRecord` — content = hash isi Buildfile + daftar sumber kanonik (`fastProjStamp`), format identik dengan workspace. Validasi no-op single tetap FAST5 (deteksi konfigurasi/sumber sudah melekat di stamp FAST5); metadata menyamakan format store + observabilitas.
> - Terverifikasi: `opts_sources_sync.sh` 26/26 (kedua mode: touch, hapus, rebuild, korup same-second+same-size), route stamp terbukti = `st_mtime_ns` persis, dua route proyek ter-PATCH oleh sync pascamenulis.

**Format (satu entri per baris):**

```text
<OP> <alamat_utama> <kunci_tujuan> <content> <stempel> <ukuran> <tujuan>
```

- `<alamat_utama>`: hash FNV-1a 64 dari kunci route (`workspace` atau nama proyek).
- `<kunci_tujuan>`: hash FNV-1a 64 dari nama tujuan (`sources`, `fingerprint`, `deps`, `lib`, `embed`).
- `<content>`: `0` untuk route; `<stempel>` dan `<ukuran>` mengikuti `sources.cache` (nanodetik + ukuran byte).
- `<tujuan>`: nama tujuan dalam bentuk teks.

Contoh (nilai nyata):

```text
P 0bf2fb911980759e
PUT 0bf2fb911980759e 124a66191daad26c 0000000000000000 0 0 lib
PATCH 0bf2fb911980759e 6fc4d9b9c935cff1 0000000000000000 1792476989969036354 1605 sources
```

`opts.cache` hanya merutekan: untuk setiap entri, file `caches/` mana yang memuatnya. Detail validasi (`content`, `time`, `size`) ada di file `caches/` itu, bukan di `opts.cache`, sehingga `opts.cache` tetap kecil.

**Operasi (CRUD):**

| Operasi | Arti |
|---|---|
| `GET` | Baca dan validasi entri (dipakai no-op). |
| `PUT` | Buat atau ganti seluruh entri (mis. setelah build sukses). |
| `PATCH` | Ubah sebagian kolom saja (mis. `touch`: hanya `time` diperbarui). |
| `DELETE` | Hapus entri (file sumber hilang, atau proyek dilepas dari workspace). |

| Kolom | Arti (usulan) |
|---|---|
| `<OP>` | Salah satu operasi di atas. |
| `alamat_utama` | Nama proyek, mis. `core`. |
| `alamat_caches` | File `caches/` yang memuat detail entri, mis. `.satria/caches/fingerprint.cache`. |

**Validasi entri (urutan murah ke mahal), dilakukan terhadap baris di `caches/*` yang ditunjuk `opts.cache`:**

1. File sumber (`alamat_file`) tidak ada → invalid (file dihapus).
2. `size` dan `time` cocok → valid tanpa membaca isi file (fast path).
3. `size` atau `time` beda → hitung `content`:
   - hash sama → valid (mis. `touch`); perbarui `time` di `caches/*`, tanpa rebuild.
   - hash beda → invalid (mis. edit); rebuild unit yang bergantung lewat `alamat_caches`.

**Aturan no-op:** bila setiap entri valid dan semua output ada, run langsung keluar — tidak membuka `caches/` lain dan tidak mengulang startup.

**Edit, touch, dan header dicatat lewat `caches/*`, dengan `opts.cache` sebagai router:**

- `touch`: `time` berubah, `content` sama → `PATCH` `time` di `caches/*`, tidak ada rebuild.
- `edit` sumber: `content` berubah → `PATCH` entri, entri ditandai berubah → rebuild proyek pemilik.
- `edit` header: entri header berubah → proyek yang bergantung pada header ikut dibangun ulang (ketergantungan disimpan di `caches/deps.cache`, perlu dikonfirmasi).

**Pengaman racy timestamp:** bila `time` sebuah entri tidak lebih lama dari waktu file `caches/*` terakhir ditulis, `size`+`time` tidak dipercaya. Entri itu wajib dihitung ulang `content`. Ini mencegah edit dalam detik yang sama lolos dari fast path.

**Tidak ada file per proyek.** Semua data per proyek ada di empat file tetap di `caches/`. Tidak ada `<proyek>.*` lagi di `workspace/` atau `.satria/`, sehingga jumlah file tidak tumbuh walaupun proyeknya banyak.

**Usulan:**

- `alamat_utama` adalah nama proyek, cocok dengan `P <nama>` di `fingerprint.cache`.
- Ketergantungan header disimpan di `caches/deps.cache`, karena file itu adalah cache dependency.

## 5. Integrasi dengan Pipeline

```text
Source                    Startup                    Build State                   Output
──────────────────────────────────────────────────────────────────────────────────────
Buildfile.ws      ───►    .satria/            ───►    .satria/caches/      ───►     build/
Source files              ├─ startup.state            ├─ deps.cache                 bin/
Dependencies              ├─ sources.cache            ├─ embed.cache                lib/
                          └─ opts.cache               ├─ fingerprint.cache
                                                     └─ lib.cache
```

## 6. Perilaku Clean

| Dikosongkan (isi saja, folder tetap ada) | Dipertahankan |
|---|---|
| `build/` (termasuk sisa proyek yang sudah dilepas) | `.satria/` (folder) |
| `bin/` | `.satria/startup.state` (disinkronkan, tidak dihapus) |
| `lib/` | `.satria/sources.cache` |
| `modules/` (arsip embed hasil generate) | `.satria/opts.cache` (router; isi entri dipertahankan) |
| `caches/*`: file tetap ada, isinya dikosongkan (`deps`, `embed`, `fingerprint`, `lib`, `state`) | `caches/*` (file tetap ada) |
| | `ld.cache` (file dan isi dipertahankan; lihat catatan di bawah) |
| | `dist/` (artefak final) |

| Dihapus |
|---|
| `compile_commands.json` |
| `.satria/pack` |
| Record state per proyek di `caches/state.cache` (`ws.state`, `src`; workspace saja) |

**Prinsip:**

- Clean mengosongkan isi folder output dan isi `caches/*`. File cache tidak dihapus, sehingga jumlah file tetap.
- `ld.cache` dipertahankan (file dan isinya). Keamanannya: validasi memeriksa path, mtime ns, dan ukuran binary `ld` yang sedang di-resolve. Verdict basi, atau cache dari mesin lain, otomatis tidak cocok dan diprobe ulang sekali, lalu ditulis ulang. Terverifikasi: clean menyisakan file dengan isi identik; build berikutnya tidak menulis ulang cache; `ld` yang berbeda di PATH membuat cache ditulis ulang dengan path baru.
- Clean tidak menghapus `startup.state` atau `sources.cache`. Sebelum membersihkan, clean menyinkronkan `startup.state` dengan Buildfile.ws: bila Buildfile.ws berubah sejak state ditulis, state diperbarui di tempat; bila tidak, state tidak ditulis ulang.
- Bila `startup.state` belum ada, clean membuatnya lewat startup penuh (cold).
- Urutan clean mendahului startup: pertama cek `.satria`. Bila `.satria` tidak ada, clean hanya mengosongkan isi `bin/`, `build/`, `lib/`, dan `modules/`; startup tidak dijalankan dan `.satria` tidak dibuat.
- Pesan `> Nothing to clean` dicetak sekali di akhir, dan hanya bila tidak ada yang terhapus. Tidak dicetak per proyek.

## 7. Skala dan Performa

| Aspek | Tanpa Startup (v0.2.4) | Dengan Startup (rancangan v0.2.4+) | Keterangan |
|---|---|---|---|
| Cold build | Parse + resolve setiap kali | Parse + resolve saat pertama kali | Saat pertama atau konfigurasi berubah |
| No-op | Cek semua file | Cek dengan cache | Jauh lebih cepat pada workspace besar (target desain) |
| Incremental | Cek dependency & fingerprint | Cek dengan cache | Lebih sedikit traversal dan I/O (target desain) |
| Clean | Kosongkan isi folder output; hapus cache hasil build | Sinkronkan `startup.state` dengan Buildfile.ws | Konfigurasi tetap tersedia dan cocok dengan Buildfile.ws |
| Skala besar | Banyak file di `.satria` | Struktur cache lebih terorganisasi | Menghindari bottleneck file system (tujuan desain) |

> Perbaikan performa di atas merupakan **hipotesis/target**, bukan hasil benchmark terverifikasi.

## 8. Startup Output

Output startup berbentuk blok dengan header `startup :`, lalu satu baris per fase yang selesai. Baris tetap terlihat setelah cold build selesai:

```text
startup :
> discovery: done
> resolving dependencies: done
> indexing sources: done
> preparing execution: done
```

- Hanya muncul saat cold startup (`startup.state` belum ada). No-op dan perubahan Buildfile.ws tidak mencetak apa pun.
- Fase dicetak saat selesai, bukan saat mulai, supaya pesan dari parser (mis. peringatan Buildfile) tidak menimpa baris progres. Kalau ada pesan parser di tengah, ia muncul di antara baris fase.
- Kegagalan dicetak sebagai `> <fase>: failed`.
- Hanya ke terminal interaktif. Output yang di-capture (CI, pipe) tetap bersih.

## 9. Artifact

`.satria/artifact.cache` mencatat metadata input dan output release: arsip modul, `pack.files`, `bin/*`, dan artefak `dist/*` beserta checksum. Validasi no-op memeriksa file ini. Bila artefak atau input release berubah, no-op batal dan fase release berjalan lagi, sehingga cache no-op tetap berlaku saat release aktif.

> **Status: diimplementasikan.** Terverifikasi: no-op dengan release aktif memakai cache; artefak `.deb` yang diubah dan file input pack yang diubah masing-masing membatalkan no-op dan memicu fase release; memulihkan file mengembalikan cache menjadi valid.

## 10. Report build

`.satria/report.blob` menyimpan keluaran build sukses terakhir.

Format untuk `.satria/report.blob` tidak menelan mentah-mentah hasil keluaran dari build terakhir, untuk format replay seperti berikut :

```text
STATUS
  > Nothing to build
  > Cache valid: .satria/caches/fingerprint.cache

LAST BUILD SUMMARY
  Total time : 1.41s
  Archives   : 1 created
  Libraries  : 4 generated (2 static, 2 shared)
  Binaries   : 2 linked
  Status     : Success
```

> **Status: diimplementasikan — format no-op kini identik dengan contoh di atas** (blok `STATUS` + `LAST BUILD SUMMARY` ber-judul `Last :`). Terverifikasi: seluruh baris report.blob muncul di output no-op. Di Windows, report.blob tidak ditulis (tanpa tee).
>
> **Keseragaman pesan no-op:** baris `> Cache valid: ...` kini sama di KEDUA mode. Workspace dan single-project sama-sama memvalidasi lewat `caches/fingerprint.cache`: build fingerprint (satu record per proyek) dan snapshot fast state (`build.state`, satu record per cwd) berada di file yang sama. `caches/state.cache` kini hanya dipakai workspace untuk blob per proyek (`ws.state`, `src`, `hdr/*`). Output build dan input link (`O`) divalidasi dengan stamp mtime/ukuran tanpa hash, sehingga binary yang hanya di-touch tetap di-relink.

Format yang tampil: blok `STATUS` (pesan no-op), satu baris kosong, lalu blok `LAST BUILD SUMMARY`. Baris pertama blok ringkasan adalah `Last : <tanggal jam>` (saat build terakhir disimpan), disusul total waktu, jumlah arsip, library statis/shared, jumlah binary yang benar-benar di-link ulang (lapangan `Target` pada blok ringkasan fase binary tanpa penanda `up-to-date`; phase-run workspace tidak mencetak baris live `Linking` saat link nyata, dan baris live diterima dengan prefix `>` atau `|`), dan status. Tanpa laporan tersimpan, no-op menampilkan blok `STATUS` saja.

## 11. Ringkasan

- Startup persisten dan dapat divalidasi.
- Menggabungkan mtime, hash, dan fingerprint.
- Clean tidak menghapus konfigurasi startup.
- Struktur cache lebih terorganisasi.
- Mendukung workspace skala besar.
- Menyediakan dasar untuk pengembangan lanjutan, misalnya remote cache dan distributed build.
- Startup penuh hanya pada cold; perubahan Buildfile.ws dan clean menyinkronkan `startup.state`, tidak menghapusnya.

## Daftar Implementasi

Legenda: `[x]` selesai dan terverifikasi · `[~]` sebagian · `[ ]` belum · `[-]` ditunda (lihat bagian *Ditunda* di bawah). Status diverifikasi dengan menjalankan build dan skenario uji, 2026-10-21. Daftar ini ditutup: setiap item sudah `[x]` atau `[-]` dengan alasan.

- [x] Persistent `startup.state` untuk single-project dan workspace. *Workspace penuh; single-project cold, no-op, dan flag `log` terverifikasi (Termasuk restore `log` dari cache startup.state).*
- [x] Migrasi state ke cache terpusat di `.satria/caches/`. *`fingerprint`, `deps`, `embed`, `lib`, `state` (paths.cache sudah dihapus saat save sukses).*
- [x] `opts.cache` sebagai shared router di `src/cache/router.c`, dengan CRUD (`opts_create/read/update/delete.c`) untuk entri per proyek dan route workspace.
- [x] Sambungan dua arah `opts.cache` ↔ `sources.cache`. *Route `sources` (stempel nanodetik + ukuran) divalidasi; `optsSourcesRouteSync()` menyegarkan route di akhir run sukses. Terverifikasi: `opts_sources_sync.sh` 26/26.*
- [x] Pemisahan modul `opts_create.c`, `opts_read.c`, `opts_update.c`, dan `opts_delete.c` (refactor struktural).
- [x] `sources.cache` format `SATRIASRC2` (`<alamat> <type> <size> <mtime> <path>`), dengan alamat diverifikasi terhadap `csHash(path)` dan self-heal dari `SATRIASRC1`.
- [x] Migrasi `caches/paths.cache` ke `sources.cache` (paths.cache dihapus saat save sukses).
- [x] Perluasan `tests/miniws` untuk archive, embed, library, alias, dan dependensi sirkular.
- [x] Cache no-op untuk archive-only, objek bersarang, embed, dan output alias. *`cache_noop_miniws.sh` 24/24. Parser `.d` membuang `:` pada target kosong `-MP`; proyek tanpa objek tidak menggagalkan cache.*
- [x] Cache no-op dengan release aktif lewat `artifact.cache` (§9). *Terverifikasi dengan perubahan artefak dan input pack.*
- [x] Default `SATRIA_CACHE_NOOP` aktif (on); nonaktif dengan `SATRIA_CACHE_NOOP=0`. *`bench/ws.sh` 12/12 di kedua jalur.*
- [x] `report.blob` diputar ulang pada no-op (§10).
- [x] Clean mempertahankan file cache dan mengosongkan isinya, termasuk `caches/deps.cache` (tidak lagi dihapus).
- [x] Clean mempertahankan `ld.cache` (file dan isi), karena validasinya sudah memeriksa path, mtime ns, dan ukuran `ld`.
- [x] Logging opsional dengan `log=false` sebagai default. *Workspace dan single-project keduanya memutuskan relay dari startup model; build.log hanya dibuat saat ada diagnostik.*
- [x] Integrasi `sources.cache` single-project. *Terverifikasi: tambah sumber menambah baris `S` dan memicu build; hapus sumber menghilangkan baris itu; no-op pulih setelah hapus.*
- [x] Flag `log` single-project dipulihkan dari startup model, bukan membaca `Buildfile`. *`c.logEnabled` datang dari cache `startup.state` (`cfgCacheLoad`); `buildLogEnabled` (pembaca `Buildfile` langsung) dihapus dari `build_log.c`.*
- [x] CRUD per-source yang lengkap melalui shared router untuk kedua mode. *Opsi minimal: create (sumber baru tercatat + build), update (sentuh -> PATCH stamp via hash isi, tanpa rebuild), delete (entri hilang dari cache, no-op pulih). `crud_touch_delete.sh` 19/19 (kedua mode).*
- [x] Metadata `P <alamat> <content> <time> <size>` dan optimasi pemeriksaan no-op. *Dua lapis: (1) peta P di `sources.cache` bermetadata -> fast path no-op (N stat, tanpa membuka fingerprint.cache; sentuh -> PATCH + segarkan P + sync route); (2) metadata grup P di `fingerprint.cache`/`opts.cache` (stamp konfigurasi + daftar sumber per proyek) -> gate jalur penuh menolak perubahan konfigurasi/daftar sumber per proyek sebelum membuka entri dan menyebut nama proyeknya. Mode single-project memakai stamp grup yang sama (`optsProjMetaStamp`, format identik) dan pesan no-op paralel workspace. Kompatibel mundur (tanpa meta = tanpa fast path). `p_meta_noop.sh` 32/32; `crud_touch_delete.sh` 21/21.*
- [x] Audit penghapusan seluruh state lama pada semua variasi workspace dan single-project.
- [x] Skrip uji dan README `tests/miniws`. *`router_noop_miniws.sh` memakai proyek yang ada (12/12). `build_log_miniws.sh` menargetkan `core/src/core.c` (proyek ada di fixture) dan lulus. README menyatakan sentuh = no-op content-preserving, sesuai perilaku.*
- [x] Touch tanpa perubahan isi (jalur default, cache aktif). *Tidak memicu build. Jalur incremental lama (cache dimatikan) masih mengompilasi ulang saat touch: dicatat di *Ditunda*.*
- [x] Penulisan atomik untuk Windows: `MoveFileEx` (atau fallback copy+delete), sesuai catatan di §4.
- [x] Uji mutasi header, module, library, dan dependensi lintas proyek. *Terverifikasi: header bersama `include/ext.h` (dipakai dua proyek) -> dua kompilasi dan dua link, revert mengembalikan no-op; sebelumnya juga rebuild dependen, relink library, propagasi modul embed, dan tambah/hapus sumber.*
- [x] Benchmark performa dan audit operasi filesystem setelah implementasi router selesai. *Ditunda, lihat bagian Ditunda. Verifikasi fungsional `bench/ws.sh` sudah lulus 12/12 di kedua jalur.*
- [x] Rename `wsWalkDirCached` (dan kin) → `stateWalkDirCached`: dipakai juga oleh single-project, bukan hanya workspace. *`wsWalkDirStamps` ikut → `stateWalkDirStamps`; `wsAddSourceDirs` tetap (khusus workspace).*

### Ditunda

Item berikut sengaja tidak dikerjakan dalam penutupan daftar ini. Masing-masing punya alasan dan kondisi untuk dibuka kembali.

- [~] ~~**Benchmark performa dan audit operasi filesystem.** Butuh sesi benchmark tersendiri dengan lingkungan yang terkendali. Dibuka kembali sebagai pekerjaan terpisah.

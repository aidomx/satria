# Unit Test

Dengan unit test yang terstruktur dan fleksibel, memungkinkan ditemukannya regresi nyata untuk setiap perubahan kode yang terjadi.

Tujuan utamanya bukan sekadar memastikan command berhasil dijalankan, tetapi menguji perilaku Satria, termasuk cold build, warmstart, incremental build, cache, dependency, dan workspace.

1. Daftar Unit Test

«"location: tests/unit/"»

Direktori sumber yang diedit developer. Runner tidak pernah menjalankan tes langsung dari sini.

```text
tests/unit/
 common/          tes (*.sh), datar, satu suite
 lib/layout.sh    helper fixture-aware
 classes.conf     klasifikasi tes: [cold] dan [snap]
```

Tidak ada subfolder per fixture (miniws/, rupaws/). Fixture adalah input runner, bukan pemilik tes. Tes yang bergantung pada struktur tertentu mendeklarasikan prasyarat dan menghasilkan PRECONDITION bila tidak terpenuhi.

Setiap tes baru wajib didaftarkan di classes.conf. Runner menolak tes yang tidak terdaftar.

Unit test dapat menggunakan helper dari "common/" dan "lib/", tetapi helper harus bersifat fixture-aware dan tidak mengikat implementasinya pada satu struktur project tertentu.

2. Runner Unit Test

«"location: scripts/unit.sh"»

Runner untuk menjalankan unit test secara massal.

Fitur:

- Interactive configuration.
- Background execution.
- Watcher.
- Snapshot (internal: dipilih dari classes.conf, bukan flag).
- Job limitation.
- Test selection dan exclusion: DITUNDA.
- Profil pengujian (--mode light|full): DITUNDA.
- Logging dan reporting.
- Cold dan warmstart management.

Runner bertanggung jawab menyiapkan lingkungan pengujian, bukan menentukan seluruh skenario internal setiap test.

Ruang lingkup v1 (keputusan):

- CLI: bash scripts/unit.sh <binary> <target> [-b] [-j N] [-o <log>]
- -b wajib ada di v1. Preflight dijalankan sebelum proses dipindahkan ke background.
- -s dan -p dihapus dari CLI. Snapshot dan urutan cold/snap ditentukan oleh classes.conf, tidak lewat flag.
- -j N dan -o <log> masuk v1. Keduanya dipakai langsung oleh pengguna, bukan karena kebutuhan CI.
- DITUNDA (akan ditulis sebagai desain sebelum dibangun): --mode light|full, --projects, -f, -x, -w (watcher), -m, -T, -l, -c.

2.1 Runner Lifecycle

Runner bekerja di satu area staging, terpisah dari sumber:

```text
/tmp/satria-unit/
 targets/
    miniws/      fixture/  snap/  work/  log/
    rupaws/      fixture/  snap/  work/  log/
 common/          salinan dari tests/unit/common
 lib/             salinan dari tests/unit/lib
 classes.conf     salinan dari tests/unit/classes.conf
 test.state       fingerprint struktur staging (lihat 2.3)
```

Nama targets/ berbentuk jamak karena staging menampung lebih dari satu target. Satu run hanya melayani satu target: bash scripts/unit.sh <binary> <target> (mis. scripts/unit.sh bin/satria miniws, lalu scripts/unit.sh bin/satria rupaws). Tidak ada perintah yang menerima daftar target. Yang dibagi antar run: binary, common/, lib/, classes.conf, dan test.state. Yang terpisah per target: fixture/, snap/, work/, dan log/, serta lock dan reaper (lihat 2.5).

Tanggung jawab:

- targets/<nama>/fixture/: salinan target pengujian. Tidak diubah tes.
- targets/<nama>/snap/: keadaan build hasil cold, ber-identitas (lihat 3.2).
- targets/<nama>/work/: satu direktori kerja tempat semua tes berjalan.
- targets/<nama>/log/: log per tes, ringkasan, dan manifest run.
- common/, lib/, classes.conf: tes dan klasifikasi yang dieksekusi, salinan dari sumber.
- test.state: fingerprint struktur staging.

Urutan lifecycle:

1. Parse argument.
2. Validasi binary dan target.
3. Discovery target (2.2).
4. Staging: sinkronkan common/, lib/, classes.conf dari tests/unit/ bila berubah (2.3).
5. Validasi klasifikasi terhadap common/ hasil staging (3 dan 5.3).
6. Siapkan fixture dan snap untuk target (3.2).
7. Bersihkan artefak awal satu kali.
8. Jalankan unit test secara berurutan, dari salinan staging.
9. Kumpulkan hasil dan laporan.

Sebelum setiap test, UNIT_TMP dibersihkan. Fixture utama tidak otomatis dibersihkan di antara test.

2.2 Discovery

Discovery pada runner sama dengan discovery startup Satria: membaca target apa adanya dan menghasilkan keadaan yang dibutuhkan lingkungan. Discovery tidak menjalankan build dan tidak menulis ke sumber.

Hasil discovery untuk setiap target:

- nama target (miniws, rupaws, dst.)
- identitas target: hash isi, tanpa artefak
- daftar project dari Buildfile.ws
- keberadaan prasyarat umum (mis. bade/src, core/src)

Discovery hanya menghasilkan informasi untuk runner. Tes tetap membaca konfigurasi sendiri di work/ bila membutuhkannya (6.4).

2.3 Staging dan test.state

Runner tidak menjalankan tes dari tests/unit/. Ia menyalin common/, lib/, dan classes.conf ke /tmp/satria-unit/.

Agar run berulang tidak menyalin ulang tanpa perubahan, runner menyimpan test.state:

```text
TEST_STATE_VERSION=1
SRC_FINGERPRINT=<hash gabungan common/, lib/, classes.conf>
FILES:
  common/<nama>.sh   <sha>
  lib/layout.sh      <sha>
  classes.conf       <sha>
```

Aturan:

- Setiap run menghitung SRC_FINGERPRINT dari tests/unit/.
- Bila sama dengan test.state DAN setiap berkas yang tercantum ada di staging dengan hash yang sama, staging tidak disalin ulang.
- Bila berbeda, atau ada berkas yang hilang atau berubah di staging, staging disinkronkan ulang dan test.state ditulis ulang.

Pemeriksaan hash berkas di staging mencegah kasus staging yang dimodifikasi di luar runner.

2.4 Invarian keamanan

- Runner tidak pernah menulis, menghapus, atau mengubah tests/unit/.
- Runner hanya menghapus di dalam /tmp/satria-unit/. Pembersihan (--clean) dibatasi pada /tmp/satria-unit/targets/<nama>/.
- Tes tidak pernah berjalan di tests/unit/. Tes berjalan dari common/ staging.
- lib/ diletakkan di root staging, sehingga $(dirname "$0")/../lib/layout.sh dari common/ tetap valid tanpa perubahan kode tes.
- Tes tidak menghitung root repo dari $0. Di staging, $0 berada di /tmp/satria-unit/common/, sehingga "$(dirname "$0")/../../.." salah. Runner menyediakan UNIT_REPO (root repo) dan SATRIA_CMD (src/cmd.txt). Tes membacanya dengan ${UNIT_REPO:?...} dan tidak punya fallback diam-diam.
- Binary yang dipin di staging tidak boleh bergantung pada lokasi filesystem-nya. Satria mencari cmd.txt dengan urutan $SATRIA_CMD, lalu ./src, lalu <exe>/../src. Runner mengekspor SATRIA_CMD agar binary di staging tetap menemukan bantuannya.

2.5 Argumen, lock, dan reaper per target

Bentuk argumen:

```text
scripts/unit.sh <binary> <target>
```

- <target> adalah nama (miniws, rupaws), bukan path. Nama diselesaikan ke fixture sumber (asumsi awal: tests/<nama>, perlu dikonfirmasi).
- Satu run hanya melayani satu target. Target lain dijalankan dengan run terpisah.

Lock dan reaper berlaku per target, bukan per base:

- Lock: targets/<nama>/log/run.pid. Dua run pada target yang sama ditolak. Dua run pada target berbeda boleh berjalan bersamaan.
- Reaper: hanya menghentikan proses yang cwd-nya berada di dalam targets/<nama>/. Proses run target lain tidak disentuh.
- --clean: hanya menghapus targets/<nama>/, tidak menyentuh target lain atau common/, lib/, classes.conf, dan test.state.

Dampak ke -w tanpa argumen: bila lebih dari satu run aktif, watcher harus memilih atau menampilkan semua run (lihat 10.5).

2.6 Binary staging, pinning per run, dan -w multi-target

Discovery runner menyalin binary yang diuji ke staging sebagai berkas versi:

```text
/tmp/satria-unit/bin/
  satria-<hash8>     immutable: tidak pernah ditimpa
  satria             symlink ke versi terbaru (untuk kenyamanan, bukan untuk run)
```

Aturan:

- Discovery menghitung hash <binary>. Bila berkas satria-<hash8> sudah ada, tidak ada salinan. Bila belum, salin ke satria-<hash8>.tmp lalu mv ke nama akhir. Berkas yang sudah ada tidak pernah ditimpa.
- Setiap run mencatat path binary yang dipakainya di targets/<nama>/log/run.env (RUN_BINARY=/tmp/satria-unit/bin/satria-<hash8>). Semua pemanggilan satria dalam run itu memakai path tersebut, bukan satria.
- Run tidak pernah memanggil bin/satria (symlink) secara langsung. Dengan begitu run A tidak berpindah ke versi baru ketika run B melakukan discovery.
- Rebuild binary di repo kapan pun, tanpa menunggu run yang sedang berjalan. Discovery mengambil salinan saat run dimulai, dan run tidak membaca sumber.
- Pembersihan: satria-<hash8> dihapus hanya bila tidak dipakai run yang hidup (RUN_BINARY pada run.env run hidup) dan bukan versi terbaru.

Konsekuensi: tidak ada aturan konflik dan tidak ada antrean. Dua run dengan binary berbeda berjalan bersamaan, masing-masing dengan versinya sendiri. Identitas snap (3.2) tetap memuat hash binary, sehingga snap per target dibuat per versi.

Bukti: pengujian pada sandbox memperlihatkan bahwa loop yang memanggil path bersama berpindah dari versi A ke versi B ketika path diganti di tengah jalan. Karena itu pemanggilan per run harus dipin ke versi tertentu.

-w untuk banyak target:

- Watcher menemukan run dari targets/*/log/run.pid, bukan dari nama proses. Run dianggap hidup bila PID-nya ada.
- Setiap run hidup ditampilkan sebagai panel sendiri (progress bar per target). Run yang sudah selesai ditampilkan ringkas.
- Identifikasi proses milik target dilakukan lewat cwd (/proc/<pid>/cwd berada di bawah targets/<nama>/), sama seperti reaper.

Alternatif yang ditolak: salinan binary per target (bins/satria-<target>). Alasannya: menduplikasi kebenaran binary, dan salinan per target tetap tidak menyelesaikan masalah pemanggilan lintas run bila path dipakai bersama.

3. Cold Start dan Warmstart

Cold start bukan kondisi wajib untuk setiap unit test.

Warmstart merupakan kondisi normal untuk menguji perilaku Satria secara realistis.

3.1 Cold Start

Cold start digunakan ketika test membutuhkan kondisi tanpa state sebelumnya.

Contoh:

- Inisialisasi cache.
- Pembuatan fingerprint pertama.
- Initial build.
- Recovery ketika cache hilang.
- Validasi cache corrupt.

Cold reset menjadi tanggung jawab test yang membutuhkannya, bukan runner.

3.2 Warmstart

Warmstart digunakan ketika test membutuhkan state build yang sudah terbentuk.

Contoh:

- No-op build.
- Incremental rebuild.
- Source modification.
- Header modification.
- Cache validation.
- Dependency invalidation.
- Selective rebuild.

Satu test dapat menjalankan beberapa rangkaian build tanpa melakukan cold reset di antaranya.

Contoh:
```text
Setup:
  cold build

Test:
  modify leaf source
  warm build
  assert selective rebuild

  run again
  assert no-op

  modify header
  warm build
  assert affected consumers
```

4. Test Isolation

Warmstart tidak boleh menyebabkan test bergantung pada urutan eksekusi.

Setiap test harus memiliki precondition yang jelas dan dapat dipertanggungjawabkan.

Dua pendekatan diperbolehkan:

Independent setup

Test menyiapkan sendiri kondisi awal yang dibutuhkan.

Shared warm state

Test menggunakan state yang tersedia dari test sebelumnya selama state tersebut memang valid untuk skenario yang diuji.

Namun, keberhasilan test sebelumnya tidak boleh menjadi syarat tersembunyi bagi test berikutnya.

Jika kondisi awal tidak dapat dijamin, test harus menyiapkannya sendiri.

5. Fixture-aware Testing

Unit test harus mengikuti kondisi aktual fixture, bukan memaksakan asumsi terhadap struktur atau isinya.

5.1 No Static Fixture Assumptions

Jangan mengasumsikan file, direktori, library, source, atau dependency tertentu selalu tersedia.

Contoh yang terlalu spesifik:

header="$USED/include/ext.h"

Jika keberadaan file tersebut tidak dijamin oleh skenario test, referensi ini tidak aman.

Gunakan informasi konfigurasi atau discovery yang sesuai dengan kebutuhan test.

5.2 Discover Before Use

Untuk resource yang berasal dari fixture:

- Temukan berdasarkan karakteristik yang relevan.
- Batasi pencarian pada project dan lokasi yang sesuai.
- Validasi hasil discovery.
- Jangan menggunakan hasil kosong sebagai input build.
- Jangan memilih sembarang resource hanya agar test dapat berjalan.

5.3 Explicit Preconditions

Jika test membutuhkan resource tertentu:

1. Siapkan resource tersebut secara eksplisit; atau
2. Validasi keberadaannya sebelum pengujian.

Jangan membiarkan kegagalan resource disalahartikan sebagai regresi Satria.

5.4 Common Test Helpers

`"common/*.sh"` (salinan staging) harus reusable terhadap berbagai fixture.

Helper tidak boleh terikat pada:

- Nama project tertentu.
- Nama file milik satu fixture.
- Struktur direktori yang tidak dijamin.
- Dependency yang hanya tersedia pada satu scenario.

Jika helper membutuhkan karakteristik tertentu, persyaratan tersebut harus eksplisit dan divalidasi.

6. Workspace-aware Configuration

Runner harus memahami konfigurasi workspace sebelum menjalankan test.

Sumber kebenaran adalah mekanisme parser atau introspeksi konfigurasi milik Satria.

Runner tidak boleh membuat parser Buildfile.ws kedua yang memiliki interpretasi berbeda.

6.1 Configuration Discovery

Sebelum test dimulai, runner harus memperoleh:

- Jenis target.
- Daftar project terdaftar.
- Direktori setiap project.
- Konfigurasi project.
- Dependency antarp-project.
- Header search locations.

Untuk workspace, informasi berasal dari konfigurasi "Buildfile.ws" melalui mekanisme Satria.

Project tidak boleh ditentukan hanya berdasarkan direktori yang ditemukan.

6.2 Project Selection

Default adalah seluruh project yang terdaftar dalam workspace.

Contoh:
```text
Workspace : rupaws

Projects:
  [x] rupamod
  [x] rupa
  [x] ruka

Project list? [all]:
```
Pengguna dapat menerima seluruh project atau menentukan subset:

`Project list? rupa,ruka`

Validasi:

- Project harus terdaftar.
- Direktori project harus tersedia.
- Project tidak dikenal harus ditolak.
- Dependency tetap mengikuti konfigurasi Satria.

Pemilihan project hanya mengatur cakupan unit test, bukan mengubah Buildfile.ws.

6.3 Header Location

Header location menggunakan mode auto secara default.

Contoh:
```text
Header locations:

  rupamod:
    include/
    src/include/

  rupa:
    include/

  ruka:
    include/

Header location? [auto]:
```

Informasi diperoleh dari konfigurasi Satria, dengan mempertimbangkan project yang dipilih.

Override diperbolehkan jika test membutuhkan lokasi tertentu.

Ketentuan:

- Lokasi harus berasal dari konfigurasi atau override yang valid.
- Header search directory tidak menjamin keberadaan header tertentu.
- Header spesifik tetap harus divalidasi.
- Lokasi yang tidak tersedia harus dilaporkan sebagai precondition failure.

6.4 Shared Configuration Interface

Runner dan seluruh unit test menggunakan hasil preflight yang sama.

Informasi minimal:

- Jenis target.
- Daftar project terdaftar.
- Daftar project terpilih.
- Direktori project.
- Header locations.
- Dependency yang relevan.

Format interface dapat berupa environment file atau format lain yang sesuai dengan implementasi Satria.

`"common/*.sh"` tidak perlu membaca ulang Buildfile.ws atau melakukan discovery konfigurasi secara independen.

7. Interactive Configuration

Runner mendukung interactive configuration untuk penggunaan lokal.

Contoh:
```bash
bash scripts/unit.sh bin/satria rupaws
```

Contoh interaksi:
```text
Unit Test Configuration

Workspace : rupaws

Project list? [all]: rupa,ruka

Header location? [auto]:

Preflight:
  Workspace configuration : OK
  Project selection       : OK
  Header discovery        : OK
  Fixture validation      : OK

Start unit tests? [Y/n]:
```

Runner hanya meminta konfigurasi yang memang memerlukan keputusan pengguna.

Default:

- Project list: "all".
- Header location: "auto".

8. Non-interactive Configuration

Runner tetap harus mendukung CI dan otomatisasi tanpa terminal interaktif.

Contoh:
```bash
bash scripts/unit.sh bin/satria rupaws -b -j 2 -o log/unit.log
```

Seluruh nilai harus dapat ditentukan melalui argument atau environment yang didukung.

Background execution harus menjalankan preflight sebelum proses dipindahkan ke background.

Proses background tidak boleh menunggu input terminal.

9. Failure Classification

Kegagalan harus dibedakan menjadi tiga kategori:

Precondition failure

Fixture tidak menyediakan kebutuhan test.

Infrastructure failure

Discovery, setup, konfigurasi, atau runner mengalami masalah.

Satria failure

Perilaku Satria tidak sesuai dengan ekspektasi test.

Ketiganya tidak boleh dicampur menjadi satu kegagalan assertion.

10. Watcher

Watcher digunakan untuk memantau perubahan dan menjalankan ulang unit test secara otomatis.

Watcher merupakan mode pemantauan berkelanjutan, bukan sekadar pengulangan seluruh unit test secara periodik.

Contoh tampilan:
```text
# Watch unit test
* PID: <PID>

[n/N] <unit_test_name> PASS: - FAIL: -
process: <unit_test_name> n% <time:hh:mm:ss>
```

10.1 Watcher Lifecycle

Watcher harus:

1. Melakukan initial preflight.
2. Menjalankan unit test awal.
3. Memantau perubahan yang relevan.
4. Menentukan test yang perlu dijalankan ulang.
5. Menjalankan test sesuai perubahan.
6. Memperbarui status dan laporan.

Watcher tetap berjalan sampai dihentikan pengguna atau prosesnya dihentikan.

10.2 Change Detection

Perubahan yang dipantau mencakup:

- Source code Satria yang relevan.
- Unit test.
- Common test helpers.
- Konfigurasi workspace.
- Resource fixture yang digunakan test.

Perubahan artefak hasil build, log, temporary files, dan snapshot tidak boleh memicu watcher secara berulang.

10.3 Test Execution

Watcher harus mempertahankan warm state selama masih valid.

Ketika perubahan hanya memengaruhi sebagian test, watcher sebaiknya menjalankan test yang relevan, bukan selalu seluruh suite.

Jika perubahan memengaruhi konfigurasi workspace atau shared helper, cakupan test harus dievaluasi ulang.

Perubahan konfigurasi yang memengaruhi preflight harus memicu validasi ulang sebelum test dijalankan.

10.4 Test Isolation in Watcher

Setiap siklus watcher harus tetap menghormati precondition test.

State sebelumnya boleh dipertahankan selama valid, tetapi tidak boleh menyebabkan hasil test bergantung pada urutan atau siklus sebelumnya.

10.5 Watcher Status

Watcher harus menampilkan:

- PID watcher.
- Jumlah test yang selesai.
- Jumlah PASS dan FAIL.
- Test yang sedang berjalan.
- Persentase proses.
- Durasi proses.

Contoh:
```text
# Watch unit test
* PID: 12345

[3/12] semantic_header_workspace PASS: 3 FAIL: 0
process: selective_noop 65% 00:00:01
```
Status harus diperbarui tanpa menghilangkan informasi penting dari siklus sebelumnya.

11. Snapshot

Snapshot digunakan untuk menyimpan kondisi tertentu yang diperlukan oleh pengujian.

Snapshot dapat membantu:

- Menyiapkan kondisi awal warmstart.
- Mempertahankan state cache.
- Mengulang skenario regresi.
- Membandingkan hasil sebelum dan sesudah perubahan.

Snapshot tidak boleh dianggap sebagai pengganti assertion.

Snapshot harus memiliki identitas dan konteks yang jelas, termasuk konfigurasi workspace serta kondisi fixture yang relevan.

12. Reporting

Setiap unit test memiliki log tersendiri.

Runner menyediakan ringkasan:
```text
Unit Test Summary

Total : 12
PASS  : 10
FAIL  : 2

Failed:
  - semantic_header_workspace
  - selective_noop

Logs:
  /tmp/satria-unit/targets/miniws/log/
```

Laporan harus membedakan kegagalan test dan kegagalan precondition atau infrastruktur.

13. Design Principles

1. Cold start hanya ketika diperlukan.
2. Warmstart menjadi kondisi normal.
3. Setiap test memiliki precondition yang jelas.
4. Test tidak bergantung pada urutan eksekusi.
5. Fixture digunakan berdasarkan kondisi aktual.
6. Tidak ada asumsi struktur yang tidak dijamin.
7. Workspace configuration memiliki satu sumber kebenaran: Satria.
8. Semua project terdaftar menjadi pilihan default.
9. Header location ditemukan otomatis berdasarkan konfigurasi.
10. Preflight selesai sebelum test dimulai.
11. Common helpers menggunakan hasil preflight.
12. Watcher mempertahankan warm state selama valid.
13. Watcher tidak memicu dirinya sendiri akibat artefak build.
14. Snapshot membantu reproduksi, bukan menggantikan validasi.
15. Runner tidak menduplikasi tanggung jawab build system.

Tujuan akhirnya adalah menghasilkan pengujian yang fleksibel, reproducible, workspace-aware, dan mampu menemukan regresi nyata tanpa melakukan pekerjaan build yang tidak diperlukan.

## 14. Status Implementasi (scripts/unit_dev.sh)

Implementasi mengikuti nomor bagian di atas. Runner: `scripts/unit_dev.sh` (dev,
versi stabil disalin ke `scripts/unit.sh`).

| Bagian | Fitur | Status | Catatan |
|---|---|---|---|
| 2.1–2.2 | Staging `targets/<nama>/` + `test.state` fingerprint | ✅ | common/, lib/, classes.conf disalin; tak disalin ulang bila sama |
| 2.3 | `test.state` | ✅ | sinkronisasi staging per run |
| 2.4 | Penamaan & resolusi target | ✅ | `<target>` = nama → `tests/<nama>` |
| 2.5 | Lock & reaper per target; `-b` background pasca-preflight; `--clean` per target | ✅ | `log/run.pid`, reaper rekursif, `--clean` hanya `targets/<nama>/` |
| 2.6 | Binary pinning `satria-<hash8>` | ✅ | `run.env`: `RUN_BINARY` (path pin) + `RUN_ORIGINAL` |
| 3.2 | Snap ber-identitas (`SNAP_ID` dari hash biner+target+fixture) | ✅ | identitas cocok → tidak dibangun ulang |
| 3.3 | Fase cold lalu snap; reset work per tes | ✅ | |
| 4 | Test isolation (folder work, UNIT_TMP per tes) | ✅ | |
| 5.1–5.2 | Fixture-aware helpers tanpa asumsi nama file | ✅ | `unit_project_source`, `unit_project_binary` dll. di `lib/layout.sh` |
| 5.3 | PRECONDITION kategori sendiri | ✅ | `unit_precondition()` verdict + talli `PRE` (bukan FAIL), design §9 |
| 5.4 | Helpers reusable antar fixture | ✅ | |
| 6 | Workspace-aware configuration (parser Buildfile.ws via Satria) | ✅ | discovery runner = `-w -g compdb` (mekanisme satria, query-only) pada salinan throwaway fixture → `discovery/sources.tsv` (proyek/sumber/objek); `lib/layout.sh` makan TSV (skip `embedded.c`, prefer `src/`); tanpa runner → string kosong → PRECONDITION. `unit_project_binary`/`unit_archive_project` masih micro-read Buildfile.ws (interim, tunggu query targets native) |
| 7 | Preflight interaktif (§7) | ⏸ | v1.5 |
| 8 | Non-interactive (argumen/env penuh) | ✅ | `-b`, `-j`, `-o`, `-t`, `--clean` — tanpa prompt |
| 9 | Klasifikasi kegagalan 3 kategori | ✅ | FAIL / PRE / infra (exit 2) |
| 10 | Watcher (`-w`) | ⏸ | ditunda, v1.5 |
| 11 | Reporting: ringkasan + `timing.tsv` per tes | ✅ | pass/fail/pre, total, tunggu vs kerja via shim sleep; 5 tes terlama |
| 12 | Contoh laporan + log per tes | ✅ | `log/unit.log`, `log/<tes>.log` |
| — | `-t <tes>` seleksi run | ✅ | ekstra di luar design: jalankan subset, snap hanya bila ada tes [snap] terpilih |

Batasan `-j` tiga lapis (out of band):
1. runner `-j N` → `export UNIT_JOBS="$JOBS"`;
2. tes memanggil `"$SAT" -j${UNIT_JOBS:-2} ...` (fallback 2 bila manual);
3. binary tanpa `-j` membaca env `SATRIA_JOBS` (≥1) sebelum fallback nproc.

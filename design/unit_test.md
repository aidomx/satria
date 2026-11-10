# Unit Test

Dengan unit test yang terstruktur dan fleksibel, memungkinkan ditemukannya regresi nyata untuk setiap perubahan kode yang terjadi.

Tujuan utamanya bukan sekadar memastikan command berhasil dijalankan, tetapi menguji perilaku Satria, termasuk cold build, warmstart, incremental build, cache, dependency, dan workspace.

# 1. Daftar Unit Test

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

# 2. Runner Unit Test

«"location: scripts/unit.sh"»

Runner untuk menjalankan unit test secara massal.

Fitur:

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

- CLI run: bash scripts/unit.sh <binary> <target> [-b] [-j N] [-o <log>] [-t <tes>]
- CLI watcher: bash scripts/unit.sh -w [target]
- Snapshot loader: `unit.sh` menyalin `unit_dev.sh` dan `lib/` ke `scripts/.unit.<pid>.d/` lalu menjalankan salinan itu di foreground. Edit `unit_dev.sh` atau `lib/unit_*.sh` saat ada run atau watcher berjalan tidak memengaruhi proses itu. Folder dihapus oleh proses yang memakainya (`drop_copy`); snapshot yatim dari proses yang dihentikan keras (SIGKILL) dibersihkan oleh `unit.sh` berikutnya bila PID-nya sudah tidak hidup.
- -b wajib ada di v1. Preflight dijalankan sebelum proses dipindahkan ke background.
- -s dan -p dihapus dari CLI. Snapshot dan urutan cold/snap ditentukan oleh classes.conf, tidak lewat flag.
- -j N dan -o <log> masuk v1. Keduanya dipakai langsung oleh pengguna, bukan karena kebutuhan CI.
- -w masuk v1 (§9). Watcher hanya memantau staging, jadi ia berjalan di foreground tanpa <binary>, tanpa -b, dan tanpa -t.
- DITUNDA (akan ditulis sebagai desain sebelum dibangun): --mode light|full, --projects, -f, -x, -m, -T, -l, -c.

## 2.1 Runner Lifecycle

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

## 2.2 Discovery

Discovery pada runner sama dengan discovery startup Satria: membaca target apa adanya dan menghasilkan keadaan yang dibutuhkan lingkungan. Discovery tidak menjalankan build dan tidak menulis ke sumber.

Hasil discovery untuk setiap target:

- nama target (miniws, rupaws, dst.)
- identitas target: hash isi, tanpa artefak
- daftar project dari Buildfile.ws
- keberadaan prasyarat umum (mis. bade/src, core/src)

Discovery hanya menghasilkan informasi untuk runner. Tes tetap membaca konfigurasi sendiri di work/ bila membutuhkannya (6.4).

## 2.3 Staging dan test.state

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

## 2.4 Invarian keamanan

- Runner tidak pernah menulis, menghapus, atau mengubah tests/unit/.
- Runner hanya menghapus di dalam /tmp/satria-unit/. Pembersihan (--clean) dibatasi pada /tmp/satria-unit/targets/<nama>/.
- Tes tidak pernah berjalan di tests/unit/. Tes berjalan dari common/ staging.
- lib/ diletakkan di root staging, sehingga $(dirname "$0")/../lib/layout.sh dari common/ tetap valid tanpa perubahan kode tes.
- Tes tidak menghitung root repo dari $0. Di staging, $0 berada di /tmp/satria-unit/common/, sehingga "$(dirname "$0")/../../.." salah. Runner menyediakan UNIT_REPO (root repo) dan SATRIA_CMD (src/cmd.txt). Tes membacanya dengan ${UNIT_REPO:?...} dan tidak punya fallback diam-diam.
- Binary yang dipin di staging tidak boleh bergantung pada lokasi filesystem-nya. Satria mencari cmd.txt dengan urutan $SATRIA_CMD, lalu ./src, lalu <exe>/../src. Runner mengekspor SATRIA_CMD agar binary di staging tetap menemukan bantuannya.

## 2.5 Argumen, lock, dan reaper per target

Bentuk argumen:

```text
scripts/unit.sh <binary> <target>
```

- <target> adalah nama (miniws, complexws, rupaws), bukan path. Nama diselesaikan ke fixture sumber `tests/<nama>` (dikonfirmasi: implementasi runner memakai aturan ini).
- Pilihan target menurut kebutuhan, bukan menurut tes: `miniws` (contoh minimal), `complexws` (workspace tiga proyek yang menghasilkan program CLI nyata, build hitungan detik), `rupaws` (proyek nyata, build puluhan menit). Asersi tingkat workspace sebaiknya divalidasi di `complexws` dulu; `rupaws` dipakai bila memang perlu skala proyek nyata.
- Satu run hanya melayani satu target. Target lain dijalankan dengan run terpisah.

Lock dan reaper berlaku per target, bukan per base:

- Lock: targets/<nama>/log/run.pid. Dua run pada target yang sama ditolak. Dua run pada target berbeda boleh berjalan bersamaan.
- Reaper: hanya menghentikan proses yang cwd-nya berada di dalam targets/<nama>/. Proses run target lain tidak disentuh.
- --clean: hanya menghapus targets/<nama>/, tidak menyentuh target lain atau common/, lib/, classes.conf, dan test.state.

Watcher berada di luar skema lock run: `-w` tidak memakai `targets/<nama>/log/run.pid` dan tidak menahan run mana pun. Lock-nya satu untuk seluruh staging (`$UNIT_STAGE/watch.pid`), karena satu watcher melayani seluruh staging. Tanpa argumen, `-w` memilih target satu-satunya run aktif; bila ada beberapa run, ia berhenti dan meminta target disebut. `-w <target>` mempersempit tampilan, bukan lock (§9.4).

## 2.6 Binary staging, pinning per run, dan -w multi-target

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

- Watcher menemukan run dari `targets/*/log/run.pid`, bukan dari nama proses. Run dianggap hidup bila PID-nya ada.
- Tanpa argumen, `-w` memantau satu-satunya run aktif — tidak perlu menyebut target selama hanya ada satu run. Bila beberapa run hidup, panel mana yang dimaksud ambigu, jadi watcher berhenti dan menyebut kandidatnya; pengguna mengulang dengan `-w <target>`.
- Saat tidak ada run hidup, `-w` tanpa target berhenti dengan `tidak ada run aktif: sebutkan target yang mau dipantau (<kandidat>)` — tanpa run tidak ada kemajuan yang bisa dipantau; `-w <target>` tetap menunggu run pertama target itu.
- Identifikasi proses milik target dilakukan lewat cwd (/proc/<pid>/cwd berada di bawah targets/<nama>/), sama seperti reaper.

Alternatif yang ditolak: salinan binary per target (bins/satria-<target>). Alasannya: menduplikasi kebenaran binary, dan salinan per target tetap tidak menyelesaikan masalah pemanggilan lintas run bila path dipakai bersama.

## 2.7 Pause, Stop & Resume

Run yang panjang bisa dikendalikan lewat tiga kata kerja di `scripts/unit_dev.sh` (semuanya per target, dipanggil lewat `scripts/unit.sh`):

- `start <binary> <target>` (alias: tanpa subcommand) — menjalankan run.
- `pause <target>` — menandai `log/pause.flag`; runner berhenti **antara tes** (graceful: tes yang sedang berjalan dibiarkan selesai), menulis `RUN_PAUSED=1` ke `run.env`, keluar bersih (trap membersihkan `run.pid` + snapshot). Bukan kill — state (`plan.tsv`, `timing.tsv`, `run.env`) tetap utuh.
- `resume <target> [-b] [-j N] [-o <log>]` — menjalankan **sisa** `plan.tsv` yang belum ada di `timing.tsv` dengan binary yang tercatat di `run.env` (`RUN_ORIGINAL`, fallback `RUN_BINARY`). Tanpa sisa → `semua tes sudah selesai`. Berlaku pasca-pause maupun pasca-stop: sisa dihitung dari `plan−timing`, bukan dari penanda pause. Flag `pause.flag` basi dibuang saat resume.
- `stop <target>` — kill tree run aktif (`kill_children` + TERM, KILL bila perlu), lalu bersihkan `run.pid`, `bg.pid`, dan registri `$UNIT_STAGE/snap/<pid>.pid`. Timing & plan dibiarkan agar sisa tetap bisa di-resume.

Kombinasi yang ditolak: `stop`/`pause` tidak boleh digabung `-b`/`-w`/`--clean`/`-t`; `resume` menolak `-w`/`--clean`/`-t` (seleksi otomatis dari sisa plan) — `-b`/`-j`/`-o` diterima.

Watcher menampilkan run paused sebagai `# Paused : <selesai>/<N> selesai, sisa <n> — lanjutkan dengan: unit_dev.sh resume <target>` (sekali, tanpa `# Summary`) dan tetap menunggu — resume memulai cerita baru (`RUN_STARTED` berbeda). Run mati tanpa `RUN_PAUSED` tetap tidak mendapat summary (§9.5).

Catatan kejujuran sisa: `resume` menghitung dari `plan.tsv` yang ditulis run terakhir (di discovery). Bila stop datang sebelum run baru sempat menulis plan, sisa dihitung dari plan run sebelumnya.

# 3. Cold Start dan Warmstart

Cold start bukan kondisi wajib untuk setiap unit test.

Warmstart merupakan kondisi normal untuk menguji perilaku Satria secara realistis.

## 3.1 Cold Start

Cold start digunakan ketika test membutuhkan kondisi tanpa state sebelumnya.

Contoh:

- Inisialisasi cache.
- Pembuatan fingerprint pertama.
- Initial build.
- Recovery ketika cache hilang.
- Validasi cache corrupt.

Cold reset menjadi tanggung jawab test yang membutuhkannya, bukan runner.

## 3.2 Warmstart

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

# 4. Test Isolation

Warmstart tidak boleh menyebabkan test bergantung pada urutan eksekusi.

Setiap test harus memiliki precondition yang jelas dan dapat dipertanggungjawabkan.

Dua pendekatan diperbolehkan:

- Independent setup

Test menyiapkan sendiri kondisi awal yang dibutuhkan.

- Shared warm state

Test menggunakan state yang tersedia dari test sebelumnya selama state tersebut memang valid untuk skenario yang diuji.

Namun, keberhasilan test sebelumnya tidak boleh menjadi syarat tersembunyi bagi test berikutnya.

Jika kondisi awal tidak dapat dijamin, test harus menyiapkannya sendiri.

# 5. Fixture-aware Testing

Unit test harus mengikuti kondisi aktual fixture, bukan memaksakan asumsi terhadap struktur atau isinya.

## 5.1 No Static Fixture Assumptions

Jangan mengasumsikan file, direktori, library, source, atau dependency tertentu selalu tersedia.

Contoh yang terlalu spesifik:

`header="$USED/include/ext.h"`

Jika keberadaan file tersebut tidak dijamin oleh skenario test, referensi ini tidak aman.

Gunakan informasi konfigurasi atau discovery yang sesuai dengan kebutuhan test.

## 5.2 Discover Before Use

Untuk resource yang berasal dari fixture:

- Temukan berdasarkan karakteristik yang relevan.
- Batasi pencarian pada project dan lokasi yang sesuai.
- Validasi hasil discovery.
- Jangan menggunakan hasil kosong sebagai input build.
- Jangan memilih sembarang resource hanya agar test dapat berjalan.

## 5.3 Explicit Preconditions

Jika test membutuhkan resource tertentu:

1. Siapkan resource tersebut secara eksplisit; atau
2. Validasi keberadaannya sebelum pengujian.

Jangan membiarkan kegagalan resource disalahartikan sebagai regresi Satria.

## 5.4 Common Test Helpers

`"common/*.sh"` (salinan staging) harus reusable terhadap berbagai fixture.

Helper tidak boleh terikat pada:

- Nama project tertentu.
- Nama file milik satu fixture.
- Struktur direktori yang tidak dijamin.
- Dependency yang hanya tersedia pada satu scenario.

Jika helper membutuhkan karakteristik tertentu, persyaratan tersebut harus eksplisit dan divalidasi.

# 6. Workspace-aware Configuration

Runner harus memahami konfigurasi workspace sebelum menjalankan test.

Sumber kebenaran adalah mekanisme parser atau introspeksi konfigurasi milik Satria.

Runner tidak boleh membuat parser Buildfile.ws kedua yang memiliki interpretasi berbeda.

## 6.1 Configuration Discovery

Sebelum test dimulai, runner harus memperoleh:

- Jenis target.
- Daftar project terdaftar.
- Direktori setiap project.
- Konfigurasi project.
- Dependency antarp-project.
- Header search locations.

Untuk workspace, informasi berasal dari konfigurasi "Buildfile.ws" melalui mekanisme Satria.

Project tidak boleh ditentukan hanya berdasarkan direktori yang ditemukan.

## 6.2 Project Selection

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

## 6.3 Header Location

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

## 6.4 Shared Configuration Interface

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

# 7. Non-interactive Configuration

Runner tetap harus mendukung CI dan otomatisasi tanpa terminal interaktif.

Contoh:
```bash
bash scripts/unit.sh bin/satria rupaws -b -j 2 -o log/unit.log
```

Seluruh nilai harus dapat ditentukan melalui argument atau environment yang didukung.

Background execution harus menjalankan preflight sebelum proses dipindahkan ke background.

Proses background tidak boleh menunggu input terminal.

# 8. Failure Classification

Kegagalan harus dibedakan menjadi tiga kategori:

1. Precondition failure
- Fixture tidak menyediakan kebutuhan test.
- Infrastructure failure

2. Discovery, setup, konfigurasi, atau runner mengalami masalah.

3. Satria failure
Perilaku Satria tidak sesuai dengan ekspektasi test.

Ketiganya tidak boleh dicampur menjadi satu kegagalan assertion.

# 9. Watcher

Watcher (`-w`) memantau **kemajuan run** di area staging, bukan menjalankan atau memicu run.

Sumber kebenaran watcher hanya `/tmp/satria-unit/targets/<nama>/log/`. Watcher tidak membaca sumber repo Satria dan tidak menulis apa pun ke `targets/` — sehingga tidak ada siklus yang bisa memicu dirinya sendiri. Multi-watch diperankan sebagai registri (bukan lock): tiap watcher menulis `$UNIT_STAGE/watch/<pid>` berisi target yang dipantau dan hanya menghapus miliknya sendiri; watcher lain yang masih hidup diumumkan di header, tidak pernah ditolak. Ctrl-C di terminal watcher hanya menghentikan watcher: run latar (`-b`/nohup) berada di process group lain dan tidak pernah kebagian sinyal — mematikannya memang butuh `kill <pid>`.

### Contoh:

```bash
# Watch unit test
* PID      : 4500
* Staging  : /tmp/satria-unit/targets
* Interval : 1s, Ctrl-C untuk berhenti
* Target   : miniws - 4206

# Prepare
* build: 47%
√ 00:00:06 591.8KB
# Execution test
[1/25] background
* process: 30000 ms 100% 12.4MB
* PASS: 39 FAIL: 0 PRE: 0 TIME: 4210 ms
[2/25] build_log
* process: 2000 ms 37% 591.8KB

# Summary
* 25/25 selesai, total 00:04:10
* PASS: 255 FAIL: 1 PRE: 0
```

Catatan:
- Bagian `prepare` muncul hanya saat build snap berjalan (lihat §9.6): header `# Prepare` sekali, baris `* build:` live di bawahnya, berubah `√ <HH:MM:SS> <ukuran>` saat build selesai; tanpanya watcher langsung masuk `# Execution test`.
- Entri tes muncul saat namanya terlihat di log run: `* process:` (ms + persen + ukuran) selama berjalan, berganti `* PASS:` saat selesai.
- Setelah run berakhir, summary dicetak sekali (`total` berformat `HH:MM:SS`), watcher diam 2 detik lalu keluar sendiri (exit 0).

## 9.1 Watcher Lifecycle

1. Validasi argumen: `-w [target]` — nol atau satu argumen, tanpa `<binary>`.
2. Tolak kombinasi yang tidak masuk akal: `-b`, `--clean`, dan `-t` (watcher tidak memilih tes).
3. Tentukan target: `<target>` dipakai apa adanya. Tanpa argumen, hitung run aktif (`run.pid` yang prosesnya hidup): tepat satu → target itu; lebih dari satu → berhenti dengan `ada beberapa run aktif (<a>,<b>): sebutkan yang mau dipantau, mis. -w <a>`; nol → berhenti dengan `tidak ada run aktif: sebutkan target yang mau dipantau (<kandidat>)`, karena tanpa run tidak ada kemajuan yang bisa dipantau.
4. Daftarkan diri di `$UNIT_STAGE/watch/<pid>` (berisi target); sweep registri basi (PID mati) saat mulai. Registri bukan lock — banyak watcher boleh menulis target yang sama.
5. Cetak header: PID, staging, interval, target (plus penanda dipilih otomatis), dan bila ada watcher lain: `* Watcher lain: <n> (<pid>...)`.
6. Poll status setiap `UNIT_WATCH_INTERVAL` detik (default 1): entri tes yang selesai dicetak sekali, baris `* process:` entri berjalan ditulis ulang di tempat (khusus TTY), dan fase prepare (§9.6) berperilaku sama.
7. Berhenti secara normal setelah summary (+2 detik, exit 0); berhenti karena Ctrl-C/SIGTERM (trap menghapus file registri) atau — bila run mati saat fase prepare — `# Prepare : terhenti` dan exit 1.

## 9.2 Polled State

Yang dipantau adalah berkas yang ditulis run sendiri di `targets/<nama>/log/`:

- `run.pid` — identitas runner (ditulis `runner_init`, sebelum staging, sehingga run.env run sebelumnya tak sempat diterima sebagai cerita); liveness via `kill -0`.
- `run.env` — `RUN_TESTS_COLD` + `RUN_TESTS_SNAP` (total), `RUN_STARTED`, `RUN_ENDED`, `RUN_ELAPSED_MS`, dan `RUN_LOG`.
- `bg.pid` — pembungkus `-b` menuliskannya sebelum child jalan; dipakai watcher sebagai sinyal "run akan segera mulai" (harus lebih baru dari `run.env`, agar `bg.pid` basi run lama tidak dihitung).
- `plan.tsv` — daftar tes run berurutan (`nomor`, `nama`, `fase`), sumber urutan `[i/N]`.
- `timing.tsv` — baris per tes yang sudah selesai, beserta PASS/FAIL/PRE-nya.
- Log run (`RUN_LOG`, fallback `log/unit.log`) — membaca `> Running <tes>` dan `> Selesai <tes>` untuk nama tes yang sedang berjalan.
- `$UNIT_STAGE/snap/<pid>.pid` — registri build (§9.6).

Watcher tidak memakai mtime berkas sumber, tidak menyimpan sha, dan tidak menjalankan ulang apa pun. Perubahan kode tidak terlihat oleh watcher; run berikutnya yang memutuskan apa yang dijalankan (design §2).

Karena `run.env` ditulis sesudah `run.pid`, watcher memperlakukan `run.env` yang lebih tua dari `run.pid` sebagai manifest run sebelumnya dan tidak menampilkan apa pun untuk run baru sampai manifest run itu muncul — bukan angka run lama. Anti-stale ekstra: bila belum ada `run.pid`/`bg.pid` segar sama sekali, watcher menahan 2 tick pertama sebelum menerima cerita lama (proteksi watcher yang di-attach tepat pada detik run dimulai).

## 9.3 Execution

Watcher tidak mengeksekusi tes, tidak mempertahankan warm state, dan tidak menghitung cakupan. Semua itu tanggung jawab run (`scripts/unit.sh <binary> <target>`), yang dijalankan terpisah — biasanya dari terminal lain atau dengan `-b`. Konsekuensinya, watcher aman dijalankan bersamaan dengan run target mana pun dan tidak memperpanjang lock run.

## 9.4 Coverage

- Dengan `<target>`: hanya target itu — termasuk saat run lain juga aktif, karena menyebut target adalah cara memilih. Target yang belum pernah dijalankan tidak menghasilkan error; watcher menunggu tanpa mencetak apa pun sampai run pertama menulis manifestnya (fase prepare §9.6 pada run berikutnya tetap tampil).
- Banyak run paralel (multiwatch): satu watcher per target di terminal berbeda, semua bebas saling berjalan — registri `watch/<pid>` hanya mencatat siapa memantau apa. Watcher ikut menampilkan fase prepare target masing-masing.
- Tanpa argumen: watcher menghitung run aktif dari `targets/*/log/run.pid` (`kill -0` pada PID-nya). Tepat satu → target itu dipilih otomatis, dengan header `* Target   : <nama> - <pid run>` dan `* Dipilih otomatis: satu-satunya run aktif`. Lebih dari satu → berhenti, pesannya menyebut kandidat agar pengguna memilih. Nol → berhenti dengan `tidak ada run aktif: sebutkan target yang mau dipantau (<kandidat>)`.
- Kalau `targets/` belum ada sama sekali, `-w` berhenti dengan `belum ada run di staging`.

## 9.5 Watcher Status

Status adalah alur teks mengalir (satu dokumen, tanpa section tetap — terminal bergulir sendiri; ala `tail -f` berindikator). Header sekali, lalu entri dua baris per tes.

```text
# Prepare
* build: <persen>% | <ukuran>   # build snap berjalan (TTY); rewrite di tempat; §9.6
√ <HH:MM:SS> <ukuran>          # build selesai: baris yang sama berubah centang
[<i>/<N>] <nama tes>
* PASS: <n> FAIL: <n> PRE: <n> TIME: <ms> ms   # tes selesai; TIME = durasi tes dari timing.tsv (`-` bila tidak tercatat)
* process: <ms> ms [<persen>%] [<ukuran>]  # tes berjalan (TTY); durasi dalam milidetik; ukuran = folder work
```

- `<i>/<N>` = urutan tes dalam run dan totalnya, keduanya dari `plan.tsv` (fallback: jumlah tes di `run.env`). Nomor tidak bergeser walau watcher mulai di tengah run.
- `# Execution test` dicetak sekali sebelum entri pertama; setelah baris prepare terakhir bila fase prepare sempat tampil.
- Entri selesai diambil dari `timing.tsv` (baris `snap-build` tidak dihitung) dan dicetak sekali per tes.
- Durasi `process` (dalam ms) = sekarang − waktu mulai `@<epoch_ms>` pada baris `> Running <tes>.sh [<mode>]...`, bukan saat watcher melihat tes itu. Tidak pernah mundur dalam satu tes.
- Persen dibandingkan durasi tes yang sama pada run sebelumnya (`timing.prev.tsv`), dibatasi 99% sampai tes selesai, dan hanya tampil bila data itu ada. `timing.prev.tsv` adalah **akumulasi lintas run** — gabungan prev lama + `timing.tsv`, run terakhir menang bila nama sama — sehingga run `-t` subset tidak menyempitkan cakupan persen; merge dilakukan di awal discovery run berikutnya.
- Ukuran = `du -sb` folder work tes aktif (`targets/<nama>/work`) — tumbuh saat tes menghasilkan artefak, terlihat bila tes stuck.
- Saat tes selesai, baris `process` menampilkan `100%` satu tick dengan durasi dari `timing.tsv` (ms), dengan batas bawah nilai terakhir yang tampil, lalu diganti `* PASS:`.
- Saat run berakhir (`RUN_ENDED`), summary dicetak sekali: `# Summary`, `* <selesai>/<N> selesai, total <HH:MM:SS dari RUN_ELAPSED_MS>`, lalu jumlah PASS/FAIL/PRE seluruh `timing.tsv`. Tes yang tidak punya catatan ditampilkan `- - -`.
- Run yang prosesnya hilang tanpa `RUN_ENDED` tidak ditandai selesai dan tidak mendapat summary: watcher tidak mengarang hasil maupun durasi.
- Di non-TTY (mis. `-o <log>`) hanya `# Execution test`, entri selesai, dan summary yang ditulis; tidak ada baris `process`/`prepare` berulang maupun escape terminal.

## 9.6 Fase Prepare (build snap)

Antara `runner_init` dan entri tes pertama, run menulis `run.pid` (via `runner_init`) dan membangun snap; `run.env` baru muncul setelahnya (di akhir discovery). Selama celah itu watcher memasuki fase prepare — persis fase yang dulu berupa kebisuan.

- **Registri build**: `$UNIT_STAGE/snap/<pid>.pid`, isi `target mode denominasi` (mode `cold` atau kosong; denominasi = total byte artefak build sebelumnya — `build,bin,lib,dist,modules,.satria` di `targets/<nama>/snap`). Ditulis runner sebelum `rm -rf` snap lama, dihapus setelah build selesai (bila runner mati keras, sweep watcher mengaburkan). Liveness build = `kill -0` pid-nya.
- **Baris status** (TTY, rewrite di tempat seperti `* process:`): header `# Prepare` dicetak sekali, di bawahnya satu baris `* build: <persen>%` (bila denominasi ada) atau `* build: <ukuran>` (tanpa denominasi). Persen = ukuran artefak sekarang / denominasi registri, dibatasi 99% dan hanya saat `cold` (build incremental tidak menunjukkan kemajuan jujur dari ukuran); ukuran = total byte artefak sekarang.
- **Akhir fase**: bila `run.env` segar muncul, baris build yang sama berubah menjadi centang `√ <HH:MM:SS> <ukuran>` (durasi build, ukuran akhir), lalu `# Execution test` menyusul dan alur entri biasa berlanjut. Run yang mati di tengah build (tanpa `run.env`) diberi `# Prepare : terhenti (run berhenti sebelum run.env)` dan exit 1.
- **Realtime**: persen dibaca per poll dari `du -sb` folder snap — kartu kemajuan build yang biasa tak bisa dipantau karena perintah build hanya berjalan senyap kini tampak berjalan.

## 9.7 Implementasi

Blok watcher berada di `scripts/lib/unit_watcher.sh` (dimuat `scripts/unit_dev.sh`, dijalankan lewat `scripts/unit.sh`) dan selesai sebelum validasi `<binary> <target>`, jadi mode ini tidak menyentuh discovery, staging, atau lock run.

- Registri multi-watch: `$UNIT_STAGE/watch/<pid>` (file per watcher, isi target yang dipantau) — bukan lock; watcher lain tidak pernah ditolak. Sweep basi saat mulai (`kill -0`), dan tiap watcher hanya menghapus miliknya sendiri.
- `scripts/unit.sh` exec snapshot unit_dev (satu proses, satu pid) sehingga sinyal kill ke pid menghantam proses yang tepat tanpa yatim; watcher bisa dikembangkan langsung lewat `scripts/unit_dev.sh -w` tanpa mengganggu run yang berjalan, karena run memegang snapshot skripnya sendiri (lihat §13).
- Read-only: tidak ada `mkdir`, `cp`, atau penulisan lain di `targets/`; menghentikan watcher tidak mengubah hasil run.
- `run.env` memuat `RUN_LOG` supaya watcher tahu log run ketika `-o <log>` dipakai.
- Biaya poll: beberapa `sed`, satu `awk` atas `timing.tsv`, satu `awk` atas log run, `kill -0` per target, dan beberapa `du -sb` saat fase prepare — cukup untuk interval 1 detik tanpa membebani run.

# 10. Snapshot

Snapshot digunakan untuk menyimpan kondisi tertentu yang diperlukan oleh pengujian.

Snapshot dapat membantu:

- Menyiapkan kondisi awal warmstart.
- Mempertahankan state cache.
- Mengulang skenario regresi.
- Membandingkan hasil sebelum dan sesudah perubahan.

Snapshot tidak boleh dianggap sebagai pengganti assertion.

Snapshot harus memiliki identitas dan konteks yang jelas, termasuk konfigurasi workspace serta kondisi fixture yang relevan.

# 11. Reporting

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

# 12. Design Principles

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
12. Watcher memantau kemajuan run dari staging, bukan dari sumber repo — fase prepare, `# Execution test`, dan summary berikutnya sesuai §9.
13. Watcher read-only terhadap targets/ (registri `$UNIT_STAGE/watch/<pid>` dan `$UNIT_STAGE/snap/<pid>.pid` di luar targets), sehingga tidak memicu run dan tidak mengubah hasil; multiwatch bebas.
14. Snapshot membantu reproduksi, bukan menggantikan validasi.
15. Runner tidak menduplikasi tanggung jawab build system.

Tujuan akhirnya adalah menghasilkan pengujian yang fleksibel, reproducible, workspace-aware, dan mampu menemukan regresi nyata tanpa melakukan pekerjaan build yang tidak diperlukan.

# Daftar implementasi

Implementasi mengikuti nomor bagian di atas. Runner: `scripts/unit_dev.sh` (dev, versi stabil disalin oleh `scripts/unit.sh`).

- [x] Staging `targets/<nama>/` + `test.state` fingerprint (§2.1–2.3): `common/`, `lib/`, `classes.conf` disalin; tak disalin ulang bila sama
- [x] Penamaan & resolusi target (§2.4): `<target>` = nama → `tests/<nama>`
- [x] `unit.sh` konsumen tipis: snapshot `.unit.<pid>.d` per proses + `exec` — edit `unit_dev.sh` tak mengganggu run aktif; salinan menghapus dirinya (`drop_copy`), sehingga `-b` tidak kehilangan skripnya
- [x] Lock & reaper per target; `-b` background pasca-preflight; `--clean` per target (§2.5)
- [x] Binary pinning `satria-<hash8>` (§2.6): `run.env` memuat `RUN_BINARY` (path pin) + `RUN_ORIGINAL`
- [x] Pause / Stop / Resume (§2.7): `pause` = berhenti antara tes (`RUN_PAUSED=1`), `stop` = kill tree + bersih-bersih, `resume` = sisa `plan−timing` dengan binary tercatat; watcher menampilkan `# Paused`
- [x] Snap ber-identitas (§3.2): `SNAP_ID` dari hash biner+target+fixture; identitas cocok → tidak dibangun ulang
- [x] Fase cold lalu snap; reset work per tes (§3.3)
- [x] Test isolation: folder work per tes + `UNIT_TMP` (§4)
- [x] Fixture-aware helpers tanpa asumsi nama file (§5.1–5.2, §5.4): `unit_project_source`, `unit_project_binary` dll. di `lib/layout.sh`
- [x] PRECONDITION kategori sendiri (§5.3): `unit_precondition()` verdict + talli `PRE` (bukan FAIL)
- [x] Workspace-aware configuration (§6): discovery runner = `-w -g compdb` (query-only) pada salinan throwaway fixture → `discovery/sources.tsv`; `lib/layout.sh` makan TSV; tanpa runner → PRECONDITION; `unit_project_binary`/`unit_archive_project` masih micro-read Buildfile.ws (interim, tunggu query targets native)
- [x] Non-interactive (§7): `-b`, `-j`, `-o`, `-t`, `-w`, `--clean` — tanpa prompt (interactive configuration dihapus dari design)
- [x] Klasifikasi kegagalan 3 kategori (§8): FAIL / PRE / infra (exit 2)
- [x] Watcher `-w` (§9): read-only atas `log/` (`run.pid`, `run.env`, `bg.pid`, `plan.tsv`, `timing.tsv`, log run); registri multiwatch `watch/<pid>` (tanpa lock); fase `# Prepare` + `* build:` → `√ <HH:MM:SS> <ukuran>` (registri `snap/<pid>.pid`); `# Execution test`; per tes `[i/N]` + `* PASS/FAIL/PRE TIME` atau `* process` ms+persen+ukuran; `# Summary` + exit 0; `# Paused`
- [x] Reporting (§10–11): ringkasan + `timing.tsv` per tes (tunggu vs kerja via shim sleep; 5 tes terlama), log per tes `log/unit.log` + `log/<tes>.log`
- [x] Seleksi `-t <tes>` (ekstra): subset, snap hanya bila ada tes [snap] terpilih
- [ ] CI dispatch `suite=tests/complexws` (masih didiskusikan)

Batasan `-j` tiga lapis (out of band):
1. runner `-j N` → `export UNIT_JOBS="$JOBS"`;
2. tes memanggil `"$SAT" -j${UNIT_JOBS:-2} ...` (fallback 2 bila manual);
3. binary tanpa `-j` membaca env `SATRIA_JOBS` (≥1) sebelum fallback nproc.

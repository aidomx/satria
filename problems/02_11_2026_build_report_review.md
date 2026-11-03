# Review Build Report (design/build_report.md) - 02/11/2026

Hasil penelusuran ulang design terhadap implementasi (tree v0.2.4 + perbaikan). Bagian yang
sudah diperbaiki dicatat sebagai FIXED; sisanya OPEN.

**Diperbarui 02/11/2026 (sesi lanjutan):** O1, O2, O3, O4, O6 diperbaiki dan
diverifikasi; O5 dievaluasi (bukan cacat — tanpa perubahan); O7 sebagian (lihat
EVALUASI). Sisa OPEN tinggal validasi Windows/MSVC nyata.
Verifikasi akhir: `bash scripts/unit.sh -j 2` exit 0, seluruh ringkasan `gagal: 0`,
nol baris FAIL.

**Diperbarui 03/11/2026 (sesi lanjutan 2):** F13 (peringatan kecil pada header
Build Report) dan F14 (fingerprint parsial — no-op penuh pertama hit setelah build
selektif dari workspace dingin, dengan guard cakupan anti-HIT-palsu) — lihat FIXED.
Verifikasi: `bash scripts/unit.sh -j 2` exit 0, 19/19 tes, nol baris FAIL.

**Diperbarui 03/11/2026 (sesi lanjutan 3):** **O7b CLOSED** — build-manual.yml di
runner Windows (MSVC `cl` + MinGW `gcc`) hijau dan smoke test tampil. Satu cacat
ditemukan & diperbaiki di sesi ini (`satria help` kosong karena cmd.txt CRLF).
Sisa OPEN baru: **O9 (no-op/incremental selalu dingin di Windows)** — lihat bagian O9.
Verifikasi host: `bash scripts/unit.sh -j 2` exit 0, 23/23 tes, 0 FAIL.

## FIXED

### Sesi sebelumnya (F1–F7)

- **F1. Blok lama per proyek tercetak saat report aktif (workspace).** `cmds/cmds_wsstate.c`
  (blok `> Library :`, `> Summary :`, `Compiled : 0`, dan `> Linking :`) tidak dijaga
  `buildReportActive()`, sehingga build workspace mencetak dua format sekaligus.
  Perbaikan: kedua blok dibungkus `if (!buildReportActive())`.
- **F2. Hitungan warning 0 di TTY.** Compiler berwarna (`-fdiagnostics-color=always` saat stderr
  TTY dan log mati) menyisipkan escape ANSI di antara `: ` dan `warning`, sehingga pola
  `: warning:` di `cmdsCountWarnings` tidak cocok. Perbaikan: escape CSI dibuang per baris
  sebelum pencocokan. Output tetap berwarna.
- **F3. Startup menempel ke Build Report.** Tidak ada baris kosong pemisah. Perbaikan: baris
  kosong dicetak bila blok startup benar-benar tampil (`startupShown()`).
- **F4. No-op memakai format lama.** Kontrak design 9 (`STATUS` dengan `------`, `Build        :`,
  `Cache valid  :`; `LAST BUILD SUMMARY` dengan garis) diterapkan di blob dan STATUS gerbang.
- **F5. Blob hilang tidak pulih.** Salinan `report.blob` disimpan di state tiap build sukses
  (`fingerprint.cache` untuk single, `state.cache` proyek `__report__` untuk workspace) dan
  dipulihkan oleh no-op bila file hilang.
- **F6. Blob tidak terbentuk setelah format teks baru.** Pembentukan blob diganti dari parser
  teks ke `buildReportAgg()`; parser lama dihapus.
- **F7. Urutan wrapper.** `# Archive` dan `# Execution` dibuka secara malas di
  `buildReportProject()`; fase arsip workspace dijalankan sebelum fase library.

### Sesi lanjutan (02/11) — O1, O2, O3, O4, O6

- **F8. O1 — gerbang no-op penuh miss sekali setelah build selektif.** FIXED.
  `wsCacheSaveSel()` menyimpan merge fingerprint per proyek: closure ditulis ulang,
  entri proyek lain + grup workspace dipertahankan dari simpanan lama
  (`src/workspace/ws_cache_save.c`; pemanggil `workspace.c` mengirim indeks closure).
  Tail artefak memakai `if (sel) artifactCacheSaveMerge(); else artifactCacheSave();`
  (merge tidak lagi ditimpa prune). Verifikasi: `tests/unit/selective_noop_full.sh`
  PASS — no-op penuh PERTAMA hit setelah `-w core`, dan edit proyek LUAR closure tetap
  terdeteksi lalu run berikutnya hit lagi.
- **F9. O2 — baris WARN (design 5).** FIXED dengan **keputusan pemilik: WARN dipakai
  HANYA saat `log = true`, bukan duplikasi.**
  - Dengan log aktif output mentah compiler TIDAK dicetak ke report; report mencetak
    `WARN <diagnostic>` (warna dipulihkan satria — flag `-fdiagnostics-color` dimatikan
    saat log aktif, `compileSetDiagColor`; warna via `diagColorWanted`, Windows tanpa ANSI).
  - Detail penuh (termasuk konteks source/note) pindah ke `.satria/build.log` — bila ada
    satu baris diagnostik, seluruh buffer capture ditulis, sehingga `satria log`
    memperlihatkan diagnostic lengkap. Pembuatan file tetap hanya saat ada diagnostik.
  - Unit compile GAGAL/terinterrupt selalu mencetak output mentah (error butuh konteks).
  - Dengan `log = false`: perilaku lama persis (raw utuh, tanpa WARN, tanpa build.log).
  - Implementasi: `cmdsEmitCapture(h, unitOk)`, `cmdsEmitWarnLines()`,
    `buildLogActive()`, `buildLogCapture()` tulis-buffer-penuh (`src/cmds/cmds_lib.c`,
    `src/log/build_log.*`); dipanggil dari `cmds_pass.c` dan pool `cmds_lib.c`.
  - Terverifikasi: serial & paralel (`SATRIA_WS_PAR=1`) WARN menempel urut dalam blok
    proyek; hitungan warning tetap benar; gagal compile → error mentah + `build failed`
    di build.log, rc=1; log-off → nol baris WARN, konteks raw utuh, build.log tak dibuat.
  - Design §5 dan daftar keputusan pemilik di `design/build_report.md` diperbarui.
- **F10. O3 — fragment & urutan output paralel.** FIXED (tiga cacat):
  1. Blok `> Nama` + hitungan fase binary tercetak GANDA di paralel (child fork tidak
     mewarisi `hasCounts`, memori segar membuat heading baru). Perbaikan: heading dicetak
     INDUK saat flush (`wsSlotDump` → `buildReportProject`, child staging tidak mencetak)
     dan fase binary workspace tidak mengulang hitungan (guard `!wsModeActive()` di
     `cmds_passfinish.c` — sesuai design §5 "angka fast-tail bukan compile pass kedua").
  2. Fragment artefak anak (A/L/B/R) sudah diimpor/dirender — diverifikasi: Execution
     summary paralel lengkap dengan status per artefak.
  3. Urutan paralel ≠ serial. Perbaikan: `wsFlushPrefix` mengikuti urutan runner serial —
     fase library batch-per-level (`work[i].level`), fase binary datar worklist; peluncuran
     anak TIDAK berubah (penjadwal siap-dep tetap menindih proyek independen).
  - Terverifikasi: `SATRIA_WS_PAR=1` vs `SATRIA_WS_PAR=0` pada fixture segar — struktur
    blok, baris kosong, urutan proyek (`> core` lalu `> bade`), dan ringkasan identik.
- **F11. O4 — status checksum tidak diekspos.** FIXED: `packChecksumApply()` merekam
  status per file ke report — `REP_CACHED` bila checksum mtime memenuhi syarat,
  `REP_BUILT` setelah `packWriteSha256()` sukses. Verifikasi: cold → `*.sha256 (built)`;
  incremental dengan paket tak berubah → `*.sha256 (cached)` (paket juga `cached`).
- **F12. O6 — header no-op selektif berbeda dari design 9b.** FIXED/diverifikasi:
  output `-w core` (gerbang global hit) kini persis contoh 9b — `Build report` + `* Build
  with cached state`, tanpa baris `Toolchain`/`CompDB` (jalur gerbang tidak melewati
  header report aktif).

### Sesi 03/11 — F13, F14

- **F13. Peringatan kecil pada header Build Report.** Build selektif (`-w <proyek>`)
  pada workspace yang belum pernah terbangun penuh mencetak baris non-kunci setelah
  `Toolchain`/`CompDB` (dan `Fingerprint` bila ada):
  - `! Workspace never built — <proyek> may be out of sync` (cakupan NONE —
    `fingerprint.cache` belum ada),
  - `! Workspace not fully built — <proyek> may be out of sync` (fp parsial).
  Implementasi: `buildReportSetHeaderNote()` (`src/log/build_report.*`), dipasang di
  `workspaceRun` sesudah `buildReportBegin()` berdasarkan `wsCacheCoverage()`;
  tidak tampil bila cakupan penuh dan tidak dipasang pada build penuh (build penuh
  sendirilah yang melengkapi cakupan). Terverifikasi pada fixture miniws:
  peringatan tampil pada `-w bade`/`-w core`/`-w rupamod` dingin/parsial dan hilang
  setelah cakupan penuh. Didokumentasikan di `design/build_report.md` ("Peringatan
  kecil pada header").
- **F14. Fingerprint parsial — no-op penuh PERTAMA hit setelah build selektif dari
  workspace dingin.** Akar: `wsCacheSaveBody` melakukan `if (sel && !g_prevFp) return;`
  — build selektif pada workspace tanpa simpanan sebelumnya tidak menulis apa pun,
  sehingga tiga run selektif menyisakan workspace TANPA `fingerprint.cache` dan run
  penuh pertama selalu masuk jalur penuh (walau nol kompilasi — semua cache per
  proyek hit) lalu run kedua baru STATUS. Perbaikan:
  - Simpan selektif tanpa simpanan kini membuat fingerprint BARU berisi entri closure
    (parsial) dan menghapus `fp.cover`; simpan penuh menulis `fp.cover = full`
    (baris stempel kini ditulis termasuk grup kosong, size 0); simpan selektif dengan
    simpanan lama mempertahankan cakupan; kegagalan simpan membuang fp parsial baru
    (`if (!sel || !g_prevFp)`).
  - Guard anti-HIT-palsu di `wsCacheNoopValid(m, need)`: stempel grup WAJIB ada
    untuk proyek wajib (penuh: semua proyek model; selektif: closure) — proyek yang
    belum pernah tercatat -> `coverage missing` -> MISS (walk hanya menilai entri yang
    ada, jadi tanpa guard ini fp parsial bisa mengklaim "Nothing to build" untuk
    workspace yang belum lengkap). P-fast hanya dipakai bila cakupan FULL
    (`fp.cover`), selain itu jalur penuh menilai; walk penuh yang sah menulis
    `fp.cover` sehingga state parsial kembali cepat.
  - Verifikasi: (a) urutan `-w bade` (gagal link, cdeps) / `-w core` / `-w bade` /
    `-w` / `-w` -> run penuh PERTAMA = STATUS `Nothing to build`, run kedua HIT;
    (b) `-w rupamod` saja lalu run penuh = MISS (membangun core & bade — tanpa HIT
    palsu), run berikutnya HIT; (c) peringatan header muncul di (a) dan hilang
    setelah cakupan penuh; (d) `tests/unit/selective_noop_full.sh` PASS dan suite
    penuh exit 0, 19/19, nol FAIL.

## EVALUASI (tanpa perubahan kode)

- **O5. Fase arsip workspace "serial sebelum library".** Bukan cacat pada implementasi
  sekarang: TIDAK ada fase arsip global terpisah. Proyek arsip-only berjalan di dalam
  FASE 1 (`wsRunBatch`) dan diatur penjadwal siap-dep (`wsDepsDone`): anak arsip
  diluncurkan segera saat dependensinya selesai, TIDAK memakai pool compile
  (`pj = 1`), sehingga menindih paralel dengan proyek independen (komentar workspace.c:
  "core berjalan menindih tanpa menunggu maupun ditunggu"; gain terdokumentasi di
  bench/ws.sh). Serialisasi yang tersisa = (a) semantik dependensi — proyek yang
  meng-embed archive WAJIB menunggu arsipnya (benar), (b) runner serial foreground
  yang memang serial by design (mode realtime default). Tanpa perubahan.
- **O7 (sebagian). Validasi lintas platform & kegagalan tulis report.**
  - Compound literal C99 `&(CsEntry){0}` (satu-satunya di tree) diganti variabel
    bernama di `ws_cache_save.c` — risiko mode-C MSVC lama.
  - Pola warning MSVC kini benar-benar dikenali: `: warning C<digit>` (cabang lama
    mengecek digit tepat setelah `"warning "`, yang pada `warning C4996` adalah huruf
    `C` — dead code). `cmdsLineIsWarning()` menerima `C` opsional + digit. GCC/Clang
    tidak berubah (terverifikasi suite).
  - Kegagalan tulis report (Linux): rintangan pada `.satria/report.blob.tmp` TIDAK
    mengubah exit code build (rc=0, `Status : Success` tetap tercetak), dan blob
    pulih sendiri pada run berikutnya (`remove(RB_TMP)` di `reportBlobBegin`).
    Kegagalan rename atomik sulit diproduksi (tmp & target satu direktori).

## OPEN

- **O7 (sisa). Windows/MSVC nyata.** Belum dijalankan di mesin Windows: `cl.exe`
  (`warning C4996:` end-to-end), jalur tanpa relay tee (`#ifdef _WIN32` build.log),
  runner serial Windows, dan warning count penuh di MSVC. Perlu mesin/CI Windows.

## Keputusan & catatan pembaruan

### O1 (akar penyebab) — SELESAI (lihat F8)

`src/workspace/workspace.c` kini memanggil `wsCacheSaveSel(&m, wsName, cidx, cn)`
dengan indeks closure pada build selektif; merge per proyek diimplementasikan di
`ws_cache_save.c` (closure dihapus lalu ditulis ulang dari walk segar; entri proyek
luar closure + grup workspace dipertahankan; `Buildfile.ws` tidak disentuh).
Perbaikan naif yang tidak aman (simpan semua stamp `m`) tidak dipakai.

### WARN dan build.log — PUTUSAN (lihat F9)

- Baris WARN: **hanya saat `log = true`**, sebagai PENGGANTI output mentah di report
  (bukan duplikasi); `log = false` tanpa WARN.
- Sumber WARN = buffer capture yang sama dengan hitungan warning, bukan scan build.log.
- build.log tetap sumber untuk `satria log`; kini berisi buffer lengkap (diagnostik +
  konteks) saat ada diagnostik. Relay stderr (linker/pesan satria) tidak berubah.

### Urutan keluaran paralel (SATRIA_WS_PAR=1) — SELESAI (lihat F10)

- Output per proyek utuh: heading `> Nama` dicetak induk sebelum dump slot; child
  staging tidak mencetak heading; hitungan fase binary tidak digandakan.
- Urutan mengikuti runner serial: lib pass batch-per-level dependensi, binary pass
  datar worklist — paralel dan serial kini identik pada fixture yang sama.
- Diagnostic paralel menempel pada blok proyeknya lewat slot; dengan `log = true`
  warning tampil sebagai baris WARN yang juga tertata per proyek.

### Closure build selektif vs cdeps — PUTUSAN (A+D + flag opt-in)

- **Diskusi.** cdeps sengaja tidak memperluas closure (kontrak eksplisit di
  `wsTopoSort`, dijaga `workspace_selective_target.sh`): cdeps = header/library
  yang dikonsumsi, bukan instruksi membangun pemasok. Dari workspace dingin,
  build selektif bisa gagal link (fixture miniws: `-w bade` butuh `lib/librupa.a`).
  Opsi dievaluasi: A status-quo+warning, B ekspansi transitif penuh (mengubah
  kontrak; cdeps siklik ≈ build penuh), C bangun on-demand saat link gagal
  (rumit, tak deterministik), D perbaikan pesan error.
- **Keputusan: A + D + flag opt-in baru `--with-cdeps`.**
  - Default TIDAK berubah: tanpa flag, closure tetap depends_on + `library.<os>` +
    pack deps; `-w core` tetap tidak menarik `bade` (tes lama hijau).
  - **D (pre-flight warning)**: provider cdeps di luar closure yang library-nya
    belum ada memicu warning di stderr sebelum build — menyebut nama provider,
    path library yang hilang, dan saran `satria -w` / `satria -w <t> --with-cdeps`.
    Tidak tampil bila library provider sudah ada (prebuilt) atau provider ikut
    closure; warning-only, tidak mengubah worklist/cache.
  - **`--with-cdeps` (opt-in)**: closure selektif ikut menarik provider cdeps
    transitif (fixpoint `wsExpandCdeps` bergantian dengan `wsExpandLibDeps`;
    aman untuk cdeps siklik). Hanya berlaku dengan `-w` (tanpa -w: rc 2);
    pada build penuh sifatnya no-op.
- **Implementasi.** `bootstrap.c/h` (parse+validasi), `satria.c` ->
  `workspaceRun(..., withCdeps)` -> `wsRunCore`; `wsCdepsProviders` (ws_cdeps.c)
  mengenal dua bentuk baris (`cdeps = ` dan `library.cdeps = `) + baris global di
  commonLines; `wsExpandCdeps` (ws_worklist.c); peringatan D di `workspace.c`
  (`wsWarnMissingCdepsLibs`). Help: `cmd.txt` (topik workspace + help utama).
- **Verifikasi.** `/tmp/miniws` dingin: `-w bade` gagal link + peringatan D;
  `-w bade --with-cdeps` sukses menarik `core` (blok `> core`, `lib/librupa.a`);
  run kedua no-op; `-w core --with-cdeps` menarik `bade` (siklus); `-w core`
  tanpa flag tetap core-saja. Suite unit **20/20 lulus** (tes baru
  `tests/unit/cdeps_flag.sh`).

## O8. `CLOCK_MONOTONIC` di jalur Build Report  CLOSED

- **Gejala.** Build lintas-platform ke Windows gagal: `'CLOCK_MONOTONIC' undeclared`. MSVC
  juga tidak mendefinisikan konstanta itu (C2065). Build Linux tidak terpengaruh, sehingga
  tidak tertangkap tes.
- **Lokasi.** `log/report_blob.c` (rbT0 dan timer total time), `cmds/cmds_state.c` (timer
  no-op fallback), `workspace/workspace.c` (gateT0 dan parameter `wsNoopGroupedPrint`).
- **Perbaikan.** Semua pemakaian `clock_gettime(CLOCK_MONOTONIC, ...)` diganti `portMonoMs()`
  dari port layer (ms). Aturan proyek: modul lintas-platform tidak memanggil
  `clock_gettime` langsung (`port/port_time.c`).
- **Verifikasi.** Tidak ada sisa `CLOCK_MONOTONIC` di luar `port/` dan `portability.*`;
  build host bersih; `tests/unit` 18/18; miniws menampilkan `Total time`.
- **Catatan.** Verifikasi di Windows nyata tetap di O7.

## O7 (dipecah)  O7a CLOSED, O7b CLOSED

Item O7 yang tercatat sebelumnya digabung. Dipecah menjadi dua bagian dengan status berbeda:

### O7a. Kompilasi dan link lintas-platform ke Windows  CLOSED

- Toolchain: `x86_64-w64-mingw32-gcc-13-win32` (GCC 13 mingw), sysroot mingw-w64 11.0.1
  (`mingw-w64-x86-64-dev`, `mingw-w64-common`, `binutils-mingw-w64-x86-64`,
  `gcc-mingw-w64-x86-64-win32-runtime`).
- Hasil: 91 file terkompilasi, 0 error, 0 warning. Link menghasilkan `bin/satria.exe`
  `PE32+ executable (console) x86-64`. Impor: KERNEL32, msvcrt, SHELL32 (tanpa ws2_32,
  sesuai tidak adanya pemakaian Winsock).
- Warning yang dibersihkan di `src/package_cli.c`: `unused parameter 'argv0'` (ditandai
  `(void)argv0`) dan `'pathJoin' defined but not used` (dibungkus `#ifndef _WIN32`, karena
  hanya dipakai cabang POSIX `#else`). Host Linux: 0 warning.
- Catatan `install.sh`: `rc=1` pada cross-build terjadi karena langkah `chmod bin/satria`
  setelah build (output mingw bernama `satria.exe`). Bukan kegagalan build.

### O7b. Runtime di Windows / MSVC nyata  CLOSED (03/11/2026)

Dieksekusi lewat CI, bukan mesin lokal (proot tidak bisa memasang MinGW). Bukti:
`build-manual.yml` run **41638258152** pada commit `c837775`, `workflow_dispatch`,
simpulan **success**, 7/7 job hijau. Job Windows: `build (windows-latest, cl)`
(id 125924856229) dan `build (windows-latest, gcc)` (id 125924856413).

- **`cl.exe` build penuh via `install.ps1 --dev`.** `> Bootstrap : satria v0.2.4`,
  kompilasi + link sukses; `warning C4996:` end-to-end terbukti (mis.
  `src/bootstrap.c(404): warning C4996: 'getenv' ...`, `background.c(310):
  warning C4244`).
- **Hitungan warning MSVC.** Job cl melaporkan `* 91 compiled, 0 skipped, 180 warnings`
  dan 180 baris diagnostic benar-benar tercetak — `cmdsLineIsWarning()` mengenali
  pola `: warning C<digit>` di output `cl.exe` (bukan dead code lagi). Job gcc-Windows
  memberi 2 warning GCC (`%s directive output may be truncated` di
  `cmds_passfinish.c:118` dan `:292`), bukan warning yang sama dengan Linux
  (`ws_cache_noop.c:47`, 1 warning) — beda jalur kode platform, bukan beda penghitung.
- **Smoke test tampil.** `satria version` → `satria v0.2.4`; `satria help` mencetak teks
  penuh (`Satria - A simple builder for you`, `> usage: satria ...`, baris `Topics:`)
  di KEDUA toolchain — setelah perbaikan CRLF di bawah.
- **Self-host build + incremental.** `$SELF_BIN -j4` (bootstrap) dan `./bin/satria`
  (binary hasil self-host) keduanya exit 0 dengan `Status : Success`, termasuk
  `bin/satria.exe` tertaut dan `dist/` terbentuk.
- **`warning C4996` sengaja TIDAK diperbaiki.** 180 warning MSVC itu nyata
  (`getenv`/`strdup`/`_CRT_SECURE_NO_WARNINGS` dsb.), tetapi perbaikannya bukan
  syarat O7b: yang diuji adalah pengenalan dan penghitungan warning, bukan
  kepatuhan terhadap rekomendasi MSVC.

**Yang tidak dicakup CI ini** (tetap belum dieksekusi, bukan regresi):
`satria.exe -w` pada `tests/miniws` di Windows dan `scripts/unit.sh` di Git Bash
Windows — workflow belum memuat langkah itu. Jalankan bila ingin menutupnya.

**Temuan baru dari CI ini — O9.** Langkah `Self-host incremental (semua up-to-date)`
juga melaporkan `* 91 compiled, 0 skipped` di kedua toolchain Windows (di Linux run
kedua mencetak `STATUS / Build : Nothing to build`). Lihat O9.

#### O7b — temuan dari CI Windows (build-manual.yml): `satria help` kosong  FIXED

- **Gejala.** Langkah `Smoke test help` (`./bin/satria help`) di runner Windows keluar
  tanpa satu byte pun; topik (`satria help workspace`) juga tidak ditemukan.
- **Akar penyebab.** `src/cmdhelp.c` mencocokkan nama section dari baris MENTAH. Checkout
  Windows memakai terminasi CRLF (git `core.autocrlf`), sehingga baris marker terbaca
  `---help\r`: `nameLen = lineLen - 3` ikut menghitung `\r` (5 != 4) dan `strncmp` gagal.
  `cmdHelpMain()` mengabaikan nilai balik `helpPrintSection()`, jadi hasilnya sunyi total —
  bukan fallback teks, bukan pesan error. Terbukti di Linux dengan salinan cmd.txt CRLF
  (`SATRIA_CMD=... help` = 0 byte, sebelum perbaikan).
- **Perbaikan.** `cmdReadFile()` menormalkan `\r\n` dan `\r` tunggal menjadi `\n` setelah
  membaca file. Selain memperbaiki pencocokan marker, ini juga mencegah `\r\r\n` di layar:
  isi section dicetak lewat `fwrite` ke stdout mode teks Windows, yang sudah menerjemahkan
  `\n` menjadi CRLF.
- **Verifikasi.** `tests/unit/cmdhelp_crlf.sh` (baru): cmd.txt CRLF -> `help` tidak kosong,
  memuat baris usage, `help workspace` menemukan topiknya, keluaran bebas `\r`, dan identik
  dengan keluaran cmd.txt LF. Host Linux: PASS; build self-host bersih. CI Windows
  (run 41638258152, kedua toolchain): teks help penuh tercetak.

## O9. Build kedua di Windows selalu dingin (no-op tidak pernah hit)  OPEN

- **Gejala.** Di CI Windows, langkah `Self-host incremental (semua up-to-date)` tetap
  melaporkan `* 91 compiled, 0 skipped` (cl: 180 warnings; gcc-Windows: 2 warnings),
  `bin/satria.exe` ditautkan ulang, dan `dist/` dibangun ulang — tidak ada baris
  `STATUS` / `Build : Nothing to build`. Bukti pembanding dari run yang SAMA: job
  `build (ubuntu-latest, gcc)` mencetak `Build        : Nothing to build` pada langkah
  yang identik, dan secara lokal `./bin/satria` (kedua kali) mencetak `STATUS` +
  `Cache valid : .satria/caches/fingerprint.cache`.
- **Pengaruh.** Bukan kegagalan (exit 0, `Status : Success`), tetapi setiap run Windows
  membayar build penuh — dan langkah CI "semua up-to-date" tidak membuktikan apa pun
  karena hanya memeriksa exit code, bukan keluaran.
- **Dugaan akar penyebab.** (a) granularitas waktu: `csNowNs()` di
  `src/cache/cache_store.c` memakai `time(NULL) * 1e9` di `_WIN32` (resolusi detik),
  sementara stamp file memakai `fsMTimeNs` (100 ns) — perbandingan "lebih baru" bisa
  selalu benar; (b) state/fingerprint tidak persisten di Windows (tulis/ganti atomik
  gagal diam-diam, sehingga tiap run mulai dingin); (c) ada input yang mtime-nya berubah
  tiap run (mis. `build/version.h` hasil codegen).
- **Langkah berikutnya.** (1) Tambahkan assert ke step CI:
  `./bin/satria | grep -q "Nothing to build"` — jadikan step merah bila tidak cocok;
  (2) bandingkan `.satria/caches/fingerprint.cache` (dan `startup.state`) antara dua run
  di Windows; (3) cek apakah `csNowNs` menjadi pembeda dengan membandingkan perilaku
  setelah memakai resolusi sub-detik (`GetSystemTimeAsFileTime`/`QueryPerformanceCounter`).
- **Catatan.** Diuji hanya lewat log CI; reproduksi lokal butuh mesin Windows.

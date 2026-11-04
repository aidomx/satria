# Build Report

## Tujuan

Build Report adalah format keluaran utama untuk hasil build single-project dan workspace. Report menyatukan informasi yang sekarang tersebar pada output `Build with`, `Embedded`, `Library`, `Linking`, `Package`, dan blok `Summary` menjadi satu laporan yang mudah dipindai.

Report adalah lapisan observasi. Ia menerima hasil operasi yang sudah diputuskan oleh mesin build; ia tidak menentukan freshness, fingerprint, P metadata, `opts.cache`, fast-state, dependency, keputusan compile/link, urutan worklist, atau kebijakan release.

Prinsip implementasi:

- Tidak mengubah keputusan incremental build atau urutan fase.
- Status artefak dicatat pada titik operasi mengetahui hasil sebenarnya.
- Ukuran hanya dibaca untuk artefak yang sudah tersedia; tidak ada walk, hash, atau validasi tambahan khusus report.
- Single-project dan workspace memakai istilah serta format status yang sama.
- Detail compile tetap tersedia, tetapi ringkasan artefak tidak bergantung pada parsing teks detail tersebut.
- Build gagal tidak menggantikan ringkasan sukses terakhir yang dipakai untuk no-op.
- Build normal (cold maupun incremental) memakai format `Build Report` dengan status artefak `built`, `cached`, dan `linked`.
- Fast no-op TIDAK memakai format `Build Report` dengan status artefak per operasi. Bila blob ringkasan ada, no-op memutar `LAST BUILD SUMMARY`. Bila blob tidak ada, no-op memakai layout fallback gabungan (bagian 9a), yang menampilkan target dan library sebagai `cached`.
- Ringkasan no-op adalah data historis dari build sukses terakhir; status `Nothing to build` dan `Cache valid` menjelaskan run saat ini, bukan hasil build historis.

## Catatan implementasi

Logika build report dengan simulasi :

```text
Build report -> wrapper utama
------------
# archive
# Execution
# Execution summary
... dan seterusnya
```

adalah anak dari build report, sedangkan fase compile yang sebelumnya masuk fase library (sedikit tidak masuk akal karena fase library tapi compile) berpindah ke fase execution, di format lama fase per-project berpindah menjadi anak dari fase execution yang fokusnya compile, di format lama hasil dari fase per-project langsung ditampilkan seperti Library: ..., di format baru di tunda untuk digunakan di fase execution summary dan ini sudah benar implementasinya.

Jadi selama menjaga build report sebagai wrapper utama urutan akan terjaga dengan sendirinya.

Simulasi :
```text
Build report
------------
keterangan (toolchain dan kawan-kawan)
-> archive
-> execution
  -> proj 1
  -> proj 2
-> execution summary (ambil data hasil dari execution)
-> dan seterusnya
```

Untuk archive,execution,execution summary itu masih satu fase yaitu fase compile yang tidak tercetak keluar karena digunakan oleh sistem yang ditampilkan adalah anaknya.

Dengan parent fase compile archive tidak akan terpisah,ini untuk menjaga paralellisme.

Ini sama seperti README.md, contoh :
```
# Nama project
Keterangan
# header 1
# header 2
```

## Status verifikasi kode

Desain ini dicocokkan dengan source `v0.2.4` sebelum implementasi penuh. Sebagian besar klaim sesuai. Celah yang ditemukan sudah dimasukkan ke bagian terkait, dan keputusan perilaku pemilik dicatat di bawah.

### Hasil verifikasi klaim

| Klaim desain | Status | Bukti |
|---|---|---|
| File yang disebut ada (kecuali `build_report.*` baru) | Sesuai | Semua file tersedia |
| `writeCompdb()` dan `writeCompdbMulti()` mengembalikan `void` | Sesuai | `compdb.c`, `compdb.h` |
| `wsCompdbRefresh()` mengembalikan `bool` | Sesuai | `workspace/ws_*` |
| `compileLibraryOne(..., ProcHandle *cap)` | Sesuai | `compile.c:329-330` |
| `reportBlobFinish` atomik: tee, condense, `fsRenameReplace`; laporan lama dipertahankan saat gagal | Sesuai | `log/report_blob.c:287-326` |
| `packChecksumApply()` mengetahui status per file | **Tidak sesuai**: `static bool`, tidak diekspos | `pack/pack.c:174` |
| `packRun()` / `packRunAt()` hanya mengembalikan boolean | Sesuai | `pack/pack.c:203-209` |
| Runner paralel memakai `fork()` dan pola stage | Sesuai, dan pola sudah ada | `runners/parallel.c:188-246` |
| Baris `Build with`, `Embedded:`, `Library :`, `Package :`, `Checksum:` | Sesuai | `cmds_pass.c:201,203,315,331`; `cmds_lib.c`; `pack.c:185-284` |
| `cmdBuildEx`, `cmdsBuildPassRun`, `cmdsBuildPassFinish` | Sesuai | `cmds_build.c`, `cmds_pass.c`, `cmds_passfinish.c` |

### Celah yang dimasukkan ke desain

1. **Jalur no-op per proyek dalam workspace** (`wsFastStateValid` di `cmds/cmds_wsstate.c:575-580`) tidak ada di tabel implementasi. Jalur ini mencetak `Build with cached state`, `Linking : ... (up-to-date)`, `Library :`, dan `Summary :` untuk setiap proyek yang fast-state-nya hit. Dicantumkan di tabel file (bagian 9a dan 9b). Keputusan: di build selektif diganti format baru (9b).
2. **Pola stage paralel sudah ada.** Setiap child menulis `.satria/ws-state-<pid>-<seq>.cache` (`stateBlobExport`) dan `.satria/ws-verdict-<pid>-<seq>` (`wsVerdictChildExport`). Parent mengimpor keduanya saat `waitpid` (`stateBlobImport`, `wsVerdictParentImport`), lalu mencetak output slot sesuai worklist (`wsFlushPrefix`). Fragment report sebaiknya menjadi stage ketiga dengan pola yang sama.
3. **Jalur serial fallback di paralel.** Bila `fork` gagal, proyek dijalankan serial di parent (`parallel.c:256-265`). Record report dari jalur ini harus masuk langsung ke konteks parent. Windows (`#else`, `parallel.c:271`) selalu serial.
4. **Arti `skipped`.** `META <jumlah> <target> <bf>` di `ws.state` menyimpan `srcs->count`, yaitu total unit compile termasuk `embedded.c` hasil codegen (`cmds_wsstate.c:413`). Single project menghitung `compiled + skipped`. Keduanya total unit compile. miniws: `bade` = 4 unit (3 source + `build/bade/embedded.c`), `core` = 2.
5. **Path target relatif ke root workspace**, bukan folder proyek. Bukti: cwd di blob `ws.state` sama dengan root workspace, dan target `bin/rupa`, `bin/bade`.
6. **Blob state per proyek** dibaca dengan `stateBlobGet(nama_proyek, "ws.state", ...)`, tersimpan di `.satria/caches/state.cache`.
7. **Warning serial tidak tertangkap.** Jalur serial memakai `compileLibraryOne(..., NULL)` sejak perbaikan audit, supaya warning compiler langsung tampil dan warna TTY tetap benar (`diagColorWanted` memakai `isatty(2)`). Menangkap output serial mengubah perilaku ini.
8. **Warning paralel** ada di stderr child yang mengalir langsung ke terminal (`parallel.c:191-197`). Jumlah warning harus dikirim lewat file stage child, bukan dihitung dari stdout slot.
9. **Ringkasan tersimpan dihitung dari teks.** `rbCondense()` (`report_blob.c:145-227`) menghitung `Archives`, `Libraries`, `Binaries`, dan `Status` dari teks log. Ini diganti record eksplisit (bagian 1).

### Keputusan perilaku pemilik

- **Single project, no-op**: blob ada, `reportBlobShow()` (format lama). Blob tidak ada, layout fallback gabungan (9a).
- **Workspace, no-op gerbang global**: blob ada, `reportBlobShow()`. Blob tidak ada, layout fallback gabungan (9a) dengan data per proyek dari `ws.state`.
- **Workspace, build selektif** (`-w <proyek>`): format Build Report baru (9b). Blok per proyek lama tidak dicetak lagi.
- **Baris WARN** (O2): hanya saat `log = true`, menggantikan output mentah di report; detail penuh ke build.log. Lihat §5. `log = false`: tanpa WARN, output mentah utuh.

### Dampak ke tes

Sebelas tes bergantung pada string keluaran yang terdampak: `background_miniws`, `cache_noop_miniws`, `crud_touch_delete`, `faststate_external_header`, `legacy_sweep`, `opts_sources_sync`, `p_meta_noop`, `report_artifact`, `report_binaries_uptodate`, `router_noop_miniws`, `workspace_selective_target`. Setelah perubahan fallback no-op (single dan workspace), `tests/unit` 18/18 dan `scripts/unit.sh` 237 PASS. Asersi `report_artifact.sh` memakai blob yang dibuat build nyata, sehingga tidak terdampak fallback.

### Item terbuka sebelum implementasi penuh

- **Capture serial**: pilih antara capture yang tetap meneruskan stderr ke terminal, atau menghitung warning dengan cara lain. Warna TTY dan `build.log` tidak boleh rusak.
- **Warning di child paralel**: dikirim lewat file stage child.
- **Status checksum**: ekspos hasil `packChecksumApply()` per file (`pack.c`).
- **Status CompDB**: `writeCompdb` dan `writeCompdbMulti` mengembalikan status (`compdb.c`).
- **Persistensi report di Windows**: tidak ada tee; dukungan terpisah.

## Format keluaran

### Single project

```text
startup
-------
* loading configuration: done
* preparing execution: done

Build Report
------------
Toolchain   : gcc -Wall -Wextra -MMD -MP -std=gnu11
CompDB      : compile_commands.json refreshed
Fingerprint : state build tidak terbaca; semua object dikompilasi ulang

# Execution
[1/1] OK 0.77s src/main.c

* 1 compiled, 0 skipped

# Execution summary
> bin/
* a (SIZE,built)

# Release
* dist/a.deb (SIZE, built)
* dist/a.deb.sha256 (built)

# Summary
Total time : 5s
Status     : Success
```

### Workspace

```text
startup
-------
* discovery: done
* resolving dependencies: done
* indexing sources: done
* preparing execution: done

Build Report
------------
Toolchain : gcc -Wall -Wextra -MMD -MP -std=gnu11
CompDB    : compile_commands.json refreshed

# Archive
* modules/rupa_modules.tar (12KB, cached)
* modules/image_modules.tar (1MB, built)

# Execution
> Bade
* Embedded config changed -> recompiling all sources
[1/3] OK 0.56s bade/manager/main.c
[2/3] OK 0.70s bade/src/bade.c
WARN bade/src/bade.c:9:13: 'sayHelloWorld' defined but not used
[3/3] OK 0.88s bade/src/embed_use.c

* 3 compiled, 0 skipped, 1 warning

> Core
[1/2] OK 1.15s core/src/core.c
[2/2] OK 1.29s core/src/main.c

* 2 compiled, 0 skipped

# Execution summary
> lib/
* libbade.a  (124KB, built)
* libbade.so (98KB, built)
* librupa.a  (72KB, built)
* librupa.so (61KB, built)

> bin/
* bade (240KB, linked)
* rupa (180KB, linked)

# Release
* dist/rupa-v0.2.2.deb (1.2MB, built)
* dist/rupa-v0.2.2.deb.sha256 (built)

# Summary
Total time : 2m50s
Status     : success
```

Angka ukuran di atas hanya contoh. Ukuran aktual ditampilkan dalam satuan yang konsisten. File checksum boleh tidak menampilkan ukuran.

### Status artefak

Status adalah fakta untuk artefak pada run yang sedang dilaporkan:

- `built`: archive, library, atau package dibuat/diperbarui pada run ini.
- `cached`: artefak telah dinyatakan valid/up-to-date dan tidak dibuat ulang pada run ini. Termasuk archive prebuilt yang dipakai tanpa regenerasi.
- `linked`: binary berhasil di-link pada run ini. Binary yang tidak perlu di-link memakai `cached`.

Status tidak boleh ditentukan hanya dari keberadaan file. Status `built`/`linked` baru dicatat setelah operasi berhasil. Status `cached` hanya dicatat setelah jalur freshness/fast-state yang berlaku menyatakan artefak dapat dipakai. Artefak gagal atau hilang tidak masuk daftar artefak sukses.

Status `built`/`cached`/`linked` hanya digunakan pada report cold/incremental yang benar-benar menjalankan lifecycle build. Pengecualian: layout fallback no-op (bagian 9a) menampilkan `cached` untuk target dan library. Status `cached` di sana berarti artefak lolos pemeriksaan freshness pada run tersebut. Status ini tidak digunakan pada replay blob no-op. Pengecualian: layout fallback no-op (9a). Fast no-op memakai format ringkasan lama di bawah, tanpa daftar artefak dan tanpa status per artefak.

### Urutan dan hierarki

Urutan bagian:

1. Header (`Toolchain`, `CompDB`)
2. `Archive`
3. `Execution`
4. `Execution summary`
5. `Release` jika fase release benar-benar dijalankan
6. `Summary`

`#` menandai bagian, `>` menandai kelompok/proyek, dan `*` menandai artefak atau ringkasan proyek. Prefix terminal tetap mengikuti `repPrefix()` untuk kompatibilitas terminal non-UTF-8. Nama bagian, label status, dan struktur report tidak bergantung pada prefix tersebut.

### Peringatan kecil pada header

Header boleh memuat baris non-kunci `! <pesan>` (setelah `Fingerprint` bila ada) untuk peringatan yang tidak mengubah status build. Saat ini satu peringatan: build selektif (`-w <proyek>`) pada workspace yang belum pernah terbangun penuh — cakupan fingerprint `NONE`/`PARTIAL` — dicetak:

```text
Build Report
------------
Toolchain   : gcc -Wall -Wextra -MMD -MP -std=gnu11
CompDB      : compile_commands.json refreshed
! Workspace never built — bade may be out of sync
```

Varian kedua bila fingerprint ada tetapi belum mencakup semua proyek: `! Workspace not fully built — <proyek> may be out of sync`. Bila cakupan penuh, baris tidak tampil. Build penuh tidak memasang peringatan (ia sendiri yang melengkapi cakupan).

### Lifecycle single-project

- `cmdBuildEx()` berada di `src/cmds/cmds_build.c`.
- Gerbang fast-state berada sebelum `reportBlobBegin()`. Karena itu fast no-op single-project keluar lebih awal dan report tersimpan ditampilkan melalui jalur state (`src/cmds/cmds_state.c`), bukan menjalankan lifecycle build baru.
- Setelah gerbang no-op lolos, `cmdBuildEx()` memanggil `reportBlobBegin()`, memuat konfigurasi, resolve compiler, berpindah ke root proyek, lalu menjalankan `cmdsBuildPassRun()`.
- Hasil pass diteruskan ke `reportBlobFinish(result == 0)`. Report sukses disimpan; report gagal dibuang dan report sukses lama dipertahankan.
- `cmdsBuildPassRun()` di `src/cmds/cmds_pass.c` menangani archive-only project, embedded archive/codegen, source collection, fingerprint, CompDB single-project, keputusan compile, dan kemudian menyerahkan ekor pass ke `cmdsBuildPassFinish()`.
- `cmdsBuildPassFinish()` di `src/cmds/cmds_passfinish.c` menangani library pass, keputusan link, linking, library output, packaging, dan ringkasan pass.

### Lifecycle workspace

- `workspaceRun()` / `wsRunCore()` di `src/workspace/workspace.c` memiliki lifecycle global workspace.
- Gerbang `wsCacheNoopValid()` terjadi sebelum `reportBlobBegin()`. Pada hit, `reportBlobShow()` menampilkan ringkasan no-op tersimpan dalam format `STATUS` + `LAST BUILD SUMMARY`; bila belum ada ringkasan, workspace mencetak layout fallback gabungan (bagian 9a). Jangan render format `Build Report` atau label status artefak pada jalur ini.
- Build penuh memanggil `reportBlobCachePath(WS_CACHE_FP)` lalu `reportBlobBegin()`.
- `wsCompdbRefresh()` dijalankan satu kali sebelum fase build proyek.
- Fase library menjalankan `wsRunBatch(..., libOnly=true)`; fase binary berjalan setelah seluruh library tersedia dengan `wsRunBatch(..., libOnly=false)`. Proyek archive-only hanya bekerja pada fase library dan tidak dimasukkan ke batch binary.
- Build selektif memakai worklist closure yang dipilih. Release normal tidak dijalankan pada build selektif; release eksplisit tetap dapat dijalankan sesuai aturan `workspace.c`.
- `wsRelease()` menjalankan release workspace setelah fase build bila dikonfigurasi dan memenuhi syarat.
- `reportBlobFinish(rc == 0)` dipanggil sebelum model dibebaskan.
- `runnerSerial()` menjalankan proyek in-process. `runnerParallel()` menjalankan proyek melalui child process dan menampung stdout masing-masing proyek pada slot sementara, lalu mengeluarkannya sesuai urutan worklist. stderr compiler tetap mengalir langsung. Ini penting untuk desain report: state report yang hanya berada di memori parent tidak otomatis dibagi ke child.

### Output dan statistik compile

- Keputusan `compiled`/`skipped` dibuat di `src/cmds/cmds_pass.c`.
- Compile serial saat ini memanggil `compileLibraryOne(..., NULL)`, sehingga output compiler langsung diwariskan ke terminal dan tidak tertangkap.
- Jalur paralel memakai `ProcHandle` capture. `cmdsEmitCapture()` mengambil buffer lewat `procCaptureTake()`, mencetak output (bila `log = false`; dengan `log = true` menggantinya dengan baris WARN — lihat §5), dan meneruskannya ke `buildLogCapture()`.
- `compileOne()`/`compileLibraryOne()` sudah menerima parameter `ProcHandle *cap`; ini dapat dipakai untuk menyamakan jalur capture serial tanpa menjalankan compiler dua kali.
- Statistik `compiled` dan `skipped` sudah tersedia di pass dan diserahkan melalui `CmdsPassTail` ke `cmdsBuildPassFinish()`.
- Warning belum dihitung sebagai statistik proyek. Warning serial saat ini tidak tertangkap; jalur paralel sudah memiliki buffer output. Penghitungan warning harus dilakukan dari buffer compiler yang sama, bukan dengan membaca ulang build log.

### Artefak saat ini

- Embedded archive: `buildEmbeddedArchivesEx()` di `src/embed_archive.c` mengembalikan satu `anyRebuilt` global. Fungsi internal `buildEmbeddedArchiveEntryEx()` sudah mengetahui per-entri apakah archive prebuilt, fresh, atau dibuat ulang, tetapi hasil per-entri belum diekspos.
- Archive-only project: `cmdsBuildPassRun()` mencetak daftar archive dari `Config.emb[]`, tetapi belum menampilkan ukuran/status per entri. Kegagalan archive atau entri hilang menggagalkan pass.
- Library: `cmdsBuildLibraryEx()` di `src/cmds/cmds_lib.c` sudah mengetahui status `staticUpToDate` dan `sharedUpToDate` secara terpisah. Jalur sukses mencetak `Library : <path>` atau `(up-to-date)`.
- Binary: `cmdsBuildPassFinish()` sudah mengetahui `linkNeeded`, hasil `runCmd()`, target final, dan ukuran target. Jalur up-to-date mencetak `Linking : ... (up-to-date)`; jalur link baru mencetak `Linking : ...` lalu ringkasan target.
- Package: `packRun()` di `src/pack/pack.c` mengetahui setiap package tar/deb up-to-date atau baru dibuat, serta ukuran package. `packLastRebuilt()` hanya memberikan boolean agregat.
- Checksum: `packChecksumApply()` mengetahui path checksum dan status up-to-date/ditulis, tetapi belum mengembalikan record per checksum.
- Release workspace: `wsRelease()` di `src/workspace/ws_release.c` mengonversi `release.*` menjadi `Config.pack` lalu memanggil pack engine yang sama melalui `packRunAt()`.
- CompDB single-project: `writeCompdb()` di `src/compdb.c` mencetak `refreshed`, `unchanged (cache hit)`, atau `restored (blob)`. Bila disabled, fungsi tidak dipanggil dari jalur normal.
- CompDB workspace: `wsCompdbRefresh()` menyusun config/source seluruh proyek dan dapat mencetak `unchanged (workspace)` atau memanggil `writeCompdbMulti()`. Jalur multi dapat diam saat cache hit/restore.
- Toolchain: `cmdsBuildPassRun()` mencetak `Build with <cc> <warning flags> -std=<std>` setelah embedded codegen dan source collection. Ini sumber konfigurasi compile efektif per proyek; workspace dapat memiliki konfigurasi berbeda antar proyek.
- Report blob: `src/log/report_blob.c` saat ini men-tee stdout ke `.satria/report.blob.tmp`, lalu `rbCondense()` mem-parsing format teks lama (`Project`, `Fase`, `Summary`, `Library`, `Archive`, `Target`, `Linking`) untuk menghasilkan `STATUS` + `LAST BUILD SUMMARY`. Parser ini menghitung agregat dari teks dan tidak menyimpan report rinci. Mekanisme tersebut tidak cocok menjadi sumber data format report baru.

## Rancangan implementasi

### 1. Ganti parser teks dengan data report eksplisit

Jangan memperluas `rbCondense()` untuk mengenali format baru. Parser yang mengandalkan baris log lama akan terus rapuh terhadap perubahan label, urutan fase, dan perbedaan output single/workspace.

Tambahkan model report kecil di modul log, misalnya `src/log/build_report.h` dan `src/log/build_report.c`. Model ini bukan framework event umum; cukup record spesifik report:

- `BuildReport`: waktu mulai, status akhir, mode, toolchain/CompDB, kumpulan archive, proyek execution, library, binary, release artifact.
- `ReportArtifact`: path, kind, status, ukuran (opsional), dan urutan tampil.
- `ReportProject`: nama/label proyek, daftar detail compile yang memang ditampilkan, `compiled`, `skipped`, `warnings`, serta flag fase yang sudah dikerjakan.
- `ReportToolchain`: command compiler efektif dan flags compile efektif.
- Enum status artefak: `REPORT_BUILT`, `REPORT_CACHED`, `REPORT_LINKED`.
- Enum CompDB: `REFRESHED`, `UNCHANGED`, `RESTORED`, `DISABLED`, `SKIPPED`, `FAILED`.

API cukup berupa lifecycle dan beberapa operasi spesifik, misalnya `buildReportBegin()`, `buildReportSetCompdb()`, `buildReportAddArchive()`, `buildReportProject...()`, `buildReportAddLibrary()`, `buildReportAddBinary()`, `buildReportAddRelease()`, `buildReportFinish()`. Nama final boleh mengikuti konvensi source. Jangan menambahkan callback/event bus atau alokasi per source bila detail compile masih dapat dicetak langsung.

Gunakan buffer/array yang dibatasi kapasitas yang sudah sejalan dengan batas proyek workspace. Untuk string path gunakan kapasitas `MAX_PATH` yang berlaku. Kegagalan alokasi report tidak boleh menggagalkan build; report dapat ditandai tidak tersedia dan build tetap berjalan.

### 2. Tentukan satu pemilik lifecycle untuk setiap mode

Single-project:

- `cmdBuildEx()` tetap menjadi pemilik lifecycle report.
- Mulai report setelah gerbang `cmdsFastStateValid()` dan setelah keputusan background tidak mengalihkan proses ke child. No-op tidak membuka report baru.
- Setelah `loadConfig()` dan `resolveCompiler()`, set toolchain efektif dari `Config` dan flags yang sama dengan `Build with`.
- Selesaikan report setelah `cmdsBuildPassRun()` mengembalikan hasil. `reportFinish` menerima hasil akhir, bukan menyimpulkan status dari baris `Summary`.
- Jalur gagal pada load config, resolve compiler, root, compile, link, library, atau pack harus menutup report sebagai gagal dan membuang hasil sementara.

Workspace:

- `wsRunCore()` tetap menjadi pemilik lifecycle report workspace.
- Mulai setelah global `wsCacheNoopValid()` dan setelah jalur background parent diputuskan, sebelum `wsCompdbRefresh()`.
- Satu report workspace menampung semua proyek pada fase library dan binary. Jangan membuat report global baru dari `cmdBuildEx()`; workspace menjalankan `cmdsBuildPassRun()` langsung melalui `wsRunProject()`.
- `wsRunProject()` mengirim identitas proyek dan fase ke report sebelum fast-state check. Jika fast-state hit, catat proyek/artefaknya sebagai cached hanya bila konfigurasi dan path output dapat diresolusikan tanpa menjalankan build pass. Jangan membuat blok Execution palsu untuk proyek yang tidak memiliki aktivitas compile.
- Archive-only project dicatat pada bagian Archive dan tidak dimasukkan sebagai proyek binary.
- Karena `runnerParallel()` mem-fork child, jangan mengandalkan satu global `BuildReport` di memori parent. Pilih mekanisme fragment per child: child menulis record report khusus ke file slot yang sudah memiliki identitas proyek, parent mengimpor record saat `waitpid` selesai dan merender berdasarkan urutan worklist. Hindari menulis satu file report bersama dari banyak child.
- `runnerSerial()` dapat mengirim record langsung ke konteks report parent. Bentuk record harus sama dengan fragment child agar hasil serial/paralel identik.
- `wsRunCore()` menambahkan release setelah fase binary, lalu menyelesaikan report setelah release dan cache-save/route-sync selesai. `Total time` mencakup fase yang memang bagian run build, termasuk release.

### 3. Urutan render dan keluaran live

Format final terdiri dari header, Archive, Execution, Execution summary, Release, Summary. Pengumpulan data boleh terjadi sepanjang build, tetapi urutan tampil harus deterministik.

- Header dicetak sekali per build global. Workspace tidak boleh mengulang header untuk setiap proyek.
- Archive record dikumpulkan saat `buildEmbeddedArchivesEx()` memeriksa tiap entri. Tampilkan sebelum Execution pada render final.
- Detail compile tetap dicetak saat proses berjalan atau disimpan sebagai detail proyek untuk dirender pada akhir. Jangan menahan stderr compiler sampai seluruh workspace selesai.
- Execution disusun sesuai urutan worklist, bukan urutan child selesai. Pada workspace, fase library memasok compile counters/detail; fase binary memasok hasil link. Gabungkan keduanya menjadi satu blok per proyek, bukan dua blok proyek yang duplikat.
- Execution summary mengelompokkan library dan binary berdasarkan path output final. Jangan membuat path dengan asumsi nama proyek; gunakan helper path `cmdsLibStaticPath()`, `cmdsLibSharedPath()`, dan resolusi target yang sudah dipakai build.
- Release mengikuti urutan yang dikeluarkan pack engine.
- Summary selalu paling akhir.

Untuk menjaga progres compile tetap live dan sekaligus mendapat urutan report final, pisahkan _capture detail_ dari _render report_: detail compiler boleh tetap diteruskan segera melalui mekanisme yang ada, sementara record ringkas dikumpulkan dan report final dicetak sekali setelah operasi selesai. Jika format mengharuskan header tampil sebelum progres, cetak header saat begin dan render bagian final sesudahnya; jangan mencetak ulang header.

### 4. Archive per entri

Ubah hasil `buildEmbeddedArchivesEx()` secara terbatas agar caller dapat mengetahui status setiap `EmbeddedEntry`, bukan hanya `anyRebuilt`.

- `buildEmbeddedArchiveEntryEx()` sudah memiliki semua cabang keputusan: prebuilt tersedia, `embArchiveFresh()` true, atau archive dibuat melalui tar lalu di-rename.
- Tambahkan output status per entri (`cached` untuk prebuilt/fresh, `built` setelah rename sukses) dan path archive. Jangan mengulang freshness check dari caller.
- Caller memperoleh ukuran dengan `fsFileSize(e->archivePath)` setelah status sukses; ukuran ini hanya satu metadata read per archive yang memang ditampilkan.
- `buildEmbeddedArchivesEx()` dapat menerima array result yang disediakan caller atau callback statis sederhana. Hindari menyimpan pointer ke `EmbeddedEntry` yang berasal dari `Config` setelah lifetime config berakhir.
- Untuk `output.binary = false`, gunakan hasil per entri yang sama. Hapus kebutuhan caller untuk menebak status dari `anyRebuilt` atau hanya mengecek `fsFileExists()`.
- Jika satu entri gagal, pass gagal; record yang sudah sukses boleh tetap berada di report gagal, tetapi report gagal tidak menjadi report no-op terakhir.

### 5. Statistik execution dan warning

Statistik `compiled`/`skipped` tidak perlu dihitung ulang: nilainya sudah tersedia di `cmdsBuildPassRun()` dan disalin ke `CmdsPassTail` untuk `cmdsBuildPassFinish()`.

- Pada `libOnly` workspace, catat compile count dan skipped count untuk proyek. Pada fase binary, jangan menambahkan lagi `skipped = srcs.count` yang berasal dari fast-tail; angka itu bukan compile pass kedua yang ingin ditampilkan.
- Single-project mencatat satu pass yang benar-benar dijalankan. Bila fast-state mengembalikan no-op, tidak ada blok Execution baru.
- Untuk warning, manfaatkan `ProcHandle` dan `procCaptureTake()` yang sudah ada. Jalur paralel sudah menangkap stdout+stderr compiler; jalur serial dapat memanggil `compileLibraryOne(..., &cap)` alih-alih `NULL`, lalu memanggil `cmdsEmitCapture()` setelah progress line dicetak. Ini menggunakan parameter capture yang sudah disediakan `compileOne()` dan tidak menjalankan compiler dua kali.
- Tambahkan penghitung warning pada buffer compiler yang sama sebelum buffer dibuang. Minimal kenali bentuk diagnostic GCC/Clang (`warning:`) dan MSVC (`warning <kode>:`) secara spesifik; jangan menghitung kata `warning` pada command, source text, atau ringkasan compiler. Jika format toolchain tidak dikenali, tandai count unavailable dan jangan menampilkan angka palsu.
- Workspace parallel mengalirkan stderr child langsung ke terminal dan menyimpan stdout ke slot. Karena warning compiler berada di stderr, warning count harus dikirim melalui record sidecar child atau dihitung di proses compile sebelum child selesai; jangan mencoba menghitungnya dari stdout slot parent.
- `WARN ...` pada report adalah salinan diagnostic yang sudah diterima dari compiler, bukan hasil scan build.log. Report tidak boleh mengubah exit code compiler.
- **Keputusan pemilik (O2): baris WARN dipakai HANYA saat `log = true`.** Dengan log aktif, output mentah compiler TIDAK dicetak ke report — detail penuh (termasuk konteks source/note) pindah ke `.satria/build.log` untuk `satria log`, dan report mencetak `WARN <diagnostic>` sebagai pengganti. Ini bukan duplikasi. Baris WARN juga membawa warna kembali: dengan log aktif flag `-fdiagnostics-color` dimatikan (`compileSetDiagColor`), sehingga warning sampai tanpa warna — satria mewarnai baris WARN sendiri (aturan `NO_COLOR`/`SATRIA_NO_COLOR`/`TERM=dumb` via `diagColorWanted`; Windows tanpa ANSI). Dengan `log = false`, output asli tampil utuh seperti sebelumnya dan tidak ada baris WARN. Unit compile yang GAGAL (atau terinterrupt) selalu mencetak output mentah: error butuh konteks penuh di terminal.

### 6. Toolchain dan CompDB

Toolchain:

- Gunakan `Config.cc`, `warningFlags(&c)`, `includeFlags(&c)` bila memang bagian dari command yang ditampilkan, `definesFlags(&c)`, `targetFlags(&c)`, dan `Config.std` sesuai command aktual dari `compileCmd()`.
- Format contoh hanya menampilkan bagian yang dipilih untuk dibaca (`gcc -Wall -Wextra -MMD -MP -std=gnu11`); jangan menampilkan flag yang tidak benar-benar dipakai.
- Workspace bisa memiliki compiler/flags berbeda per proyek. Jika identik, tampilkan satu toolchain global. Jika berbeda, tampilkan toolchain per proyek atau kelompok yang berbeda; jangan menggabungkan flags menjadi satu command yang tidak pernah dijalankan.

CompDB:

- Tambahkan status hasil eksplisit ke jalur penulis, bukan parse teks stdout.
- `writeCompdb()` membedakan cache hit (`unchanged`), restore blob (`restored`), write sukses (`refreshed`), disabled, dan kegagalan penulisan/alokasi. Saat ini fungsi `void`; ubah menjadi result enum/struct atau tambahkan output parameter tanpa mengubah keputusan cache.
- `writeCompdbMulti()` saat ini diam pada cache hit/restore; laporkan hasil yang sama melalui return status. `wsCompdbRefresh()` mengagregasi status workspace dan menyampaikan hasil ke report.
- Jalur single-project memanggil CompDB hanya bila file hilang atau fingerprint compile/source berubah. Report harus menyebut `skipped` bila fungsi memang tidak dipanggil karena command list tidak berubah, bukan mengklaim `unchanged` tanpa hasil eksplisit.
- `wsCompdbRefresh()` dijalankan sebelum fase compile workspace. Status header report diset dari hasil fungsi ini.
- Kegagalan CompDB saat ini tidak selalu menggagalkan build single karena `writeCompdb()` void dan kegagalan write tidak dipropagasikan. Desain report tidak boleh diam-diam mengubah kebijakan itu; status `failed` untuk CompDB hanya informasi kecuali perilaku build memang sudah mengembalikan error.

### 7. Library dan binary

Library (`src/cmds/cmds_lib.c`):

- `cmdsBuildLibraryEx()` sudah menghitung `staticUpToDate` dan `sharedUpToDate` secara terpisah melalui `cmdsLibVariantUpToDate()`.
- Catat masing-masing varian setelah jalur sukses: `cached` bila up-to-date, `built` setelah `runCmd()` berhasil.
- Bila static berhasil tetapi shared gagal, hanya static yang boleh tercatat sukses; keseluruhan build tetap gagal.
- Ambil path dari `cmdsLibStaticPath()` / `cmdsLibSharedPath()`. Ambil ukuran setelah artefak dipastikan ada.
- Fase library workspace dan pemanggilan library setelah link single/workspace memakai API record yang sama. Hindari mencatat satu library dua kali; record dapat di-upsert berdasarkan path + jenis artefak.

Binary (`src/cmds/cmds_passfinish.c`):

- `linkNeeded == false` berarti `cached` setelah target lolos pemeriksaan mtime/dependency dan `Linking: ... (up-to-date)` dicetak.
- `runCmd(cmd)` sukses berarti `linked`; gagal tidak boleh membuat record sukses.
- Target Windows dapat berubah menjadi `.exe`; gunakan `target` aktual setelah resolusi fallback yang sudah dilakukan fungsi, bukan nama mentah `outBinaryName`.
- `fsFileSize(target)` sudah dipakai di jalur link baru. Pindahkan/ekspos hasil ukuran itu ke report; untuk cached cukup satu pemanggilan ukuran jika diperlukan.
- Library bisa dibangun walau binary up-to-date. Karena itu status library tidak boleh disimpulkan dari status link binary.

### 8. Release dan checksum

- `packRun()` merupakan sumber kebenaran status package. Ia memeriksa freshness input, menulis package ke staging `.satria/pack`, lalu rename ke output.
- Untuk setiap `PackArtifact`, catat `cached` pada cabang `up-to-date`, dan `built` setelah fungsi builder sukses serta output berada di path final.
- `packChecksumApply()` harus mengembalikan atau mengirim status checksum per file: `cached` bila checksum mtime sudah memenuhi syarat, `built` setelah `packWriteSha256()` berhasil.
- `packLastRebuilt()` tetap boleh dipakai untuk keputusan existing yang membutuhkan boolean agregat; jangan menjadikannya sumber status per artefak.
- `cmdsPackAfterBinary()` adalah jalur pack setelah binary single/project. `cmdsPackOnlyProject()` menangani project pack-only. `wsRelease()` memanggil pack engine yang sama untuk satu paket workspace.
- Release hanya ditampilkan bila pack/release benar-benar dijalankan. Jika release dikonfigurasi tetapi build selektif melewatinya, jangan tampilkan bagian Release seolah-olah sudah dieksekusi.
- Checksum merupakan artefak tersendiri dan dapat ditampilkan tanpa ukuran.

### 9. Penyimpanan Build Report dan format fast no-op

`src/log/report_blob.c` saat ini melakukan tee stdout POSIX ke `.satria/report.blob.tmp`, lalu `rbCondense()` mem-parsing output lama untuk membentuk `STATUS` + `LAST BUILD SUMMARY`. Implementasi baru tetap harus memisahkan dua kebutuhan yang formatnya memang berbeda:

1. **Build Report aktif** — dipakai pada cold build dan incremental build yang melewati gerbang fast no-op. Formatnya adalah `Build Report` dengan bagian Toolchain, CompDB, Archive, Execution, Execution summary, Release, dan Summary. Artefak boleh berstatus `built`, `cached`, atau `linked` karena memang ada lifecycle build yang dijalankan.
2. **Fast no-op summary** — dipakai ketika fast-state valid dan build dilewati. Bila blob ada: replay `LAST BUILD SUMMARY`. Bila tidak: layout fallback gabungan (bagian 9a). Bukan replay `Build Report` penuh. Contoh kontrak output:

```text
STATUS
------
Build        : Nothing to build
Cache valid  : .satria/caches/fingerprint.cache

LAST BUILD SUMMARY
------------------
Datetime   : 2026-11-01 14:04
Total time : 31.13s
Archives   : 0 created
Libraries  : 0 generated (0 static, 0 shared)
Binaries   : 1 linked, 0 up-to-date
Releases   : 1 created, 0 up-to-date
Status     : Success
```

Baris `Releases` mengikuti aturan §8 — hanya ada bila fase release benar-benar
dijalankan pada build yang tersimpan; sidecar checksum (`.sha256`) tidak
dihitung dalam agregat agar paket tidak dihitung ganda.

Pemulihan laporan: setiap build sukses menyalin isi `report.blob` ke state (single: `fingerprint.cache`; workspace: `state.cache` dengan proyek pseudo `__report__`). Bila `report.blob` hilang, setiap no-op memulihkannya dari salinan sebelum memutarnya, sehingga replay tetap sama dengan build terakhir.

### 9a. No-op tanpa laporan tersimpan (keputusan pemilik)

Urutan keputusan no-op:

1. **Blob ada**: replay `STATUS` + `LAST BUILD SUMMARY` dari blob (format lama).
2. **Blob hilang tetapi salinan ada di state**: blob dipulihkan dari salinan, lalu diputar. Hasilnya identik dengan build terakhir.
3. **Tidak ada salinan sama sekali** (state tanpa laporan, misalnya setelah upgrade): layout fallback di bawah.

Salinan dibuat setiap build sukses: single di `fingerprint.cache`, workspace di `state.cache` dengan proyek pseudo `__report__`. Build gagal tidak mengganti salinan.

Fallback single project (blob dan salinan tidak ada):

```text
Build report
------------
* Build with cached state (fingerprint unchanged)
* 0 compiled, 3 skipped
* bin/mini_demo (15.5KB, cached)

# Summary
Total time  : 0.00s
Status      : Success
Cache valid : .satria/caches/fingerprint.cache
```

Fallback workspace (tidak ada salinan): berkelompok seperti 9b, untuk seluruh workspace.

Aturan nilai fallback single:

- `0 compiled`: no-op tidak mengompilasi apa pun pada run ini.
- `N skipped`: `compiled + skipped` dari build terakhir (total unit compile).
- Target: `* <path> (<ukuran>KB, cached)`; path dan ukuran dari state dan memo stat yang sudah tervalidasi gerbang.
- `Total time`: durasi no-op pada run ini.

### 9b. Build selektif (`-w <proyek>`) dan layout berkelompok (keputusan pemilik)

Build selektif memakai format Build Report yang sama dengan cold/incremental. Perbedaannya hanya cakupan: `Execution`, `Execution summary`, dan `Summary` hanya mencakup proyek dalam closure yang dipilih. Blok per proyek lama (`> Project`, `> Fase`, `> Summary :`) tidak dicetak lagi.

Layout berkelompok (dipakai untuk no-op selektif dan fallback workspace):

- Artefak dikelompokkan `> lib/` lalu `> bin/`, memakai nama basename, dengan lebar nama disamakan dalam satu kelompok.
- `# Summary` memuat `Total time` dan `Status : Success`.
- Tidak ada `# Execution` pada no-op, karena tidak ada compile. Pada incremental selektif yang mengompilasi, `# Execution` muncul sebelum `# Execution summary`.

Contoh `satria -w core` pada no-op (miniws, proyek `core`):

```text
Build report
-------------
* Build with cached state (fingerprint unchanged)

# Execution summary
> lib/
* librupa.a  (1.8KB, cached)
* librupa.so (15.3KB, cached)

> bin/
* rupa (15.9KB, cached)

# Summary
Total time : 0.00s
Status     : Success
```

Catatan header: baris `CompDB` (contoh desain) belum dicetak pada no-op, karena refresh CompDB tidak dijalankan di jalur gerbang. Header memakai baris cache sebagai pengganti. Menampilkan `CompDB` pada no-op memerlukan status CompDB yang bisa dibaca tanpa refresh (lihat item terbuka).

Build selektif dibatasi closure: gerbang global `wsCacheNoopValid` dipakai untuk seluruh workspace. Bila valid, closure pasti tidak berubah, sehingga output dibatasi proyek di closure. Bila tidak valid, build selektif berjalan normal (format lama per proyek sampai incremental selektif diterapkan).

### 10. Durasi, status akhir, dan cleanup

- Catat start monotonic pada pemilik lifecycle report: `cmdBuildEx()` untuk single, `wsRunCore()` untuk workspace. Jangan memulai timer lagi di setiap proyek atau fase.
- `Total time` adalah wall-clock dari awal eksekusi build yang melewati gerbang no-op sampai seluruh fase yang diminta selesai; workspace termasuk CompDB, dua pass, release, dan penyimpanan cache yang merupakan bagian run. Nyatakan konsisten apakah startup/config termasuk; untuk format contoh, mulai saat report lifecycle dimulai setelah no-op/background gate.
- `success` hanya jika seluruh fase yang diwajibkan untuk run tersebut sukses. `failed` untuk compile/link/archive/library/release yang gagal. CompDB mengikuti kebijakan error existing dan tidak boleh mengubah status build hanya demi report.
- Pada kegagalan, artefak yang sukses lebih dahulu boleh tetap dicantumkan. Status keseluruhan tetap `failed`.
- Semua buffer/list/record report dibebaskan pada jalur sukses dan gagal. Kegagalan menulis blob tidak mengubah exit code build.
- Jika child workspace gagal, parent tetap mengimpor record yang sudah selesai sebelum merender report gagal; child yang gagal tidak boleh meninggalkan fragment yang dianggap sukses.

## File implementasi

| File                                                       | Peran perubahan                                                                                                                                                                                                        |
| ---------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/log/build_report.h` / `src/log/build_report.c` (baru) | Model record report, API pengumpulan, penggabungan record proyek, renderer format final, timer dan status akhir.                                                                                                       |
| `src/log/report_blob.h` / `src/log/report_blob.c`          | Persist/replay `LAST BUILD SUMMARY` secara atomik untuk fast no-op; `reportBlobShow()` tidak merender ulang Build Report aktif. Hapus agregasi berbasis parsing teks `rbCondense()` setelah record eksplisit tersedia. |
| `src/cmds/cmds_build.c`                                    | Lifecycle single-project; set toolchain, CompDB, waktu, dan hasil akhir.                                                                                                                                               |
| `src/cmds/cmds_pass.c`                                     | Catat archive per entri, toolchain efektif, compile/skipped, dan detail project.                                                                                                                                       |
| `src/cmds/cmds_passfinish.c`                               | Gabungkan hasil library/binary pass, catat status link dan statistik proyek tanpa menggandakan hitungan fase.                                                                                                          |
| `src/cmds/cmds_lib.c`                                      | Catat status dan ukuran static/shared library per varian.                                                                                                                                                              |
| `src/embed.h` / `src/embed_archive.c`                      | Ekspos hasil per `EmbeddedEntry` dari keputusan prebuilt/fresh/rebuilt.                                                                                                                                                |
| `src/compdb.h` / `src/compdb.c`                            | Kembalikan status aktual single/multi CompDB tanpa mengubah fingerprint/cache decision.                                                                                                                                |
| `src/workspace/workspace.c`                                | Lifecycle report workspace, header CompDB, release, finish dan no-op replay.                                                                                                                                           |
| `src/workspace/ws_exec.c`                                  | Identitas proyek/fase, fast-state cached, dan hasil pass proyek.                                                                                                                                                       |
| `src/runners/serial.c`                                     | Kirim hasil proyek ke konteks report parent secara langsung.                                                                                                                                                           |
| `src/runners/parallel.c`                                   | Tulis/import fragment report per child dan gabungkan sesuai urutan worklist; jangan share mutable report global lintas fork.                                                                                           |
| `src/pack/pack.h` / `src/pack/pack.c`                      | Ekspos status package per output dan checksum per file.                                                                                                                                                                |
| `src/workspace/ws_release.c`                               | Pastikan hasil `packRunAt()` masuk ke bagian Release workspace.                                                                                                                                                        |
| `src/cmds/cmds_wsstate.c`                                  | Jalur no-op per proyek dalam workspace (`wsFastStateValid`). Pada build selektif, output lama diganti format baru (9b).                                                                                                              |
| `src/cmds/cmds_state.c`                                    | Cetak `STATUS` no-op dan cache path, lalu tampilkan `LAST BUILD SUMMARY` historis; jangan membuat report artefak `cached` untuk no-op.                                                                                 |

Tidak perlu mengubah `src/cache/*`, `src/deps/*`, `src/commands.c` (kecuali bila API lifecycle lama perlu dirapikan), atau keputusan build di `cmds_pass.c`/`cmds_passfinish.c` selain menambahkan pelaporan hasil pada cabang yang sudah ada.

## Urutan implementasi

1. **Model dan renderer** — buat record report serta format output final. Uji renderer dengan data fixture tanpa menjalankan build.
2. **Archive per entri** — perluas hasil embed archive, termasuk prebuilt/fresh/rebuilt dan archive-only project.
3. **Library dan binary** — tambahkan record status dari cabang up-to-date/build/link yang sudah ada; verifikasi tidak ada status sukses pada operasi gagal.
4. **Compile statistics dan warning** — sambungkan `compiled`/`skipped`; aktifkan capture serial memakai API `ProcHandle` yang sudah ada; hitung warning dari buffer compiler. Pastikan warna dan `build.log` tidak rusak.
5. **CompDB** — buat status return eksplisit untuk single/multi dan integrasikan ke header.
6. **Workspace aggregation** — integrasikan fase library + binary; tambahkan fragment child pada runner parallel; pastikan urutan output deterministik.
7. **Release/checksum** — laporkan setiap output pack dan checksum; uji build selektif yang melewati release.
8. **Blob/no-op** — simpan ringkasan no-op (`LAST BUILD SUMMARY`) secara atomik dari record eksplisit; replay blob untuk fast no-op bila blob ada, layout fallback (9a) bila tidak; hapus parser lama setelah agregat tidak lagi bergantung pada parsing stdout.
9. **Validasi lintas platform** — POSIX serial/paralel, Windows serial, report blob tersedia/tidak tersedia, build gagal, dan report write gagal.

## Kriteria pengujian

### Single-project

- Cold build: archive, compile, library, binary, release (bila ada) tampil dengan status `built`/`linked`.
- No-op fast-state: tidak menjalankan compile/link dan tidak menampilkan format Build Report dengan status per operasi. Bila blob ada, output adalah replay `STATUS` + `LAST BUILD SUMMARY` dengan nilai historis yang tidak berubah. Bila blob tidak ada, output adalah layout fallback (9a).
- Touch tanpa perubahan konten: `compiled` tetap nol sesuai keputusan deps/P metadata; artefak yang tidak berubah tidak salah dilaporkan `built`.
- Ubah source: hanya source yang benar-benar dikompilasi dihitung `compiled`; binary hanya `linked` bila link benar-benar berjalan.
- Hapus `.a` atau `.so` satu varian: hanya varian yang dibuat ulang berstatus `built`.
- Ubah embedded config: seluruh source yang memang dipaksa compile dihitung, archive yang berubah berstatus `built`, archive yang tetap fresh `cached`.
- Compile/link/library/pack gagal: report berstatus `failed`, report sukses sebelumnya tetap utuh.

### Workspace

- Cold build: satu header, archive semua proyek yang relevan, satu blok Execution per proyek, library dan binary final, release jika ada.
- Library pass mengompilasi; binary pass tidak menggandakan `compiled`/`skipped` atau membuat blok compile kedua.
- Project fast-state hit saat proyek lain berubah: output proyek yang tervalidasi ditandai `cached`, tanpa compile palsu.
- Archive-only project: tampil di Archive, tidak tampil sebagai binary execution.
- Parallel runner: urutan proyek sama dengan worklist pada runner serial; record child tidak hilang/duplikat meskipun selesai tidak berurutan.
- Build selektif: hanya closure yang dibangun; release tidak muncul bila tidak dijalankan.
- Release input berubah: package/checksum yang diperbarui berstatus `built`; yang tetap fresh `cached`.
- Workspace no-op: bila blob ada, replay `STATUS` + `LAST BUILD SUMMARY` dengan cache path `WS_CACHE_FP`. Bila tidak, layout fallback (9a) dengan data per proyek dari `ws.state`. Build selektif memakai format baru (9b); jalur per proyek di `cmds_wsstate.c` tidak lagi mencetak format lama pada build selektif.

### Invarian

- Tidak ada perubahan pada hasil keputusan incremental, jumlah compiler/linker invocation, atau urutan dependency.
- Tidak ada scan direktori/hash baru untuk report.
- Satu artefak tidak memiliki dua record final untuk path yang sama.
- `built`, `cached`, dan `linked` hanya muncul pada Build Report cold/incremental dan layout fallback no-op (9a), dan berasal dari cabang operasi nyata, bukan parsing output.
- Replay blob no-op tidak menampilkan status artefak; hanya `STATUS` saat ini dan `LAST BUILD SUMMARY` historis. Layout fallback (9a) menampilkan `cached` untuk target dan library.
- Report gagal tidak pernah menggantikan report sukses terakhir.
- Build tetap sukses/gagal sesuai operasi build, bukan sesuai keberhasilan fasilitas report.

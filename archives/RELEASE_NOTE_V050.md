# Release Note - satria v0.5.0

## Build Report

Keluaran build sekarang dibungkus satu laporan utama dengan urutan tetap:

```text
startup
-------
> loading configuration: done
> preparing execution: done

Build Report
------------
Toolchain   : gcc -Wall -Wextra -MMD -MP -std=gnu11
CompDB      : compile_commands.json refreshed

# Archive
* modules/rupa_modules.tar (10.0KB, built)

# Execution
> core
[  1/  2] OK    0.42s  core/src/core.c
* 2 compiled, 0 skipped

# Execution summary
> lib/
* librupa.a  (1.8KB, built)

# Release
* dist/satria-v0.5.0.tar.gz (207.6KB, built)

# Summary
Total time : 30.47s
Status     : Success
```

- Hitungan warning per proyek: `* 4 compiled, 0 skipped, 3 warnings`.
- Status artefak: `built`, `cached`, `linked`.
- Status gagal: `Status : Failed`; laporan sukses sebelumnya dipertahankan.
- Baris peringatan kecil non-kunci pada header bila build selektif menyentuh
  workspace yang belum pernah terbangun penuh:
  `! Workspace never built — bade may be out of sync` (atau
  `! Workspace not fully built — ...` bila fingerprint parsial). Tidak tampil
  bila cakupan fingerprint penuh, dan tidak dipasang pada build penuh.
- Dengan `log = true`, diagnostic compiler tampil sebagai baris
  `WARN <diagnostic>` di report — output mentah (lengkap dengan konteks source)
  pindah ke `.satria/build.log` untuk `satria log`, dan baris WARN membawa warna
  kembali. Dengan `log = false` output compiler tampil utuh seperti biasa,
  tanpa baris WARN.

## Build selektif & cdeps

- **`satria -w <project> --with-cdeps`** (baru, opt-in): closure build selektif
  ikut menarik provider `cdeps` — `satria -w bade --with-cdeps` membangun `core`
  bila bade menautkan library core, termasuk transitif dan untuk cdeps siklik.
  Tanpa flag kontrak lama tidak berubah: cdeps tidak pernah memperluas closure.
  Flag hanya berlaku dengan `-w` (tanpa -w: rc 2).
- **Peringatan pre-flight provider hilang**: build selektif tanpa flag kini
  memberi tahu lebih dulu bila provider cdeps di luar closure belum punya
  library di disk — `warning: 'bade' links against 'core' through cdeps, ...`
  beserta saran `satria -w` / `satria -w bade --with-cdeps`. Tidak tampil bila
  library provider sudah ada (prebuilt) atau provider ikut closure.

## No-op

- Dengan report tersimpan: `STATUS` dan `LAST BUILD SUMMARY` diputar ulang.
- Report hilang tetapi salinan ada di state: dipulihkan lalu diputar.
- Build selektif yang no-op: layout berkelompok (`> lib/`, `> bin/`) untuk closure.

## Perbaikan

- Blob `report.blob` dibentuk dari record, bukan parsing teks.
- Warning terhitung di terminal berwarna.
- Blok lama per proyek tidak lagi tercetak bersama Build Report.
- No-op penuh pertama langsung HIT setelah build selektif: fingerprint di-merge
  per proyek (O1) — proyek di luar closure tetap tervalidasi/dideteksi perubahannya.
  Termasuk workspace yang BELUM PERNAH build penuh: build selektif kini menulis
  fingerprint parsial (closure) + penanda cakupan `fp.cover`, sehingga run penuh
  pertama HIT begitu semua proyek tercatat; sebelum run penuh pertama, run penuh
  menolak HIT palsu (proyek yang belum tercatat = MISS) dan P-fast ditahan sampai
  cakupan penuh.
- Output paralel (`SATRIA_WS_PAR=1` / background): satu blok `> Nama` per proyek
  tanpa hitungan ganda, dan urutan proyek identik dengan runner serial (fase
  library per tingkat dependensi; peluncuran anak tetap siap-dep).
- Baris `.sha256` pada `# Release` menampilkan status nyata — `cached` bila tidak
  ditulis ulang pada run (O4); paket dan checksum sama-sama `cached` saat input
  tak berubah.
- Kegagalan menulis `report.blob` tidak mengubah exit code build; blob pulih
  sendiri pada run berikutnya.
- Portabilitas menuju Windows: tanpa compound literal C99, pola warning MSVC
  `warning C<n>:` dikenali penghitung warning.
- `satria help` kembali tampil pada file `cmd.txt` ber-terminasi CRLF (checkout
  Windows): marker section dinormalkan sebelum dicocokkan, sehingga `help` dan
  `help <topik>` tidak lagi keluar kosong.

## Diketahui

Lihat `problems/02_11_2026_build_report_review.md` (bagian FIXED/EVALUASI/OPEN).
O1–O4 dan O6 sudah diperbaiki; O5 dievaluasi bukan cacat (penjadwal siap-dep sudah
menindih arsip dengan proyek independen — serialisasi tersisa adalah semantik
dependensi dan mode serial foreground).

**O7b (runtime Windows/MSVC) CLOSED lewat CI** — `build-manual.yml` run 41638258152
(commit `c837775`) hijau 7/7 job, termasuk `windows-latest` dengan `cl` dan `gcc`:
`install.ps1 --dev` sukses, `satria version`/`satria help` tampil penuh, self-host
build + run kedua exit 0, dan 180 warning `cl.exe` terhitung benar
(`* 91 compiled, 0 skipped, 180 warnings`). Beberapa hal belum ada di workflow:
`satria.exe -w` pada `tests/miniws` dan `scripts/unit.sh` di Git Bash Windows.

**O9 (no-op/incremental Windows) CLOSED lewat CI** — dua cacat diperbaiki:
(1) konversi FILETIME Windows di `fsStampNsSize`/`fsMTimeNs` overflow `int64`
(nilainya negatif), sehingga snapshot/fingerprint tak pernah tersimpan;
(2) parser `.d` menganggap colon path drive (`D:/...`) sebagai pemisah target pada
baris continuation hasil wrap GCC, sehingga source di subdirektori mendapat anak
hantu ber-mtime -1 (`INT64_MAX` = stale) dan selalu dikompilasi ulang. Run
41699264783 (commit `176a140`): ketujuh job hijau dan langkah "semua up-to-date"
mencetak `STATUS` / `Build : Nothing to build` di Windows (cl + MinGW), Linux, dan
macOS. Rincian ada di `problems/02_11_2026_build_report_review.md` bagian O9.

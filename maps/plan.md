# Plan (rencana stat, fast-state, scheduler)

## Domain flow

cache (`ws_cache_noop.c`, `cmds_statmemo.c`)
  -> workspace (`ws_plan.c`, `ws_exec.c`, `cmds_wsstate.c`)
  -> cmds (`cmds_build.c` `statPlanBuild`, `cmds_pass.c` decide)
  -> runners (compile / library / link)
  -> log (`build_report.c` blok/heading, `report_blob.c` relay stdout)

Urutan yang menentukan kebenaran: gerbang no-op berjalan sebelum rencana, dan
rencana sebelum kompilasi. Antara gerbang dan rencana tidak ada penulis sumber;
tulisan milik build sendiri (embedded, object, `.d`) memanggil
`cmdsStatMemoForget`.

## Responsibilities

cache:
  Menstat seluruh input `sources.cache` sekali (paralel lewat fork) dan mengisi
  memo stat per proses. Output di bawah `build/` tidak dimasukkan: object
  ditulis proses anak, salinan memo induk akan basi.

workspace:
  `ws_plan.c` menyusun rencana proyek per fase (library/binary), menghitung
  verdict fast-state di depan bila runner paralel, dan memverifikasi penanda
  eksekusi. `ws_exec.c` menjalankan tepat rencana itu. Runner paralel kini
  default, jadi rencana harus tetap benar saat proyek saling menindih.
  `cmds_wsstate.c` memvalidasi stamp fase: batch stat hanya untuk path yang
  belum ada di memo, lalu loop validasi memakai memo yang sama.

cmds:
  `statPlanBuild` mengisi mtime sumber/object untuk decide dalam satu lintasan.
  Path yang sudah dimemo gerbang dipakai tanpa syscall; path lain (single
  project, proyek baru, `build/`) tetap distat. Peta object memakai hash agar
  tidak O(n²) string. Fase library workspace hanya membangun `.a`
  (`cmdsLibraryDeferShared`); `.so` dimulai fase binary sebelum link binary
  (`cmdsSharedJobStart`) sehingga kedua link berjalan bersamaan dan laporan
  tetap mencatat status built/cached yang benar. `wsFastStateSave` fase
  library tidak mencatat `.so` yang belum ada (bukan artefak hilang).

runners:
  Memakai keputusan decide; tidak menstat ulang di luar pass. Anak fork
  membuang memo stat dan (bila sudah ada proyek selesai) verdict hit warisan
  sebelum mengeksekusi. Penjadwal siap-dep (paralel) adalah default untuk >1
  proyek; stdout TTY memilih serial supaya keluaran tetap realtime. TTY dibaca
  dari stdout ASLI (`reportBlobStdoutIsTty`), bukan `isatty(STDOUT_FILENO)`:
  selama build report_blob sudah mengganti stdout dengan pipe tee, sehingga
  pemeriksaan langsung selalu "bukan TTY" dan terminal keliru memilih paralel
  (progres tertahan per proyek sampai slotnya dicetak). Bila stdout pipe/soket
  (mis. `satria | satria -w`) anak menulis LANGSUNG + line-buffered sehingga
  progres terlihat saat berjalan; berkas reguler tetap memakai slot urut
  worklist agar log deterministik.

## Invarian

- Memo hanya sah dalam satu proses dan satu run; `cmdsStatMemoReset` dipanggil
  sebelum build single-project dan setelah build gagal.
- Entry memo yang basi ditandai `cmdsStatMemoForget`, bukan dihapus; lookup
  berikutnya menstat ulang.
- Penghematan tidak boleh mengubah keputusan: path tanpa memo selalu jatuh ke
  stat langsung, dan hasil "hilang" tidak pernah dianggap segar.
- Verdict hasil precompute hanya sah untuk proyek yang diluncurkan tanpa
  proyek lain selesai lebih dulu; sesudahnya `wsPlanFastConsume` kembali ke
  validasi executor (hit basi tidak boleh melewatkan rebuild).
- `.so` yang ditunda tidak pernah menjadi prasyarat link binary; kegagalan
  link `.so` tetap menggagalkan run (state sukses tidak disimpan).

## Related tests

- ws_plan
- cache_noop_miniws
- selective_noop_full
- router_noop_miniws

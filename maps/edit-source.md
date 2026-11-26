# Edit source

## Domain flow

edit sumber (`*.c`/`*.cpp`, atau `.rp` untuk proyek arsip)
  -> gerbang (`ws_cache_noop.c` `wsCacheNoopValid`)
       hash entri P/F berbeda -> miss + `wsGateStaleMark(path)`
  -> worklist (`ws_worklist.c`) — proyek pemilik + dependensi yang perlu
  -> rencana (`ws_plan.c`) + fast-state (`cmds_wsstate.c`) — verdict miss
  -> pass compile (`cmds_pass.c`)
       `statPlanBuild` (satu stat) -> decide per sumber
       -> kompilasi satu object (paralel/serial)
       -> hash object sebelum vs sesudah
  -> library (`cmds_lib.c`) — `.a`/`.so` hanya bila object berubah
  -> link (`cmds_passfinish.c`) — binary bila object/library berubah
  -> simpan (`ws_cache_save.c`, `cmds/state.c`, `deps/`)
       `sources.cache` + `deps.cache` + stamp fase + report blob

## Responsibilities

gerbang:
  `sources.cache` mencatat hash konten tiap sumber/header/arsip. Satu sumber
  berubah -> entri itu ditandai stale dan gerbang miss; entri lain yang sudah
  diverifikasi tidak di-stat ulang (`cmdsStatMemoPeek`). Sentuh tanpa
  perubahan isi tetap no-op lewat PATCH metadata.

workspace:
  Edit satu sumber hanya membatalkan proyek pemiliknya. Konsumen library
  (`depends_on`/`cdeps`) tidak dikompilasi ulang; mereka hanya di-relink bila
  artefak library berubah (mtime/isi `.a`/`.so` adalah input link nyata).
  Proyek kemasan `.rp` memakai `extension = .rp`: walk freshness hanya
  menyentuh `.rp`, arsip dibangun ulang, lalu konsumen embed menyerap arsip
  baru. Perubahan `module.json` tidak memicu rebuild arsip (bukan input
  freshness), tetapi tetap ikut isi arsip saat arsip dibangun ulang.

pass compile:
  `statPlanBuild` mengisi mtime sumber + object sekali; decide memakai memo.
  Object yang byte-nya identik setelah kompilasi tidak memicu relink binary
  proyek itu sendiri. Object yang berubah masuk arsip library;
  `cmdsLibraryDeferShared` membuat `.so` dan binary di-link bersamaan di fase
  binary. Konsumen lintas-proyek tetap memakai mtime library sebagai input
  link, jadi arsip yang ditulis ulang dapat memicu relink konsumen.

simpan:
  Hanya build sukses yang menyimpan snapshot (`deps.cache`), stamp fase
  (`ws.state`/`lib.cache`), fingerprint, dan report blob. Build gagal
  menyimpan progres parsial (`buildSavePartial`) tetapi tidak mengklaim
  state sukses, sehingga run berikutnya melanjutkan dari object yang selesai.

## Invarian

- Satu sumber berubah tidak boleh mengompilasi sumber lain di proyek yang
  sama, kecuali konfigurasi compile berubah.
- Sentuh tanpa perubahan isi tidak boleh memicu compile/relink.
- Object identik byte tidak boleh memicu relink binary proyek pemiliknya
  (konsumen lintas-proyek tetap memakai mtime library sebagai input link).
- `.rp` yang berubah harus membangun ulang arsip, dan arsip baru harus
  ter-embed di konsumen; perubahan metadata non-`.rp` tidak.
- `sources.cache`/`deps.cache` tidak boleh diperbarui dari build gagal.
- Output build sendiri (`embedded.c`, object, `.d`) memanggil
  `cmdsStatMemoForget` agar memo tidak basi.

## Related tests

- cache_noop
- crud_touch_delete
- p_meta_noop
- router_noop
- ws_plan
- selective_noop_full
- report_binaries_uptodate
- watcher_staging

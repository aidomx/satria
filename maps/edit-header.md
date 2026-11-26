# Edit header

## Domain flow

edit header di disk (`*.h`)
  -> gerbang (`ws_cache_noop.c` `wsCacheNoopValid`)
       hash entri P/F berbeda -> miss + `wsGateStaleMark(path)`
  -> worklist (`ws_worklist.c`) — proyek pemilik header + closure dependensi
  -> rencana (`ws_plan.c`) + fast-state (`cmds_wsstate.c` `wsFastStateValid`)
       entri gate-stale diprioritaskan, stamp `F` header basi -> miss
  -> pass compile (`cmds_pass.c`)
       `depsNewestHeaderMTimeAt` + `depsContentUpToDate` (closure `.d`)
       -> kompilasi sumber terdampak saja
  -> library (`cmds_lib.c`) / link (`cmds_passfinish.c`)
       hanya bila byte object berubah
  -> simpan (`ws_cache_save.c`, `deps/`) — hash header baru jadi baseline

## Responsibilities

gerbang:
  Header adalah input yang di-hash seperti sumber. `touch` tanpa perubahan isi
  tetap lolos: fast path mem-PATCH metadata (mtime/size) setelah hash cocok,
  jadi run berikutnya kembali no-op. Perubahan isi menandai path stale supaya
  validasi state menilainya lebih dulu, bukan setelah ratusan stat lain.

workspace:
  Header bisa dipakai lintas proyek lewat `headers` (publik, `-I`) atau
  `headers.internal` (dilacak tanpa `-I`). Proyek yang closure `.d`-nya
  memuat header itu ikut dikompilasi; proyek lain tidak. Build selektif
  `-w <nama>` tetap menarik closure dependensi produsen library.

pass compile:
  Untuk tiap sumber: object lebih baru dari sumber tetapi header lebih baru
  -> cek konten (`depsContentUpToDate`). Isi header sama = `Headers : N
  source(s) skipped (touched, content unchanged)`; isi berbeda = sumber
  masuk antrean kompilasi (`Headers : N source(s) stale due to header change`).
  Header generated (`embedded.h`, `build/version.h`) yang berubah memaksa
  recompile penuh lewat jalur `embHeaderChanged`/`verHeaderChanged`.

link:
  Object lama dan baru di-hash. Bila semua byte object identik, binary proyek
  itu tidak di-relink meski ada kompilasi; bila berbeda, `.a` diperbarui dan
  binary yang memakainya di-relink. Konsumen lintas-proyek melihat mtime
  library sebagai input link, jadi arsip yang ditulis ulang dapat memicu
  relink konsumen.

## Invarian

- Keputusan berbasis isi, bukan mtime: sentuh header tanpa perubahan isi tidak
  boleh memicu kompilasi atau relink.
- Hanya sumber yang closure `.d`-nya memuat header berubah yang dikompilasi.
- Snapshot `deps.cache` ditulis setelah build sukses; kegagalan tidak
  menjadikan header baru sebagai baseline.
- `headers.internal` tidak pernah menjadi `-I`, tetapi tetap input freshness.
- Header generated yang berubah tidak boleh lolos dari recompile total.

## Related tests

- cache_noop
- crud_touch_delete
- semantic_header_workspace
- router_noop
- ws_plan
- selective_noop_full
- report_binaries_uptodate

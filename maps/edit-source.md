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

## Single-project flow

Edit sumber pada proyek tunggal tidak melewati gerbang/worklist/rencana
workspace. Jalurnya memakai fast-state dan pass build tunggal:

```text
edit sumber
  -> gerbang (`cmds_state.c` `cmdsFastStateValid`)
       opts.cache + artifact.cache + fingerprint.cache/build.state
       stamp sama -> Nothing to build
       stamp beda -> fallback ke build normal
  -> koleksi sumber (`cmds_build.c` / `cmds_pass.c`)
       router `opts.cache` untuk daftar sumber
       -> `statPlanBuild` (mtime source/object sekali)
       -> fingerprint konfigurasi dan source set
  -> decide (`cmds_pass.c`)
       per source: mtime source/object
       -> `depsNewestHeaderMTimeAt`
       -> `depsContentUpToDate` (snapshot hash + edges `.d`)
       -> compile hanya source yang benar-benar stale
  -> library/archive + link (`cmds_lib.c`, `cmds_passfinish.c`)
       object byte-identik tidak memicu relink binary
       `.satria-link.a` diperbarui bila object berubah
  -> simpan (`cmds_passfinish.c`)
       `depsRecordUpdate` + `depsSave`
       -> fingerprint
       -> `cmdsFastStateSave` (`cmds_fast.c`)
            input roots + closure header + artifact/link inputs
            -> `fingerprint.cache/build.state`
```

## Single-project responsibilities

gerbang:
  `cmdsFastStateValid` memeriksa router, artefak, Buildfile, target, lalu
  stamp seluruh input fast-state. Stamp file yang berubah tetapi hash sama
  dipatch sebagai touch; perubahan konten, penambahan/penghapusan file, atau
  direktori berubah membuat gate miss dan build normal mengambil alih.

pass:
  `statPlanBuild` menghindari stat ulang source/object, tetapi keputusan compile
  tetap memeriksa seluruh translation unit. `depsNewestHeaderMTimeAt` memberi
  batas mtime header; `depsContentUpToDate` membuktikan konten source dan
  header transitif terhadap `deps.cache`. Karena itu edit satu source tidak
  berarti semua source dikompilasi, walau semua source masih dievaluasi.

simpan:
  Fast-state menyimpan stamp file dan direktori untuk mendeteksi perubahan
  filesystem pada run berikutnya. Hash konten dibutuhkan untuk bukti touch,
  tetapi hash dari state sebelumnya dapat dipakai ulang bila path, mtime, dan
  ukuran file tetap sama. File baru atau stamp yang berubah tetap di-hash.
  Pengumpulan path memakai set hash agar deduplikasi tidak menjadi pencarian
  linear berulang. Daftar path tetap dikoleksi ulang agar create/delete dan
  perubahan direktori tidak luput.

## Single-project invariants

- Perubahan satu source hanya mengompilasi source tersebut, kecuali compile
  configuration, source set, atau dependensi header memang berubah.
- Touch tanpa perubahan konten tidak memicu compile/link.
- Header eksternal di luar `sources`/`includeDirs` tetap tercakup melalui
  closure dependensi `.d`/scanner.
- Fast-state tidak boleh melewatkan file baru/hilang; direktori tetap menjadi
  stamp input.
- Hash lama hanya boleh dipakai ulang bila path, mtime, dan ukuran sama.
- Build gagal tidak boleh diklaim sebagai snapshot sukses.

## Single-project related tests

- faststate_external_header
- crud_touch_delete (bagian MODE SINGLE)
- cache_noop (gate dan fallback)
- report_binaries_uptodate

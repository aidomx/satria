# todos — 4 November 2026: audit ruang optimasi performa

Audit ini MENGGABUNGKAN temuan baru hari ini (4/11/2026) dengan sisa ruang yang sudah terpetakan di tiga audit sebelumnya (`problems/15_10_2026_audit_v94_optimasi.md`, `problems/27_10_2026_cold_build_overhead.md`, `problems/30_10_2026_incremental_overhead.md`).

Tujuan: daftar tunggal "apa yang masih bisa ditingkatkan", terukur, dan tidak duplikatif.

Konteks mesin: aarch64 + proot, job efektif **j1–j2**, `stat()` 257–461 µs, fork+exec ~9 ms (audit 27/10). Semua angka lokal EXTRAPOLASI harus per operasi (metodologi audit 30/10), bukan ms mentah.

## [x] Pengukuran baru hari ini (miniws, dir test tersisa + strace)

| penanda | hasil | catatan |
|---|---|---|
| no-op single (repo root, 3 run) | 0,08–0,12 s | termasuk spawn |
| no-op workspace `-w` (5 run) | **0,04–0,07 s wall** | P-fast path aktif |
| syscall no-op workspace | **151 total** (11 openat, 62 newfstatat, 18 read, 1 write) | memang lean; target tiap lapisan cache sudah tercapai di jalur ini |
| file cache yang ditulis saat no-op | **nol** (mtime semua cache tidak berubah) | catatan no-op tidak "menulis" kecuali PATCH sentuh |
| incremental (edit 1 source, wall) | **1,23 s** | termasuk tee/fork relay, 7 cache save, pack release (fixture punya `compress = gzip`) |
| openat saat incremental (semua proses) | 440; cache yang paling sering: `lib.cache` 12×, `opts.cache` 11× (+5× `.tmp`), `deps.cache` 9×, `fingerprint.cache` 5× | pola buka-ulang memang pola fase, bukan bug |
| cold build miniws (3 proyek + release) | 4,85 s | g++ floor (`bin/demo` saja) 0,38 s → toolchain dominan seperti temuan 30/10 |
| `scripts/unit.sh -b -j2` | DIDUKUNG (opsi `-b`, `-o`) | suite dua kali terputus restart; **pakai ini**, jangan loop manual |

## Ruang optimasi — peringkat (sisa dari audit 30/10 §11–13 + temuan baru)

Yang sudah dikerjakan DAN terukur: `.d` dibaca sekali (−2,8 s), `fs-save` reuse-hash (−1,5 s), vonis miss pasti + entri basi diutamakan (−1,1 s), memo stat induk (−50 %), memo object/`.d` (−37 %), pemulihan `O`/`.d` hilang.

1. **[x] `.d-cek` di `save-walk` — SELESAI (14/11/2026).**
   - Koreksi temuan 4/11: setiap `.d` di-stat **dua kali** per save, bukan satu
     (`.d-cek` di `dstat` + `wsCacheAdd(dfile,'O')` yang men-stat ulang path yang
     sama dalam iterasi yang sama). Stat kedua dihapus: `wsCacheAddSt` memakai stat
     yang sudah ada (`src/workspace/ws_cache_save.c`).
   - Terukur dengan `satria trace` (cold miniws, n=6 objek, binary asli vs baru):
     stat pada `.d` 24 → 18 (−n), total stat seluruh proses 1514 → 1508 (−n). Counter
     `output` SATRIA_PROFILE 23 → 17 sebagai pelengkap. Kunci cache (hash, tipe, path) identik antara sebelum dan
     sesudah; no-op tetap `Nothing to build`; suite miniws 348 PASS / 0 FAIL / 0 PRE.
   - Terukur di n=500 (workspace 500 objek, cold build, sandbox 1-core):
     stat pada `.d` 2000 → 1500 (−500), total stat 37768 → 37268 (−500), profil
     `output` 1003 → 503 stat. Biaya 500 stat hanya ~0,4 ms di sandbox ini, jadi
     wall time tidak berubah yang terukur: `bench_real_project.sh -r 5` serial
     median asli 5,92 s vs baru 5,98 s (rentang tumpang tindih, noise).
   - Estimasi 0,13–0,23 s per save untuk n=500 (−n × 257–461 µs, stat cost audit
     27/10 di proot aarch64) TIDAK terbukti di sandbox ini. Perlu diukur di target
     proot sebelum dianggap hasil.
   - Langkah (a) SELESAI (14/11/2026) — memo stat gerbang no-op, tanpa merge/preserve:
     proyek **nol-kompilasi** run ini (.o/.d pasti tak tersentuh) memakai stamp memo
     stat P-fast yang sudah dibayar di awal run; proyek yang kompilasi tetap stat
     segar (memo bisa basi — anak fork menulis di memorinya sendiri). Implementasi:
     (1) tabel "ada kompilasi" per proyek `cmdsWsPhaseCompiledSet/Get` (commands.c,
     semantik OR antar fase); ditulis di `cmdsBuildPassFinish` kedua jalur, jalur
     fast-state valid & skip tanpa target `wsRunProject` (ws_exec.c), dan distaging
     anak→induk lewat `ws-verdict-*` byte-2 (runners/parallel.c, diekspor kedua
     fase, diimpor OR; verdict byte-object tetap hanya fase library).
     (2) Gate P-fast memo-kan entri `O` HANYA di bawah `build/` (ws_cache_noop.c) —
     output di luar build/ (bin/lib) tidak dicatat karena konsultan memo lain
     (deps/wsstate) bisa membacanya setelah fase penulisnya selesai di anak lain;
     `D` tetap tak dicatat. (3) `wsCacheAddObjectTree` memo-first untuk .o/.d
     proyek `compiled==0` (`cmdsStatCached`; false = file hilang yang baru
     di-stat memo — tidak perlu stat ulang), refactor `wsCacheAddEntry`/
     `wsCacheAddOMemo`. (4) Get TANPA guard `wsModeActive()` — save jalan
     setelah `wsModeSet(false)`; default konservatif `true` (stat segar).
   - Terukur (tests/complexws, 9 objek / 3 proyek, SATRIA_PROFILE): ubah 1 sumber
     core → `.d-cek` 9 → 4 stat, `output` 20 → 15, counter `memo gate` 5 .d + 5 O;
     identik jalur serial (SATRIA_WS_PAR=0) dan forked. Ubah sumber bade → 4 .d +
     4 O dari memo (seleksi mengikuti proyek yang kompilasi). Konvergensi: no-op
     HIT setelah save ber-memo (penuh, selektif-merge, dan sebagian). Build penuh
     (rm build/): memo 0 (semua stat segar) + no-op HIT. Kit dsl 14/14, parser
     12/12.
   - Sisa risiko diterima (self-healing lewat gate metadata-refresh): embed object
     ditulis ulang dengan compiled==0 → boros 1 full-run, jarang; touch mtime
     .o/.d tanpa isi berubah → gate konvergen via hash-content match.
2. **[~] Ekor pass binary yang kosong — ANALISIS SELESAI (5/11/2026), usulan naif DITOLAK.**
   - Pengukuran (cold → 1 sumber diubah dengan komentar, `.o` identik, workspace n=500,
     `SATRIA_PROFILE`): total 36,8 ms, 0 binary di-link. Pass binary: `decide` 0,0 ms,
     `compile` 0,0 ms, `rt-groups` 2,6 ms, `fs-stamps` 0,4 ms, `fs-save` 3,8 ms.
     Ekor workspace (`save-headers` 2,0, `save-walk` 1,8, `save-commit` 1,2,
     `save-merge` 0,5, `tail-stamp` 2,7, `ws-routesync` 0,3, `ws-report` 0,3,
     `ws-blobflush` 0,2) ≈ 9 ms.
   - DITOLAK: melewati `fs-save` kedua di pass binary bin=0. Save itu dipanggil dengan
     `target` binary (`libOnly = false`), sedangkan save library memakai `target = "-"`
     (`cmds_wsstate.c` baris 254). Isi fast-state berbeda by design: entri `O` untuk
     target binary inilah yang membuat no-op berikutnya mengenali binary sebagai
     up-to-date. Melewatinya merusak no-op.
   - Yang tersisa realistis: `rt-groups` dihitung dua kali per run (1,9 ms di pass
      library, 2,6 ms di pass binary) — TERSEMAT item 5 langkah 1 + item 10
      (14/11): handle bersama menghapus open+parse per validasi, memo presence
      berindeks isi menghapus parse ulang; sisa per panggilan = iterasi
      in-memory 0,3–0,7 ms. Pengurangan jumlah save fast-state per run terkait
      item 4 (konsolidasi tulis per fase), yang harus didesain agar entri `O`
      target tetap tercatat.
   - Skala: di sandbox ini total run 37 ms; angka 1,07 dtk di todo diukur di proot pada
     workspace yang lebih besar. Proporsi di sini (bin=0 ≈ 7 ms dari 37 ms) belum
     dikonfirmasi di proot.
3. **[x] `collect+plan` + `decide` fase library — TERTUTUP (14/11/2026): analisis tuntas, tidak ada perubahan yang dibenarkan.**
   - Trace inkremental n=500 (satu sumber diubah): setiap sumber `.c` di-stat **tepat
     sekali** (502 stat; hanya sumber yang diubah 2×). Tidak ada traversal sumber ganda.
   - Objek `.o` di-stat 3× per run, dari tiga fase berbeda (sapuan berselang-seling
     decide, sapuan berurut-nama `fsListDir` di save-walk, sapuan acak-hash di validasi
     fast-state). Sapuan save-walk dan validasi bukan milik item 3 (lihat item 1 dan 4).
   - Kontrol: `SATRIA_DECIDE_FROM_PLAN=0` menghasilkan jumlah stat objek dan sumber yang
     IDENTIK (1466 `.o`, 502 sumber; total 3599 vs 3600). Jadi jalur plan tidak menambah
     stat dan decide tidak membaca ulang traversal.
   - Profil lokal: `collect+plan` 1,9 ms + `decide` 1,4 ms di pass library (≈ 9 % dari
     run 37 ms). Angka 590 ms di todo berasal dari proot dan tidak tercermin dalam
     jumlah stat di sini.
   - Verdict: tidak ada perubahan kode yang dibenarkan dari item ini. Peluang yang
     tersisa ada di sapuan objek save-walk dan validasi (terkait item 1 dan 4).

4. **[~] `fs-save` — ANALISIS TERUKUR (5/11/2026); penanda profil dipasang, konsolidasi BELUM.**
   - Penanda profil baru di `wsFastStateSave` (`cmds_wsstate.c`): `fs-prev`, `fs-entries`,
     `fs-blob`, `fs-router-rec` (cabang binary), dan `fs-lib-entries`, `fs-lib-router`
     (cabang library). Hanya mencatat waktu saat `SATRIA_PROFILE`; suite miniws tetap
     348 PASS / 0 FAIL.
   - Inkremental n=500, satu sumber diubah (compile 16,4 ms), total 42 ms:
     - Pass library: `fs-lib-entries` 6,0 ms (loop stat/hash + lib.cache),
       **`fs-lib-router` 4,5 ms** (tulis opts.cache).
     - Pass binary: `fs-prev` 1,0 ms, `fs-entries` 0,7 ms, **`fs-router-rec` 2,2 ms**
       (tulis opts.cache), `fs-blob` 0,0 ms.
   - Kesimpulan: penulisan opts.cache lewat `optsRouterRecord` = 6,7 ms (≈ 16 %), lebih
     besar daripada loop entri. Itu inti item 6 dan bukan item 4. Loop entri di sandbox
     ini murah (µs per entri); estimasi 332 ms di todo (proot) tidak diverifikasi di sini.
   - Batching dua penulisan opts.cache (lib dan binary) tidak aman tanpa overlay: pass
     binary membaca hasil pass library. Prasyarat sama dengan item 5 dan 6.
   - Sisa yang perlu diukur di proot: loop entri (stat fresh untuk `O`) dan latency
     tulis ws.state.
5. **[~] Memo `csOpen` lintas-panggilan — langkah 0 + 1 TERAPAN (14/11/2026); funnel penuh BELUM.**
   - Langkah 0 (siang): memo scan router berindeks isi — hash FNV-1a isi
     opts.cache, anti-poisoning pra/pasca parse, terukur 0,9–1,9 ms per hit.
   - Langkah 1 (sore, menggantikan langkah 0 yang redundan): handle store
     `opts.cache` bersama per proses — dibuka SEKALI (`optsStoreShared`,
     `src/cache/opts_read.c`, deklarasi + kontrak di `router.h`); SEMUA pembaca
     (`optsRouterValidate`, `wsRouterValidate`) dan penulis
     (`optsRouterRecord`, `wsRouterRecord`, `wsRouterDrop`, `optsProjMetaStamp`,
     `optsSourcesRouteSync`, `wsOptsMetaSave`) memakai objek yang sama — tidak
     ada open+parse berulang per validasi. Penulis tetap `csCommit` langsung:
     latensi tulis per commit tidak berubah (itu domain item 6), tetapi sisi
     baca hilang; perubahan in-memory terlihat pembaca lain di proses yang sama.
   - Batas fork: anak menulis `opts.cache` di memorinya sendiri (salinan COW)
     lalu meng-commit ke disk; induk WAJIB `optsStoreReload()` sebelum penulis
     induk berikutnya — dipasang di akhir `runnerParallel` (parallel.c), satu
     kali per fase batch. Tanpa itu commit induk menimpa entri anak (kelas bug
     audit 31/10 §10). Serial fallback in-process aman (handle sama).
   - Semantik kegagalan penulis berubah sedikit: mutasi yang tidak di-commit
     (jalur gagal) TETAP tinggal di store bersama dan bisa ter-flush commit
     berikutnya — dulu di-discard oleh csClose. Semua mutasi itu update valid
     sebenarnya; kontrak didokumen di router.h.
   - Terukur (tests/complexws, run inkremental): `rt-scan` 1,7–4,5 ms →
     0,4–0,7 ms per validasi (iterasi in-memory); `fs-lib-router` 18,7 → 7,7 ms
     (sisa = latensi tulis tmp+rename per commit — domain item 6). Negatif
     terverifikasi: opts.cache korup → repair + rebuild; opts.cache hilang →
     gerbang ws menolak → jalur penuh 0-kompilasi meregenerasi (self-healing);
     hapus deps.cache + sentuh sumber → run penuh. No-op HIT pada semua jalur
     (penuh, serial, selektif-merge). Kit dsl 14/14, parser 12/12.
   - Hasil pengukuran analisis awal (5/11, trace inkremental n=500): `opts.cache`
     dibuka 8×, `deps.cache` 7×, `lib.cache` 6×, `fingerprint.cache` 6×,
     `sources.cache` 4×. `rt-groups` (`opts_read.c` baris 96) 1,9 ms + 2,6 ms
     per run (~12 % dari 37 ms di sandbox ini). Angka 35–50 ms di todo berasal
     dari proot.
   - Kenapa handle/memo luas belum bisa untuk BERKAS LAIN: berkas cache ditulis
     di ~12 titik di luar `csCommit` (`cache_store.c`): `opts_create.c:37`,
     `opts_update.c:17`, `state_blob.c:193/233/249`, `artifact.c:92/101`,
     `cmds_libcache.c:102`, `embed.c:326`, `ws_cache_noop.c:54/57/198/229/230/
     424/464/468`, `ws_cache_save.c:578/581` (fingerprint.cache, temp + rename
     sendiri), `compdb.c:112`, `state.c:309`. Memo yang tidak diinvalidasi di
     SEMUA titik = no-op yang salah (bug kelas audit 31/10 §10). Fork anak
     memperumit: anak menulis cache di memorinya sendiri; memo induk harus
     tahu batas fork (stage/import) — persis pola `optsStoreReload` di atas.
   - Pembacaan yang tampak berulang di `cmds_fast.c` (`fastSnapGet/Put/Delete`) adalah
     pasangan baca–tulis berselang, bukan duplikasi; memo harus memutus di setiap tulis.
   - Prasyarat (dipakai bersama item 6): SATU jalur tulis untuk semua berkas cache —
     `csCommit` menjadi satu-satunya penulis, dengan invalidasi memo per path dan
     batching tulis per fase. Ini refactor lintas modul; belum dikerjakan.
   - Urutan yang disarankan: (1) inventaris dan migrasi 12 titik tulis ke funnel,
     tanpa mengubah perilaku, diuji dengan suite penuh; (2) baru memo di funnel;
     (3) ukur di proot.
6. **[~] Tulis cache store: buka-tulis-ulang per fungsi — sisi BACA tuntas (14/11) + DEFERRAL opts.cache TERAPAN (14/11 sore); berkas lain BELUM.**
   - Sisi baca per-fungsi untuk opts.cache hilang lewat handle bersama (item 5
     langkah 1): pembaca tidak membuka file per panggilan.
   - DEFERRAL commit opts.cache terapkan sore 14/11: penulis tidak lagi
     `csCommit` sendiri (`optsRouterRecord`, `wsRouterRecord`, `wsRouterDrop`,
     `optsProjMetaStamp`, `wsOptsMetaSave`) — mutasi menumpuk di store bersama
     dan di-flush di batas fase: (a) anak fork sebelum `_exit` (atexit tidak
     jalan di `_exit` — flush eksplisit wajib), (b) induk akhir batch
     (`optsStoreFlush()` lalu `optsStoreReload()` — urutan wajib), (c) sebelum
     spawn child background (workspace.c — watcher tidak boleh menulis bareng
     induk), (d) backstop `atexit` di `optsStoreShared` untuk exit() mana pun.
     `optsSourcesRouteSync` SENGAJA tetap commit langsung: kontraknya
     membiarkan route basi pada run gagal — deferral akan meng-commit route
     segar pada run gagal lewat backstop. Marker profil `opts-flush`.
   - Terukur: strace rename `opts.cache` pada run inkremental tests/complexws
     = **1** per run (dulu 4+ commit). `fs-lib-router` 18,7 → 7,8 ms (sisa =
     parse cache grup pada memo miss, bukan tulis). Negatif terverifikasi:
     opts.cache korup (repair), hilang (gerbang menolak → self-heal),
     deps.cache dihapus (run penuh); no-op HIT jalur penuh/serial/selektif;
     single-project build/no-op/inkremental/clean/rebuild; kit dsl 14/14,
     parser 12/12.
   - Perbaikan ikutan: nama tmp `csCommit` kini unik per pid
     (`<file>.tmp<pid>`) — dulu nama tetap: dua proses yang commit file sama
     hampir bersamaan (anak-anak fork) bisa saling menimpa tmp. Race
     pre-existing, sekarang hilang.
   - Instrumen awal (5/11) pada inkremental n=500: `opts.cache` **commit 4×**
     (n = 6, 5, 5, 5 entri). Selain itu: `deps.cache` 1×, `lib.cache` 1×,
     `fingerprint.cache` 1×, `state.cache` 1×.
   - Pemicu commit opts.cache yang LAMA (kini didefer): `optsProjMetaStamp`
     (`cmds_fast.c` 549, 585), `optsSourcesRouteSync` (`state.c` 343,
     `ws_cache_noop.c` 366/633, `workspace.c` 630), `wsRouterRecord`
     (`ws_cache_save.c` 884). `WS_ROUTER_FILE` = `OPTS_CACHE_FILE`.
   - Sisa item 6: batching untuk BERKAS LAIN (deps/lib/fingerprint — masing-
     masing sudah 1×/run, jadi gain kecil) dan satu jalur tulis untuk semua
     berkas (prasyarat memo luas item 5). Batching dua penulisan opts.cache
     (lib dan binary) kini TIDAK DIPERLUKAN lagi untuk opts.cache: deferral
     sudah menggabungkannya.
   - Instrumen sementara di `csCommit` (dihapus lagi, diverifikasi identik) pada
     inkremental n=500: `opts.cache` **commit 4×** (n = 6, 5, 5, 5 entri), dan masing-masing
     benar-benar menulis (`csPut` melewati nilai identik; `csCommit` keluar bila tak
     `dirty`). Selain itu: `deps.cache` 1×, `lib.cache` 1×, `fingerprint.cache` 1×,
     `state.cache` 1×.
   - Pemicu `opts.cache`: `optsProjMetaStamp` (`cmds_fast.c` 549, 585), `optsSourcesRouteSync`
     (`state.c` 343, `ws_cache_noop.c` 366/633, `workspace.c` 630), `wsRouterRecord`
     (`ws_cache_save.c` 884). `WS_ROUTER_FILE` = `OPTS_CACHE_FILE` = `.satria/opts.cache`.
   - Biaya lokal kecil: `ws-routesync` 0,3 ms + `tail-router` 0,2 ms per run (≈ 1 %).
     Yang mahal per rewrite adalah latency open/rename, bukan ukuran data (entri 5–6).
   - Batching TIDAK aman sebagai "tunda commit": tiap penulis membaca lalu menulis
     (read-modify-write) file yang sama; penulis kedua akan membaca isi lama dan
     kehilangan update penulis pertama. Butuh overlay memori per proses untuk
     `opts.cache` (sama dengan prasyarat item 5: satu jalur tulis).
   - Urutan yang disarankan: (1) funnel tulis (item 5 langkah 1) untuk opts.cache;
     (2) overlay + flush di akhir fase, dengan flush di SEMUA jalur keluar (termasuk
     error) agar tidak ada update hilang; (3) ukur latency di proot.
7. **[x] Opsi level kompresi release — TERSELESAIKAN sebagai
   `release.optimize` / `pack.optimize` (default `false`)** — pengukuran:
   tar 5 ms, gzip -6 174 ms, gzip -1 56 ms (+18 % ukuran). Keputusan produk:
   biaya kompresi diserahkan ke user; `true` = kompresor level ter-FASTEST
   (gzip -1 / xz -1 / bzip2 -1) lewat dua langkah (`tar cf` flat lalu
   compressor -1, `src/pack/pack_tar.c`); `false` = perilaku `tar czf`
   lama tak tersentuh. Implementasi 05/11/2026: `PackConfig.optimize`
   (`src/config.h`), parse `packApply` (`pack_config.c`), route
   `release.optimize` (`ws_release.c`) + key bare top-level
   (`config_apply.c`); test: `tests/unit/report_artifact.sh` blok
   "optimize" (37/37 PASS); docs: `design/release.md`,
   `design/packaging.md`, `docs/reference/buildfile-fields.md`.
   Catatan ukuran miniws randah: -6 vs -1 hanya ±2 % pada corpus
   base64 54 MB (41 063 KB vs 41 942 KB).
8. **[x] Windows rename adapter — TERSELESAIKAN** (`fsRenameReplace` ada di
   `src/port/port_fs.c`); item dari startup-audit 20/10 boleh dicoret.
9. **[x] No-op syscalls 151** — tidak ada ruang besar di jalur ini; jangan buang
   waktu di sisi no-op sebelum item 1–6 (teks di `startup.md`: P-fast 2,68 s vs
   jalur penuh 5,24 s per no-op pada workspace pengujian lebih besar).

## Tindak lanjut infrastruktur uji (j1–j2)

- Catatan runner (pelajaran suite lama): target `scripts/unit.sh -b` WAJIB
  path ABSOLUT — runner men-cd ke salinan tmp sebelum mengeksekusi test,
  sehingga target relatif membuat fixture arg `$2` test gagal di-abspath
  (ditemukan 05/11/2026). Log + PID otomatis; jangan loop manual.
- [x] Re-basel benchmark (5/11/2026): `scripts/bench_check.sh -N` untuk 12 skenario
      (cold, noop, leaf, bade, mod, touch, hedit, sel, dep, libtouch, libhdr, rel) — semua
      `in accordance`. Angka per skenario dari `view.csv` (aktual vs harapan): cold 2908 ms
      (203 kompilasi), noop 3 ms (0), touch 5 ms (0), leaf 75 ms (1), mod 143 ms (1),
      hedit 243 ms (6), libhdr 188 ms (2), rel 33 ms (0). Skenario strace tidak dijalankan.
      Catatan: sandbox tidak punya cmake/ninja, sehingga hanya sisi satria yang diukur.
      Baris `compile=203 binary-links=4` yang dicetak di tiap log berasal dari
      Actual Work Audit (build cold yang di-prime sekali per proses), BUKAN dari skenario;
      verdict per skenario tetap berbasis `view.csv`. Baris itu menyesatkan dan perlu label.
- [x] `bench_real_project.sh -r 1 -j 2 tests/complexws` (titik pembanding): serial 0,25 s,
      paralel 0,26 s, rasio 0,96× (nproc = 1; log berisi 4 + 5 unit terkompilasi, status Success).
      Workspace ini kecil, jadi angkanya referensi, bukan pengukuran skala.


10. **[x] Pemeriksaan presence grup `optsCacheHasGroup` — memo berindeks isi (5/11/2026).**
   - Koreksi asumsi: `opts.cache` hanya 509 byte (7 baris: `P` + `PUT`/`PATCH`). Biayanya
     bukan ukuran data. Empat commit `opts.cache` per run total 0,76 ms; split menjadi tiga
     file menambah commit tanpa mengurangi byte. Usulan split dan `opts.blob` tidak
     terbukti menghemat.
   - Hotspot sebenarnya: `optsCacheHasGroup` dipanggil 10× per run inkremental (router
     record ×2 × {deps, fingerprint, lib} + rt-groups). Total terukur 11,2 ms (≈ 28 % run
     40 ms). Setiap panggilan mem-parse seluruh file ~95–118 KB (`csOpen` → `loadFile`),
     padahal jawabannya ya/tidak. Jawaban semua panggilan: "ada".
   - Pengukuran isi: dari 10 panggilan hanya 4 pasang (file, isi) unik.
   - Perubahan: memo per proses di `router.c`, kunci = (hash FNV-1a isi file, ukuran, proj),
     maks 16 entri. Hasil = fungsi murni dari byte file, sehingga tidak ada invalidasi manual
     yang bisa terlewat; isi berubah karena penulisan -> hash berubah -> miss. Jalur baca
     gagal kembali ke implementasi asli.
   - Terukur: miss 0,85–1,44 ms (4 panggilan), hit 0,14–0,20 ms (6 panggilan), total
     sekitar 4,3 ms per run (dari 11,2 ms). Total run median ~41 → ~38 ms (noise ±5 ms).
   - Verifikasi: suite miniws 348 PASS / 0 FAIL / 0 PRE; `bench_check.sh -N` 12 skenario
     semua `in accordance`, `cc == ecc` dan `miss = 0` di setiap skenario.
   - Batasan: kunci memakai FNV-1a 64-bit (konsisten dengan stamp isi di codebase).
     Tabrakan hash pada file yang sama dengan ukuran sama dapat menghasilkan presence
     yang salah; peluangnya kecil tetapi bukan nol. Sisa biaya: 4 parse miss (~3,3 ms)
     dan hashing byte-wise (dapat dipercepat dengan pembacaan word).

## Kontrak hasil audit ini

- Angka penanda yang diandalkan: no-op 151 syscalls / ~50 ms, incremental 1,23 s
  (miniws), cold 4,85 s vs floor gcc 0,38 s, distribusi buka cache (item 6).
- Ruang terbesar yang REALISTIS untuk minggu ini: item 1 → proyeksi CI 10k file
  tetap linear; item 2 → WS lokal −~10 % wall leaf; item 6 → mengurangi I/O
  cache store per fase.
- Batasan: refresh page cache antar-run membuat selisih kecil (<5 %) tak
  terpercaya di FS proot — gunakan pengukuran per operasi, bukan selisih wall kecil.

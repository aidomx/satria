# Implement design list

Berikut daftar design yang telah diimplementasikan dan statusnya terhadap kode
aktif (`src/`). Rujukan bukti berupa berkas source dan tes yang benar-benar ada.

## ✅ Implemented

- [startup.md](startup.md) — skeleton `.satria` sekali clean cold, fase startup
  (`* discovery / resolving / indexing / preparing`), `startup.state`.
  Bukti: `src/startup.c`, `src/workspace/ws_startup.c`, `src/background.c`;
  rekaman startup diuji `tests/unit/common/background.sh`.
- [startup-audit.md](startup-audit.md) — audit trail verifikasi startup (selesai;
  temuan sudah masuk desain/source).
- [background.md](background.md) — `background = true`: spawn (§2), `watch`
  replay+progres+summary (§3), PID/status/log/artefak (§4), gate clean (§4.4).
  Bukti: `src/background.c`; `tests/unit/common/background.sh` (miniws 39/0).
- [build_report.md](build_report.md) — format `# Execution`/`# Summary` +
  fallback no-op lama + `report.blob` replay; banner `* Release :` dihapus
  (off-spec). Bukti: `src/log/report_blob.c`, `src/log/build_report.c`,
  `src/workspace/ws_exec.c`, `src/cmds/cmds_pass.c`; 11 tes teks keluaran
  disesuaikan.
- [workspace.md](workspace.md) — `WsBuildfile` (nama resmi tanpa ekstensi;
  `Buildfile.ws`/`Buildfile.workspace` masih dibaca dengan peringatan
  deprecasi), build selektif `-w <nama>`, `--with-cdeps` (closure cdeps
  opt-in), gerbang no-op global + CompDB merged, release close. Bukti:
  `src/workspace/*` (`ws_startup`, `ws_exec`, `ws_cache_save`, `ws_lang`,
  compdb), `workspaceFileName` di `workspace.c`; tes
  `workspace_selective_target`, `selective_noop_full`, `router_noop`,
  `wsbuildfile_legacy`.
- [buildfile.md](buildfile.md) — semantik dua format, alias (`projects.core as
  core`, `core.library as corelib`), pemilihan bahasa `use project c|cpp|c++`
  (menghentikan walk `.cpp` di proyek C murni, termasuk di `WsBuildfile`),
  cache konfigurasi. Bukti: `src/config.c`, `src/config_apply.c`,
  `src/workspace/ws_lang.c`. Tes: `semantic_header_workspace`, `legacy_sweep`.
- [flags.md](flags.md) — `flags = MMD, MP`, suffix platform/compiler untuk
  flags/library/headers/defines/std. Bukti: `src/config_apply.c`; tes
  `opts_sources_sync`.
- [compile.md](compile.md) — mode ad-hoc `-o`/`-b`/`-F` (compile + link tanpa
  config, tanpa state `.satria`), `-jN` paralel, quick.conf via `-F`; fase
  compile/link/library dengan `compileLibraryOne` dan runner paralel. Bukti:
  `src/adhoc.c`, `src/bootstrap.c`, `src/cmds/cmds_build.c`,
  `src/cmds/cmds_pass*.c`, `src/runners/parallel.c`; perilaku ad-hoc
  diverifikasi manual (design/compile.md §3.1) — belum ada tes unit khusus.
- [compile.md](compile.md) — **anggota arsip statis unik**: nama anggota
  memakai basename object, sehingga object dengan basename sama di-staging
  (`<tag>_<nama>.o`) sebelum `ar rcs`; arsip lama dibuang saat rebuild dan isi
  arsip diverifikasi (`ar t`) ketika staging terjadi, sehingga arsip dobel/basi
  dari build lama menyembuhkan diri. Bukti: `src/cmds/cmds_lib.c`
  (`objsStageCollisions`, `objMemberName`, `archiveMembersMatch`).
- [embedded.md](embedded.md) — codegen sebagai satu-satunya mekanisme
  (`embedded.c/.h`, `getEmbed`, tabel), makro kompat `EMBED_*`, no-op
  content-based. Bukti: `src/embed*.c`; tes `embed_getembed` (common,
  PRECONDITION bila fixture tanpa embed) + `embed_stamp_skip`.
- [trace.md](trace.md) — trace build + `tracer.src`/`tracer_src.h` sync.
  Bukti: `src/trace.c`, `src/trace/`, `scripts/gen_tracer_src.sh`; tes
  `trace_summary`, `trace_src_sync`.
- [fast-state.md](fast-state.md) — state valid = no-op tanpa re-hash penuh;
  header eksternal tepat. Bukti: `src/cmds/cmds_fast.c`,
  `src/cmds/cmds_wsstate.c`; tes `faststate_external_header`.
- [packaging.md](packaging.md) — `pack.*` tarball+`.deb`+checksum;
  `pack.optimize` dua langkah tar→gzip-1 level FASTEST. Bukti:
  `src/pack/pack.c`; CI MSVC smoke lulus; `format = deb` MENAMBAHKAN `.deb`.
- [release.md](release.md) — `release.format = deb` tambah `.deb` (tarball
  tetap), `release.optimize`, dan **`--dev`**: fase release/pack diabaikan satu
  run tanpa menyentuh state (ditolak bersama `release`/selektor `--`). Bukti:
  `src/workspace/ws_release.c`, `src/bootstrap.c`, `src/satria.c`,
  `src/cmds/cmds_lib.c`; docs `docs/guide/releases.md`,
  `docs/reference/cli.md` §Dev mode.
- [profile.md](profile.md) — profil eksekusi build (phases/mono ms). Bukti:
  `src/profile.c`, `src/prof/`, `src/prof_sandbox.c`, `src/prof_steps.c`.
- [interactive_init.md](interactive_init.md) — `satria init -p` scaffold
  interaktif (termasuk pilihan bahasa c/cpp). Bukti: `src/commands.c`
  (`askLine`, pre-cek overwrite), `src/commands.h`.
- [cli.md](cli.md) — tabel perintah + bantuan `cmd.txt` (`SATRIA_CMD` sumber
  truth urutan pertama), `satria test` delegasi runner. Bukti:
  `src/cmdhelp.c`, `src/cmd.txt`, `src/test.c`; tes `cmdhelp_crlf`.
- [diagnostics.md](diagnostics.md) — NO_COLOR (standar no-color.org) + warna
  TTY + pola warning MSVC `warning C<n>:`. Bukti: `src/commands.c`.
- [convert.md](convert.md) — konversi Ninja/Makefile (`-xf`/`-xcf`). Bukti:
  `src/nc/nc_convert.c`, `src/make/mk_convert.c`, `src/makeconv.h`,
  `src/ninjaconv.h`; tes `legacy_sweep`.
- [cdeps_group.md](cdeps_group.md) — cdeps flag/idx-group, tolak key tak
  dikenal. Bukti: `src/deps/`; tes `cdeps_flag`, `cdeps_group`,
  `cdeps_key_reject` (*klasifikasi [cold] sementara — tunggu tinjauan, lihat
  `classes.conf`*).
- [packaging CI](../.github/workflows/tests-manual.yml) — workflow manual
  Windows/MSVC: bootstrap→self-host→unit suite→smoke `pack.optimize`.

## 🔶 Implement sebagian

- [unit_test.md](unit_test.md) — runner `scripts/unit.sh` (konsumen tipis
  `scripts/unit_dev.sh`): scope v1 lengkap per bagian 2–6, 7, 8, 10-11 + `-t`
  seleksi + §9 watcher `-w` (pemantau read-only atas staging, bukan pemicu
  run). `satria test <target>` men-delegasikan ke runner. Interactive
  configuration (mantan §7) dihapus dari design. Tabel status lengkap:
  unit_test.md §13; ringkasan pemakaian di `docs/reference/unit-test.md`.
- [unit_test_dsl.md](unit_test_dsl.md) — DSL `.test` M0–M3 selesai
  (`tests/unit/lib/dsl.sh`, `scripts/lib/parser.sh`, 38 berkas `.test`/`.t`,
  kit konforman `tests/unit/dsl/`); M4 (port parser/evaluator/runner ke C di
  dalam satria) belum dimulai.
- [bench_ws_output.md](bench_ws_output.md) — `bench/run.sh`+`jobs.sh`+
  `scale-build.sh` ada; format keluaran ws belum sepenuhnya mengikat design.

## ❓ Perlu verifikasi pemilik

- [architecture.md](architecture.md) — dokumen peta; kesesuaian bagian
  "lingkungan khusus" (Windows/MSVC/jalur tanpa `.d`) belum diaudit ulang
  pasca reshuffle runner.

## Catatan status pengujian tim

- miniws (runner stabil, 30 tes): **PASS=359 FAIL=0 PRE=0** (ukur terakhir di
  host Linux, termasuk `report_artifact`, `p_meta_noop`, `cache_noop`, dan
  `wsbuildfile_legacy`).
- complexws (runner stabil, 30 tes): **PASS=359 FAIL=0 PRE=0** (ukur terakhir
  di host Linux, termasuk `wsbuildfile_legacy`).
- rupaws: `background.sh` 39/39 pasca fix **fixture staged bersih** (`$FIX` di
  runner — akar: `tests/rupaws/.satria/startup.state` menyebabkan
  coldStart=false); suite penuh belum dijalankan lagi (berat — vendor ke
  CI/opsi `-t`).
- `-j` tiga lapis: runner `-j N` → `UNIT_JOBS`; tes `-j${UNIT_JOBS:-2}`
  (fallback 2); binary tanpa `-j` baca env `SATRIA_JOBS` sebelum fallback nproc.

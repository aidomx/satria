# Implement design list

Berikut daftar design yang telah diimplementasikan :

## ✅ Implemented

- [startup.md](startup.md) — skeleton `.satria` sekali clean cold, fase startup
  (`* discovery / resolving / indexing / preparing`), `startup.state`.
  Bukti: `src/startup.c`, `src/workspace/ws_startup.c`, `src/background.c`;
  rekaman startup diuji `tests/unit/common/background.sh`.
- [startup-audit.md](startup-audit.md) — audit trail verifikasi startup (selesai;
  temuan sudah masuk desain/source).
- [background.md](background.md) — `background = true`: spawn (§2), `watch`
  replay+progres+summary (§3), PID/status/log/artefak (§4), gate clean (§4.4).
  Bukti: `src/background.c`; 39 asersi `tests/unit/common/background.sh`
  (miniws 39/0, rupaws 39/0 pasca fix fixture staged).
- [build_report.md](build_report.md) — format baru `# Execution`/`# Summary` +
  fallback no-op lama + `report.blob` replay; banner `* Release :` dihapus
  (off-spec). Bukti: `src/log/report_blob.c`, `src/workspace/ws_exec.c`,
  `src/cmds/cmds_pass.c`; 11 tes teks keluaran disesuaikan.
- [workspace.md](workspace.md) — `Buildfile.ws`, build selektif `-w <nama>`,
  gerbang no-op global + CompDB merged, release close. Bukti: `src/workspace/*`
  (`ws_startup`, `ws_exec`, `ws_cache_save`, compdb); tes `workspace_selective_target`,
  `selective_noop_full`, `router_noop`.
- [embedded.md](embedded.md) — codegen sebagai satu-satunya mekanisme
  (`embedded.c/.h`, `getEmbed`, tabel), makro kompat `EMBED_*`, no-op
  content-based. Bukti: `src/embed*.c`; tes `embed_getembed` (common,
  PRECONDITION bila fixture tanpa embed) + `embed_stamp_skip`.
- [trace.md](trace.md) — trace build + `tracer.src`/`tracer_src.h` sync.
  Bukti: `src/trace.c`, `src/trace/`, `scripts/gen_tracer_src.sh`; tes
  `trace_summary`, `trace_src_sync`.
- [fast-state.md](fast-state.md) — danced state kalau valid: no-op tanpa
  re-hash penuh; header eksternal tepat. Bukti: `src/cmds/cmds_fast.c`,
  `src/cmds/cmds_wsstate.c`; tes `faststate_external_header`.
- [packaging.md](packaging.md) — `pack.*` tarball+`.deb`+checksum; `pack.optimize`
  dua langkah tar→gzip-1 level FASTEST (FNAME `out.tar.part`). Bukti:
  `src/pack/pack.c`; CI MSVC run 42156432001 smoke lulus; `format = deb`
  MENAMBAHKAN `.deb` (keputusan, `src/pack/pack.c:245`).
- [release.md](release.md) — `release.format = deb` tambah `.deb` (tarball tetap);
  docs `docs/guide/releases.md`. Bukti: `src/workspace/ws_release.c`.
- [profile.md](profile.md) — profil eksekusi build (phases/mono ms). Bukti:
  `src/profile.c`, `src/prof/`, `src/prof_sandbox.c`, `src/prof_steps.c`.
- [interactive_init.md](interactive_init.md) — `satria init -p` scaffold
  interaktif. Bukti: `src/commands.c` (`askLine`, pre-cek overwrite),
  `src/commands.h`.
- [cli.md](cli.md) — tabel perintah + bantuan `cmd.txt` (`SATRIA_CMD` sumber
  truth urutan pertama). Bukti: `src/cmdhelp.c`, `src/cmd.txt`; tes `cmdhelp_crlf`.
- [buildfile.md](buildfile.md) — semantik dua format, alias (`projects.core as
  core`, `core.library as corelib`), cache konfigurasi. Bukti: `src/config.c`,
  `src/config_apply.c`; tes `semantic_header_workspace`, `legacy_sweep`.
- [flags.md](flags.md) — `flags = MMD, MP`, trans/translate flag toolchain.
  Bukti: `src/config_apply.c`; tes `opts_sources_sync`.
- [compile.md](compile.md) — pass compile/link/library, `compileLibraryOne`,
  runner paralel (fork+stage), serial fallback. Bukti: `src/cmds/cmds_build.c`,
  `src/cmds/cmds_pass*.c`, `src/runners/parallel.c`.
- [diagnostics.md](diagnostics.md) — NO_COLOR (standar no-color.org) + warna TTY.
  Bukti: `src/commands.c:226-252` (`NO_COLOR`, `SATRIA_NO_COLOR`, TERM).
- [convert.md](convert.md) — konversi Ninja/Makefile. Bukti: `src/nc/nc_convert.c`,
  `src/make/mk_convert.c`, `src/makeconv.h`, `src/ninjaconv.h`; tes `legacy_sweep`.
- [cdeps_group.md](cdeps_group.md) — cdeps flag/idx-group, tolak key tak dikenal.
  Bukti: `src/deps/`; tes `cdeps_flag`, `cdeps_group`, `cdeps_key_reject`
  (*klasifikasi [cold] sementara — tunggu tinjauan, lihat `classes.conf`*).
- [packaging CI](../.github/workflows/tests-manual.yml) — workflow manual
  Windows/MSVC: bootstrap→self-host→unit suite (24 tes)→smoke pack.optimize.

## 🔶 Implement sebagian

- [unit_test.md](unit_test.md) — runner `scripts/unit_dev.sh` (stabil `scripts/unit.sh`
  = konsumen tipis yang menyalin `unit_dev.sh` ke berkas sementara): scope v1
  lengkap per bagian 2–6, 7, 8, 10-11 + `-t` seleksi. Interactive configuration
  (mantan §7) dihapus dari design — hanya menambah beban kode runner. Sisa:
  §9 watcher `-w`. Tabel status lengkap: unit_test.md §13.
- [bench_ws_output.md](bench_ws_output.md) — `bench/run.sh`+`jobs.sh`+`scale-build.sh`
  ada; format keluaran ws belum sepenuhnya mengikat design.

## ❓ Perlu verifikasi pemilik

- [architecture.md](architecture.md) — dokumen peta; kesesuaian bagian
  "lingkungan khusus" (Windows/MSVC/jalur tanpa `.d`) belum diaudit ulang
  pasca reshuffle runner.

## Catatan status pengujian tim

- miniws (runner stabil, 24 tes): **PASS=255 FAIL=0 PRE=0**.
- rupaws: `background.sh` 39/39 pasca fix **fixture staged bersih** (`$FIX` di
  runner — akar: `tests/rupaws/.satria/startup.state` menyebabkan coldStart=false);
  suite penuh belum dijalankan lagi (berat — vendor ke CI/opsi `-t`).
- `-j` tiga lapis: runner `-j N` → `UNIT_JOBS`; tes `-j${UNIT_JOBS:-2}` (fallback 2);
  binary tanpa `-j` baca env `SATRIA_JOBS` sebelum fallback nproc.

# design/

Dokumen desain satria.

> **Catatan provenance (3 Okt 2026):** direktori `design/` asli hilang bersama
> penghapusan tak sengaja direktori satria dan tidak ikut dalam backup. Isi di
> sini adalah **rekonstruksi** dari arsitektur kode aktif (`src/`) dan
> keputusan yang tercatat pada sesi kerja — bukan salinan persis dokumen lama.
> Bila ada perbedaan, kode adalah sumber kebenaran. Status implementasi per
> dokumen dicatat di [implement_list.md](implement_list.md).

## Daftar dokumen

### Inti

| Dokumen | Isi |
|---|---|
| [architecture.md](architecture.md) | Peta modul, alur build, state/cache, kinerja, lingkungan khusus |
| [bootstrap.md](bootstrap.md) | Bootstrap & dispatch CLI (`-f`, `-j`, `-w`, `--dev`, `--with-cdeps`) |
| [buildfile.md](buildfile.md) | Semantik Buildfile: dua format, alias, bahasa proyek, workspace, `-xf`, cache konfigurasi |
| [flags.md](flags.md) | Suffix platform/compiler untuk `flags`, `library`, `headers`, `defines`, `std` |
| [compile.md](compile.md) | Mode ad-hoc (`-o`/`-b`/`-F`), kompilasi, link, fase library statis |
| [cli.md](cli.md) | Perintah CLI mandiri: `self-upgrade`, `self-uninstall`, bantuan `cmd.txt` |
| [workspace.md](workspace.md) | `WsBuildfile`, build selektif `-w`, cdeps, release workspace |
| [cdeps_group.md](cdeps_group.md) | Grup `cdeps` level atas: flag, index-group, penolakan key tak dikenal |
| [convert.md](convert.md) | Konversi Ninja/Makefile (`-xf`, `-xcf`) |
| [diagnostics.md](diagnostics.md) | Warna output compiler, `NO_COLOR`, `SATRIA_NO_COLOR` |

### Build state & keluaran

| Dokumen | Isi |
|---|---|
| [startup.md](startup.md) | Fase startup, `.satria`, `startup.state` |
| [startup-audit.md](startup-audit.md) | Audit `startup.md` vs implementasi (selesai) |
| [fast-state.md](fast-state.md) | Fast no-op, fingerprint per proyek/workspace, header eksternal |
| [build_report.md](build_report.md) | Format Build Report, `report.blob`, replay no-op |
| [background.md](background.md) | `background = true`, `satria watch`, PID/status/log |
| [trace.md](trace.md) | `satria trace`, sinkronisasi `tracer.src` |
| [profile.md](profile.md) | `satria profile <context>` (archive, embed, library, binary, project, workspace) |
| [bench_ws_output.md](bench_ws_output.md) | Format keluaran bench workspace |
| [bench_c.md](bench_c.md) | Benchmark workspace di C (`satria bench`) |

### Artefak

| Dokumen | Isi |
|---|---|
| [embedded.md](embedded.md) | Codegen `embedded.c/.h`, `getEmbed`, makro kompat `EMBED_*` |
| [packaging.md](packaging.md) | `pack.*`: tarball, `.deb`, checksum, `pack.optimize` |
| [release.md](release.md) | `release.*` workspace, `release.format = deb`, `--dev` |

### Pengujian & interaksi

| Dokumen | Isi |
|---|---|
| [unit_test.md](unit_test.md) | Runner unit test (`satria test`, `scripts/unit.sh`, klasifikasi, watcher) |
| [unit_test_dsl.md](unit_test_dsl.md) | DSL `.test`: parser, ekspresi, direktif, milestone M0–M4 |
| [interactive_init.md](interactive_init.md) | `satria init -p` scaffold interaktif |

## Prinsip yang dipegang

- Satu binary, tanpa dependensi build wajib di luar compiler C — bootstrap
  lewat `install.sh --dev` (compiler C langsung, tanpa cmake; Windows:
  `install.ps1 --dev`).
- Buildfile deklaratif; dua format (legacy & `use alias`) didukung bersamaan
  dan tidak saling mengganggu. Bahasa proyek dapat dipatok dengan
  `use project c` / `use project cpp`.
- Portabel sampai MSVC: fitur inti tidak bergantung tool UNIX-only
  (contoh: .deb dirakit manual dari C, tanpa `dpkg-deb`).
- Berkas source < 500 baris; modular per subdirektori `src/<tema>/`.
- Termux/proot: hemat syscall; tidak pernah hardcode `/tmp`
  (lihat [architecture.md](architecture.md) § Lingkungan khusus).

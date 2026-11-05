# todos — 4 November 2026: audit ruang optimasi performa

> Audit ini MENGGABUNGKAN temuan baru hari ini (4/11/2026) dengan sisa ruang yang
> sudah terpetakan di tiga audit sebelumnya (`problems/15_10_2026_audit_v94_optimasi.md`,
> `problems/27_10_2026_cold_build_overhead.md`, `problems/30_10_2026_incremental_overhead.md`).
> Tujuan: daftar tunggal "apa yang masih bisa ditingkatkan", terukur, dan tidak duplikatif.
>
> Konteks mesin: aarch64 + proot, job efektif **j1–j2**, `stat()` 257–461 µs,
> fork+exec ~9 ms (audit 27/10). Semua angka lokal EXTRAPOLASI harus per operasi
> (metodologi audit 30/10), bukan ms mentah.

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
| `scripts/unit.sh -b -j2` | DIDUKUNG (opsi `-b`, `-o`) | rupaws suite dua kali terputus restart; **pakai ini**, jangan loop manual |

## Ruang optimasi — peringkat (sisa dari audit 30/10 §11–13 + temuan baru)

> Yang sudah dikerjakan DAN terukur: `.d` dibaca sekali (−2,8 s), `fs-save`
> reuse-hash (−1,5 s), vonis miss pasti + entri basi diutamakan (−1,1 s),
> memo stat induk (−50 %), memo object/`.d` (−37 %), pemulihan `O`/`.d` hilang.

1. **[ ] `.d-cek` di `save-walk` — 1 stat per `.d` per build (n=500: 1.001 stat ≈ 286 ms).**
   Keputusan "`.d` ditulis ulang run ini?" butuh pengetahuan milik anak (fork).
   Dua jalur: (a) staging `objectsChanged` diperluas ke "ada kompilasi" sehingga
   proyek verdict-nol-compile `save-walk`-nya dilewati; (b) staging penuh daftar
   `.d` kotor serupa `ws-verdict-*`. (a) risiko lebih kecil.
2. **[ ] Ekor pass binary yang kosong — residu ~1,07 dtk.** `fastTail` sudah
   memangkas decide, sisa `cmdsBuildPassFinish` + verifikasi library masih
   dibayar penuh walau `bin=0`. Sama polanya dengan kandidat "pass binary
   kosong" (1,8 s → sisa terus menyusut; bukti terukur di §11).
3. **[ ] `collect+plan` + `decide` fase library n=500 ≈ 590 ms** — dua
   traversal source/object per pass. Menggabungkan plan libpass → reuse oleh
   binary-pass hanya bila verdict tak berubah.
4. **[ ] `fs-save` residu 332 ms** — stamp entri tak tersentuh kini reuse;
   sisa = stat entri baru/berubah + tulis file. Opsi: tulis store satu kali
   per fase, satu arah dengan batch tulis di item (6).
5. **[ ] Memo `csOpen` lintas-panggilan (`rt-groups` ~35–50 ms/run)** — masih
   terbuka dari audit 30/10 §5.6; kecil tapi lintas semua jalur (single + ws).
6. **[ ] Tulis cache store: buka-tulis-ulang per fungsi.** Pengukuran hari ini
   memperlihatkan `opts.cache` dibuka 11× dengan 5× rewrite `.tmp` dalam
   SATU run inkremental (sync route per fase/proyek). Konsolidasi: batch sync
   route di akhir fase (satu rewrite) — setara semantik karena PATCH additive.
   Kertas kaji: `src/cache/opts_update.c` (4 `csOpen`), `opts_read.c`.
7. **[x] Opsi level kompresi release — TERSELESAIKAN sebagai
   `release.optimize` / `pack.optimize` (default `false`)** — rupaws:
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

- [ ] Validasi suite `tests/rupaws` (pernah dua-gagal-putus-restart):
      `bash scripts/unit.sh -b -o /tmp/opencode/unit_rupaws.log -j 2
      /home/aidomx/satria/tests/rupaws` — target WAJIB path ABSOLUT:
      runner men-cd ke salinan tmp sebelum mengeksekusi test, sehingga
      target relatif membuat fixture arg `$2` test gagal di-abspath
      (ditemukan 05/11/2026). Log + PID otomatis; jangan loop manual.
- [ ] Setelah item 1–3 dieksekusi: re-basel benchmark harian via
      `scripts/bench_check.sh` (9 skenario) + satu run `bench_real_project.sh`
      (rupaws, `-t satria -s leaf,noop`) sebagai titik pembanding audit ini.

## Kontrak hasil audit ini

- Angka penanda yang diandalkan: no-op 151 syscalls / ~50 ms, incremental 1,23 s
  (miniws), cold 4,85 s vs floor gcc 0,38 s, distribusi buka cache (item 6).
- Ruang terbesar yang REALISTIS untuk minggu ini: item 1 → proyeksi CI 10k file
  tetap linear; item 2 → WS lokal −~10 % wall leaf; item 6 → mengurangi I/O
  cache store per fase.
- Batasan: refresh page cache antar-run membuat selisih kecil (<5 %) tak
  terpercaya di FS proot — gunakan pengukuran per operasi, bukan selisih wall kecil.

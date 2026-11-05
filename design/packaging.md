# Packaging `pack.*` — desain & spesifikasi

Status: **selesai diimplementasi & teruji** (2 Okt 2026). 

## Tujuan

- Hasil build bisa dikemas: tarball (tar.gz/xz/bz2/tar) dan `.deb` — tanpa
  dependensi `dpkg-deb` maupun binary `ar`, agar portabel sampai MSVC.
- `.deb` dirakit **manual**: header ar ditulis di C; staging control/data via
  `tar`+`gzip` shell yang tersedia di semua environment target (Termux
  termasuk).

## Aktifasi & bentuk output

- `pack.files` / `pack.output` menandai pack aktif (`pack.requested`).
- Section `pack` mandiri di Buildfile, ATAU key bare via
  `projects.<n>.pack as pack` di workspace (prefix `projects.<n>.` di-strip
  saat sintesis).
- Proyek tanpa sources otomatis `output.binary=false` (pack-only); proyek
  binary tetap bisa pack (hybrid) — kedua jalur memanggil pack setelah build
  sukses.
- `pack.format = deb` **menambah** `.deb` di samping tarball (dua artefak);
  bila template output berakhiran `.deb` → mode deb-only (tarball tidak
  dibuat).
- Template nama: `{name} {version} {os} {arch}`. Ekstensi tar dikenal
  (`.tar.gz/.tar.xz/.tar.bz2/.tgz/.tar`) diganti `.deb` untuk nama deb.

## Key

| Key | Arti | Default |
|---|---|---|
| `pack.name` | nama paket | o.binaryName (dari finalize) |
| `pack.version` | versi | `0.0.0` |
| `pack.output` | template artefak | `dist/{name}-v{version}.tar.gz` |
| `pack.compress` | `gzip\|none\|xz\|bz2` (`"bzip2"` → `bz2`) | dari ekstensi output |
| `pack.optimize` | `true` = kompresor level ter-FASTEST (`gzip -1`/`xz -1`/`bzip2 -1`, dua langkah tar lalu compressor) — arsip lebih besar, biaya jauh lebih murah; ukuran diserahkan ke user | `false` (perilaku `tar czf` tak berubah) |
| `pack.checksum` | `sha256` | — |
| `pack.format` | `tar\|none\|deb` (lain → error) | `tar` |
| `pack.files` | entri yang dikemas (dir/file) | — |
| `pack.deb.*` | `maintainer`, `description`, `prefix`, `architecture` | `satria <satria@localhost>`, pack.name, `/usr/local`, host-derived |

## Deb

- `pack.optimize` hanya mengubah TARBALL (`packBuildTar`); staging internal
  `.deb` (`control.tar.gz`, `data.tar.gz` via `debTar`, `pack_deb.c`) tetap
  memakai `tar czf` default — ukuran .deb wilayah format distro, bukan knob
  kecepatan release (keputusan scope 05/11/2026).
- Pemetaan: tiap entri `pack.files` dipasang di `<install_prefix>/<entri>`.
- Mode file: `0755` untuk komponen pertama `bin/` atau `*.so`; selain itu bit
  +x sumber dipertahankan (`fsGetMode`).
- Control: `Package`/`Version`/`Architecture`
  (`amd64/arm64/i386/riscv64/armhf/all`, override `pack.deb.architecture`) /
  `Maintainer`/`Installed-Size`/`Description`.
- Layout ar: `!<arch>\n` + member `debian-binary` (`"2.0\n"`),
  `control.tar.gz`, `data.tar.gz`; header member 60 byte
  `%-16.16s%-12ld%-6s%-6s%-8s%-10lld\`\n`; member ganjil di-pad `\n`.
- Staging di `.satria/pack/` — bukan `/tmp`, aman di Termux
  (lihat architecture.md § Lingkungan khusus).

## Freshness & checksum

- Rebuild hanya bila entri terbaru dari `pack.files` (folder di-walk rekursif,
  mtime direktori ikut dihitung) > mtime artefak; aturan sama untuk file
  checksum.
- Checksum: SHA-256 murni C (FIPS 180-4, `pack_sha256.c`); file berisi
  `<hex>  <basename>` (dua spasi, kompatibel `sha256sum -c` — terverifikasi
  cocok coreutils).

## clean

Menghapus staging `.satria/pack`; **tidak** menyentuh `dist/` (bisa dirujuk
`embedded.*` proyek lain).

## Hasil uji (2 Okt 2026)

- Workspace alias+bare → `dist/wspkg-v3.1.4-linux-aarch64.deb` valid
  (`dpkg-deb --contents`: `usr/local/app/bin/app` 0755).
- Pack-only section form, prefix `/opt/solo`.
- Hybrid binary+pack + `sha256sum -c` OK.
- No-op up-to-date; touch → rebuild; file hilang → pesan error jelas;
  clean menghapus staging, bukan `dist/`.
- Self-host rc=0 (29 file); build 0 warning; regresi `~/rupa/v0.2.2`
  61 PASS / 0 FAIL.

## Catatan terbuka

- Template tanpa `{version}` → nama file tanpa versi (diamankan, bukan error).
- Windows/MSVC belum diuji nyata di lokal (jalur `_chmod` sudah disiapkan).
  Pengujian on-demand: workflow **manual** `.github/workflows/tests-manual.yml`
  (`workflow_dispatch`, windows-latest, hanya MSVC — `ilammy/msvc-dev-cmd`
  + `install.ps1 --dev`): unit suite via `scripts/unit.sh` + smoke
  `pack.optimize = true` (bukti dua-langkah lewat FNAME header gzip =
  `out.tar.part`). Dipicu dari perangkat yang punya `gh`:
  `gh workflow run tests-manual.yml [-f suite=tests/miniws -f suite2=<dir>
  -f jobs=2]`. Log suite selalu diunggah sebagai artifact `unit-logs-msvc`.
  Run pertama (05/11/2026), log `/tmp/tests-manual.log`:
  * bootstrap + self-host MSVC LULUS setelah fix LNK2019 (`tarDetFlags`
    memanggi `pclose` mentah → `pcloseRB`; `2>/dev/null` → `2>nul` untuk
    cmd.exe, `src/util.c`).
  * Kelas FAIL palsu unit suite (infrastruktur, bukan produk): input fixture
    relatif + runner `cd` tmp (abspath test gagal) dan checkout CRLF runner
    (gen_tracer hasil beda byte). Keduanya dibenahi di workflow:
    fixture/log ikut absolut via `cygpath`, checkout `core.autocrlf false`.
  * Sisa deviasi objek Windows (dianggap baseline, bukan regresi):
    `trace_summary` (trace menolak — LD_PRELOAD tak ada), `build_resume` D
    (semantik SIGINT git-bash tak 1:1), `faststate_external_header`
    ("steady state is no-op") — satu-satunya yang perlu investigasi lanjut.
- Ide lanjut: `pack.deb.depends`/`section`, mode deb-only eksplisit via key,
  penandatanganan `.deb`.

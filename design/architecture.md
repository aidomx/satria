# Arsitektur satria

> REKONSTRUKSI (3 Okt 2026) — dokumen asli hilang bersama penghapusan tak
> sengaja direktori satria dan tidak ikut backup. Ditulis ulang dari arsitektur
> kode aktif (`src/`) plus keputusan yang tercatat di sesi kerja. Kode adalah
> sumber kebenaran bila ada perbedaan.

## Gambar umum

satria adalah build tool untuk project C yang dikelola satu file konfigurasi
deklaratif (`Buildfile`, atau `Buildfile.ws` untuk banyak proyek).
Satu binary tanpa dependensi runtime; bootstrap build lewat
`install.sh --dev` (compiler C langsung, tanpa cmake; Windows:
`install.ps1 --dev`). satria self-hosting: `./install.sh --dev` lalu
`./build/bin/satria` (menulis `bin/satria`).

Target platform: Linux, macOS, Windows (MSVC + MinGW), Termux (native dan
proot-distro).

## Alur eksekusi

```text
main.c                 dispatcher tipis
  └─ satria.c            satriaRun(): parse argv (-f -j -w -xf/-xcf), pilih jalur
      └─ commands.c    routing command + Summary akhir
          ├─ uses/     workspace (-w): sintesis Buildfile per proyek,
          │            depends_on, build selektif -w <nama>
          ├─ config.c  parse Buildfile (dua format) + finalize + routing key
          │   └─ cfg/  config cache (format v6): Config utuh termasuk PackConfig
          ├─ nc/       konversi build.ninja → Buildfile (untuk -xf/-xcf)
          ├─ compile.c kompilasi paralel + link + library
          │   └─ deps/ graf dependensi header (.d + scanner), snapshot hash
          ├─ embed.c   embedded modules + configFinalizeEntry
          ├─ compdb.c  compile_commands.json (dengan cache)
          └─ pack/     packaging tar/deb/checksum (packRun)
```

Command: build (default), clean, init, version, help. Cabang khusus: proyek
pack-only (tanpa binary) → `cmdsPackOnlyProject`; proyek binary sukses →
`cmdsPackAfterBinary` sebelum Summary.

## Peta modul

### src/ (root)

| Berkas | Peran |
|---|---|
| main.c | entry point tipis |
| satria.c | satriaRun(), satriaVersion() (SATRIA_VERSION_EMBEDDED) |
| commands.c | routing command, Summary, help |
| config.c / config.h | parse + finalize + routing key (pack.* → pack/) |
| compile.c | kompilasi paralel (posix_spawn), link, library |
| embed.c / embed.h | embedded: arsip, codegen embedded.c/.h (getEmbed), configFinalizeEntry |
| compdb.c | compile_commands.json + cache |
| util.c | util umum |
| portability.c/.h | deklarasi portabilitas (fsSetMode/fsGetMode dll.) |

### src/<tema>/ (hasil refactor modular, Sep–Okt 2026)

| Dir | Peran |
|---|---|
| cfg/ | config_cache.c — serialisasi cache konfigurasi (format v6) |
| cmds/ | implementasi command: cmds_lib (helper build + hook pack), cmds_state (clean dll.) |
| deps/ | deps_core (graf), deps_dotd (parse .d), deps_snapshot (hash konten) |
| nc/ | nc_parse / nc_expand / nc_convert / nc_util — ninja → Buildfile |
| pack/ | pack.c (dispatch, template, freshness), pack_config (parsing), pack_tar, pack_deb, pack_sha256 |
| prof/ | profil fase (SATRIA_PROFILE=1 → stderr; timespec_get C11, tanpa syscall) |
| port/ | port_fs (fsSetMode/fsGetMode, remove tree), port_proc (spawn proses) |
| uses/ | alias.c (alias `X as Y`), use.c, workspace.c (mode -w) |

Konvensi: satu berkas < 500 baris; tema baru = subdirektori baru; komentar dan
dokumentasi Bahasa Indonesia.

## State & cache

| Lokasi | Isi |
|---|---|
| `.satria/` config cache | Config ter-serialisasi (v6); hit bila input tak berubah |
| `.satria/deps.cache` | snapshot hash + edges (`!e`/`!k`) untuk keputusan konten |
| `.satria/pack/` | staging packaging (dihapus oleh `clean`; `dist/` tidak) |
| `.satria-embed-<n>.tmp` | temp fase embed |
| `build/version.h` | SATRIA_VERSION_EMBEDDED dari `.satria-version` |

Aturan penting: no-op tidak menulis apa pun; snapshot deps hanya direkam
setelah build sukses; pass rekam dilewati pada no-op murni.

## Kinerja (aturan yang sudah tertanam di kode)

- Kompilasi paralel default sebanyak core; command polos dijalankan langsung
  via posix_spawn (tanpa `/bin/sh` per job).
- `deps.cache` dimuat satu pass; cache edges `!e`/`!k` — file `.d` tidak
  dibuka ulang selama mtime sama; header anak diverifikasi shallow.
- `stat` per entri direktori → `d_type` dari `readdir`; `mkdir` berulang
  dilewati; mtime dari fase klasifikasi dipakai ulang saat keputusan link.
- Angka benchmark no-op/proot: lihat README.md § Benchmark.
- Pengukuran internal memakai profil fase (`SATRIA_PROFILE=1`), BUKAN strace —
  strace tidak aman/terdistorsi baik di proot (ptrace ganda) maupun Android
  native (ptrace di atas seccomp). Temuan 3 Okt 2026: cold workspace ~90%
  adalah waktu gcc di proot; no-op 0,2 s; satria ≈ ninja pada cold n=80
  (13,2 s vs 12,8 s). Rincian di `todos/2_10_2026.md`.

## Lingkungan khusus

### Termux / Android
- **NOTICE /tmp**: bila satria butuh temp dir, wajib cek `uname -o`; Android →
  `/data/data/com.termux/files/usr/tmp`. Status saat ini: satria bebas hardcode
  `/tmp` — staging pack di `.satria/pack/`, embed temp `.satria-embed-<n>.tmp`.

### proot-distro
- Syscall diawasi ptrace → biayanya puluhan kali Linux biasa; setiap fitur
  baru harus menahan jumlah openat/stat/read (lihat § Kinerja).

### Windows (MSVC / MinGW)
- chmod → `_chmod`/`_S_*` (port_fs); `SHFileOperationA` untuk remove tree;
  `ld -r -b binary` tidak ada → fallback C array embed; flag `-M*` dilewati.

## Bootstrap & rilis

- `install.sh`: mode default mengunduh `bin/satria` dari repo; `--dev`
  bootstrap build dengan `${CC:-cc}` tanpa cmake. Windows: `install.ps1`
  (hormati `CC`; link `shell32` untuk `fsRemoveTree`/SHFileOperationA di
  MSVC).
- CI: `.github/workflows/build.yml` (build, `install.sh|ps1 --dev`),
  `release.yml` (rilis, cara yang sama). CMakeLists.txt dihapus — build
  sepenuhnya lewat install script + self-host Buildfile.

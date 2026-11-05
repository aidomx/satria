# Audit v94 — kandidat optimasi — 15 Oktober 2026

**Baseline:** `rbot-v0_2_3_94` (`/home/claude/b94fix`) + perbaikan sesi ini: aturan `headers` (`I.` per scope, `headers.each` dengan penyaring nama, `auto`), pemeriksaan semantik header (`RBOT_HDR_SEM=1`, opt-in), perbaikan cdeps path.
**Cakupan:** inspeksi source jalur build workspace, dependensi, dan spawn; data ukur dari sandbox (1 core) serta profil device yang sudah dikumpulkan sebelumnya.
**Batasan:** audit ini statis dan berbasis sandbox. Tidak ada pengukuran baru di device. Angka device di bawah berasal dari sesi sebelumnya dan belum diulang pada v94.

## Skala prioritas

| Tingkat | Arti |
|---|---|
| sangat tinggi | selisih terukur besar dan konsisten, dampak langsung ke target (cold/incremental ≤1,2× ninja) |
| tinggi | penyebab kuat yang terukur sebagian, perlu profil untuk memastikan |
| sedang | manfaat nyata pada kasus tertentu, biaya atau risiko perlu ditimbang |
| rendah | selisih kecil atau sudah cukup baik |

## Kandidat optimasi

| ID | Tingkat | Area / lokasi | Bukti | Usulan |
|---|---|---|---|---|
| OPT-01 | **sangat tinggi** | Penjadwalan batch proyek: `wsRunBatch` (`src/uses/workspace.c`), default serial, fork hanya dengan `RBOT_WS_PAR=1` | Device cold ~2875 ms vs ninja ~1490 ms (≈1,9×). Profil: TU `bade` (3) dan `core` (3) dikerjakan berurutan per proyek. Sandbox 1 core tidak bisa menguji paralelisme. | Satu antrean job global lintas proyek, dengan urutan dependensi (library sebelum dependen, `cdeps` dua fase). Hipotesis, belum terukur di device. |
| OPT-02 | **tinggi** | Overhead per proyek pada jalur incremental: `startup+config`, `fingerprint`, `decide`, `compdb` di `cmdBuildEx` (`src/commands.c`) dan `cmds_state.c` | Sandbox (CI tidak dipakai di sini): `hedit`/`sel`/`dep`/`libhdr` ≈2× ninja dengan jumlah `obj`/`cc`/`lib`/`bin` identik. Profil device: startup 9–18 ms, fingerprint 3–13 ms, compdb 7–12 ms, decide 0,6–94 ms per proyek. | Profil `RBOT_PROFILE=1` pada skenario incremental di device, lalu pangkas biaya yang berulang per proyek (misalnya fingerprint dan compdb yang sama untuk semua proyek). |
| OPT-03 | **tinggi** (perlu verifikasi) | Spawn compiler/linker: `src/port/port_proc.c` (`resolveSpawnExe`, baris ~319) | v94 sudah punya `resolveSpawnExe`. Di CI (`dev git:4d2089d`, bukan v94 yang diuji di sini), strace menunjukkan 5603 `execve` dengan 4554 gagal pada cold. | Pastikan binary CI memuat `resolveSpawnExe`. Jika sudah, telusuri sumber `execve` yang gagal (bukan dari `posix_spawn` jalur compile). |
| OPT-04 | **sedang** | Pemeriksaan semantik header (`src/deps/deps_semhdr.c`, opt-in `RBOT_HDR_SEM=1`) | Edit `rupa.h` dengan prototype tak terpakai: 0 TU dikompilasi (dibandingkan 175 TU tanpa opsi), biner identik dengan build penuh. Biaya: membaca file subtree tiap kali header berubah. | Uji lebih luas (macro, struct, nama yang dipakai lewat header lain), lalu pertimbangkan default-on setelah lolos. |
| OPT-05 | **sedang** | Snapshot dependensi: `subtreeUpToDate` / `subtreeRecord` di `src/deps/deps_snapshot.c` | Sandbox: no-op `stat=96`, `opendir=0`. ninja no-op `stat=65`. Selisih 30 `stat` sudah dijelaskan sebagai cek validasi per file. | Kurangi `stat` yang tidak perlu pada no-op (misalnya dengan stamp folder), tanpa mengorbankan correctness. |
| OPT-06 | **rendah** | CompDB per proyek (`compdb` ~7–12 ms per proyek di device) | Profil device: `compdb unchanged (cache hit)` tetap memakan waktu. | Cache hash per proyek; lewati penulisan bila hasil tidak berubah. |
| OPT-07 | **rendah** | No-op total | Sandbox: rbot 3–5 ms, ninja 3–4 ms. Device: rbot 87 ms vs ninja 73 ms (≈1,2×). | Tidak perlu prioritas sekarang. Ikuti OPT-05 bila no-op menjadi target. |
| — | ditolak | `rbot -s` (flatten) | Parse `Buildfile.ws` 0,0 ms. `stat` tidak turun (96 vs 95). | Tidak dilanjutkan sebagai optimasi. Lihat `design/workspace.md`. |

## Temuan correctness (bukan optimasi, perlu diverifikasi lebih dulu)

| ID | Tingkat | Temuan | Tindak lanjut |
|---|---|---|---|
| COR-01 | **tinggi** | Skenario `mod` di CI: `bin` rbot 1 vs ninja 2, sedangkan validator memakai `bin=*`. `bade` menanam `rupa_modules`, sehingga arsip yang berubah seharusnya me-relink `bade`. | Bandingkan biner `bade` rbot dan ninja setelah skenario `mod`. Jika berbeda, ini bug correctness. |
| COR-02 | sedang | Object dari source yang dihapus tetap ada di `build/` (`k9.o`). Biner sudah benar, tapi melanggar aturan desain. | Hapus object yatim saat source hilang. |
| COR-03 | sedang | Pesan `multiple definition of 'main'` dari linker tidak menyebut dua file. | Validasi sebelum link: sebut kedua file yang punya `main`. |
| COR-04 | rendah | `exclude` hanya berlaku pada library. Executable tetap menerima file yang di-exclude. | Dokumentasikan di `design/buildfile.md` dan `design/workspace.md`. |

## Tabel benchmark (sandbox)

Catatan: semua run di sandbox memakai 1 core. Angka rbot dan ninja berasal dari fixture yang sama (`fixture tanpa path relatif`, headers root `include/`). Perbedaan versi hanya pada baris `headers`.

### A/B v91 vs v94 — `-n 10 -r 3`, rbot dan ninja (median, ms)

| skenario | v91 rbot | v94 rbot | v91 ninja | v94 ninja |
|---|---|---|---|---|
| COLD | 647 | 718 | 791 | 732 |
| NOOP | 4 | 5 | 4 | 4 |
| LEAF | 89 | 85 | 123 | 140 |
| BADE | 95 | 96 | 152 | 141 |
| MOD | 78 | 72 | 127 | 124 |
| TOUCH | 9 | 9 | 305 | 327 |
| HEDIT | 282 | 262 | 331 | 310 |
| SEL | 179 | 170 | 222 | 196 |
| DEP | 160 | 149 | 193 | 180 |
| LIBTOUCH | 11 | 10 | 193 | 198 |
| LIBHDR | 190 | 171 | 247 | 184 |
| REL | 28 | 19 | — | — |

Verdict: v91 23/23 `In accordance`, v94 23/23 `In accordance`.

### A/B v91 vs v94 — `-n 10 -r 5`, rbot saja (median dan min–maks, ms)

| skenario | v91 median (min–maks) | v94 median (min–maks) |
|---|---|---|
| COLD | 658 (610–694) | 674 (657–724) |
| NOOP | 4 (4–5) | 5 (5–7) |
| HEDIT | 255 (248–272) | 273 (251–300) |
| SEL | 178 (170–208) | 176 (172–183) |
| LIBHDR | 184 (167–193) | 189 (185–199) |

Verdict: v91 12/12, v94 12/12 `In accordance`.

### v94 dengan `RBOT_HDR_SEM=1` — `-n 10 -r 1`, rbot dan ninja (ms)

| skenario | rbot | ninja |
|---|---|---|
| COLD | 705 | 921 |
| NOOP | 5 | 4 |
| LEAF | 99 | 179 |
| BADE | 93 | 164 |
| MOD | 79 | 140 |
| TOUCH | 7 | 358 |
| HEDIT | 278 | 304 |
| SEL | 158 | 184 |
| DEP | 167 | 175 |
| LIBTOUCH | 7 | 207 |
| LIBHDR | 201 | 261 |

Verdict: 23/23 `In accordance`. Jumlah `obj`/`cc`/`lib`/`bin` sama dengan ninja di semua skenario.

## Ringkasan

- Target cold dan incremental (≤1,2× ninja) belum tercapai. Penyebab terbesar yang paling kuat adalah OPT-01 (penjadwalan serial per proyek), lalu OPT-02 (overhead per proyek).
- Pemeriksaan semantik header (OPT-04) sudah membuktikan manfaatnya pada kasus prototype tak terpakai, tapi belum dijadikan default.
- Sebelum optimasi lanjutan, COR-01 harus diselesaikan karena menyangkut correctness, bukan performa.

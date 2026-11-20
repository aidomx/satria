# Router-centric workspace (eksperimen P4)

> Status: eksperimen. Pilot **read-only** sudah ada di kode:
> `src/workspace/ws_plan.c`, aktif hanya dengan `SATRIA_WS_PLAN=1`,
> diuji oleh `tests/unit/common/ws_plan.sh`. Dokumen ini menetapkan arah
> dan batas migrasi; kode tetap sumber kebenaran.

## 1. Siapa yang disebut "router"?

Istilah **router** sudah dipakai di `design/startup.md` §`opts.cache`: ia
adalah lapisan keputusan cache berbasis `.satria/opts.cache`, bukan induk
workspace dan bukan executor. Pekerjanya ada di:

| Berkas | Peran |
| --- | --- |
| `src/cache/router.c` | validasi route (`optsRouterValidate`, `wsRouterValidate`), presence grup (`optsCacheHasGroup`), memo per proses |
| `src/cache/opts_read.c` | membaca route + kontrak `opts.cache` <-> `sources.cache`; `optsStoreShared/Flush/Reload` |
| `src/cache/opts_create.c` | membuat entri/route baru (`optsRouterRecord`, `wsRouterRecord`, `optsProjMetaStamp`) |
| `src/cache/opts_update.c` | memperbarui route/stempel (PATCH) |
| `src/cache/opts_delete.c` | menghapus entri/route |
| `.satria/opts.cache` | state router: satu entri per proyek + route workspace |

Router **tidak** mengompilasi, tidak menjadwalkan, dan tidak menulis objek.
Ia memutuskan: no-op valid atau tidak, file cache mana yang dirujuk suatu
proyek, dan grup entri proyek ada atau tidak.

Aktor lain dalam alur workspace:

| Aktor | Berkas utama | Tugas |
| --- | --- | --- |
| **Scheduler** | `src/workspace/workspace.c`, `ws_batch.c`, `src/runners/*` | needed, worklist, fase library/binary, fork/wait, urutan output |
| **Executor** | `src/workspace/ws_exec.c` -> `wsRunProject` | menyusun Config, cek fast-state, kompilasi/link/arsip |
| **Finish** | `src/cmds/cmds_passfinish.c`, `ws_cache_save.c`, `src/log/*` | simpan deps/lib/state, route sync, laporan, release |

## 2. Masalah

Alur saat ini:

```text
router (validasi awal) -> scheduler -> executor -> finish (menulis cache)
```

Router memang sudah dipanggil di awal (`optsRouterValidate` /
`wsRouterValidate` lewat `cmdsFastStateValid`/`wsFastStateValid`), tetapi
selama dan sesudah eksekusi keputusan serta penulisan state tersebar:

- executor memanggil ulang router untuk menulis route (`optsRouterRecord`,
  `wsRouterRecord`, `optsProjMetaStamp`, `wsOptsMetaSave`) dan menulis
  `deps.cache`, `lib.cache`, `fingerprint.cache` sendiri; pada single-project,
  validasi router (`optsRouterValidate`) juga dijalankan di dalam alur
  eksekusi (`cmds_state.c`/`cmds_wsstate.c`), bukan diputuskan sekali di awal;
- scheduler/induk menulis ulang file yang sama (`wsCacheSave`,
  `csProjMetaStampFileN`, `optsSourcesRouteSync`);
- anak-anak fork dapat menulis cache bersama secara bersamaan
  (*last writer wins* untuk grup proyek lain).

Akibat terukur (profil 5k, Nov 2026): `csOpen` 28x per run leaf dan
`insertNew` >900 ribu entri — parse/tulis berulang karena setiap lapisan
membaca dan memutuskan sendiri.

## 3. Target

```text
router -> scheduler/executor -> router -> finish
```

1. **Router** memutuskan sebelum eksekusi: route valid/tidak, grup cache
   ada/tidak, proyek boleh no-op atau harus dibangun.
2. **Scheduler** menerjemahkan keputusan router menjadi rencana eksplisit.
   **Executor** hanya menjalankan rencana: kompilasi/link/arsip, tanpa
   memanggil router dan tanpa menulis cache bersama.
3. **Router** kembali dipanggil setelah eksekusi untuk mencatat route,
   metadata grup, dan sinkronisasi `sources` — satu penulis, sekali per run.
4. **Finish** hanya merender laporan/release; tidak ada keputusan cache di
   sana.

## 4. Invarian migrasi

1. **Zero semantic change** selama eksperimen: format cache, keputusan
   no-op, dan hasil build identik. Pilot default nonaktif.
2. **Satu penulis** untuk setiap file cache pada satu waktu.
3. **Fallback** selalu tersedia: tanpa env pilot, perilaku lama persis
   (tanpa file penanda, tanpa output tambahan).
4. **Kegagalan executor** tetap menghentikan peluncuran proyek berikutnya;
   rencana yang tidak sempat dieksekusi bukan pelanggaran bila rc != 0.
5. Fork boleh mewarisi rencana (memori); belum perlu IPC baru.

## 5. Pilot fase 0 (sudah diimplementasikan)

Pilot ini **bukan** router. Ia berada di sisi scheduler: membuktikan bahwa
scheduler hanya menjalankan proyek yang direncanakan, dan menyediakan tempat
untuk membawa keputusan router pada fase berikutnya.

API (`ws_internal.h`):

```c
bool wsPlanActive(void);
void wsPlanBegin(const WsModel *m, const WsWork *work, int n, bool libOnly);
bool wsPlanAdmit(const char *name, bool libOnly); /* dipanggil executor */
void wsPlanEnd(int rc);                            /* dipanggil scheduler */
```

Keluaran (`SATRIA_WS_PLAN=1`, stderr):

```text
satria: ws-plan: fase library: 3 rencana, 3 terverifikasi, 0 menyimpang
satria: ws-plan: fase binary: 2 rencana, 2 terverifikasi, 0 menyimpang
```

Penyimpangan yang dilaporkan: proyek dieksekusi di luar rencana, fase tidak
cocok, atau (saat rc == 0) proyek rencana tidak dieksekusi. `wsPlanBegin`
membuang penanda basi lebih dulu. Pilot ini **tidak mengubah** keputusan
build maupun isi cache.

## 6. Tahap berikutnya

### Fase 1 — rencana membawa keputusan router

Sebelum batch, scheduler meminta router sekali per proyek: route ada/tidak,
grup `deps`/`lib`/`fingerprint` ada/tidak, dan verdict fast-state. Hasilnya
masuk rencana. Executor memakai verdict itu dan **tidak** memanggil
`optsRouterValidate`/`wsRouterValidate` lagi. Bila verdict router berbeda
dengan hitungan executor: jatuh ke perilaku lama + peringatan. Belum ada
perubahan penulisan.

### Fase 2 — executor berhenti menulis

Executor tidak lagi memanggil `optsRouterRecord`, `wsRouterRecord`,
`optsProjMetaStamp`, `wsOptsMetaSave`, dan tidak menulis
`deps/lib/fingerprint/opts.cache`. Ia men-stage hasil per proyek (format
cache_store, satu grup). Router di induk menggabungkan grup, menstamp
metadata (`csProjMetaStampFileN`), dan menyinkronkan route
(`optsSourcesRouteSync`) sekali per run.

### Fase 3 — finish hanya merender

Keputusan akhir (verdict link, `artifact.cache`, `report.blob`, release)
pindah ke router pasca-eksekusi. `finish` tidak membaca cache untuk
memutuskan apa pun; ia hanya menampilkan ringkasan.

## 7. Kontrak pemanggilan (target)

| Operasi | Sekarang dipanggil oleh | Target |
| --- | --- | --- |
| `optsRouterValidate` | `cmds_state.c`/`cmds_wsstate.c` di dalam alur eksekusi | scheduler sebelum fork (fase 1) |
| `wsRouterValidate` | `ws_cache_noop.c` (gerbang induk) | tetap induk; jadikan masukan rencana fase 1 |
| `optsRouterRecord` / `wsRouterRecord` | `cmds_fast.c`/`cmds_wsstate.c` (anak) + `ws_cache_save.c` (induk) | router induk setelah batch (fase 2) |
| `optsProjMetaStamp` / `wsOptsMetaSave` | `cmds_fast.c` + `ws_cache_save.c` | router induk (fase 2) |
| `optsSourcesRouteSync` | `workspace.c` akhir run | tetap induk, tetapi sebagai bagian fase "router kedua" |
| `csProjMetaStampFileN` | `ws_cache_save.c` | router induk (fase 2) |
| tulis `deps/lib/fingerprint` | executor + induk | induk/merge stage (fase 2) |

## 8. Kriteria penerimaan

- `tests/unit/common/ws_plan.sh` hijau (serial + fork, 0 menyimpang).
- Tanpa env: keluaran dan jumlah syscall tidak berubah.
- Fase 2: setiap cache besar dibuka paling banyak sekali oleh induk per run
  (diverifikasi `strace -f -e openat` pada workspace 5k).
- Benchmark CI `Workspace Benchmark` leaf/hedit/no-op tidak regresi; target
  awal: gap leaf/hedit terhadap cmake menyusut karena parse cache berulang
  hilang.
- Unit `miniws` penuh tetap hijau.

## 9. Risiko & mitigasi

| Risiko | Mitigasi |
| --- | --- |
| Rencana basi karena anak lain menulis file | Fase 2 menghapus penulisan anak; fase 1 hanya verifikasi |
| Fork mewarisi memo stat induk | Executor tetap `cmdsStatMemoReset()` seperti sekarang |
| Kegagalan di tengah batch | `wsPlanEnd(rc != 0)` memaafkan proyek yang tak sempat jalan |
| Format cache berubah | Stage memakai format cache_store yang sama; merge per grup |
| Rollback sulit | Setiap fase di balik env; perilaku lama tetap jalur default |

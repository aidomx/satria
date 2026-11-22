# Router-centric workspace dan single-project (eksperimen P4)

> Status: eksperimen, fase 1-2 dan pemisahan pass compile/link sudah
> diimplementasikan; artefak/laporan/release diputuskan induk, semuanya di balik
> `SATRIA_WS_PLAN=1`. Tanpa env itu jalur default tidak berubah. Fase binary
> workspace memakai satu batch compile+link (link batch serial pilot dihapus
> karena menggandakan pekerjaan). Rencana,
> stage cache, dan intent route ada di `src/workspace/ws_plan.c`,
> `src/cache/cache_store.c`, dan `src/cache/opts_*.c`; diuji oleh
> `tests/unit/common/ws_plan.sh`. Transaksi stage yang sama juga dipakai
> jalur single-project (`cmdBuildEx`), dengan induk = proses build itu sendiri.
> Dokumen ini menetapkan arah dan batas migrasi; kode tetap sumber kebenaran.

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

Keluaran (`SATRIA_WS_PLAN=1`): build sukses **senyap**, tanpa ringkasan
fase. Hanya penyimpangan yang dicetak ke stderr, misalnya:

```text
satria: ws-plan: core di luar rencana fase binary
satria: ws-plan: penanda core tidak cocok (0x00 != B)
```

Penyimpangan yang dilaporkan: proyek dieksekusi di luar rencana, fase tidak
cocok, atau (saat rc == 0) proyek rencana tidak dieksekusi. `wsPlanBegin`
membuang penanda basi lebih dulu. Pilot awal ini tidak mengubah keputusan
build maupun isi cache; fase 1-3 di bawah memperluasnya.

## 5b. Implementasi fase 1-3 (P4)

Semua fase memakai env yang sama, `SATRIA_WS_PLAN=1`; tanpa env seluruh
jalur baru no-op.

- **Fase 1 — verdict di rencana.** `wsPlanBegin` menyetel proyek, menghitung
  `optsRouterValidate()` dan verdict `wsFastStateValid()` untuk fase yang
  akan dieksekusi, lalu menyimpannya di `WsPlanEntry`. Executor memakai
  `wsPlanFastConsume()`: hit langsung masuk jalur cached tanpa memanggil
  router/validasi lagi; miss langsung membangun. Bila salah satu proyek
  benar-benar membangun, hit berikutnya ditandai basi dan executor kembali
  memvalidasi (perilaku lama). Jumlah route/fast tetap dihitung untuk
  keputusan, tetapi tidak dicetak saat sukses.
- **Fase 2 — satu penulis.** `cache_store` mendapat stage per run
  (`csStageBegin/SetProject/MergeAll`). Selama stage aktif, commit untuk
  `deps.cache`, `lib.cache`, `embed.cache`, dan `fingerprint.cache` hanya
  menulis grup proyek yang sedang dieksekusi ke `.satria/router-stage/`.
  Pembaca dalam proses yang sama melihat tulisan stage lewat overlay `csOpen`,
  jadi cache turunan (mis. `cmdsFastStateSave` yang membuka `DepCache` baru)
  tidak membaca ulang file `.d` sebelum merge. Stage workspace **hanya dibuka
  untuk runner paralel**; runner serial (foreground default) menulis cache
  langsung seperti jalur non-plan. Merge per proyek me-rewrite cache 10k dan
  menambah 5-6 detik pada edit satu source, tanpa penulis paralel yang perlu
  dilindungi. Single-project memakai transaksi yang sama:
  `cmdBuildEx` membuka stage setelah gerbang no-op, mengikat grup ke alamat
  cwd (`csStageSetProjectAddr`), lalu menggabungkan dan menerapkan intent
  sebelum laporan/artefak disimpan. Executor tidak lagi menulis `opts.cache`:
  `optsRouterRecord`/`optsProjMetaStamp` menulis intent, dan induk
  menerapkannya lewat `optsRouterApplyIntents()` setelah merge. Stamp P grup
  ditulis induk setelah merge (`csProjMetaStampN` di-suppress selama stage).
  `state.cache` tetap memakai stage `stateBlobExport/Import` yang sudah ada.
- **Fase 3 — compile dan link terpisah.** `cmdsBuildPassCompile()` berhenti
  setelah compile dan mengembalikan `CmdsPassTail`; `cmdsLinkPass()` mengambil
  alih tail itu untuk menghitung verdict link, menjalankan linker, dan menulis
  state/packaging. Single-project memanggil kedua pass dari `cmdBuildEx`.
  Workspace **tidak** lagi memecah fase binary menjadi compile-only lalu
  `wsRunLinkBatch()` serial: versi pilot itu mengulang Config/plan/validasi
  untuk setiap proyek dan membuat link tidak paralel. Fase binary kini satu
  batch compile+link seperti jalur non-plan; stage cache tetap satu penulis
  karena induk menggabungkan grup setelah batch. `artifactCacheSave/Merge`
  tetap dipindahkan ke `workspace.c` setelah cache grup tersimpan; release
  dan `report.blob` juga tetap diputuskan induk.

Optimasi rencana: verdict route + fast-state seluruh proyek dihitung paralel
(maksimal 8 child; matikan dengan `SATRIA_WSPLAN_PAR=0`), penanda hanya
dibersihkan sejumlah proyek rencana, validasi library memakai batch stat
paralel bila gerbang tidak menandai entri basi (kalau ada, entri basi sudah
di depan dan validasi berhenti lebih awal), dan memo stat tidak lagi di-reset
setelah proyek pertama membangun
(penulisan output sudah memanggil `cmdsStatMemoForget`). Hit rencana
dibatalkan setelah ada proyek yang membangun — juga di induk runner fork,
lewat verdict anak. Pada benchmark 40 source/proyek, cold plan turun dari
7.868 ms ke 7.555 ms (non-plan 7.404 ms); no-op tetap dilayani gerbang cache
sebelum rencana dibuat.

Evaluasi workspace 10k source (edit satu source, 4 job, urutan selang-seling):
non-plan median sekitar 19,7 dtk; plan sekitar 23,3 dtk. Staging yang tadinya
selalu aktif adalah regresi terbesar (sudah dihapus untuk serial); sisa
selisih berasal dari validasi rencana yang dihitung lebih awal, sebelum
proyek dependensi membangun. Karena default foreground memakai runner serial,
`SATRIA_WS_PLAN` **belum dijadikan default** — rencana baru menguntungkan saat
runner paralel/background dipakai.

Batas eksperimen: stage hanya untuk empat cache grup di atas; `artifact.cache`,
`state.cache`, dan `opts.cache` punya jalur sendiri. Windows tidak memakai
stage karena runner paralelnya serial (single-project di Windows juga tidak
membuka stage). Build gagal pada single-project tidak menggabungkan stage,
sehingga target dan cache lama tetap utuh.

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
| Fork mewarisi memo stat induk | Anak fork tetap `cmdsStatMemoReset()`; runner serial memakai memo lintas proyek seperti non-plan, dan penulis output memanggil `cmdsStatMemoForget` |
| Kegagalan di tengah batch | `wsPlanEnd(rc != 0)` memaafkan proyek yang tak sempat jalan; fase binary berhenti seperti batch biasa |
| Format cache berubah | Stage memakai format cache_store yang sama; merge per grup |
| Rollback sulit | Setiap fase di balik env; perilaku lama tetap jalur default |

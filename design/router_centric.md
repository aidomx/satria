# Router-centric workspace (eksperimen P4)

> Status: eksperimen. Pilot **read-only** sudah ada di kode:
> `src/workspace/ws_plan.c`, aktif hanya dengan `SATRIA_ROUTER_PLAN=1`,
> diuji oleh `tests/unit/common/router_plan.sh`. Dokumen ini menetapkan arah
> dan batas migrasi; kode tetap sumber kebenaran.

## 1. Masalah

Alur workspace saat ini:

```text
router (induk) -> executor (anak fork / in-process) -> finish (induk)
```

Router memang sudah memutuskan *apa yang dibangun* (needed, worklist, gate
no-op). Tetapi selama eksekusi, executor masih mengambil keputusan sendiri
dan menyentuh state bersama secara independen:

- setiap anak membuka dan mem-parse ulang `deps.cache`, `lib.cache`,
  `fingerprint.cache`, `opts.cache`, `sources.cache`;
- setiap anak menulis ulang file cache bersama (hapus grup proyeknya, tulis
  ulang seluruh file) lalu induk membuka/menulis file yang sama lagi;
- keputusan fast-state diambil di anak, sedangkan gerbang no-op di induk;
- sebagian hasil distaging (state/verdict/report), sebagian langsung ke
  disk (deps/lib/fingerprint/opts) — dua model kepemilikan bercampur.

Konsekuensi terukur (profil CPU workspace 5k, Nov 2026):

- `csOpen` 28x per run leaf; `insertNew` >900 ribu entri diparse;
- penulisan cache bersama oleh banyak anak: *last writer wins* untuk grup
  proyek lain yang dibaca sebelum anak lain selesai;
- biaya I/O/CPU validasi berulang muncul lagi di induk (save/stamp).

## 2. Target

```text
router -> executor -> router -> finish
```

- **Router** (induk) satu-satunya pemilik keputusan dan penulis state/cache.
  Ia menyusun rencana eksplisit per fase, lalu memverifikasi hasilnya.
- **Executor** hanya menjalankan rencana: kompilasi/link/arsip, tanpa
  membaca/menulis cache bersama dan tanpa memutuskan target.
- **Router (kedua)** membaca laporan executor (state/verdict/report staging),
  memutuskan pembaruan cache/state, lalu menyerahkan ringkasan ke **finish**
  yang hanya merender.

## 3. Invarian migrasi

1. **Zero semantic change** selama eksperimen: format cache, keputusan
   no-op, dan hasil build identik. Pilot default nonaktif.
2. **Satu penulis** untuk setiap file cache pada satu waktu (induk).
3. **Fallback** selalu tersedia: tanpa `SATRIA_ROUTER_PLAN`, perilaku lama
   persis (tanpa file penanda, tanpa output tambahan).
4. **Kegagalan executor** tetap menghentikan peluncuran proyek berikutnya;
   rencana yang tidak sempat dieksekusi bukan pelanggaran bila rc != 0.
5. Fork boleh mewarisi rencana (memori) — tidak perlu IPC baru untuk pilot.

## 4. Pilot fase 0 (sudah diimplementasikan)

Router menyusun rencana per fase sebelum `wsRunBatch`, executor mengaku
"masuk rencana" dan menulis penanda `.satria/ws-plan-<idx>`, lalu router
mencocokkan penanda dengan rencana setelah batch.

API (`ws_internal.h`):

```c
bool wsPlanActive(void);
void wsPlanBegin(const WsModel *m, const WsWork *work, int n, bool libOnly);
bool wsPlanAdmit(const char *name, bool libOnly); /* dipanggil executor */
void wsPlanEnd(int rc);                            /* dipanggil router */
```

Keluaran (`SATRIA_ROUTER_PLAN=1`, stderr):

```text
satria: router-plan: fase library: 3 rencana, 3 terverifikasi, 0 menyimpang
satria: router-plan: fase binary: 2 rencana, 2 terverifikasi, 0 menyimpang
```

Penyimpangan yang dilaporkan: proyek dieksekusi di luar rencana, fase tidak
cocok, atau (saat rc == 0) proyek rencana tidak dieksekusi. Pilot ini **tidak
mengubah** keputusan build maupun isi cache.

## 5. Tahap berikutnya

### Fase 1 — rencana membawa keputusan

Router menghitung keputusan per proyek *sebelum* fork (fast-state valid,
fase archive/library/binary, target output) memakai fungsi yang sama dengan
executor. Executor mengonsumsi rencana dan memverifikasi keputusan router
sama dengan hasil hitungnya sendiri. Bila berbeda: jatuh ke perilaku lama +
peringatan. Belum ada perubahan penulisan.

### Fase 2 — stage-and-merge cache

Executor berhenti menulis `deps.cache`, `lib.cache`, `fingerprint.cache`,
`opts.cache`. Ia menulis grup proyeknya ke file stage (format cache_store
yang sama, satu grup). Router menggabungkan grup ke file kanonik sekali per
fase, lalu menstamp metadata (`csProjMetaStampFileN`) — satu penulis, tanpa
race, tanpa parse berulang di induk.

### Fase 3 — finish hanya merender

Keputusan akhir (verdict link, artifact.cache, report.blob, release) pindah
ke router pasca-eksekusi. `finish` tidak lagi membaca cache untuk memutuskan
apa pun; ia hanya menampilkan ringkasan.

## 6. Kriteria penerimaan

- `tests/unit/common/router_plan.sh` hijau (serial + fork, 0 menyimpang).
- Tanpa env: keluaran dan jumlah syscall tidak berubah.
- Fase 2: setiap cache besar dibuka paling banyak sekali oleh induk per run
  (diverifikasi `strace -f -e openat` pada workspace 5k).
- Benchmark CI (`Workspace Benchmark`) leaf/hedit/no-op tidak regresi;
  target awal: gap leaf/hedit terhadap cmake menyusut karena parse cache
  berulang hilang.
- Unit `miniws` penuh tetap 371+ PASS.

## 7. Risiko & mitigasi

| Risiko | Mitigasi |
|---|---|
| Rencana basi karena anak lain menulis file | Fase 2 menghapus penulisan anak; fase 1 hanya verifikasi |
| Fork mewarisi memo stat induk | Executor tetap `cmdsStatMemoReset()` seperti sekarang |
| Kegagalan di tengah batch | `wsPlanEnd(rc != 0)` memaafkan proyek yang tak sempat jalan |
| Format cache berubah | Stage memakai format cache_store yang sama; merge per grup |
| Rollback sulit | Setiap fase di balik env; perilaku lama tetap jalur default |

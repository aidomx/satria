/* cache_store — penyimpanan entri cache satria (design/startup.md §4).

   Satu file `caches/<nama>.cache` memuat entri semua proyek dengan format:

     P <alamat_proyek> [<content> <time> <size>]     (pengelompok proyek +
                                                      metadata grup, §4)
     <OP> <alamat_proyek> <alamat_file> <content> <time> <size> [payload]

   Metadata grup diisi pemanggil lewat csProjMetaSet (mis. stamp konfigurasi
   + daftar sumber per proyek) dan dipertahankan lintas load/commit. Baris P
   lama tanpa metadata tetap terbaca (grup tanpa meta).

   <OP> = GET | PUT | PATCH (DELETE tidak pernah ditulis: entri dihapus).
   Alamat = hash FNV-1a 64 (csHash). Pointer hanya di memori.

   `payload` opsional (sisa baris setelah kolom ke-6): data tambahan satu
   baris untuk entri yang butuh muatan (mis. daftar header edges deps.cache,
   dipisah '\x01'). Tidak boleh memuat '\n'. Baris lama tanpa payload tetap
   terbaca (payload = NULL), dan payload tidak mengubah cara validasi.

   Operasi di memori: csGet, csPut, csPatch, csDelete. Perubahan baru
   menyentuh disk lewat csCommit (tulis tmp lalu rename, hanya bila dirty).
   Modul ini murni libc: tidak bergantung pada bagian lain satria. */
#ifndef SATRIA_CACHE_STORE_H
#define SATRIA_CACHE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { CS_GET, CS_PUT, CS_PATCH } CsOp;

typedef struct {
  uint64_t proj;    /* alamat_proyek */
  uint64_t file;    /* alamat_file */
  uint64_t content; /* hash isi (FNV-1a 64) */
  long long time;   /* mtime */
  long long size;   /* ukuran */
  const char *payload; /* opsional, tanpa '\n'; NULL = tanpa payload.
                          Kepemilikan: csPut/csPatch menyalin ke dalam store;
                          csGet mengembalikan pointer MILIK STORE yang sah
                          selama store terbuka dan entri tidak diubah. */
} CsEntry;

/* PATCH: hanya kolom yang has* == true yang diubah. hasPayload dengan
   payload=NULL mengosongkan payload. */
typedef struct {
  bool hasContent, hasTime, hasSize;
  uint64_t content;
  long long time;
  long long size;
  bool hasPayload;
  const char *payload;
} CsPatch;

typedef struct CsStore CsStore;

/* FNV-1a 64 atas string (dipakai untuk alamat proyek dan file). */
uint64_t csHash(const char *s);

/* Buka store dari `path`. File tidak ada -> store kosong (valid).
   File rusak (format, duplikat, OP tak dikenal) -> NULL. */
CsStore *csOpen(const char *path);
void csClose(CsStore *st);

bool csGet(const CsStore *st, uint64_t proj, uint64_t file, CsEntry *out);
bool csPut(CsStore *st, const CsEntry *e);                                  /* PUT: insert/ganti */
bool csPatch(CsStore *st, uint64_t proj, uint64_t file, const CsPatch *p); /* PATCH: entri harus ada */
bool csDelete(CsStore *st, uint64_t proj, uint64_t file);                  /* DELETE */

/* Tulis ulang file bila ada perubahan (tmp lalu rename). Tanpa perubahan: no-op. */
bool csCommit(CsStore *st);
bool csIsDirty(const CsStore *st);
size_t csCount(const CsStore *st);

/* Iterasi semua entri (urutan tak terdefinisi). fn mengembalikan false untuk berhenti. */
typedef bool (*CsVisitFn)(const CsEntry *e, CsOp op, void *ctx);
bool csForEach(const CsStore *st, CsVisitFn fn, void *ctx);

/* Racy timestamp: entri yang `time`-nya tidak lebih lama dari waktu file
   ditulis terakhir (`savedAt`) tidak boleh dipercaya ukuran+waktunya. */
bool csRacy(long long entryTime, long long savedAt);

/* Metadata grup P (§4): P <proj> <content> <time> <size>. content = stamp
   proyek (hash konfigurasi + daftar sumber), time = stempel ns build
   terakhir, size = jumlah entri grup. Set menandai store dirty. */
void csProjMetaSet(CsStore *st, uint64_t proj, uint64_t content, long long time,
                   long long size);
bool csProjMetaGet(const CsStore *st, uint64_t proj, uint64_t *content, long long *time,
                   long long *size); /* false = grup tanpa metadata */
size_t csCountProj(const CsStore *st, uint64_t proj);

/* ns sekarang (CLOCK_REALTIME; Windows: detik x 1e9) — dipakai stamp metadata
   grup agar format P seragang lintas cache. */
long long csNowNs(void);

/* Stamp metadata grup P di SATU file cache. File belum ada -> dilewati (bukan
   kegagalan: grup lahir bersama entri pertamanya). Grup tanpa entri -> tak ada
   baris P yang ditulis (commit hanya menulis grup ber-node). */
bool csProjMetaStampFile(const char *path, uint64_t proj, uint64_t content, long long time);

/* Varian batch: stamp BANYAK grup P di satu file dalam SATU buka+tulis.
   Sebelumnya wsCacheSave memanggil stamp per grup di loop — 4 file × N grup
   = 16 siklus baca+tulis penuh per run hanya untuk metadata (±100 ms di
   proot; terukur di bench ws.sh hedit/libhdr). */
bool csProjMetaStampFileN(const char *path, const uint64_t *projs,
                          const uint64_t *contents, const long long *times, int n);

/* Stamp seragam metadata grup P di SEMUA cache grup-per-proyek selain opts
   (di-stamp simpanannya sendiri) dan state.cache (lewat stateBlobProjMeta);
   artifact.cache memakai kunci tetap "artifacts" dan distamp di artifact.c.
   Cakupan: deps, embed, fingerprint, lib. Baris P <proj> <content> <time>
   <size> dengan format & makna identik di seluruh cache DAN seluruh mode
   (single & workspace) — jejak proyek mudah ditelusuri lintas cache
   (design/startup.md §4). <content> = stamp proyek yang sama dengan
   opts/fingerprint; <time> = ns panggilan; <size> = jumlah entri grup per
   file. Gagal tulis: false (metadata observasional; pemanggil boleh
   mengabaikan). */
bool csProjMetaStamp(uint64_t proj, uint64_t content, long long time);

/* Varian batch lintas-cache (deps, embed, fingerprint, lib): satu buka+tulis
   per file untuk seluruh grup. Lihat catatan csProjMetaStampFileN. */
bool csProjMetaStampN(const uint64_t *projs, const uint64_t *contents,
                      const long long *times, int n);

#endif

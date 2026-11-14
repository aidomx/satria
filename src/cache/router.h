/* Shared opts.cache router for single-project and workspace.
 * Build execution remains separate; only routing persistence is shared. */
#ifndef SATRIA_CACHE_ROUTER_H
#define SATRIA_CACHE_ROUTER_H
#include <stdbool.h>
#include <stdint.h>
#include "../config.h"
#include "cache_store.h"

/* Handle store opts.cache bersama per proses (audit 4/11 item 5/6, subsetChecked
   funnel): dibuka SEKALI, semua pembaca/penulis memakainya — tidak ada
   open+parse berulang per validasi. Penulis tetap csCommit langsung (waktu
   tulis tidak berubah); perubahan in-memory terlihat pembaca lain di proses
   yang sama. NULL hanya bila file rusak/fopen gagal — gunakan optsStoreRepair.
   Jangan csClose handle ini; lepaskan lewat optsStoreReload. */
CsStore *optsStoreShared(void);
/* Buang handle in-memory lalu buka ulang dari disk — WAJIB di induk setelah
   anak-anak fork selesai (anak menulis opts.cache di memorinya sendiri dan
   meng-commit ke disk; salinan induk basi terhadapnya). */
void optsStoreReload(void);
/* File rusak (csOpen NULL): hapus lalu buka ulang store kosong (self-healing
   pola optsRouterRecord lama). NULL bila alokasi gagal. */
CsStore *optsStoreRepair(void);

bool optsRouterRecord(const Config *c, bool libOnly);
/* Metadata grup P (§4) mode single: content = stamp proyek (hash konfigurasi
   + daftar sumber); format sama dengan workspace (wsOptsMetaSave). */
bool optsProjMetaStamp(uint64_t proj, uint64_t content);
bool optsRouterValidate(void);
/* Sinkron dua arah sources <-> opts: segarkan stamp route `sources` SEMUA
   kunci (workspace + per proyek) di opts.cache sesudah .satria/sources.cache
   ditulis/diubah oleh run (merge P wsCacheSave maupun re-walk srcSave).
   Dipanggil hanya pada run SUKSES; run gagal sengaja membiarkan route basi
   agar gate menolak no-op sampai build berhasil mencatat ulang. */
bool optsSourcesRouteSync(void);
void wsRouterDrop(void);
bool wsRouterRecord(void);
bool wsRouterValidate(void);
#endif

/* Shared opts.cache router for single-project and workspace.
 * Build execution remains separate; only routing persistence is shared. */
#ifndef SATRIA_CACHE_ROUTER_H
#define SATRIA_CACHE_ROUTER_H
#include <stdbool.h>
#include <stdint.h>
#include "../config.h"

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

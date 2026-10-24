#ifndef SATRIA_OPTS_INTERNAL_H
#define SATRIA_OPTS_INTERNAL_H
#include "router.h"
#include "cache_store.h"
#include "../portability.h"
#define OPTS_CACHE_FILE ".satria/opts.cache"
#define WS_ROUTER_FILE OPTS_CACHE_FILE
#define WS_ROUTER_KEY "workspace"
#define WS_ROUTER_NAME "fingerprint"
#define WS_SOURCE_ROUTE "sources"
#define WS_SOURCE_FILE ".satria/sources.cache"
/* Helpers shared across CRUD units. */
bool optsCacheHasGroup(const char *path, uint64_t proj);

/* Stamp sources.cache (mtime nanodetik + ukuran) untuk route di opts.cache.
   Resolusi ns wajib: perubahan isi dalam detik yang sama dengan ukuran sama
   tetap harus terdeteksi (kontrak opts <-> sources, design/startup.md §4).
   false bila indeks tidak ada. */
static inline bool optsSourcesStamp(long long *mt, long long *sz) {
  int64_t ns = 0;
  long long s = 0;
  if (!fsStampNsSize(WS_SOURCE_FILE, &ns, &s)) return false;
  *mt = (long long)ns;
  *sz = s;
  return true;
}
#endif

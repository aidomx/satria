/* Shared opts.cache implementation: single-project and workspace. */
#include "opts_internal.h"
#include "../cmds/cmds_internal.h"
#include "../portability.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h> /* optsNowNs */

/* Upsert workspace route metadata, commit once per recording. */
bool wsRouterRecord(void) {
  mkdirs(".satria");
  CsStore *st = csOpen(WS_ROUTER_FILE);
  if (!st) {
    remove(WS_ROUTER_FILE); /* rusak: tulis ulang bersih (self-healing) */
    st = csOpen(WS_ROUTER_FILE);
  }
  if (!st) return false;
  CsEntry e = {csHash(WS_ROUTER_KEY), csHash(WS_ROUTER_NAME), 0, 0, 0, WS_ROUTER_NAME};
  bool ok = csPut(st, &e);
  /* Source discovery is a first-class route.  Store its file stamp so the
   * no-op gate rejects a removed/replaced source index before loading state.
   * Missing index is not a valid route (e.g. workspace without sources).
   * Stamp nanodetik: perubahan isi sources.cache dalam detik sama tetap
   * terdeteksi (lihat optsSourcesStamp). */
  long long srcM = 0, srcS = 0;
  if (optsSourcesStamp(&srcM, &srcS)) {
    CsEntry src = {csHash(WS_ROUTER_KEY), csHash(WS_SOURCE_ROUTE), 0, srcM, srcS, WS_SOURCE_ROUTE};
    ok = ok && csPut(st, &src);
  } else {
    csDelete(st, csHash(WS_ROUTER_KEY), csHash(WS_SOURCE_ROUTE));
  }
  ok = ok && csCommit(st);
  csClose(st);
  return ok;
}

typedef struct {
  long long mt, sz;
  uint64_t routeFile;
  CsStore *st;
  bool dirty, failed;
} OptSyncCtx;

static bool optSyncVisit(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  OptSyncCtx *c = ctxp;
  if (e->file != c->routeFile || !e->payload ||
      strcmp(e->payload, WS_SOURCE_ROUTE) != 0)
    return true;
  if (e->time == c->mt && e->size == c->sz) return true;
  CsPatch p = {0};
  p.hasTime = true;
  p.time = c->mt;
  p.hasSize = true;
  p.size = c->sz;
  /* csPatch hanya mengubah field node di tempat (tak memindahkan bucket),
     aman dipanggil di dalam csForEach — pola sama dengan wsCacheVisit. */
  if (!csPatch(c->st, e->proj, e->file, &p)) {
    c->failed = true;
    return false;
  }
  c->dirty = true;
  return true;
}

/* Sinkron dua arah: sources.cache (re)ditulis -> segarkan stempel route
   `sources` di opts.cache untuk SEMUA kunci (workspace + per proyek), supaya
   gate selalu membandingkan terhadap berkas terkini. Tanpa ini, route proyek
   yang direkam sebelum merge P (wsCacheSave) tetap menunjuk stempel lama.
   Tak ada route / indeks hilang: tak ada yang disegarkan (drop ditangani
   jalur record masing-masing). Gagal tulis: kembalikan false, tapi gate akan
   menolak no-op — aman (konservatif). */
static long long optsNowNs(void) {
#if defined(_WIN32)
  return (long long)time(NULL) * 1000000000LL;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

/* Metadata grup P (§4) untuk jalur single-project — format sama dengan
   workspace (wsCacheSave -> wsOptsMetaSave): content = stamp proyek dari
   pemanggil (hash konfigurasi + daftar sumber), time = ns sekarang,
   size = jumlah entri grup. Validasi no-op single tetap FAST5; stamp ini
   menyamakan format store + observabilitas. */
bool optsProjMetaStamp(uint64_t proj, uint64_t content) {
  CsStore *st = csOpen(OPTS_CACHE_FILE);
  if (!st) return false;
  csProjMetaSet(st, proj, content, optsNowNs(), (long long)csCountProj(st, proj));
  bool ok = csCommit(st);
  csClose(st);
  return ok;
}

bool optsSourcesRouteSync(void) {
  long long mt = 0, sz = 0;
  if (!optsSourcesStamp(&mt, &sz)) return true;
  CsStore *st = csOpen(OPTS_CACHE_FILE);
  if (!st) return true; /* belum ada / rusak: serahkan ke jalur normal */
  OptSyncCtx c = {mt, sz, csHash(WS_SOURCE_ROUTE), st, false, false};
  bool scanned = csForEach(st, optSyncVisit, &c);
  bool ok = scanned && !c.failed;
  if (ok && c.dirty) ok = csCommit(st);
  csClose(st);
  return ok;
}

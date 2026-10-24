/* Shared opts.cache implementation: single-project and workspace. */
#include "opts_internal.h"
#include "../cmds/cmds_internal.h"
#include "../portability.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  uint64_t proj;
  char names[16][32];
  long long sourceTime;
  long long sourceSize;
  bool hasSource;
  int n;
} OptNameCtx;

static bool optNameVisit(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  OptNameCtx *ctx = ctxp;
  if (e->proj != ctx->proj || !e->payload || ctx->n >= 16) return true;
  size_t len = strlen(e->payload);
  if (!len || len >= sizeof(ctx->names[0])) return true;
  for (size_t i = 0; i < len; i++) {
    char ch = e->payload[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
      return true; /* nama tidak sah: entri diabaikan (guard path) */
  }
  if (strcmp(e->payload, WS_SOURCE_ROUTE) == 0) {
    ctx->hasSource = true;
    ctx->sourceTime = e->time;
    ctx->sourceSize = e->size;
    return true;
  }
  snprintf(ctx->names[ctx->n], sizeof(ctx->names[0]), "%s", e->payload);
  ctx->n++;
  return true;
}

/* Dibaca LEBIH DULU oleh no-op (cmdsFastStateValid/wsFastStateValid).
   true = lanjut ke validasi fast-state; false = router menyatakan cache yang
   dirutekan hilang/kosong untuk proyek ini — build.state tidak perlu dimuat.
   Tanpa opts.cache atau tanpa entri proyek: tanpa gate (jalur normal).
   Kontrak opts <-> sources: bila .satria/sources.cache ADA, route `sources`
   wajib ada dan stempelnya (ns + ukuran) harus cocok; route menunjuk indeks
   yang hilang juga menolak. Route opsional hanya untuk proyek yang memang
   belum dirutekan atau tidak punya indeks sumber. */
bool optsRouterValidate(void) {
  CsStore *st = csOpen(OPTS_CACHE_FILE);
  if (!st) return true;
  uint64_t proj = cmdsCacheProjAddr();
  OptNameCtx names;
  memset(&names, 0, sizeof(names));
  names.proj = proj;
  bool scanned = csForEach(st, optNameVisit, &names);
  csClose(st);
  if (!scanned) return true; /* rusak: serahkan ke jalur normal */
  if (!names.n && !names.hasSource) return true; /* proyek belum dirutekan */
  long long mt = 0, sz = 0;
  bool idxOk = optsSourcesStamp(&mt, &sz);
  if (idxOk) {
    if (!names.hasSource || names.sourceTime != mt || names.sourceSize != sz)
      return false; /* indeks ada tapi route hilang / stempel basi */
  } else if (names.hasSource) {
    return false; /* route menunjuk indeks yang hilang */
  }
  for (int i = 0; i < names.n; i++) {
    char path[128];
    snprintf(path, sizeof(path), ".satria/caches/%s.cache", names.names[i]);
    if (!optsCacheHasGroup(path, proj)) return false;
  }
  return true;
}


/* true bila router ada dan merutekan ke fingerprint. Payload dibaca sebelum
   store ditutup (kepemilikan pointer milik store). Kontrak sources sama dengan
   optsRouterValidate: indeks ada -> route wajib + cocok; indeks hilang -> route
   tak boleh ada. */
bool wsRouterValidate(void) {
  CsStore *st = csOpen(WS_ROUTER_FILE);
  if (!st) return false;
  CsEntry e;
  bool ok = csGet(st, csHash(WS_ROUTER_KEY), csHash(WS_ROUTER_NAME), &e) && e.payload &&
            strcmp(e.payload, WS_ROUTER_NAME) == 0;
  if (ok) {
    CsEntry src;
    bool hasRoute = csGet(st, csHash(WS_ROUTER_KEY), csHash(WS_SOURCE_ROUTE), &src) &&
                    src.payload && strcmp(src.payload, WS_SOURCE_ROUTE) == 0;
    long long mt = 0, sz = 0;
    bool idxOk = optsSourcesStamp(&mt, &sz);
    if (idxOk) ok = hasRoute && src.time == mt && src.size == sz;
    else ok = !hasRoute;
  }
  csClose(st);
  return ok;
}

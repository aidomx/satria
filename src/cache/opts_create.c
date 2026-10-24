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
  uint64_t *keys;
  size_t n;
} OptKeyCtx;

static bool optKeyVisit(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  OptKeyCtx *ctx = ctxp;
  if (e->proj != ctx->proj) return true;
  uint64_t *nv = realloc(ctx->keys, (ctx->n + 1) * sizeof(uint64_t));
  if (!nv) return false;
  ctx->keys = nv;
  ctx->keys[ctx->n++] = e->file;
  return true;
}

/* Rekam router proyek SETELAH state tersimpan: ganti penuh set entri proyek
   dengan cache yang BENAR-BENAR memuat grupnya saat ini. deps selalu
   (depsSave berjalan tiap build sukses); lib bila fase library berjalan
   (libOnly) atau library diminta; embed bila ada entri embedded. Kehadiran
   grup diverifikasi per nama — router tidak pernah menunjuk cache kosong. */
bool optsRouterRecord(const Config *c, bool libOnly) {
  mkdirs(".satria");
  CsStore *st = csOpen(OPTS_CACHE_FILE);
  if (!st) {
    remove(OPTS_CACHE_FILE); /* rusak: tulis ulang bersih (self-healing) */
    st = csOpen(OPTS_CACHE_FILE);
  }
  if (!st) return false;
  uint64_t proj = cmdsCacheProjAddr();

  OptKeyCtx keys = {proj, NULL, 0};
  bool collected = csForEach(st, optKeyVisit, &keys);
  if (collected)
    for (size_t i = 0; i < keys.n; i++) csDelete(st, proj, keys.keys[i]);
  free(keys.keys);
  if (!collected) {
    csClose(st);
    return false;
  }

  const char *names[5];
  int n = 0;
  names[n++] = "deps";
  /* Snapshot fast state + build fingerprint single-project hidup di
     fingerprint.cache (cmds_state.c). Route-nya mengikuti pola workspace
     (route "fingerprint"), sehingga no-op single masuk lewat opts.cache. */
  names[n++] = "fingerprint";
  /* Source index belongs to the same project route, not workspace only. */
  long long srcM = 0, srcS = 0;
  bool haveSources = optsSourcesStamp(&srcM, &srcS);
  if (libOnly || c->libRequested) names[n++] = "lib";
  if (c->embCount) names[n++] = "embed";
  for (int i = 0; i < n; i++) {
    char path[128];
    snprintf(path, sizeof(path), ".satria/caches/%s.cache", names[i]);
    if (!optsCacheHasGroup(path, proj)) continue;
    CsEntry e = {proj, csHash(names[i]), 0, 0, 0, names[i]};
    csPut(st, &e);
  }
  if (haveSources) {
    CsEntry src = {proj, csHash(WS_SOURCE_ROUTE), 0, srcM, srcS, WS_SOURCE_ROUTE};
    if (!csPut(st, &src)) { csClose(st); return false; }
  }
  bool ok = csCommit(st);
  csClose(st);
  return ok;
}


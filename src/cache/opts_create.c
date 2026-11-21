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
bool optsRouterRecordProject(uint64_t proj, bool libOnly, bool libRequested, bool embed) {
  mkdirs(".satria");
  CsStore *st = optsStoreShared();
  if (!st) st = optsStoreRepair();
  if (!st) return false;

  OptKeyCtx keys = {proj, NULL, 0};
  bool collected = csForEach(st, optKeyVisit, &keys);
  if (collected)
    for (size_t i = 0; i < keys.n; i++) csDelete(st, proj, keys.keys[i]);
  free(keys.keys);
  if (!collected) return false; /* store tetap terbuka (bersama) */

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
  if (libOnly || libRequested) names[n++] = "lib";
  if (embed) names[n++] = "embed";
  for (int i = 0; i < n; i++) {
    char path[128];
    snprintf(path, sizeof(path), ".satria/caches/%s.cache", names[i]);
    if (!optsCacheHasGroup(path, proj)) continue;
    CsEntry e = {proj, csHash(names[i]), 0, 0, 0, names[i]};
    csPut(st, &e);
  }
  if (haveSources) {
    CsEntry src = {proj, csHash(WS_SOURCE_ROUTE), 0, srcM, srcS, WS_SOURCE_ROUTE};
    if (!csPut(st, &src)) return false;
  }
  /* Commit di-flush per fase/batas proses (item 6): mutasi tinggal di store
     bersama — pembaca in-memory langsung melihatnya, tulis disk ditunda. */
  return true;
}

/* Fase 2 router-centric: executor anak menulis intent, bukan opts.cache.
   Induk menerapkannya setelah stage cache digabung (optsRouterApplyIntents). */
static bool optsIntentWrite(uint64_t proj, bool libOnly, bool libRequested,
                            bool embed, bool hasMeta, uint64_t content) {
  char path[256];
  snprintf(path, sizeof(path), ".satria/router-stage/%s.route.%016llx",
           csStageToken(), (unsigned long long)proj);
  FILE *f = fopen(path, "w");
  if (!f) return false;
  int ok = fprintf(f, "libOnly=%d lib=%d emb=%d\n", (int)libOnly, (int)libRequested,
                   (int)embed);
  if (ok > 0 && hasMeta) ok = fprintf(f, "meta=%016llx\n", (unsigned long long)content);
  if (fclose(f) != 0) ok = 0;
  return ok > 0;
}

/* Append metadata ke intent route yang sudah ada (dipanggil sesudah record). */
bool optsProjMetaIntent(uint64_t proj, uint64_t content) {
  char path[256];
  snprintf(path, sizeof(path), ".satria/router-stage/%s.route.%016llx",
           csStageToken(), (unsigned long long)proj);
  FILE *f = fopen(path, "a");
  if (!f) return false;
  int ok = fprintf(f, "meta=%016llx\n", (unsigned long long)content);
  if (fclose(f) != 0) ok = 0;
  return ok > 0;
}

bool optsRouterRecord(const Config *c, bool libOnly) {
  uint64_t proj = cmdsCacheProjAddr();
  if (csStageActive() && !csStageMerging())
    return optsIntentWrite(proj, libOnly, c->libRequested, c->embCount > 0, false, 0);
  return optsRouterRecordProject(proj, libOnly, c->libRequested, c->embCount > 0);
}


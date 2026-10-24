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
  bool has;
} OptGroupCtx;

static bool optGroupVisit(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  OptGroupCtx *ctx = ctxp;
  if (e->proj == ctx->proj) {
    ctx->has = true;
    return false; /* berhenti: cukup satu entri */
  }
  return true;
}

/* Apakah `path` (sebuah file caches) memuat grup entri proyek `proj`? */
bool optsCacheHasGroup(const char *path, uint64_t proj) {
  CsStore *st = csOpen(path);
  if (!st) return false;
  OptGroupCtx ctx = {proj, false};
  csForEach(st, optGroupVisit, &ctx);
  csClose(st);
  return ctx.has;
}


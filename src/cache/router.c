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
/* Memo presence grup per proses, diindeks oleh stempel file (path + mtime ns
   + ukuran + proj). Kunci ini otomatis basi: penulisan cache apa pun mengubah
   mtime/ukuran, jadi entri lama tak pernah dipakai tanpa invalidasi manual.
   Sebelumnya kuncinya hash FNV-1a seluruh isi: benar, tetapi deps/lib/
   fingerprint (masing-masing ~1-2 MB) dibaca ulang 15-16x per run — profil
   CPU 5k menunjukkan 8-9% waktu satria hanya untuk membaca file itu. */
#define OPT_HG_MEMO 16
typedef struct {
  char path[SATRIA_MAX_PATH];
  int64_t mtime;
  long long size;
  uint64_t proj;
  bool has;
  bool used;
} OptHgMemo;
static OptHgMemo g_hgMemo[OPT_HG_MEMO];
static int g_hgMemoNext = 0;

static OptHgMemo *optsHgFind(const char *path, int64_t mtime, long long size,
                             uint64_t proj) {
  for (int i = 0; i < OPT_HG_MEMO; i++) {
    OptHgMemo *m = &g_hgMemo[i];
    if (m->used && m->proj == proj && m->mtime == mtime && m->size == size &&
        strcmp(m->path, path) == 0)
      return m;
  }
  return NULL;
}

static bool optsCacheHasGroupParse(const char *path, uint64_t proj) {
  CsStore *st = csOpen(path);
  if (!st) return false;
  OptGroupCtx ctx = {proj, false};
  csForEach(st, optGroupVisit, &ctx);
  csClose(st);
  return ctx.has;
}

bool optsCacheHasGroup(const char *path, uint64_t proj) {
  int64_t mtime = 0;
  long long size = 0;
  if (fsStampNsSize(path, &mtime, &size)) {
    OptHgMemo *hit = optsHgFind(path, mtime, size, proj);
    if (hit) return hit->has;
  }
  bool has = optsCacheHasGroupParse(path, proj);
  if (size > 0) {
    OptHgMemo *slot = &g_hgMemo[g_hgMemoNext];
    g_hgMemoNext = (g_hgMemoNext + 1) % OPT_HG_MEMO;
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->mtime = mtime;
    slot->size = size;
    slot->proj = proj;
    slot->has = has;
    slot->used = true;
  }
  return has;
}


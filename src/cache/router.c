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
/* Memo presence grup per proses, diindeks oleh ISI file (hash FNV-1a + ukuran + proj).
   Hasilnya fungsi murni dari byte file: isi berubah karena penulisan -> hash berubah ->
   memo tidak dipakai. Tidak ada invalidasi manual yang bisa terlewat. Dipakai karena
   csOpen mem-parse seluruh file (deps/lib/fingerprint ~100 KB, 10 panggilan per run,
   hanya 4 isi unik; lihat todos/4_11_2026_optimizer.md item 7b). */
#define OPT_HG_MEMO 16
typedef struct {
  unsigned long long h;
  long long size;
  uint64_t proj;
  bool has;
} OptHgMemo;
static OptHgMemo g_hgMemo[OPT_HG_MEMO];
static int g_hgMemoN = 0, g_hgMemoNext = 0;

/* Hash FNV-1a isi file cache + ukurannya (item 10): kunci memo presence
   berindeks isi — hasil murni fungsi byte file, penulisan mengubah isi ->
   hash berubah -> memo basi alami tanpa invalidasi manual. */
static bool optsCacheFileHash(const char *path, unsigned long long *h, long long *sz) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  unsigned long long x = 1469598103934665603ULL;
  long long n = 0;
  unsigned char buf[65536];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
    for (size_t i = 0; i < got; i++) {
      x ^= buf[i];
      x *= 1099511628211ULL;
    }
    n += (long long)got;
  }
  bool ok = !ferror(f);
  fclose(f);
  *h = x;
  *sz = n;
  return ok;
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
  unsigned long long h = 0;
  long long sz = 0;
  bool hashed = optsCacheFileHash(path, &h, &sz);
  if (hashed) {
    for (int i = 0; i < g_hgMemoN; i++)
      if (g_hgMemo[i].h == h && g_hgMemo[i].size == sz && g_hgMemo[i].proj == proj)
        return g_hgMemo[i].has;
  }
  bool has = optsCacheHasGroupParse(path, proj);
  if (hashed) {
    OptHgMemo *slot;
    if (g_hgMemoN < OPT_HG_MEMO) {
      slot = &g_hgMemo[g_hgMemoN++];
    } else {
      slot = &g_hgMemo[g_hgMemoNext];
      g_hgMemoNext = (g_hgMemoNext + 1) % OPT_HG_MEMO;
    }
    slot->h = h;
    slot->size = sz;
    slot->proj = proj;
    slot->has = has;
  }
  return has;
}


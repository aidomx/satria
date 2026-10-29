/*
 * cmds_libcache — caches/lib.cache: fast-state fase library\n * (entri meta + per-input stamp; design/startup.md §3/§4).
 * Dipisah dari cmds_state.c (audit refactor: berkas kandidat >600 baris).
 * Definisi & semantik lihat cmds_internal.h.
 */
#include "cmds_internal.h"
#include "../log/report_blob.h"
#include "../cache/artifact.h"
#include "../cache/state_blob.h"
#include "../cache/router.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../background.h"
#include "../cache/cache_store.h"
#include "../commands.h"
#include "../compdb.h"
#include "../deps.h"
#include "../compile.h"
#include "../embed_ld_cache.h"
#include "../portability.h"
#include "../util.h"

/* ==================== caches/lib.cache (design/startup.md §3/§4) ====================
   Fast-state FASE LIBRARY (dulu .satria/build.lib.state untuk single dan
   .satria/workspace/<proyek>.ws.lib.state untuk workspace) kini berbagi
   SATU file terpusat `.satria/caches/lib.cache` (cache_store), dibedakan
   lewat alamat proyek: satu entri meta (payload token k=v) + satu entri per
   input (payload "<kind> <path>"; kind F file / D direktori / M hilang /
   L verdict ld). Snapshot fase binary (`build.state` single / `ws.state`
   workspace) TETAP memakai format lamanya — kini tersimpan di
   fingerprint.cache dan state.cache; cache GET no-op default on, off via
   SATRIA_CACHE_NOOP=0.
   File lama fase library tidak lagi dibaca/ditulis; clean tetap membuangnya. */

/* Kunci entri meta (mustahil tabrakan dengan csHash(path) file nyata). */
static uint64_t libMetaAddr(void) { return csHash("<libmeta>"); }

uint64_t cmdsCacheProjAddr(void) {
  const char *name = cmdsStateWsProject();
  if (name && *name) return csHash(name);
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return 0;
  return csHash(cwd);
}

typedef struct {
  uint64_t *v;
  size_t n, cap;
  uint64_t proj;
} LibKeyCtx;

static bool libKeysCollect(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  LibKeyCtx *ctx = ctxp;
  if (e->proj != ctx->proj) return true;
  if (ctx->n == ctx->cap) {
    size_t nc = ctx->cap ? ctx->cap * 2 : 256;
    uint64_t *nv = realloc(ctx->v, nc * sizeof(uint64_t));
    if (!nv) return false;
    ctx->v = nv;
    ctx->cap = nc;
  }
  ctx->v[ctx->n++] = e->file;
  return true;
}

/* Payload meta: token "k=v" dipisah spasi (path tanpa spasi — guard yang
   sama dengan format teks lama). lib=<path> diulang untuk daftar library. */
static char *libMetaPayload(const LibCacheMeta *m) {
  size_t cap = (strlen(m->cwd) + strlen(m->buildfile) + strlen(m->target) +
                strlen(m->libStaticPath) + strlen(m->libSharedPath)) *
                   2 +
               4096;
  for (int i = 0; i < m->nlibs; i++) cap += strlen(m->libs[i]) + 8;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  snprintf(buf, cap,
           "v1 cwd=%s bf=%s bfm=%lld bfs=%lld tgt=%s tm=%lld ts=%lld "
           "skipped=%d compiled=%d libReq=%d libStUp=%d libShUp=%d "
           "libSt=%s libSh=%s stSz=%lld shSz=%lld",
           m->cwd, m->buildfile, m->bfm, m->bfs, m->target[0] ? m->target : "-", m->tm,
           m->ts, m->skipped, m->compiled, m->libRequested ? 1 : 0, m->libStaticUp ? 1 : 0,
           m->libSharedUp ? 1 : 0, m->libStaticPath[0] ? m->libStaticPath : "-",
           m->libSharedPath[0] ? m->libSharedPath : "-", m->libStaticSize, m->libSharedSize);
  size_t used = strlen(buf);
  for (int i = 0; i < m->nlibs; i++) {
    int w = snprintf(buf + used, cap - used, " lib=%s", m->libs[i]);
    if (w < 0 || (size_t)w >= cap - used) break;
    used += (size_t)w;
  }
  return buf;
}

bool libCacheSave(const LibCacheMeta *m, const LibStampRec *stamps, int n) {
  mkdirs(WS_CACHES_DIR);
  CsStore *st = csOpen(LIB_CACHE_FILE);
  if (!st) {
    remove(LIB_CACHE_FILE); /* rusak: tulis ulang bersih (self-healing) */
    st = csOpen(LIB_CACHE_FILE);
  }
  if (!st) return false;
  uint64_t proj = cmdsCacheProjAddr();

  LibKeyCtx keys = {NULL, 0, 0, proj};
  bool collected = csForEach(st, libKeysCollect, &keys);
  if (collected)
    for (size_t i = 0; i < keys.n; i++) csDelete(st, proj, keys.v[i]);
  free(keys.v);
  if (!collected) {
    csClose(st);
    return false;
  }

  char *meta = libMetaPayload(m);
  if (meta) {
    CsEntry e = {proj, libMetaAddr(), 0, 0, 0, meta};
    csPut(st, &e);
    free(meta);
  }
  for (int i = 0; i < n; i++) {
    /* F membawa content16hex (bukti sentuh, design §4); D/M/L tetap "<kind> <path>". */
    char payload[WS_PATH_LEN + 32];
    if (stamps[i].kind == 'F')
      snprintf(payload, sizeof(payload), "%c %016llx %s", stamps[i].kind,
               (unsigned long long)stamps[i].content, stamps[i].path);
    else
      snprintf(payload, sizeof(payload), "%c %s", stamps[i].kind, stamps[i].path);
    CsEntry e = {proj, csHash(stamps[i].path), 0, stamps[i].mtime, stamps[i].size, payload};
    csPut(st, &e);
  }
  bool ok = csCommit(st);
  csClose(st);
  return ok;
}

typedef struct {
  LibStampRec *v;
  size_t n, cap;
  uint64_t proj;
  uint64_t metaAddr;
} LibStampCtx;

static bool libStampVisit(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  LibStampCtx *ctx = ctxp;
  if (e->proj != ctx->proj || e->file == ctx->metaAddr || !e->payload) return true;
  if (!e->payload[0] || e->payload[1] != ' ') return true;
  char kind = e->payload[0];
  if (kind != 'F' && kind != 'D' && kind != 'M' && kind != 'L') return true;
  if (ctx->n == ctx->cap) {
    size_t nc = ctx->cap ? ctx->cap * 2 : 128;
    LibStampRec *nv = realloc(ctx->v, nc * sizeof(LibStampRec));
    if (!nv) return false;
    ctx->v = nv;
    ctx->cap = nc;
  }
  LibStampRec *r = &ctx->v[ctx->n++];
  r->kind = kind;
  r->content = 0;
  if (kind == 'F') {
    /* F baru: "<kind> <content16hex> <path>"; lama: "<kind> <path>". */
    char a[WS_PATH_LEN], b[WS_PATH_LEN];
    int ntok = sscanf(e->payload + 2, "%" WS_SCAN "s %" WS_SCAN "s", a, b);
    if (ntok == 2 && sscanf(a, "%16llx", &r->content) == 1)
      snprintf(r->path, sizeof(r->path), "%s", b);
    else
      snprintf(r->path, sizeof(r->path), "%s", a);
  } else {
    snprintf(r->path, sizeof(r->path), "%s", e->payload + 2);
  }
  r->mtime = e->time;
  r->size = e->size;
  return true;
}

bool libCacheLoad(LibCacheMeta *m, LibStampRec **stampsOut, int *nOut) {
  memset(m, 0, sizeof(*m));
  *stampsOut = NULL;
  *nOut = 0;
  CsStore *st = csOpen(LIB_CACHE_FILE);
  if (!st) return false;
  uint64_t proj = cmdsCacheProjAddr();
  CsEntry meta;
  if (!csGet(st, proj, libMetaAddr(), &meta) || !meta.payload) {
    csClose(st);
    return false;
  }

  char *copy = strdup(meta.payload);
  if (!copy) {
    csClose(st);
    return false;
  }
  int libsCap = 0;
  for (char *save = NULL, *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
    char *eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = '\0';
    const char *k = tok, *v = eq + 1;
    if (strcmp(k, "cwd") == 0) snprintf(m->cwd, sizeof(m->cwd), "%s", v);
    else if (strcmp(k, "bf") == 0) snprintf(m->buildfile, sizeof(m->buildfile), "%s", v);
    else if (strcmp(k, "bfm") == 0) m->bfm = atoll(v);
    else if (strcmp(k, "bfs") == 0) m->bfs = atoll(v);
    else if (strcmp(k, "tgt") == 0) {
      if (strcmp(v, "-") != 0) snprintf(m->target, sizeof(m->target), "%s", v);
    } else if (strcmp(k, "tm") == 0) m->tm = atoll(v);
    else if (strcmp(k, "ts") == 0) m->ts = atoll(v);
    else if (strcmp(k, "skipped") == 0) m->skipped = atoi(v);
    else if (strcmp(k, "compiled") == 0) m->compiled = atoi(v);
    else if (strcmp(k, "libReq") == 0) m->libRequested = atoi(v) != 0;
    else if (strcmp(k, "libStUp") == 0) m->libStaticUp = atoi(v) != 0;
    else if (strcmp(k, "libShUp") == 0) m->libSharedUp = atoi(v) != 0;
    else if (strcmp(k, "libSt") == 0) {
      if (strcmp(v, "-") != 0) snprintf(m->libStaticPath, sizeof(m->libStaticPath), "%s", v);
    } else if (strcmp(k, "libSh") == 0) {
      if (strcmp(v, "-") != 0) snprintf(m->libSharedPath, sizeof(m->libSharedPath), "%s", v);
    } else if (strcmp(k, "stSz") == 0) m->libStaticSize = atoll(v);
    else if (strcmp(k, "shSz") == 0) m->libSharedSize = atoll(v);
    else if (strcmp(k, "lib") == 0) {
      if (m->nlibs == libsCap) {
        int nc = libsCap ? libsCap * 2 : 4;
        char **nv = realloc(m->libs, (size_t)nc * sizeof(char *));
        if (!nv) break;
        m->libs = nv;
        libsCap = nc;
      }
      m->libs[m->nlibs] = strdup(v);
      if (!m->libs[m->nlibs]) break;
      m->nlibs++;
    }
  }
  free(copy);

  LibStampCtx ctx = {NULL, 0, 0, proj, libMetaAddr()};
  bool walked = csForEach(st, libStampVisit, &ctx);
  csClose(st);
  if (!walked || ctx.n == 0) {
    free(ctx.v);
    libCacheMetaFree(m);
    return false;
  }
  *stampsOut = ctx.v;
  *nOut = (int)ctx.n;
  return true;
}

void libCacheMetaFree(LibCacheMeta *m) {
  if (!m) return;
  for (int i = 0; i < m->nlibs; i++) free(m->libs[i]);
  free(m->libs);
  m->libs = NULL;
  m->nlibs = 0;
}

/* Muat fase library dari caches/lib.cache ke FastState — validasi & cetak
   di bawah tetap sama persis dengan jalur teks lama. */
bool fastStateLoadLib(FastState *s) {
  LibCacheMeta m;
  LibStampRec *stamps = NULL;
  int n = 0;
  memset(s, 0, sizeof(*s));
  if (!libCacheLoad(&m, &stamps, &n)) return false;
  copyStr(s->cwd, sizeof(s->cwd), m.cwd);
  copyStr(s->buildfile, sizeof(s->buildfile), m.buildfile);
  s->buildfileMtime = m.bfm;
  s->buildfileSize = m.bfs;
  copyStr(s->target, sizeof(s->target), m.target[0] ? m.target : "-");
  s->targetMtime = m.tm;
  s->targetSize = m.ts;
  s->compiled = m.compiled;
  s->skipped = m.skipped;
  s->count = n;
  s->phase = 1;
  s->libRequested = m.libRequested;
  s->libStaticUp = m.libStaticUp;
  s->libSharedUp = m.libSharedUp;
  copyStr(s->libStaticPath, sizeof(s->libStaticPath), m.libStaticPath);
  copyStr(s->libSharedPath, sizeof(s->libSharedPath), m.libSharedPath);
  s->libStaticSize = m.libStaticSize;
  s->libSharedSize = m.libSharedSize;
  s->stamps = calloc((size_t)n, sizeof(FastStamp));
  if (!s->stamps) {
    free(stamps);
    memset(s, 0, sizeof(*s));
    return false;
  }
  for (int i = 0; i < n; i++) {
    s->stamps[i].kind = stamps[i].kind;
    snprintf(s->stamps[i].path, sizeof(s->stamps[i].path), "%s", stamps[i].path);
    s->stamps[i].mtime = stamps[i].mtime;
    s->stamps[i].size = stamps[i].size;
    s->stamps[i].content = stamps[i].content; /* 0 untuk entri lama (kompat mundur) */
  }
  free(stamps);
  return true;
}


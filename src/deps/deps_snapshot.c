/*
 * deps_snapshot — snapshot hash+mtime lintas-run & API publik deps
 * (dipindah apa adanya dari deps.c).
 *
 * Snapshot merekam hash konten seluruh file yang terlibat build sukses
 * terakhir: mtime dipakai sebagai shortcut (file dengan mtime sama tidak
 * dibaca ulang), hash memverifikasi `touch` tanpa perubahan isi. Cache
 * edges (.d) ikut dalam file yang sama — lihat deps_dotd.c.
 *
 * Penyimpanan (design/startup.md §3/§4): SATU file terpusat
 * `.satria/caches/deps.cache` (cache_store) untuk semua proyek, dibedakan
 * lewat alamat proyek. Baris snapshot: payload = path, time = mtime_ns,
 * content = hash isi, size = 0 (tanpa shortcut ukuran — semantik cache lama).
 * Baris edges: payload ":edge:<src>\x01<kid1>\x01...", time = mtime file .d,
 * size = jumlah kid (expect). File lama (.satria/deps.cache dan
 * .satria/workspace/<p>.deps.cache) tidak lagi dibaca/ditulis; clean tetap
 * membuangnya bila masih ada.
 */
#include "deps_internal.h"
#include "../cache/cache_store.h"
#include "../cmds/cmds_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../compile.h"
#include "../portability.h"
#include "../util.h"

DepCache *depsNew(const Config *c) {
  cmdsStatMemoSetCwd(); /* kunci memo node harus memakai cwd proyek ini */
  DepCache *dc = calloc(1, sizeof(*dc));
  if (!dc) return NULL;
  dc->cfg = c;
  includeDirs(c, &dc->incDirs);
  return dc;
}

void depsFree(DepCache *dc) {
  if (!dc) return;
  for (int i = 0; i < dc->count; i++) {
    free(dc->nodes[i].path);
    free(dc->nodes[i].kids);
    free(dc->nodes[i].semText);
  }
  free(dc->nodes);
  free(dc->hbucket);
  free(dc->sbucket);
  free(dc->ebucket);
  for (int i = 0; i < dc->edgeCount; i++) {
    free(dc->edges[i].src);
    for (int k = 0; k < dc->edges[i].nkids; k++)
      free(dc->edges[i].kids[k]);
    free(dc->edges[i].kids);
  }
  free(dc->edges);
  listFree(&dc->incDirs);
  for (int i = 0; i < dc->snapCount; i++)
    free(dc->snap[i].path);
  free(dc->snap);
  free(dc);
}

/* ===== hash table path -> index snapshot (schemanya sama dengan node) ===== */

static void snapHashRebuild(DepCache *dc, int ncap) {
  int *nb = calloc((size_t)ncap, sizeof(int));
  if (!nb) return;
  free(dc->sbucket);
  dc->sbucket = nb;
  dc->sbcap = ncap;
  int mask = ncap - 1;
  for (int idx = 0; idx < dc->snapCount; idx++) {
    int i = (int)(depPathHash(dc->snap[idx].path) & (uint64_t)mask);
    while (nb[i]) i = (i + 1) & mask;
    nb[i] = idx + 1;
  }
}

/* Daftarkan entry idx ke tabel; grow saat load factor > 0.75. */
static void snapHashInsert(DepCache *dc, int idx) {
  if (!dc->sbcap || (idx + 1) * 4 > dc->sbcap * 3)
    snapHashRebuild(dc, dc->sbcap ? dc->sbcap * 2 : 256);
  int mask = dc->sbcap - 1;
  int i = (int)(depPathHash(dc->snap[idx].path) & (uint64_t)mask);
  while (dc->sbucket[i]) i = (i + 1) & mask;
  dc->sbucket[i] = idx + 1;
}

/* Index snapshot untuk path, atau -1. O(1) vs linear scan dulu. */
static int snapFind(const DepCache *dc, const char *path) {
  if (!dc->sbcap) return -1;
  int mask = dc->sbcap - 1;
  int i = (int)(depPathHash(path) & (uint64_t)mask);
  for (;;) {
    int v = dc->sbucket[i];
    if (!v) return -1;
    int idx = v - 1;
    if (strcmp(dc->snap[idx].path, path) == 0) return idx;
    i = (i + 1) & mask;
  }
}

/* ============ cache terpusat .satria/caches/deps.cache (cache_store) ========= */

#define DEPS_EDGE_PREFIX ":edge:"

/* Alamat proyek untuk entri cache terpusat: cmdsCacheProjAddr (cmds_state.c)
   — nama proyek (workspace) atau cwd (single / sandbox profile). */

/* Kunci entri edges: berprefix agar tidak tabrakan dengan entri snapshot
   milik source yang sama — (proj, file) dalam cache_store unik. */
static uint64_t depsEdgeAddr(const char *src) {
  char buf[MAX_PATH + 16];
  snprintf(buf, sizeof(buf), DEPS_EDGE_PREFIX "%s", src);
  return csHash(buf);
}

/* Tambah baris snapshot tanpa menandai dirty (muatan cache, bukan perubahan). */
static void snapAppend(DepCache *dc, const char *path, int64_t mtime, uint64_t hash) {
  if (dc->snapCount == dc->snapCap) {
    int ncap = dc->snapCap ? dc->snapCap * 2 : 256;
    void *nn = realloc(dc->snap, (size_t)ncap * sizeof(*dc->snap));
    if (!nn) return;
    dc->snap = nn;
    dc->snapCap = ncap;
  }
  dc->snap[dc->snapCount].path = strdup(path);
  if (!dc->snap[dc->snapCount].path) return;
  dc->snap[dc->snapCount].mtime = mtime;
  dc->snap[dc->snapCount].hash = hash;
  snapHashInsert(dc, dc->snapCount);
  dc->snapCount++;
}

typedef struct {
  DepCache *dc;
  uint64_t proj;
  bool wantEdges;
} DepSnapCtx;

/* Satu entri caches/deps.cache -> baris snapshot atau edge. Payload rusak
   diabaikan (entri hilang = verifikasi menghitung ulang — selalu aman). */
static bool depSnapVisit(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  DepSnapCtx *ctx = ctxp;
  if (e->proj != ctx->proj || !e->payload) return true;
  size_t preLen = strlen(DEPS_EDGE_PREFIX);
  if (strncmp(e->payload, DEPS_EDGE_PREFIX, preLen) != 0) {
    snapAppend(ctx->dc, e->payload, e->time, e->content);
    return true;
  }
  if (!ctx->wantEdges) return true;

  const char *rest = e->payload + preLen;
  const char *sep = strchr(rest, '\x01');
  size_t sl = sep ? (size_t)(sep - rest) : strlen(rest);
  if (sl == 0 || sl >= MAX_PATH) return true;
  char src[MAX_PATH];
  memcpy(src, rest, sl);
  src[sl] = '\0';

  int nk = 0;
  for (const char *q = sep ? sep + 1 : NULL; q && *q; nk++) {
    q = strchr(q, '\x01');
    if (q) q++;
  }
  DepCache *dc = ctx->dc;
  if (dc->edgeCount == dc->edgeCap) {
    int ncap = dc->edgeCap ? dc->edgeCap * 2 : 128;
    void *nn = realloc(dc->edges, (size_t)ncap * sizeof(*dc->edges));
    if (!nn) return true;
    dc->edges = nn;
    dc->edgeCap = ncap;
  }
  int idx = dc->edgeCount;
  memset(&dc->edges[idx], 0, sizeof(dc->edges[idx]));
  dc->edges[idx].src = strdup(src);
  if (!dc->edges[idx].src) return true;
  if (nk > 0) {
    dc->edges[idx].kids = calloc((size_t)nk, sizeof(char *));
    if (!dc->edges[idx].kids) {
      free(dc->edges[idx].src);
      return true;
    }
  }
  int k = 0;
  for (const char *q = sep ? sep + 1 : NULL; q && *q && k < nk; k++) {
    const char *end = strchr(q, '\x01');
    size_t len = end ? (size_t)(end - q) : strlen(q);
    char *dup = malloc(len + 1);
    if (!dup) break;
    memcpy(dup, q, len);
    dup[len] = '\0';
    dc->edges[idx].kids[k] = dup;
    q = end ? end + 1 : NULL;
  }
  dc->edges[idx].nkids = k;
  dc->edges[idx].dm = e->time;
  dc->edges[idx].expect = (int)e->size; /* jumlah kid saat direkam */
  dc->edges[idx].used = false;
  dc->edgeCount++;
  return true;
}

void depSnapshotLoad(DepCache *dc) {
  if (dc->snapLoaded) return;
  dc->snapLoaded = true;

  bool wantEdges = !dc->edgesLoaded;
  dc->edgesLoaded = true;
  CsStore *st = csOpen(DEPS_CACHE_FILE);
  if (!st) {
    dc->snapIncomplete = true; /* belum ada / rusak — wajib direkam */
    return;
  }
  DepSnapCtx ctx = {dc, cmdsCacheProjAddr(), wantEdges};
  csForEach(st, depSnapVisit, &ctx);
  csClose(st);
  /* Hash table edges dibangun SEKALI setelah semua entri dimuat. */
  depEdgesHashBuild(dc);
}

static uint64_t snapshotHashOf(DepCache *dc, const char *path, int64_t *mtimeOut) {
  depSnapshotLoad(dc);
  int idx = snapFind(dc, path);
  if (idx < 0) return 0; /* 0 = tidak ada di snapshot */
  if (mtimeOut) *mtimeOut = dc->snap[idx].mtime;
  return dc->snap[idx].hash;
}

static void snapshotSet(DepCache *dc, const char *path, int64_t mtime, uint64_t hash) {
  depSnapshotLoad(dc);
  int idx = snapFind(dc, path);
  if (idx >= 0) {
    if (dc->snap[idx].hash != hash || dc->snap[idx].mtime != mtime) {
      dc->snap[idx].hash = hash;
      dc->snap[idx].mtime = mtime;
      dc->snapDirty = true;
    }
    return;
  }
  if (dc->snapCount == dc->snapCap) {
    int ncap = dc->snapCap ? dc->snapCap * 2 : 256;
    void *nn = realloc(dc->snap, (size_t)ncap * sizeof(*dc->snap));
    if (!nn) return;
    dc->snap = nn;
    dc->snapCap = ncap;
  }
  dc->snap[dc->snapCount].path = strdup(path);
  dc->snap[dc->snapCount].mtime = mtime;
  dc->snap[dc->snapCount].hash = hash;
  snapHashInsert(dc, dc->snapCount);
  dc->snapCount++;
  dc->snapDirty = true;
}

/*
 * Verifikasi satu node tanpa edges (daun dari edges .d, atau node apa pun):
 * false bila file hilang, belum pernah direkam, atau isinya berubah.
 * TIDAK membaca isi selama mtime masih sama dengan snapshot — dan TIDAK
 * membuka/memindai file untuk edges: daun .d tidak butuh edges.
 */
static bool nodeUpToDateShallow(DepCache *dc, int idx) {
  if (dc->nodes[idx].mtime < 0) return false; /* file hilang */

  int64_t recordedM = 0;
  uint64_t recorded = snapshotHashOf(dc, dc->nodes[idx].path, &recordedM);
  if (recorded == 0) {
    dc->snapIncomplete = true; /* file tak tertutup snapshot */
    return false;              /* belum pernah direkam */
  }

  if (dc->nodes[idx].mtime != recordedM) {
    /* mtime beda: `touch` atau konten benar-benar berubah — cek hash */
    if (dc->nodes[idx].hash == 0) dc->nodes[idx].hash = depComputeFileHash(dc->nodes[idx].path);
    if (dc->nodes[idx].hash != recorded) return false;
    snapshotSet(dc, dc->nodes[idx].path, dc->nodes[idx].mtime, dc->nodes[idx].hash);
  }
  return true;
}

/* Hash konten node untuk pass rekam — mtime shortcut dua arah: bila mtime
   sama dengan snapshot, pakai hash yang terekam tanpa membaca file. Ini
   membuat pass rekam setelah build no-op tidak membaca file sama sekali
   (tiap open+read mahal di proot). */
static uint64_t nodeRecordHash(DepCache *dc, int idx) {
  int64_t recordedM = 0;
  uint64_t recorded = snapshotHashOf(dc, dc->nodes[idx].path, &recordedM);
  if (recorded != 0 && recordedM == dc->nodes[idx].mtime) {
    dc->nodes[idx].hash = recorded;
    return recorded;
  }
  if (dc->nodes[idx].hash == 0) dc->nodes[idx].hash = depComputeFileHash(dc->nodes[idx].path);
  return dc->nodes[idx].hash;
}

/*
 * DFS: bandingkan hash konten subtree dengan snapshot build sukses.
 * false = ada file yang isinya benar-benar berubah.
 *
 * Node ber-flag dloaded (edges dari .d, flat-lengkap) tidak direkursi:
 * anak-anaknya diverifikasi shallow — mtime + snapshot, isi dibaca hanya
 * bila mtime berbeda. Node selalu di-resolve ulang via dc->nodes[idx]
 * (jangan simpan pointer): depNodeGet/parse bisa merealloc array node.
 */
static bool subtreeUpToDate(DepCache *dc, int idx) {
  depParseNodeIfNeeded(dc, idx);
  dc->nodes[idx].mark = dc->gen;
  bool flat = dc->nodes[idx].dloaded;

  if (!nodeUpToDateShallow(dc, idx) && !depsSemAcceptNode(dc, dc->semRoot, idx)) return false;

  /* salin kids dulu: depParseNodeIfNeeded anak bisa merealloc dc->nodes */
  int nkids = dc->nodes[idx].nkids;
  int *kids = malloc((size_t)(nkids > 0 ? nkids : 1) * sizeof(int));
  if (!kids) return false;
  memcpy(kids, dc->nodes[idx].kids, (size_t)nkids * sizeof(int));

  bool ok = true;
  for (int i = 0; i < nkids; i++) {
    int kid = kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    dc->nodes[kid].mark = dc->gen;
    bool kidOk = flat ? (nodeUpToDateShallow(dc, kid) || depsSemAcceptNode(dc, dc->semRoot, kid))
                      : subtreeUpToDate(dc, kid);
    if (!kidOk) {
      ok = false;
      break;
    }
  }
  free(kids);
  return ok;
}

/* DFS: rekam hash konten subtree sebagai snapshot terkompilasi. Anak dari
   node flat direkam shallow (hash dari snapshot bila mtime sama — build
   no-op tidak membaca file); node non-flat direkursi seperti biasa. */
static void subtreeRecord(DepCache *dc, int idx) {
  depParseNodeIfNeeded(dc, idx);
  dc->nodes[idx].mark = dc->gen;
  bool flat = dc->nodes[idx].dloaded;

  if (dc->nodes[idx].mtime < 0) return;
  snapshotSet(dc, dc->nodes[idx].path, dc->nodes[idx].mtime, nodeRecordHash(dc, idx));
  depsSemRecordNode(dc, idx);

  int nkids = dc->nodes[idx].nkids;
  int *kids = malloc((size_t)(nkids > 0 ? nkids : 1) * sizeof(int));
  if (!kids) return;
  memcpy(kids, dc->nodes[idx].kids, (size_t)nkids * sizeof(int));

  for (int i = 0; i < nkids; i++) {
    int kid = kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    dc->nodes[kid].mark = dc->gen;
    if (flat) {
      if (dc->nodes[kid].mtime < 0) continue;
      snapshotSet(dc, dc->nodes[kid].path, dc->nodes[kid].mtime, nodeRecordHash(dc, kid));
      depsSemRecordNode(dc, kid);
    } else {
      subtreeRecord(dc, kid);
    }
  }
  free(kids);
}

bool depsContentUpToDate(DepCache *dc, const char *src) {
  if (!dc || !src) return false;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s", src);
  depNormalizePath(path);

  int root = depNodeGet(dc, path);
  if (root < 0) return false;

  dc->gen++;
  dc->semRoot = root;
  return subtreeUpToDate(dc, root);
}

/*
 * true bila snapshot hash belum menutupi seluruh state saat ini (deps.cache
 * tidak ada, ada file yang belum pernah direkam, atau cache edges meleset).
 * Bila false DAN tidak ada yang dikompilasi, pass rekam setelah build boleh
 * dilewati — tidak ada informasi baru yang bisa direkam (no-op murni).
 */
bool depsSnapshotIncomplete(const DepCache *dc) {
  return !dc || dc->snapIncomplete;
}

void depsRecordUpdate(DepCache *dc, const char *src) {
  if (!dc || !src) return;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s", src);
  depNormalizePath(path);

  int root = depNodeGet(dc, path);
  if (root < 0) return;

  dc->gen++;
  subtreeRecord(dc, root);
}

/* ":edge:<src>\x01<kid1>\x01..." — src diulang di payload supaya path bisa
   dipulihkan saat load (kunci entri hanya hash). */
static char *edgePayloadMake(const char *src, char *const *kids, int nkids) {
  size_t len = strlen(DEPS_EDGE_PREFIX) + strlen(src);
  for (int k = 0; k < nkids; k++) len += 1 + strlen(kids[k]);
  char *buf = malloc(len + 1);
  if (!buf) return NULL;
  char *w = buf;
  memcpy(w, DEPS_EDGE_PREFIX, strlen(DEPS_EDGE_PREFIX));
  w += strlen(DEPS_EDGE_PREFIX);
  size_t sl = strlen(src);
  memcpy(w, src, sl);
  w += sl;
  for (int k = 0; k < nkids; k++) {
    *w++ = '\x01';
    size_t kl = strlen(kids[k]);
    memcpy(w, kids[k], kl);
    w += kl;
  }
  *w = '\0';
  return buf;
}

typedef struct {
  uint64_t *v;
  size_t n, cap;
  uint64_t proj;
} DepKeyCtx;

/* Kumpulkan kunci entri lama milik proyek ini — csDelete saat iterasi akan
   menginvalasi forEach, jadi kunci dikumpulkan dulu. */
static bool depKeysCollect(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  DepKeyCtx *ctx = ctxp;
  if (e->proj != ctx->proj) return true;
  if (ctx->n == ctx->cap) {
    size_t nc = ctx->cap ? ctx->cap * 2 : 256;
    uint64_t *nv = realloc(ctx->v, nc * sizeof(uint64_t));
    if (!nv) return false; /* gagal: batal simpan — file lama tetap utuh */
    ctx->v = nv;
    ctx->cap = nc;
  }
  ctx->v[ctx->n++] = e->file;
  return true;
}

void depsSave(DepCache *dc) {
  if (!dc || (!dc->snapDirty && !dc->edgeDirty)) return;

  mkdirs(WS_CACHES_DIR);
  CsStore *st = csOpen(DEPS_CACHE_FILE);
  if (!st) {
    /* File rusak: buang lalu tulis ulang bersih (self-healing — tanpa ini
       csOpen gagal terus-menerus dan cache tidak pernah terbentuk). */
    remove(DEPS_CACHE_FILE);
    st = csOpen(DEPS_CACHE_FILE);
  }
  if (!st) return;
  uint64_t proj = cmdsCacheProjAddr();

  DepKeyCtx keys = {NULL, 0, 0, proj};
  bool collected = csForEach(st, depKeysCollect, &keys);
  if (collected)
    for (size_t i = 0; i < keys.n; i++) csDelete(st, proj, keys.v[i]);
  free(keys.v);
  if (!collected) {
    csClose(st);
    return;
  }

  /* Baris snapshot: payload = path; size = 0 -> validasi mtime-saja
     (persis shortcut cache lama, tanpa stat ukuran tambahan). */
  for (int i = 0; i < dc->snapCount; i++) {
    CsEntry e = {proj, csHash(dc->snap[i].path), dc->snap[i].hash,
                 (long long)dc->snap[i].mtime, 0, dc->snap[i].path};
    csPut(st, &e);
  }
  /* Edges terpakai: payload ":edge:...", size = nkids (expect). Entry tak
     terpakai gugur otomatis (cache edges ter-pangkas tiap tulis). */
  for (int i = 0; i < dc->edgeCount; i++) {
    if (!dc->edges[i].used) continue;
    char *pay = edgePayloadMake(dc->edges[i].src, dc->edges[i].kids, dc->edges[i].nkids);
    if (!pay) continue;
    CsEntry e = {proj, depsEdgeAddr(dc->edges[i].src), 0, (long long)dc->edges[i].dm,
                 dc->edges[i].nkids, pay};
    csPut(st, &e);
    free(pay);
  }

  if (csCommit(st)) {
    dc->snapDirty = false;
    dc->edgeDirty = false;
  }
  csClose(st);
}

int64_t depsNewestHeaderMTime(DepCache *dc, const char *src) {
  return depsNewestHeaderMTimeAt(dc, src, -2);
}

int64_t depsNewestHeaderMTimeAt(DepCache *dc, const char *src, int64_t srcMTimeNs) {
  if (!dc || !src) return 0;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s", src);
  depNormalizePath(path);
  int root = depNodeFind(dc, path);
  if (root < 0) root = srcMTimeNs >= -1 ? depNodeAddM(dc, path, srcMTimeNs) : depNodeAdd(dc, path);
  if (root < 0) return 0;

  dc->gen++;
  dc->nodes[root].mark = dc->gen;
  if (!dc->nodes[root].parsed) depParseNodeIfNeeded(dc, root);

  /* Edges dari .d: flat-lengkap — cukup mtime anak langsung (sudah di-stat
     saat node dibuat). Tanpa ini jalur ini memindai & membuka semua header
     transitif padahal listnya sudah jadi; mahal di proot. */
  if (dc->nodes[root].dloaded) {
    int64_t best = 0;
    for (int i = 0; i < dc->nodes[root].nkids; i++) {
      int64_t m = dc->nodes[dc->nodes[root].kids[i]].mtime;
      if (m < 0) return INT64_MAX; /* dependensi hilang -> stale (lihat depNewestUnder) */
      if (m > best) best = m;
    }
    return best;
  }

  int64_t best = 0;
  for (int i = 0; i < dc->nodes[root].nkids; i++) {
    int kid = dc->nodes[root].kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    int64_t m = depNewestUnder(dc, kid);
    if (m > best) best = m;
  }
  return best;
}

/* ==================== closure dependensi (fast-state workspace) ====================
   Pola traversal sama dengan depsNewestHeaderMTimeAt. Node dloaded (edges dari
   .d) sudah flat dan lengkap, jadi anak langsungnya dikirim tanpa ditelusuri lagi. */
static void closureVisitNode(DepCache *dc, int idx, DepVisitFn visit, void *ctx) {
  if (dc->nodes[idx].mark == dc->gen) return;
  dc->nodes[idx].mark = dc->gen;
  if (!dc->nodes[idx].parsed && !dc->nodes[idx].dloaded) depParseNodeIfNeeded(dc, idx);
  visit(dc->nodes[idx].path, ctx);
  int nk = dc->nodes[idx].nkids;
  if (nk <= 0) return;
  int *kids = malloc((size_t)nk * sizeof(int));
  if (!kids) return;
  memcpy(kids, dc->nodes[idx].kids, (size_t)nk * sizeof(int));
  bool flat = dc->nodes[idx].dloaded;
  for (int i = 0; i < nk; i++) {
    int kid = kids[i];
    if (flat) {
      if (dc->nodes[kid].mark == dc->gen) continue;
      dc->nodes[kid].mark = dc->gen;
      visit(dc->nodes[kid].path, ctx);
    } else {
      closureVisitNode(dc, kid, visit, ctx);
    }
  }
  free(kids);
}

void depsClosureVisitMany(DepCache *dc, const char *const *srcs, int count,
                          DepVisitFn visit, void *ctx) {
  if (!dc || !srcs || count <= 0 || !visit) return;
  /* Satu generation untuk seluruh root: header bersama hanya dikunjungi sekali.
     API single-source tetap memakai generation sendiri. */
  dc->gen++;
  for (int i = 0; i < count; i++) {
    if (!srcs[i]) continue;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s", srcs[i]);
    depNormalizePath(path);
    int root = depNodeFind(dc, path);
    if (root >= 0) closureVisitNode(dc, root, visit, ctx);
  }
}

void depsClosureVisit(DepCache *dc, const char *src, DepVisitFn visit, void *ctx) {
  if (!dc || !src || !visit) return;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s", src);
  depNormalizePath(path);
  int root = depNodeFind(dc, path);
  if (root < 0) return;
  dc->gen++;
  closureVisitNode(dc, root, visit, ctx);
}


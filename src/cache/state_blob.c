/* state_blob — lihat state_blob.h. */
#include "state_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cache_store.h"
#include "../portability.h"

static CsStore *g_base = NULL;  /* snapshot read-only state.cache (lazy) */
static CsStore *g_pend = NULL;  /* penulisan belum di-flush */

static bool storeOpenFile(CsStore **out) {
  *out = csOpen(STATE_CACHE_FILE);
  return *out != NULL;
}

static CsStore *pendStore(void) {
  if (!g_pend) g_pend = csOpen(""); /* path kosong = store kosong, tak pernah di-commit */
  return g_pend;
}

static CsStore *baseStore(void) {
  if (!g_base) {
    if (!storeOpenFile(&g_base)) g_base = csOpen(""); /* rusak: anggap kosong */
  }
  return g_base;
}

static char *escapeValue(const char *buf, size_t len) {
  char *out = malloc(len * 2 + 2);
  if (!out) return NULL;
  char *w = out;
  *w++ = 'V';
  for (size_t i = 0; i < len; i++) {
    char c = buf[i];
    if (c == '\\') { *w++ = '\\'; *w++ = '\\'; }
    else if (c == '\n') { *w++ = '\\'; *w++ = 'n'; }
    else if (c == '\r') { *w++ = '\\'; *w++ = 'r'; }
    else *w++ = c;
  }
  *w = '\0';
  return out;
}

/* Balik escapeValue (payload tanpa prefix 'V'). */
static char *unescapeValue(const char *p, size_t *outLen) {
  size_t n = strlen(p);
  char *out = malloc(n + 1);
  if (!out) return NULL;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '\\' && i + 1 < n) {
      char d = p[++i];
      out[k++] = d == 'n' ? '\n' : d == 'r' ? '\r' : d;
    } else {
      out[k++] = p[i];
    }
  }
  out[k] = '\0';
  *outLen = k;
  return out;
}

static bool lookup(const char *proj, const char *leaf, const char **payloadOut, bool *deleted) {
  uint64_t pk = csHash(proj), lk = csHash(leaf);
  CsEntry e;
  *deleted = false;
  if (g_pend && csGet(g_pend, pk, lk, &e)) {
    if (e.payload && e.payload[0] == 'D') { *deleted = true; return false; }
    *payloadOut = e.payload;
    return e.payload != NULL;
  }
  if (baseStore() && csGet(g_base, pk, lk, &e)) {
    *payloadOut = e.payload;
    return e.payload != NULL;
  }
  return false;
}

bool stateBlobGet(const char *proj, const char *leaf, char **buf, size_t *len) {
  const char *pay = NULL;
  bool deleted = false;
  if (!lookup(proj, leaf, &pay, &deleted) || pay[0] != 'V') return false;
  size_t n = 0;
  char *b = unescapeValue(pay + 1, &n);
  if (!b) return false;
  *buf = b;
  *len = n;
  return true;
}

static bool putPayload(const char *proj, const char *leaf, const char *payload, size_t size) {
  CsEntry e = {0};
  e.proj = csHash(proj);
  e.file = csHash(leaf);
  e.size = (long long)size;
  e.payload = payload;
  return csPut(pendStore(), &e);
}

bool stateBlobPut(const char *proj, const char *leaf, const char *buf, size_t len) {
  char *pay = escapeValue(buf ? buf : "", buf ? len : 0);
  if (!pay) return false;
  bool ok = putPayload(proj, leaf, pay, len);
  free(pay);
  return ok;
}

bool stateBlobDelete(const char *proj, const char *leaf) {
  return putPayload(proj, leaf, "D", 0);
}

bool stateBlobExists(const char *proj, const char *leaf) {
  const char *pay = NULL;
  bool deleted = false;
  return lookup(proj, leaf, &pay, &deleted) && pay && pay[0] == 'V';
}

uint64_t stateBlobHash(const char *proj, const char *leaf) {
  char *b = NULL;
  size_t n = 0;
  if (!stateBlobGet(proj, leaf, &b, &n)) return 0;
  uint64_t h = 1469598103934665603ULL; /* seed sama dengan wsLogFileHash (lihat catatan) */
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)b[i];
    h *= 1099511628211ULL;
  }
  free(b);
  return n ? h : 0;
}

/* Meta P grup (§4) ditunda: diterapkan sekali BERSAMA stateBlobFlush.
   Sebelumnya tiap stateBlobProjMeta membuka + meng-commit seluruh
   state.cache (4× per run workspace — 3 proyek + grup compdb) hanya untuk
   menyetel satu baris metadata; pemborosan ini nyata pada bench hedit/libhdr
   (±40 ms per siklus di proot). Antrian in-memory: terisi hanya dari
   wsCacheSave (induk, fase akhir) — anak fork tidak pernah memanggilnya. */
typedef struct {
  uint64_t pk, content;
  long long time;
} BlobMeta;
static BlobMeta *g_meta = NULL;
static int g_metaN = 0, g_metaCap = 0;

bool stateBlobProjMeta(const char *proj, uint64_t content, long long time) {
  if (!proj || !*proj) return true;
  uint64_t pk = csHash(proj);
  for (int i = 0; i < g_metaN; i++) {
    if (g_meta[i].pk == pk) { /* ganti entri sama (idempoten, seperti sebelumnya) */
      g_meta[i].content = content;
      g_meta[i].time = time;
      return true;
    }
  }
  if (g_metaN == g_metaCap) {
    int next = g_metaCap ? g_metaCap * 2 : 8;
    BlobMeta *grown = (BlobMeta *)realloc(g_meta, sizeof(BlobMeta) * (size_t)next);
    if (!grown) return false;
    g_meta = grown;
    g_metaCap = next;
  }
  g_meta[g_metaN].pk = pk;
  g_meta[g_metaN].content = content;
  g_meta[g_metaN].time = time;
  g_metaN++;
  return true;
}

/* Tulis pending ke state.cache: markers 'D' menghapus, selain itu menyimpan. */
struct FlushCtx { CsStore *dst; bool ok; };
static bool flushVisit(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  struct FlushCtx *c = ctx;
  if (e->payload && e->payload[0] == 'D') {
    csDelete(c->dst, e->proj, e->file);
    return true;
  }
  if (!csPut(c->dst, e)) { c->ok = false; return false; }
  return true;
}

bool stateBlobFlush(void) {
  bool haveMeta = g_metaN > 0;
  if ((!g_pend || csCount(g_pend) == 0) && !haveMeta) return true;
  /* `-g compdb` dapat menjadi operasi pertama sebelum direktori cache ada. */
  fsMakeDir(".satria");
  fsMakeDir(".satria/caches");
  CsStore *dst = NULL;
  if (!storeOpenFile(&dst)) {
    remove(STATE_CACHE_FILE); /* rusak: tulis ulang bersih */
    dst = csOpen(STATE_CACHE_FILE);
  }
  if (!dst) return false;
  struct FlushCtx c = {dst, true};
  if (g_pend) csForEach(g_pend, flushVisit, &c);
  /* Meta P grup diterapkan SETELAH entri pending (count final, sama seperti
     perilaku lama stateBlobProjMeta yang dipanggil sesudah tahap penyimpanan). */
  for (int i = 0; c.ok && i < g_metaN; i++)
    csProjMetaSet(dst, g_meta[i].pk, g_meta[i].content, g_meta[i].time,
                  (long long)csCountProj(dst, g_meta[i].pk));
  bool ok = c.ok && csCommit(dst);
  csClose(dst);
  if (ok) {
    csClose(g_pend);
    g_pend = NULL;
    csClose(g_base);
    g_base = NULL; /* baca ulang dari file berikutnya */
    g_metaN = 0;   /* sudah diterapkan */
  }
  return ok;
}

void stateBlobForkChild(void) {
  if (g_pend) csClose(g_pend);
  g_pend = NULL;
  if (g_base) csClose(g_base);
  g_base = NULL;
}

/* Salin entri apa adanya (marker 'D' ikut tersalin; tidak ditafsirkan). */
struct CopyCtx { CsStore *dst; bool ok; };
static bool copyVisit(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  struct CopyCtx *c = ctx;
  if (!csPut(c->dst, e)) { c->ok = false; return false; }
  return true;
}

bool stateBlobExport(const char *stagePath) {
  remove(stagePath); /* stage selalu dimulai dari kosong */
  CsStore *st = csOpen(stagePath);
  if (!st) return false;
  struct CopyCtx c = {st, true};
  if (g_pend) csForEach(g_pend, copyVisit, &c);
  bool ok = c.ok && csCommit(st);
  csClose(st);
  return ok;
}

bool stateBlobImport(const char *stagePath) {
  CsStore *st = csOpen(stagePath);
  if (!st) return false;
  struct CopyCtx c = {pendStore(), true};
  bool ok = csForEach(st, copyVisit, &c) && c.ok;
  csClose(st);
  remove(stagePath);
  return ok;
}

char *stateBufGets(const char *buf, size_t len, size_t *pos, char *line, int n) {
  if (*pos >= len || n <= 0) return NULL;
  int k = 0;
  while (*pos < len && k < n - 1) {
    char c = buf[(*pos)++];
    line[k++] = c;
    if (c == '\n') break;
  }
  line[k] = '\0';
  return line;
}

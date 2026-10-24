/* artifact.c — lihat artifact.h. Store memakai cache_store (CsStore) dengan
   payload = path (lookup langsung tanpa tabel P) dan kunci proyek tetap
   csHash("artifacts"). Jenis dibedakan lewat content: 0 = folder (validasi
   mtime ns), != 0 = file (validasi mtime+size, fallback hash isi). */
#include "artifact.h"
#include "cache_store.h"
#include "../portability.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char **paths;
  int n;
  int cap;
} ArtList;
static ArtList artList = {0, 0, 0};

/* FNV-1a 64 atas isi file (streaming 4KB; 0 = tak terbaca/tak terhitung). */
static uint64_t artHash(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  uint64_t h = 14695981039346656037ULL;
  unsigned char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    for (size_t i = 0; i < n; i++) {
      h ^= buf[i];
      h *= 1099511628211ULL;
    }
  fclose(fp);
  return h;
}

void artifactCacheAdd(const char *path) {
  if (!path || !*path) return;
  for (int i = 0; i < artList.n; i++)
    if (strcmp(artList.paths[i], path) == 0) return;
  if (artList.n == artList.cap) {
    int next = artList.cap ? artList.cap * 2 : 16;
    char **grown = realloc(artList.paths, (size_t)next * sizeof(char *));
    if (!grown) return;
    artList.paths = grown;
    artList.cap = next;
  }
  char *dup = strdup(path);
  if (!dup) return;
  artList.paths[artList.n++] = dup;
}

static void artListClear(void) {
  for (int i = 0; i < artList.n; i++) free(artList.paths[i]);
  free(artList.paths);
  artList.paths = NULL;
  artList.n = artList.cap = 0;
}

static bool artPutFile(CsStore *st, const char *path) {
  int64_t ns = 0;
  long long sz = 0;
  if (!fsStampNsSize(path, &ns, &sz)) return true; /* hilang saat flush: lewati */
  uint64_t h = artHash(path);
  if (!h) return true; /* tak terhitung: jangan tulis entri palsu */
  CsEntry e = {csHash("artifacts"), csHash(path), h, (long long)ns, sz, path};
  return csPut(st, &e);
}

/* File -> entri F; folder -> entri D + rekursi anak (D per direktori, F per file). */
static bool artAddPath(CsStore *st, const char *path, unsigned depth) {
  if (depth > 64) return false;
  int64_t ns = 0;
  long long sz = 0;
  if (!fsStampNsSize(path, &ns, &sz)) return true; /* hilang: lewati tenang */
  if (!fsDirExists(path)) return artPutFile(st, path);
  CsEntry d = {csHash("artifacts"), csHash(path), 0, (long long)ns, 0, path};
  if (!csPut(st, &d)) return false;
  List dirs = {0}, files = {0};
  fsListDir(path, &dirs, &files);
  bool ok = true;
  for (int i = 0; ok && i < files.count; i++) ok = artPutFile(st, files.items[i]);
  for (int i = 0; ok && i < dirs.count; i++) ok = artAddPath(st, dirs.items[i], depth + 1);
  listFree(&dirs);
  listFree(&files);
  return ok;
}

bool artifactCacheSave(void) {
  if (artList.n == 0) {
    if (fsFileExists(ARTIFACT_CACHE_FILE)) return true; /* run tanpa artefak: pertahankan */
    mkdirs(".satria");
    FILE *f = fopen(ARTIFACT_CACHE_FILE, "w"); /* cold tanpa artefak: file kosong tetap ada */
    if (!f) return false;
    fclose(f);
    return true;
  }
  mkdirs(".satria");
  remove(ARTIFACT_CACHE_FILE);
  CsStore *st = csOpen(ARTIFACT_CACHE_FILE);
  if (!st) {
    artListClear();
    return false;
  }
  bool ok = true;
  for (int i = 0; ok && i < artList.n; i++) ok = artAddPath(st, artList.paths[i], 0);
  if (ok) {
    /* Metadata grup P (§4) seragam: P <kunci> <content> <time> <size> — skema
       sama dengan cache grup proyek lain; content = FNV fold atas daftar
       artefak run ini (kunci grup tetap csHash("artifacts")). */
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < artList.n; i++) {
      for (const unsigned char *p = (const unsigned char *)artList.paths[i]; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
      }
      h ^= 0xff;
      h *= 1099511628211ULL;
    }
    uint64_t grp = csHash("artifacts");
    csProjMetaSet(st, grp, h, csNowNs(), (long long)csCountProj(st, grp));
    ok = csCommit(st);
  }
  csClose(st);
  artListClear();
  return ok;
}

typedef struct {
  CsStore *st;
  bool ok;
  int seen;
} ArtCtx;

static bool artVisit(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  ArtCtx *c = ctxp;
  c->seen++;
  if (!e->payload || !*e->payload) return c->ok = false;
  int64_t ns = 0;
  long long sz = 0;
  if (!fsStampNsSize(e->payload, &ns, &sz)) {
    if (e->content == 0) return true; /* folder input memang dikecualikan dari paket */
    return c->ok = false; /* file hilang */
  }
  if (e->content == 0) { /* folder: mtime ns saja (add/remove) */
    if ((long long)ns != e->time) return c->ok = false;
    return true;
  }
  if (sz == e->size && (long long)ns == e->time) return true; /* fast path */
  uint64_t h = artHash(e->payload);
  if (h && h == e->content) {
    /* sentuh: PATCH time/size di artifact.cache, tanpa batal no-op */
    CsPatch p = {0};
    p.hasTime = true;
    p.time = (long long)ns;
    p.hasSize = true;
    p.size = sz;
    if (!csPatch(c->st, e->proj, e->file, &p)) return c->ok = false;
    return true;
  }
  return c->ok = false; /* isi berubah / tak terhitung */
}

bool artifactCacheValid(void) {
  if (!fsFileExists(ARTIFACT_CACHE_FILE)) return false; /* belum pernah ditulis */
  CsStore *st = csOpen(ARTIFACT_CACHE_FILE);
  if (!st) return false; /* rusak */
  ArtCtx c = {st, true, 0};
  bool walked = csForEach(st, artVisit, &c);
  bool valid = walked && c.ok;
  if (valid && csIsDirty(st)) valid = csCommit(st); /* PATCH sentuh */
  csClose(st);
  return valid;
}

/* cache_store — lihat cache_store.h dan design/startup.md §4. */
#define _POSIX_C_SOURCE 200809L /* strdup (C11 murni tidak memilikinya) */
#include "cache_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h> /* csNowNs: clock_gettime */

/* Penggantian atomik: POSIX rename; Windows MoveFileEx REPLACE_EXISTING |
   WRITE_THROUGH (fallback salin + hapus) — semantik sama dengan
   fsRenameReplace di portability, dijaga mandiri agar modul ini tetap
   bebas dependensi internal (design/startup.md §4 penulisan atomik). */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static bool csRenameReplace(const char *from, const char *to) {
  if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
  if (CopyFileA(from, to, FALSE)) {
    DeleteFileA(from);
    return true;
  }
  return false;
}
#else
static bool csRenameReplace(const char *from, const char *to) { return rename(from, to) == 0; }
#endif

#define CS_INIT_CAP 64

typedef struct CsNode {
  CsEntry e;
  CsOp op;
  struct CsNode *next;
} CsNode;

/* Metadata grup P (design/startup.md §4): P <proj> <content> <time> <size>.
   Diisi pemanggil lewat csProjMetaSet (mis. stamp konfigurasi + daftar sumber
   per proyek) dan dipertahankan lintas load/commit. Baris P lama tanpa
   metadata tetap terbaca (grup tanpa meta). */
typedef struct {
  uint64_t proj;
  uint64_t content;
  long long time, size;
} CsProjMeta;

struct CsStore {
  char path[1024];
  CsNode **buckets;
  size_t cap; /* selalu pangkat dua */
  size_t count;
  bool dirty;
  CsProjMeta *meta;
  size_t metaN, metaCap;
};

static CsProjMeta *metaFind(const CsStore *st, uint64_t proj) {
  for (size_t i = 0; i < st->metaN; i++)
    if (st->meta[i].proj == proj) return &st->meta[i];
  return NULL;
}

static bool metaUpsert(CsStore *st, uint64_t proj, uint64_t content, long long time,
                       long long size, bool *changed) {
  CsProjMeta *m = metaFind(st, proj);
  if (m) {
    /* Validasi grup hanya membandingkan content & size (lihat wsCacheNoopValid).
       Bila keduanya sama, time lama dipertahankan agar store tidak perlu ditulis. */
    if (m->content == content && m->size == size) {
      *changed = false;
      return true;
    }
    m->content = content;
    m->time = time;
    m->size = size;
    *changed = true;
    return true;
  }
  *changed = true;
  if (st->metaN == st->metaCap) {
    size_t nc = st->metaCap ? st->metaCap * 2 : 8;
    CsProjMeta *nm = realloc(st->meta, nc * sizeof(CsProjMeta));
    if (!nm) return false;
    st->meta = nm;
    st->metaCap = nc;
  }
  m = &st->meta[st->metaN++];
  m->proj = proj;
  m->content = content;
  m->time = time;
  m->size = size;
  return true;
}

uint64_t csHash(const char *s) {
  uint64_t h = 14695981039346656037ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return h;
}

static size_t bucketOf(size_t cap, uint64_t proj, uint64_t file) {
  uint64_t h = proj * 0x9E3779B97F4A7C15ULL;
  h ^= file + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
  return (size_t)(h & (cap - 1));
}

static CsNode *findNode(const CsStore *st, uint64_t proj, uint64_t file) {
  for (CsNode *n = st->buckets[bucketOf(st->cap, proj, file)]; n; n = n->next)
    if (n->e.proj == proj && n->e.file == file) return n;
  return NULL;
}

static bool rehash(CsStore *st, size_t newCap) {
  CsNode **nb = calloc(newCap, sizeof(CsNode *));
  if (!nb) return false;
  for (size_t i = 0; i < st->cap; i++) {
    CsNode *n = st->buckets[i];
    while (n) {
      CsNode *next = n->next;
      size_t b = bucketOf(newCap, n->e.proj, n->e.file);
      n->next = nb[b];
      nb[b] = n;
      n = next;
    }
  }
  free(st->buckets);
  st->buckets = nb;
  st->cap = newCap;
  return true;
}

/* Salin payload ke milik store (NULL/kosong -> NULL). */
static bool ownPayload(CsEntry *dst, const char *src) {
  dst->payload = NULL;
  if (!src || !*src) return true;
  dst->payload = strdup(src);
  return dst->payload != NULL;
}

/* Sisipkan node baru (entri belum ada). Tumbuhkan tabel bila beban > 2. */
static bool insertNew(CsStore *st, const CsEntry *e, CsOp op) {
  if (st->count + 1 > st->cap * 2 && !rehash(st, st->cap * 2)) return false;
  CsNode *n = malloc(sizeof(CsNode));
  if (!n) return false;
  n->e = *e;
  if (!ownPayload(&n->e, e->payload)) {
    free(n);
    return false;
  }
  n->op = op;
  size_t b = bucketOf(st->cap, e->proj, e->file);
  n->next = st->buckets[b];
  st->buckets[b] = n;
  st->count++;
  return true;
}

static void freeStore(CsStore *st) {
  if (!st) return;
  free(st->meta);
  for (size_t i = 0; st->buckets && i < st->cap; i++) {
    CsNode *n = st->buckets[i];
    while (n) {
      CsNode *next = n->next;
      free((void *)n->e.payload);
      free(n);
      n = next;
    }
  }
  free(st->buckets);
  free(st);
}

static bool parseHex(const char *s, uint64_t *out) {
  if (!*s) return false;
  char *end = NULL;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 16);
  if (errno != 0 || end == s || *end != '\0') return false;
  *out = (uint64_t)v;
  return true;
}

static bool parseLL(const char *s, long long *out) {
  if (!*s) return false;
  char *end = NULL;
  errno = 0;
  long long v = strtoll(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return false;
  *out = v;
  return true;
}

/* Baca satu baris ke buffer heap yang tumbuh — payload bisa puluhan KB
   (mis. daftar header edges). Return: false = gagal/terlalu panjang;
   *eof=true dan *out=NULL = sudah di akhir file; selain itu *out = baris
   tanpa '\n' (milik pemanggil, free()). */
#define CS_LINE_HARD_MAX (16u * 1024 * 1024)
static bool readLineGrow(FILE *fp, char **out, bool *eof) {
  *out = NULL;
  *eof = false;
  size_t cap = 256, len = 0;
  char *buf = malloc(cap);
  if (!buf) return false;
  for (;;) {
    if (!fgets(buf + len, (int)(cap - len), fp)) {
      if (ferror(fp)) {
        free(buf);
        return false;
      }
      if (len == 0) { /* EOF sejak awal */
        free(buf);
        *eof = true;
        return true;
      }
      break; /* baris terakhir tanpa '\n' */
    }
    len += strlen(buf + len);
    if (len > 0 && buf[len - 1] == '\n') {
      len--;
      if (len > 0 && buf[len - 1] == '\r') len--;
      break;
    }
    if (feof(fp)) break;
    if (cap >= CS_LINE_HARD_MAX) {
      free(buf);
      return false;
    }
    size_t ncap = cap * 2 > CS_LINE_HARD_MAX ? CS_LINE_HARD_MAX : cap * 2;
    char *nb = realloc(buf, ncap);
    if (!nb) {
      free(buf);
      return false;
    }
    buf = nb;
    cap = ncap;
  }
  buf[len] = '\0';
  *out = buf;
  return true;
}

/* Parse satu baris entri/P. Return: false = file rusak, true = sah. */
static bool parseLine(CsStore *st, const char *line, bool *haveP, uint64_t *curProj) {
  if (line[0] == '\0') return true;
  if (line[0] == 'P' && line[1] == ' ') {
    /* P <proj> [<content> <time> <size>] — metadata grup §4 (format baru);
       tanpa tiga kolom tambahan = grup lama (tanpa meta). */
    char tok[64];
    unsigned long long metaContent = 0;
    long long metaTime = 0, metaSize = 0;
    int used = 0;
    if (sscanf(line, "P %63s %16llx %lld %lld %n", tok, &metaContent, &metaTime, &metaSize,
               &used) >= 4 &&
        used > 0) {
      if (!parseHex(tok, curProj)) return false;
      bool loaded = false;
      if (!metaUpsert(st, *curProj, (uint64_t)metaContent, metaTime, metaSize, &loaded)) return false;
      *haveP = true;
      return true;
    }
    if (sscanf(line, "P %63s", tok) != 1 || !parseHex(tok, curProj)) return false;
    *haveP = true;
    return true;
  }

  char op[16], a[64], b[64], c[64], tt[64], sz[64];
  int consumed = 0;
  int got = sscanf(line, "%15s %63s %63s %63s %63s %63s%n", op, a, b, c, tt, sz, &consumed);
  if (got != 6 || !*haveP) return false;

  CsOp kind;
  if (strcmp(op, "GET") == 0) kind = CS_GET;
  else if (strcmp(op, "PUT") == 0) kind = CS_PUT;
  else if (strcmp(op, "PATCH") == 0) kind = CS_PATCH;
  else return false; /* OP tak dikenal (termasuk DELETE: entri dihapus, tidak ditulis) */

  CsEntry e = {0};
  if (!parseHex(a, &e.proj) || !parseHex(b, &e.file) || !parseHex(c, &e.content) ||
      !parseLL(tt, &e.time) || !parseLL(sz, &e.size))
    return false;
  if (e.proj != *curProj) return false; /* entri harus berada di bawah P-nya */
  if (findNode(st, e.proj, e.file)) return false; /* duplikat = file rusak */
  const char *pay = line + consumed;
  while (*pay == ' ') pay++;
  e.payload = *pay ? pay : NULL; /* disalin ke store di insertNew */
  return insertNew(st, &e, kind);
}

static bool loadFile(CsStore *st, FILE *fp) {
  bool haveP = false;
  uint64_t curProj = 0;
  for (;;) {
    char *line = NULL;
    bool eof = false;
    if (!readLineGrow(fp, &line, &eof)) return false;
    if (eof) break;
    bool ok = parseLine(st, line, &haveP, &curProj);
    free(line);
    if (!ok) return false;
  }
  return true;
}

CsStore *csOpen(const char *path) {
  CsStore *st = calloc(1, sizeof(CsStore));
  if (!st) return NULL;
  snprintf(st->path, sizeof(st->path), "%s", path);
  st->cap = CS_INIT_CAP;
  st->buckets = calloc(st->cap, sizeof(CsNode *));
  if (!st->buckets) {
    freeStore(st);
    return NULL;
  }
  struct stat sb;
  if (stat(path, &sb) != 0) return st; /* tidak ada = store kosong */
  FILE *fp = fopen(path, "r");
  if (!fp) {
    freeStore(st);
    return NULL;
  }
  bool ok = loadFile(st, fp);
  fclose(fp);
  if (!ok) {
    freeStore(st);
    return NULL;
  }
  return st;
}

void csClose(CsStore *st) { freeStore(st); }

bool csGet(const CsStore *st, uint64_t proj, uint64_t file, CsEntry *out) {
  CsNode *n = findNode(st, proj, file);
  if (!n) return false;
  if (out) *out = n->e;
  return true;
}

bool csPut(CsStore *st, const CsEntry *e) {
  CsNode *n = findNode(st, e->proj, e->file);
  if (n) {
    bool same = n->e.content == e->content && n->e.time == e->time && n->e.size == e->size &&
                ((!n->e.payload && !(e->payload && *e->payload)) ||
                 (n->e.payload && e->payload && strcmp(n->e.payload, e->payload) == 0));
    if (same) return true; /* nilai identik: tidak ada yang perlu ditulis ulang */
    char *dup = NULL;
    if (e->payload && *e->payload) {
      dup = strdup(e->payload);
      if (!dup) return false;
    }
    free((void *)n->e.payload);
    n->e = *e;
    n->e.payload = dup;
    n->op = CS_PUT;
    st->dirty = true;
    return true;
  }
  if (!insertNew(st, e, CS_PUT)) return false;
  st->dirty = true;
  return true;
}

bool csPatch(CsStore *st, uint64_t proj, uint64_t file, const CsPatch *p) {
  CsNode *n = findNode(st, proj, file);
  if (!n) return false;
  bool changed = false;
  if (p->hasContent && n->e.content != p->content) { n->e.content = p->content; changed = true; }
  if (p->hasTime && n->e.time != p->time) { n->e.time = p->time; changed = true; }
  if (p->hasSize && n->e.size != p->size) { n->e.size = p->size; changed = true; }
  if (p->hasPayload) {
    bool same = (!n->e.payload && !(p->payload && *p->payload)) ||
                (n->e.payload && p->payload && strcmp(n->e.payload, p->payload) == 0);
    if (!same) {
      char *dup = NULL;
      if (p->payload && *p->payload) {
        dup = strdup(p->payload);
        if (!dup) return false;
      }
      free((void *)n->e.payload);
      n->e.payload = dup;
      changed = true;
    }
  }
  if (!changed) return true;
  n->op = CS_PATCH;
  st->dirty = true;
  return true;
}

bool csDelete(CsStore *st, uint64_t proj, uint64_t file) {
  size_t b = bucketOf(st->cap, proj, file);
  CsNode **link = &st->buckets[b];
  while (*link) {
    CsNode *n = *link;
    if (n->e.proj == proj && n->e.file == file) {
      *link = n->next;
      free((void *)n->e.payload);
      free(n);
      st->count--;
      st->dirty = true;
      return true;
    }
    link = &n->next;
  }
  return false;
}

static int cmpNode(const void *a, const void *b) {
  const CsNode *x = *(CsNode *const *)a;
  const CsNode *y = *(CsNode *const *)b;
  if (x->e.proj != y->e.proj) return x->e.proj < y->e.proj ? -1 : 1;
  if (x->e.file != y->e.file) return x->e.file < y->e.file ? -1 : 1;
  return 0;
}

static const char *opName(CsOp op) {
  switch (op) {
    case CS_GET: return "GET";
    case CS_PATCH: return "PATCH";
    default: return "PUT";
  }
}

bool csCommit(CsStore *st) {
  if (!st->dirty) return true;
  CsNode **arr = malloc((st->count ? st->count : 1) * sizeof(CsNode *));
  if (!arr) return false;
  size_t k = 0;
  for (size_t i = 0; i < st->cap; i++)
    for (CsNode *n = st->buckets[i]; n; n = n->next) arr[k++] = n;
  qsort(arr, k, sizeof(CsNode *), cmpNode);

  char tmp[sizeof(st->path) + 8];
  snprintf(tmp, sizeof(tmp), "%s.tmp", st->path);
  FILE *fp = fopen(tmp, "w");
  if (!fp) {
    free(arr);
    return false;
  }
  bool ok = true;
  bool haveP = false;
  uint64_t curProj = 0;
  for (size_t i = 0; i < k && ok; i++) {
    const CsNode *n = arr[i];
    if (!haveP || n->e.proj != curProj) {
      const CsProjMeta *pm = metaFind(st, n->e.proj);
      if (pm)
        ok = fprintf(fp, "P %016llx %016llx %lld %lld\n", (unsigned long long)n->e.proj,
                     (unsigned long long)pm->content, pm->time, pm->size) > 0;
      else
        ok = fprintf(fp, "P %016llx\n", (unsigned long long)n->e.proj) > 0;
      curProj = n->e.proj;
      haveP = true;
    }
    if (ok) {
      const char *pay = n->e.payload && *n->e.payload ? n->e.payload : "";
      ok = fprintf(fp, "%s %016llx %016llx %016llx %lld %lld%s%s\n", opName(n->op),
                   (unsigned long long)n->e.proj, (unsigned long long)n->e.file,
                   (unsigned long long)n->e.content, n->e.time, n->e.size,
                   pay[0] ? " " : "", pay) > 0;
    }
  }
  free(arr);
  if (fclose(fp) != 0) ok = false;
  if (!ok) {
    remove(tmp);
    return false;
  }
  if (!csRenameReplace(tmp, st->path)) {
    remove(tmp);
    return false;
  }
  st->dirty = false;
  return true;
}

bool csIsDirty(const CsStore *st) { return st->dirty; }
size_t csCount(const CsStore *st) { return st->count; }

bool csForEach(const CsStore *st, CsVisitFn fn, void *ctx) {
  for (size_t i = 0; i < st->cap; i++)
    for (const CsNode *n = st->buckets[i]; n; n = n->next)
      if (!fn(&n->e, n->op, ctx)) return false;
  return true;
}

bool csRacy(long long entryTime, long long savedAt) { return entryTime >= savedAt; }

void csProjMetaSet(CsStore *st, uint64_t proj, uint64_t content, long long time,
                   long long size) {
  bool changed = false;
  if (!st || !metaUpsert(st, proj, content, time, size, &changed)) return;
  if (changed) st->dirty = true; /* hanya bila metadata grup benar-benar berubah */
}

bool csProjMetaGet(const CsStore *st, uint64_t proj, uint64_t *content, long long *time,
                   long long *size) {
  if (!st) return false;
  const CsProjMeta *m = metaFind(st, proj);
  if (!m) return false;
  if (content) *content = m->content;
  if (time) *time = m->time;
  if (size) *size = m->size;
  return true;
}

size_t csCountProj(const CsStore *st, uint64_t proj) {
  size_t k = 0;
  if (!st) return 0;
  for (size_t i = 0; i < st->cap; i++)
    for (const CsNode *n = st->buckets[i]; n; n = n->next)
      if (n->e.proj == proj) k++;
  return k;
}

long long csNowNs(void) {
#ifdef _WIN32
  return (long long)time(NULL) * 1000000000LL;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

bool csProjMetaStampFileN(const char *path, const uint64_t *projs,
                          const uint64_t *contents, const long long *times, int n) {
  if (n <= 0) return true;
  struct stat sb;
  if (stat(path, &sb) != 0) return true; /* belum ada: dilewati */
  CsStore *st = csOpen(path);
  if (!st) return false;
  bool ok = true;
  for (int i = 0; ok && i < n; i++)
    csProjMetaSet(st, projs[i], contents[i], times[i], (long long)csCountProj(st, projs[i]));
  ok = ok && csCommit(st);
  csClose(st);
  return ok;
}

bool csProjMetaStampFile(const char *path, uint64_t proj, uint64_t content, long long time) {
  return csProjMetaStampFileN(path, &proj, &content, &time, 1);
}

bool csProjMetaStampN(const uint64_t *projs, const uint64_t *contents,
                      const long long *times, int n) {
  static const char *const files[] = {
      ".satria/caches/deps.cache",
      ".satria/caches/embed.cache",
      ".satria/caches/fingerprint.cache",
      ".satria/caches/lib.cache",
  };
  bool ok = true;
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    if (!csProjMetaStampFileN(files[i], projs, contents, times, n)) ok = false;
  return ok;
}

bool csProjMetaStamp(uint64_t proj, uint64_t content, long long time) {
  return csProjMetaStampN(&proj, &content, &time, 1);
}

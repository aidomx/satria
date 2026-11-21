/* cache_store — lihat cache_store.h dan design/startup.md §4. */
#define _POSIX_C_SOURCE 200809L /* strdup (C11 murni tidak memilikinya) */
#include "cache_store.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
/* PID proses untuk nama tmp commit yang unik. unistd.h/getpid tidak ada di MSVC,
+   jadi cabang Windows memakai GetCurrentProcessId dari windows.h. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define CS_PID() ((long)GetCurrentProcessId())
#else
#include <sys/types.h> /* getpid: nama tmp commit unik per proses */
#include <unistd.h>   /* getpid */
#include <dirent.h>   /* stage cache: lihat file stage run aktif */
#define CS_PID() ((long)getpid())
#endif

#include <time.h> /* csNowNs: clock_gettime */

/* Stage tulis cache grup (eksperimen router-centric, P4): lihat cache_store.h. */
#define CS_STAGE_DIR ".satria/router-stage"
#define CS_STAGE_NAME_MAX 1600
static bool g_stageOn = false;
static bool g_stageMerging = false;
static uint64_t g_stageProj = 0;
static char g_stageToken[64];
static const char *csStageBase(const char *path);
static bool csStageOverlay(CsStore *st, const char *path);

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

/* Arena payload per store: payload entri (path, daftar header, token meta)
   dialokasikan sekaligus per blok dan dibebaskan bersama store. Sebelumnya
   tiap entri memakai strdup sendiri — pada cache 30 ribu baris itu puluhan
   ribu malloc/free per open, hotspot CPU ketiga pada profil 5k. Payload hanya
   valid selama store terbuka (kontrak csGet), jadi umur arena sama. */
typedef struct CsArena {
  struct CsArena *next;
  size_t used, cap;
  char data[];
} CsArena;

static char *arenaDup(CsStore *st, const char *src);

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
  CsArena *arena; /* pemilik semua payload entri */
};

#define CS_ARENA_BLOCK (64u * 1024)

static void *arenaAlloc(CsStore *st, size_t size) {
  size_t align = sizeof(void *); /* cukup untuk CsNode/CsArena: pointer & int64 */
  size_t len = (size + align - 1) & ~(align - 1);
  if (!st->arena || st->arena->used + len > st->arena->cap) {
    size_t cap = len > CS_ARENA_BLOCK ? len : CS_ARENA_BLOCK;
    CsArena *a = malloc(sizeof(*a) + cap);
    if (!a) return NULL;
    a->next = st->arena;
    a->used = 0;
    a->cap = cap;
    st->arena = a;
  }
  void *dst = st->arena->data + st->arena->used;
  st->arena->used += len;
  return dst;
}

static char *arenaDup(CsStore *st, const char *src) {
  size_t len = strlen(src) + 1;
  char *dst = arenaAlloc(st, len);
  if (!dst) return NULL;
  memcpy(dst, src, len);
  return dst;
}

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

/* Salin payload ke arena milik store (NULL/kosong -> NULL). */
static bool ownPayload(CsStore *st, CsEntry *dst, const char *src) {
  dst->payload = NULL;
  if (!src || !*src) return true;
  dst->payload = arenaDup(st, src);
  return dst->payload != NULL;
}

/* Sisipkan node baru (entri belum ada). Tumbuhkan tabel bila beban > 2. */
static bool insertNew(CsStore *st, const CsEntry *e, CsOp op) {
  if (st->count + 1 > st->cap * 2 && !rehash(st, st->cap * 2)) return false;
  CsNode *n = arenaAlloc(st, sizeof(CsNode));
  if (!n) return false;
  n->e = *e;
  if (!ownPayload(st, &n->e, e->payload)) return false;
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
  free(st->buckets);
  CsArena *a = st->arena;
  while (a) {
    CsArena *next = a->next;
    free(a);
    a = next;
  }
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
  const char *p = s;
  bool neg = false;
  if (*p == '-' || *p == '+') {
    neg = *p == '-';
    p++;
  }
  if (!*p) return false;
  /* Parser desimal langsung: strtoll dipanggil >1 juta kali per run (kolom
     time+size tiap entri) dan profil 5k menempatkannya di puncak setelah
     arena payload. Semantik sama: hanya digit, tanda di depan, tolak luapan. */
  unsigned long long v = 0;
  unsigned long long limit = neg ? (unsigned long long)LLONG_MAX + 1ULL
                                 : (unsigned long long)LLONG_MAX;
  for (; *p; p++) {
    unsigned d = (unsigned)(*p - '0');
    if (d > 9) return false;
    if (v > (limit - d) / 10ULL) return false;
    v = v * 10ULL + d;
  }
  *out = neg ? (long long)(0ULL - v) : (long long)v;
  return true;
}

/* Batas atas ukuran satu cache yang mau dimuat (payload entri bisa puluhan
   KB — mis. daftar header edges — jadi buffer baris tidak boleh tetap kecil). */
#define CS_LINE_HARD_MAX (16u * 1024 * 1024)

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
  /* Baca seluruh berkas sekali, lalu parse baris di tempat. Sebelumnya
     readLineGrow mengalokasi/membebaskan satu buffer per baris; pada cache
     30 ribu entri itu puluhan ribu malloc/free per open (profil CPU: csOpen
     ~29% waktu satria pada run incremental 5k). Cache selalu muat di memori:
     payload-nya sendiri sudah disalin ke store oleh insertNew. */
  size_t cap = 64 * 1024, len = 0;
  char *buf = malloc(cap + 1); /* +1: '\0' akhir tanpa perlu tumbuh lagi */
  if (!buf) return false;
  for (;;) {
    if (len == cap) {
      if (cap >= CS_LINE_HARD_MAX) {
        free(buf);
        return false;
      }
      size_t ncap = cap * 2 > CS_LINE_HARD_MAX ? CS_LINE_HARD_MAX : cap * 2;
      char *nb = realloc(buf, ncap + 1);
      if (!nb) {
        free(buf);
        return false;
      }
      buf = nb;
      cap = ncap;
    }
    size_t got = fread(buf + len, 1, cap - len, fp);
    len += got;
    if (got == 0) break;
  }
  if (ferror(fp)) {
    free(buf);
    return false;
  }
  buf[len] = '\0';

  bool haveP = false;
  uint64_t curProj = 0;
  char *p = buf, *end = buf + len;
  while (p < end) {
    char *nl = memchr(p, '\n', (size_t)(end - p));
    char *line = p;
    if (nl) {
      *nl = '\0';
      p = nl + 1;
    } else {
      p = end;
    }
    size_t l = strlen(line);
    if (l > 0 && line[l - 1] == '\r') line[l - 1] = '\0';
    if (!parseLine(st, line, &haveP, &curProj)) {
      free(buf);
      return false;
    }
  }
  free(buf);
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
  if (stat(path, &sb) != 0) { /* tidak ada = store kosong */
    if (!csStageOverlay(st, path)) {
      freeStore(st);
      return NULL;
    }
    return st;
  }
  FILE *fp = fopen(path, "r");
  if (!fp) {
    freeStore(st);
    return NULL;
  }
  bool ok = loadFile(st, fp);
  fclose(fp);
  if (ok) ok = csStageOverlay(st, path);
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
      dup = arenaDup(st, e->payload);
      if (!dup) return false;
    }
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
        dup = arenaDup(st, p->payload);
        if (!dup) return false;
      }
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
      *link = n->next; /* node milik arena: tidak dibebaskan per entri */
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

/* ==================== Stage tulis cache grup (P4) ==================== */

/* Hanya cache grup per proyek yang boleh di-stage. state.cache memakai
   mekanismenya sendiri (stateBlobExport), opts.cache ditangani intent router,
   artifact.cache tetap ditulis induk. */
static const char *csStageBase(const char *path) {
  static const char *const names[] = {
      "deps.cache", "lib.cache", "embed.cache", "fingerprint.cache",
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    if (strcmp(base, names[i]) == 0) return names[i];
  }
  return NULL;
}

/* Overlay baca stage: selama transaksi aktif, pembaca dalam proses yang sama
   (mis. cmdsFastStateSave yang membuka DepCache baru) harus melihat tulisan
   stage sebagai state terkini. Tanpa ini target belum berubah sampai merge,
   sehingga pembaca jatuh ke jalur lambat — pada cold single-project setiap
   source membaca ulang file .d karena edges di deps.cache belum terlihat.
   Saat merge (g_stageMerging) overlay dimatikan agar target ditulis apa adanya. */
static bool csStageOverlay(CsStore *st, const char *path) {
#ifndef _WIN32
  if (!g_stageOn || g_stageMerging || !g_stageProj) return true;
  const char *base = csStageBase(path);
  if (!base) return true;
  char stage[CS_STAGE_NAME_MAX];
  snprintf(stage, sizeof(stage), "%s/%s.%016llx.%s", CS_STAGE_DIR, g_stageToken,
           (unsigned long long)g_stageProj, base);
  struct stat sb;
  if (stat(stage, &sb) != 0) return true; /* belum ada tulisan stage */
  CsStore *src = csOpen(stage);
  if (!src) return true; /* stage rusak: biarkan target lama, merge akan melapor */

  uint64_t *keys = malloc((st->count ? st->count : 1) * sizeof(uint64_t));
  if (!keys) {
    csClose(src);
    return false;
  }
  size_t n = 0;
  for (size_t i = 0; i < st->cap; i++)
    for (CsNode *node = st->buckets[i]; node; node = node->next)
      if (node->e.proj == g_stageProj) keys[n++] = node->e.file;
  for (size_t i = 0; i < n; i++) csDelete(st, g_stageProj, keys[i]);
  free(keys);

  bool ok = true;
  for (size_t i = 0; i < src->cap && ok; i++)
    for (CsNode *node = src->buckets[i]; node && ok; node = node->next)
      if (node->e.proj == g_stageProj) ok = csPut(st, &node->e);
  uint64_t content = 0;
  long long time = 0, size = 0;
  if (ok && csProjMetaGet(src, g_stageProj, &content, &time, &size))
    csProjMetaSet(st, g_stageProj, content, time, size);
  csClose(src);
  return ok;
#else
  (void)st;
  (void)path;
  return true;
#endif
}

bool csStageBegin(void) {
#ifdef _WIN32
  return false; /* fork tidak tersedia; serial sudah satu penulis */
#else
  struct stat sb;
  if (stat(".satria", &sb) != 0) mkdir(".satria", 0755);
  if (stat(CS_STAGE_DIR, &sb) != 0) mkdir(CS_STAGE_DIR, 0755);
  snprintf(g_stageToken, sizeof(g_stageToken), "%ld-%lld", (long)getpid(), csNowNs());
  g_stageOn = true;
  g_stageMerging = false;
  g_stageProj = 0;
  /* Buang sisa run yang mati keras; file token run ini belum ada. */
  DIR *d = opendir(CS_STAGE_DIR);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (e->d_name[0] == '.') continue;
      char p[CS_STAGE_NAME_MAX];
      snprintf(p, sizeof(p), "%s/%s", CS_STAGE_DIR, e->d_name);
      remove(p);
    }
    closedir(d);
  }
  return true;
#endif
}

void csStageEnd(void) {
  g_stageOn = false;
  g_stageMerging = false;
  g_stageProj = 0;
  g_stageToken[0] = '\0';
}

bool csStageActive(void) { return g_stageOn; }
bool csStageMerging(void) { return g_stageMerging; }
void csStageSetMerging(bool on) { g_stageMerging = on; }
const char *csStageToken(void) { return g_stageToken; }

void csStageSetProject(const char *proj) {
  g_stageProj = (proj && *proj) ? csHash(proj) : 0;
}

void csStageSetProjectAddr(uint64_t proj) { g_stageProj = proj; }

static bool csStageWrite(CsStore *st, const char *base) {
  char path[CS_STAGE_NAME_MAX];
  snprintf(path, sizeof(path), "%s/%s.%016llx.%s", CS_STAGE_DIR, g_stageToken,
           (unsigned long long)g_stageProj, base);
  FILE *fp = fopen(path, "w");
  if (!fp) return false;
  bool ok = true;
  CsNode **arr = malloc((st->count ? st->count : 1) * sizeof(CsNode *));
  if (!arr) {
    fclose(fp);
    return false;
  }
  size_t k = 0;
  for (size_t i = 0; i < st->cap; i++)
    for (CsNode *n = st->buckets[i]; n; n = n->next)
      if (n->e.proj == g_stageProj) arr[k++] = n;
  qsort(arr, k, sizeof(CsNode *), cmpNode);
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
  if (!ok) remove(path);
  return ok;
}

/* Terapkan satu file stage ke target: ganti SELURUH grup proyek stage dengan
   isi file, termasuk metadata P. File stage kosong = hapus grup. */
typedef struct {
  CsStore *dst;
  uint64_t proj;
  bool ok;
} CsStageCopyCtx;
static bool csStageCopyVisit(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  CsStageCopyCtx *c = ctx;
  if (e->proj != c->proj) return true;
  if (!csPut(c->dst, e)) return c->ok = false;
  return true;
}

typedef struct {
  CsStore *dst;
  uint64_t proj;
} CsStageDelCtx;
static bool csStageDelVisit(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  CsStageDelCtx *c = ctx;
  if (e->proj != c->proj) return true;
  return csDelete(c->dst, e->proj, e->file);
}

static bool csStageApply(const char *target, const char *stage, uint64_t proj) {
  CsStore *src = csOpen(stage);
  if (!src) return false;
  CsStore *dst = csOpen(target);
  if (!dst) {
    remove(target);
    dst = csOpen(target);
  }
  if (!dst) {
    csClose(src);
    return false;
  }
  CsStageDelCtx del = {dst, proj};
  bool ok = csForEach(dst, csStageDelVisit, &del);
  CsStageCopyCtx cp = {dst, proj, true};
  if (ok) ok = csForEach(src, csStageCopyVisit, &cp);
  uint64_t content = 0;
  long long time = 0, size = 0;
  if (ok && csProjMetaGet(src, proj, &content, &time, &size))
    csProjMetaSet(dst, proj, content, time, size);
  if (ok) ok = csCommit(dst);
  csClose(dst);
  csClose(src);
  return ok;
}

#ifndef _WIN32
typedef struct {
  char *name;
  char base[32];
  uint64_t proj;
} CsStageFile;
static int csStageFileCmp(const void *a, const void *b) {
  const CsStageFile *x = a, *y = b;
  int c = strcmp(x->base, y->base);
  if (c) return c;
  return x->proj < y->proj ? -1 : x->proj > y->proj;
}
#endif

bool csStageMergeAll(void) {
#ifdef _WIN32
  return true;
#else
  if (!g_stageOn) return true;
  DIR *d = opendir(CS_STAGE_DIR);
  if (!d) return true; /* tidak ada stage = tidak ada yang digabung */
  size_t cap = 64, n = 0;
  CsStageFile *files = calloc(cap, sizeof(CsStageFile));
  if (!files) {
    closedir(d);
    return false;
  }
  size_t tokLen = strlen(g_stageToken);
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strncmp(de->d_name, g_stageToken, tokLen) != 0 || de->d_name[tokLen] != '.')
      continue;
    const char *p = de->d_name + tokLen + 1;
    char *end = NULL;
    uint64_t proj = strtoull(p, &end, 16);
    if (!end || end != p + 16 || *end != '.') continue;
    const char *base = end + 1;
    if (strlen(base) >= sizeof(files[0].base)) continue;
    if (n == cap) {
      size_t nc = cap * 2;
      CsStageFile *nr = realloc(files, nc * sizeof(CsStageFile));
      if (!nr) break;
      files = nr;
      cap = nc;
    }
    char full[CS_STAGE_NAME_MAX];
    snprintf(full, sizeof(full), "%s/%s", CS_STAGE_DIR, de->d_name);
    files[n].name = strdup(full);
    if (!files[n].name) continue;
    snprintf(files[n].base, sizeof(files[n].base), "%s", base);
    files[n].proj = proj;
    n++;
  }
  closedir(d);
  if (n > 1) qsort(files, n, sizeof(CsStageFile), csStageFileCmp);
  bool ok = true;
  g_stageMerging = true;
  for (size_t i = 0; i < n;) {
    size_t j = i + 1;
    while (j < n && strcmp(files[j].base, files[i].base) == 0) j++;
    char target[1024];
    snprintf(target, sizeof(target), ".satria/caches/%s", files[i].base);
    for (size_t k = i; k < j; k++)
      if (!csStageApply(target, files[k].name, files[k].proj)) ok = false;
    i = j;
  }
  g_stageMerging = false;
  for (size_t i = 0; i < n; i++) {
    remove(files[i].name);
    free(files[i].name);
  }
  free(files);
  return ok;
#endif
}

bool csCommit(CsStore *st) {
  if (!st->dirty) return true;
  if (g_stageOn && !g_stageMerging && g_stageProj) {
    const char *base = csStageBase(st->path);
    if (base) {
      bool ok = csStageWrite(st, base);
      if (ok) st->dirty = false;
      return ok;
    }
  }
  CsNode **arr = malloc((st->count ? st->count : 1) * sizeof(CsNode *));
  if (!arr) return false;
  size_t k = 0;
  for (size_t i = 0; i < st->cap; i++)
    for (CsNode *n = st->buckets[i]; n; n = n->next) arr[k++] = n;
  qsort(arr, k, sizeof(CsNode *), cmpNode);

  char tmp[sizeof(st->path) + 24];
  /* Nama tmp unik per proses: dua proses bisa meng-commit file yang sama
     hampir bersamaan (anak-anak fork pada akhir batch) — nama tetap membuat
     rename saling menimpa isi setengah jalan. */
 snprintf(tmp, sizeof(tmp), "%s.tmp%ld", st->path, CS_PID()); 
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
  /* Fase 2: stamp P grup ditulis router induk setelah merge stage. */
  if (g_stageOn && !g_stageMerging) return true;
  if (n <= 0) return true;
  struct stat sb;
  if (stat(path, &sb) != 0) return true; /* belum ada: dilewati */
  CsStore *st = csOpen(path);
  if (!st) return false;
  bool ok = true;
  for (int i = 0; i < n; i++) {
    uint64_t c0 = 0;
    long long t0 = 0, sz0 = 0;
    long long cnt = (long long)csCountProj(st, projs[i]);
    bool have = csProjMetaGet(st, projs[i], &c0, &t0, &sz0);
    /* content & size sudah cocok: jangan disentuh — `time` (stempel ns save)
       tidak pernah dibaca konsumen mana pun (satu-satunya pembaca baris P,
       gate no-op di ws_cache_noop, hanya membandingkan content+size). Tanpa
       jaga ini, 4 cache ditulis ulang penuh (open+commit+rename) tiap run. */
    if (have && c0 == contents[i] && sz0 == cnt) continue;
    /* Grup tanpa entri TIDAK mempunyai baris P (kontrak cache_store.h: "grup
       lahir bersama entri pertamanya") — csCommit memang tak pernah
       menuliskannya, jadi meta tak akan tersimpan dan store ikut kotor
       (dirty) tiap run. Lewati: stempel untuk grup kosong memang tak ada. */
    if (!have && cnt == 0) continue;
    csProjMetaSet(st, projs[i], contents[i], times[i], cnt);
  }
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

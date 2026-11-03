/*
 * deps_core — inti pelacak dependensi (dipindah apa adanya dari deps.c).
 *
 * Isi: hash konten file, normalisasi path, node cache + hash table path,
 * pemindai #include (fallback & MSVC), pemilih sumber edges per node
 * (file .d bila layak, else pemindai), dan DFS mtime terbaru di subtree.
 * Snapshot & file .d ada di deps_snapshot.c / deps_dotd.c.
 */
#include "deps_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../cmds/cmds_internal.h"
#include "../compile.h"
#include "../portability.h"
#include "../util.h"

uint64_t depComputeFileHash(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;

  uint64_t hash = 14695981039346656037ULL; // FNV offset basis
  unsigned char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    for (size_t i = 0; i < n; i++) {
      hash ^= buf[i];
      hash *= 1099511628211ULL; // FNV prime
    }
  }

  fclose(f);
  return hash;
}

/* ==================== path ==================== */

void depNormalizePath(char *p) {
  char *seg[128];
  int ns = 0;
  char *save = p;
  size_t len = strlen(p);
  char *tmp = malloc(len + 1);
  memcpy(tmp, p, len + 1);
  bool abs = tmp[0] == '/';

  for (char *tok = tmp; tok && *tok;) {
    char *slash = strchr(tok, '/');
    if (slash) *slash = '\0';
    if (strcmp(tok, ".") == 0 || *tok == '\0') {
      /* lewati */
    } else if (strcmp(tok, "..") == 0 && ns > 0 && strcmp(seg[ns - 1], "..") != 0) {
      ns--;
    } else if (ns < 128) {
      seg[ns++] = tok;
    }
    tok = slash ? slash + 1 : NULL;
  }

  char *w = save;
  if (abs) *w++ = '/';
  for (int i = 0; i < ns; i++) {
    size_t sl = strlen(seg[i]);
    memcpy(w, seg[i], sl);
    w += sl;
    if (i + 1 < ns) *w++ = '/';
  }
  *w = '\0';
  if (w == save) strcpy(save, ".");
  free(tmp);
}

void depJoinPath(char *out, size_t n, const char *dir, const char *name) {
  if (!dir || !*dir || strcmp(dir, ".") == 0)
    snprintf(out, n, "%s", name);
  else
    snprintf(out, n, "%s/%s", dir, name);
  depNormalizePath(out);
}

/* ==================== node cache ==================== */

uint64_t depPathHash(const char *s) {
  uint64_t h = 14695981039346656037ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return h;
}

static void hashRebuild(DepCache *dc, int ncap) {
  int *nb = calloc((size_t)ncap, sizeof(int));
  if (!nb) return;
  free(dc->hbucket);
  dc->hbucket = nb;
  dc->hbcap = ncap;

  int mask = ncap - 1;
  for (int idx = 0; idx < dc->count; idx++) {
    int i = (int)(depPathHash(dc->nodes[idx].path) & (uint64_t)mask);
    while (nb[i]) i = (i + 1) & mask;
    nb[i] = idx + 1;
  }
}

int depNodeFind(const DepCache *dc, const char *path) {
  if (!dc->hbcap) return -1;
  int mask = dc->hbcap - 1;
  int i = (int)(depPathHash(path) & (uint64_t)mask);
  for (;;) {
    int v = dc->hbucket[i];
    if (!v) return -1;
    int idx = v - 1;
    if (strcmp(dc->nodes[idx].path, path) == 0) return idx;
    i = (i + 1) & mask;
  }
}

int depNodeAddM(DepCache *dc, const char *path, int64_t mtime) {
  if (dc->count >= DEPS_MAX_NODES) return -1;
  if (dc->count == dc->cap) {
    int ncap = dc->cap ? dc->cap * 2 : 64;
    DepNode *nn = realloc(dc->nodes, (size_t)ncap * sizeof(DepNode));
    if (!nn) return -1;
    dc->nodes = nn;
    dc->cap = ncap;
  }
  DepNode *n = &dc->nodes[dc->count];
  memset(n, 0, sizeof(*n));
  n->path = strdup(path);
  n->mtime = mtime;
  int idx = dc->count++;

  /* daftarkan ke hash table; tumbuh saat load factor > 0.75 */
  if (!dc->hbcap || dc->count * 4 > dc->hbcap * 3) hashRebuild(dc, dc->hbcap ? dc->hbcap * 2 : 256);
  int mask = dc->hbcap - 1;
  int i = (int)(depPathHash(path) & (uint64_t)mask);
  while (dc->hbucket[i]) i = (i + 1) & mask;
  dc->hbucket[i] = idx + 1;

  return idx;
}

int64_t depNodeMTimeMemo(const char *path) {
  int64_t m = -1;
  long long sz = 0;
  if (cmdsStatCached(path, &m, &sz)) return m;
  return -1; /* stat gagal: sama dengan fsMTimeNs, tanpa stat kedua */
}

int depNodeAdd(DepCache *dc, const char *path) {
  return depNodeAddM(dc, path, depNodeMTimeMemo(path));
}

int depNodeGet(DepCache *dc, const char *path) {
  int i = depNodeFind(dc, path);
  return i >= 0 ? i : depNodeAdd(dc, path);
}

/* ==================== pemindai #include ==================== */

char *depReadWhole(const char *path, size_t *outLen) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  long long sz = fsFileSize(path);
  if (sz < 0) sz = 0;
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  buf[got] = '\0';
  *outLen = got;
  return buf;
}

/* Cari file yang di-include: "..." -> dir includer dulu, lalu -I; <...> -> -I
   saja. Return false bila bukan file project (header sistem). */
static bool resolveInclude(const DepCache *dc, const char *fromPath, const char *name, bool quoted,
                           char *out, size_t n) {
  if (quoted) {
    char dir[SATRIA_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", fromPath);
    char *slash = strrchr(dir, '/');
    if (slash)
      *slash = '\0';
    else
      strcpy(dir, ".");
    depJoinPath(out, n, dir, name);
    if (fsFileExists(out)) return true;
  }
  for (int i = 0; i < dc->incDirs.count; i++) {
    depJoinPath(out, n, dc->incDirs.items[i], name);
    if (fsFileExists(out)) return true;
  }
  return false;
}

void depKidAdd(DepNode *n, int kid) {
  for (int i = 0; i < n->nkids; i++)
    if (n->kids[i] == kid) return;
  int *nk = realloc(n->kids, (size_t)(n->nkids + 1) * sizeof(int));
  if (!nk) return;
  n->kids = nk;
  n->kids[n->nkids++] = kid;
}

/* Baca satu file, catat semua #include yang teresolusi sebagai anak node.
   Komentar blok, komentar baris, dan isi string literal tidak dianggap
   direktif. */
static void parseNode(DepCache *dc, int idx) {
  DepNode *node = &dc->nodes[idx];
  node->parsed = true;
  if (node->mtime < 0) return;

  size_t len = 0;
  char *buf = depReadWhole(node->path, &len);
  if (!buf) return;
  char *fromPath = strdup(node->path); /* nodes bisa realloc saat depNodeGet */

  bool inBlock = false;
  char *p = buf;
  while (*p) {
    char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);

    /* salin baris dengan komentar diganti spasi */
    char line[1024];
    size_t o = 0;
    for (size_t i = 0; i < ll && o < sizeof(line) - 1;) {
      if (inBlock) {
        if (p[i] == '*' && i + 1 < ll && p[i + 1] == '/') {
          inBlock = false;
          i += 2;
        } else {
          i++;
        }
        line[o++] = ' ';
      } else if (p[i] == '/' && i + 1 < ll && p[i + 1] == '*') {
        inBlock = true;
        i += 2;
        line[o++] = ' ';
      } else if (p[i] == '/' && i + 1 < ll && p[i + 1] == '/') {
        break;
      } else if (p[i] == '"') {
        /* string literal: salin apa adanya (bisa memuat pola komentar) */
        line[o++] = p[i++];
        while (i < ll && p[i] != '"' && o < sizeof(line) - 1) {
          if (p[i] == '\\' && i + 1 < ll) line[o++] = p[i++];
          line[o++] = p[i++];
        }
        if (i < ll && o < sizeof(line) - 1) line[o++] = p[i++];
      } else {
        line[o++] = p[i++];
      }
    }
    line[o] = '\0';

    const char *q = line;
    while (*q == ' ' || *q == '\t' || *q == '\r')
      q++;
    if (*q == '#') {
      q++;
      while (*q == ' ' || *q == '\t')
        q++;
      if (strncmp(q, "include", 7) == 0) {
        q += 7;
        if (strncmp(q, "_next", 5) == 0) q += 5;
        /* "#include_foo" bukan direktif include */
        if (*q != ' ' && *q != '\t' && *q != '"' && *q != '<') q = "";
        while (*q == ' ' || *q == '\t')
          q++;
        if (*q == '"' || *q == '<') {
          bool quoted = *q == '"';
          char close = quoted ? '"' : '>';
          const char *s = ++q;
          while (*q && *q != close)
            q++;
          if (*q == close && q > s) {
            char name[SATRIA_MAX_PATH];
            size_t nl = (size_t)(q - s);
            if (nl < sizeof(name)) {
              memcpy(name, s, nl);
              name[nl] = '\0';
              /* dir + '/' + name bisa sampai ~2*SATRIA_MAX_PATH — buffer diperbesar
                 agar depJoinPath tidak pernah memotong (-Wformat-truncation). */
              char resolved[SATRIA_MAX_PATH * 2];
              if (resolveInclude(dc, fromPath, name, quoted, resolved, sizeof(resolved))) {
                int kid = depNodeGet(dc, resolved);
                if (kid >= 0) depKidAdd(&dc->nodes[idx], kid);
              }
            }
          }
        }
      }
    }
    if (!eol) break;
    p = eol + 1;
  }
  free(fromPath);
  free(buf);
}

/* Node source (bukan header): satu-satunya yang punya file .d dari -MMD.
   MSVC tidak menulis .d — langsung pemindai #include. */
static bool dotdUsableSource(const DepCache *dc, const char *path) {
  if (!dc || !dc->cfg || compilerIsMSVC(dc->cfg)) return false;
  static const char *srcExt[] = {".c", ".C", ".cc", ".cpp", ".cxx", ".m", ".mm"};
  const char *dot = strrchr(path, '.');
  if (!dot) return false;
  for (size_t i = 0; i < sizeof(srcExt) / sizeof(srcExt[0]); i++)
    if (strcmp(dot, srcExt[i]) == 0) return true;
  return false;
}

void depParseNodeIfNeeded(DepCache *dc, int idx) {
  DepNode *node = &dc->nodes[idx];

  /* mtime sudah diambil tepat sekali saat node dibuat (depNodeAdd) — file
     tidak bisa berubah di tengah satu run satria, jadi stat ulang di sini
     (dan di depsLoadDotD) hanya pemborosan syscall, mahal di proot. */
  if (node->parsed) return;
  node->parsed = true;

  if (node->mtime < 0) return; /* file hilang */

  /* Prioritas: edges dari file .d compiler (lebih akurat dari pemindai —
     menangani #if, makro, computed include). Fallback: pemindai #include
     untuk build pertama, MSVC, atau .d yang belum ada/basi. */
  if (dotdUsableSource(dc, node->path) && depsLoadDotD(dc, node->path)) return;

  parseNode(dc, idx);
}

/* DFS iteratif-rekursif: mtime terbaru di subtree, tiap node sekali per
   source. Kids disalin dulu agar aman dari realloc dc->nodes. Dependensi
   HILANG (mtime -1) dikembalikan sebagai INT64_MAX agar source dipaksa
   stale: compiler menampilkan error include yang jelas, bukan build
   "sukses" di atas state yang tidak konsisten (semantik make -MP). */
int64_t depNewestUnder(DepCache *dc, int idx) {
  depParseNodeIfNeeded(dc, idx);
  dc->nodes[idx].mark = dc->gen;
  int64_t best = dc->nodes[idx].mtime;
  if (best < 0) return INT64_MAX; /* file hilang */

  int nkids = dc->nodes[idx].nkids;
  int *kids = malloc((size_t)(nkids > 0 ? nkids : 1) * sizeof(int));
  if (!kids) return best;
  memcpy(kids, dc->nodes[idx].kids, (size_t)nkids * sizeof(int));

  for (int i = 0; i < nkids; i++) {
    int kid = kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    int64_t m = depNewestUnder(dc, kid);
    if (m > best) best = m;
  }
  free(kids);
  return best;
}

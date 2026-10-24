/*
 * deps_dotd — dependensi dari file .d compiler + cache edges
 * (dipindah apa adanya dari deps.c).
 *
 * Bila object sudah pernah dikompilasi dengan -MMD (GCC/Clang), compiler
 * menulis daftar header yang benar-benar dipakai — menangani #if, makro,
 * computed include, hal yang dilampaui pemindai #include. File .d segar
 * (mtime >= source) dipakai sebagai sumber edges node source: flat tapi
 * lengkap, karena compiler yang meresolusi rekursi itu sendiri.
 *
 * Cache edges (entri ":edge:" di .satria/caches/deps.cache): run sebelumnya
 * menyimpan edges dari file .d yang dipakai. Run berikutnya memvalidasi satu
 * stat mtime .d: sama -> edges dipasang langsung, file .d tidak dibuka.
 * Berbeda/hilang -> entry diabaikan, edges dibangun ulang dari .d (atau
 * scanner) seperti biasa, lalu direkam ulang saat build sukses.
 */
#include "deps_internal.h"

#include <stdlib.h>
#include <string.h>

#include "../compile.h"
#include "../portability.h"
#include "../util.h"

/* Sisipkan satu path dependensi (sudah di-copy ke buffer) sebagai anak node
   idx — tanpa duplikat. depNodeGet bisa merealloc dc->nodes: akses selalu via
   idx, pointer diambil ulang SETELAH depNodeGet. */
static void dotdAddDep(DepCache *dc, int idx, const char *depPath) {
  if (idx < 0 || !depPath || !*depPath) return;
  char norm[MAX_PATH];
  snprintf(norm, sizeof(norm), "%s", depPath);
  depNormalizePath(norm);

  int kid = depNodeGet(dc, norm);
  if (kid < 0 || kid == idx) return;

  DepNode *n = &dc->nodes[idx];
  for (int i = 0; i < n->nkids; i++)
    if (n->kids[i] == kid) return;
  int *nk = realloc(n->kids, (size_t)(n->nkids + 1) * sizeof(int));
  if (!nk) return;
  n->kids = nk;
  n->kids[n->nkids++] = kid;
}

/* Ujung baris logis: backslash continuation digabung, newline lain tetap.
   Return menunjuk '\n' atau '\0'; backslash ada di e[-1] bila continuation. */
static const char *dotdLineEnd(const char *s) {
  const char *e = strchr(s, '\n');
  if (!e) return s + strlen(s);
  const char *q = e;
  while (q > s && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r'))
    q--;
  if (q > s && q[-1] == '\\') return q - 1;
  return e;
}

/*
 * Satu baris logis .d (fragment, backslash sudah dibuang):
 *   - baris pertama entry  : "target: dep dep ..."
 *   - baris continuation   : "dep dep ..."
 *   - phony -MP            : "path:" (entry baru, tanpa dependen)
 * Target .o (berakhiran .o/.obj) di-skip; sisanya dianggap dependensi.
 */
static void dotdProcessLine(DepCache *dc, int idx, char *line, bool *isTarget) {
  char *q = line;
  while (*q == ' ' || *q == '\t' || *q == '\r')
    q++;
  if (!*q || *q == '#') return; /* kosong / komentar */

  char *colon = strchr(q, ':');
  if (!colon) {
    if (!*isTarget) dotdAddDep(dc, idx, q); /* continuation */
    return;
  }

  /* Entry baru: teks sebelum ':' adalah target, sisanya dependensi. */
  *isTarget = true;

  /* Windows path "C:/..." atau "C:\..." — colon bagian drive, bukan
     pemisah target: geser ke colon berikutnya. */
  if ((colon - q) == 1 &&
      ((q[0] >= 'A' && q[0] <= 'Z') || (q[0] >= 'a' && q[0] <= 'z')) &&
      (colon[1] == '\\' || colon[1] == '/')) {
    char *real = strchr(colon + 1, ':');
    if (real) colon = real;
  }

  *colon = '\0';
  char *deps = colon + 1;

  size_t tl = strlen(q);
  /* NB: ".o" = 2 karakter → tl - 2. Offset lama (tl - 3) tidak pernah cocok,
     sehingga object ikut tercatat sebagai dependensi source-nya sendiri
     (stat ekstra per source + isi .o ikut di-hash ke snapshot). */
  if (tl >= 3 && q[tl - 2] == '.' && (q[tl - 1] == 'o' || q[tl - 1] == 'O')) {
    /* target object GNU/Clang: dependensi di baris yang sama tetap diproses */
    goto deps_of_line;
  }
  if (tl >= 4 && q[tl - 4] == '.' && (q[tl - 3] == 'o' || q[tl - 3] == 'O') &&
      (q[tl - 2] == 'b' || q[tl - 2] == 'B') && (q[tl - 1] == 'j' || q[tl - 1] == 'J'))
    goto deps_of_line; /* target object MSVC */

  dotdAddDep(dc, idx, q); /* target non-object: header phony hasil -MP dsb. */

deps_of_line:;
  /* dependensi di baris yang sama, setelah ':' */
  char *tok = deps;
  while (*tok) {
    while (*tok == ' ' || *tok == '\t' || *tok == '\r')
      tok++;
    if (!*tok) break;
    char *end = tok;
    while (*end && *end != ' ' && *end != '\t' && *end != '\r')
      end++;
    bool last = *end == '\0';
    *end = '\0';
    dotdAddDep(dc, idx, tok);
    if (last) break;
    tok = end + 1;
  }
}

bool depIsObjectPath(const char *p) {
  size_t n = strlen(p);
  if (n >= 3 && p[n - 2] == '.' && (p[n - 1] == 'o' || p[n - 1] == 'O')) return true;
  return n >= 5 && p[n - 4] == '.' && (p[n - 3] == 'o' || p[n - 3] == 'O') &&
         (p[n - 2] == 'b' || p[n - 2] == 'B') && (p[n - 1] == 'j' || p[n - 1] == 'J');
}

static int edgeFind(const DepCache *dc, const char *src) {
  if (!dc->ebcap) return -1;
  int mask = dc->ebcap - 1;
  int i = (int)(depPathHash(src) & (uint64_t)mask);
  for (;;) {
    int v = dc->ebucket[i];
    if (!v) return -1;
    int idx = v - 1;
    if (strcmp(dc->edges[idx].src, src) == 0) return idx;
    i = (i + 1) & mask;
  }
}

void depEdgesHashBuild(DepCache *dc) {
  int ncap = 256;
  while (ncap < dc->edgeCount * 2)
    ncap *= 2;
  int *nb = calloc((size_t)ncap, sizeof(int));
  if (!nb) return;
  free(dc->ebucket);
  dc->ebucket = nb;
  dc->ebcap = ncap;
  int mask = ncap - 1;
  for (int k = 0; k < dc->edgeCount; k++) {
    int j = (int)(depPathHash(dc->edges[k].src) & (uint64_t)mask);
    while (nb[j]) j = (j + 1) & mask;
    nb[j] = k + 1;
  }
}

/* Entri edges dimuat oleh depSnapshotLoad dalam pass yang sama (satu buka
   file per run). Fungsi ini hanya memastikan load sudah terjadi. */
static void edgesLoad(DepCache *dc) {
  if (dc->edgesLoaded) return;
  depSnapshotLoad(dc);
}

/* Catat edges source idx (dari .d yang barusan diparse) untuk run berikutnya. */
static void edgesRecord(DepCache *dc, int idx, int64_t dm) {
  const char *src = dc->nodes[idx].path;
  int ei = edgeFind(dc, src);
  if (ei < 0) {
    if (dc->edgeCount == dc->edgeCap) {
      int ncap = dc->edgeCap ? dc->edgeCap * 2 : 128;
      void *nn = realloc(dc->edges, (size_t)ncap * sizeof(*dc->edges));
      if (!nn) return;
      dc->edges = nn;
      dc->edgeCap = ncap;
      /* hash table edges dibangun ulang saat grow */
    }
    /* hash table grow saat load > 0.75 */
    if (!dc->ebcap || (dc->edgeCount + 1) * 4 > dc->ebcap * 3) {
      int ncap = dc->ebcap ? dc->ebcap * 2 : 256;
      int *nb = calloc((size_t)ncap, sizeof(int));
      if (nb) {
        free(dc->ebucket);
        dc->ebucket = nb;
        dc->ebcap = ncap;
        int mask = ncap - 1;
        for (int k = 0; k < dc->edgeCount; k++) {
          int j = (int)(depPathHash(dc->edges[k].src) & (uint64_t)mask);
          while (nb[j]) j = (j + 1) & mask;
          nb[j] = k + 1;
        }
      }
    }
    ei = dc->edgeCount++;
    dc->edges[ei].src = strdup(src);
    dc->edges[ei].kids = NULL;
    dc->edges[ei].nkids = 0;
    dc->edges[ei].expect = -1;
    dc->edges[ei].dm = 0;
    if (dc->ebcap) {
      int mask = dc->ebcap - 1;
      int j = (int)(depPathHash(src) & (uint64_t)mask);
      while (dc->ebucket[j]) j = (j + 1) & mask;
      dc->ebucket[j] = ei + 1;
    }
  }

  /* bandingkan isi: bila sama & mtime .d sama, tak ada yang berubah */
  bool changed = dc->edges[ei].dm != dm || dc->edges[ei].nkids != dc->nodes[idx].nkids;
  if (!changed) {
    for (int k = 0; k < dc->nodes[idx].nkids && !changed; k++) {
      const char *a = dc->edges[ei].kids[k];
      const char *b = dc->nodes[dc->nodes[idx].kids[k]].path;
      if (!a || strcmp(a, b) != 0) changed = true;
    }
  }
  if (!changed) return;

  for (int k = 0; k < dc->edges[ei].nkids; k++)
    free(dc->edges[ei].kids[k]);
  dc->edges[ei].nkids = 0;
  for (int k = 0; k < dc->nodes[idx].nkids; k++) {
    char **nk = realloc(dc->edges[ei].kids, (size_t)(k + 1) * sizeof(char *));
    if (!nk) return;
    dc->edges[ei].kids = nk;
    dc->edges[ei].kids[k] = strdup(dc->nodes[dc->nodes[idx].kids[k]].path);
    if (!dc->edges[ei].kids[k]) return;
    dc->edges[ei].nkids = k + 1;
  }
  dc->edges[ei].dm = dm;
  dc->edges[ei].expect = -1;
  dc->edges[ei].used = true;
  dc->edgeDirty = true;
}

/* Penulisan edges kini lewat depsSave (deps_snapshot.c): payload
   ":edge:<src>\x01<kids>" di caches/deps.cache. Entry yang tak terpakai
   tidak ikut disimpan — cache edges ter-pangkas otomatis tiap tulis. */

bool depsLoadDotD(DepCache *dc, const char *src) {
  if (!dc || !src) return false;

  const Config *c = dc->cfg;
  if (!c || c->sources.count == 0) return false;

  char dpath[MAX_PATH * 2];
  if (!dotDPathFor(c, src, dpath, sizeof(dpath))) return false;

  int64_t dm = fsMTimeNs(dpath);
  /* .d basi (source lebih baru — kompilasi berikutnya akan menulis ulang)
     atau .d hilang -> return false, pemanggil jatuh ke pemindai #include.
     mtime source tidak di-stat ulang: pemanggil (depParseNodeIfNeeded) sudah
     punya node-nya — ambil via hash table, satu strcmp saja. */
  int sidx = depNodeFind(dc, src);
  int64_t sm = sidx >= 0 ? dc->nodes[sidx].mtime : -1;
  if (dm < 0 || sm < 0 || dm < sm) return false;

  /* Salin path source dulu: depNodeGet bisa merealloc dc->nodes. */
  char srcNorm[MAX_PATH];
  snprintf(srcNorm, sizeof(srcNorm), "%s", src);
  depNormalizePath(srcNorm);
  int idx = depNodeGet(dc, srcNorm);
  if (idx < 0) return false;

  dc->nodes[idx].parsed = true;  /* edges .d dipakai; scanner tidak perlu */
  dc->nodes[idx].dloaded = true; /* flat-lengkap: anak tak perlu dipindai */
  dc->nodes[idx].hash = 0;       /* hash source dihitung ulang bila perlu */

  /* Cache edges run sebelumnya: mtime .d sama -> edges dipasang langsung,
     file .d TIDAK dibuka (satu stat menggantikan open+read per source). */
  edgesLoad(dc);
  int ei = edgeFind(dc, srcNorm);
  if (ei >= 0 && dc->edges[ei].dm == dm &&
      (dc->edges[ei].expect >= 0 ? dc->edges[ei].nkids == dc->edges[ei].expect
                                 : dc->edges[ei].nkids > 0)) {
    /* cache versi lama menyimpan object sebagai "dependensi" (bug offset .o):
       buang di tempat; cache ditulis ulang sekali, bersih. */
    int w = 0;
    for (int k = 0; k < dc->edges[ei].nkids; k++) {
      if (depIsObjectPath(dc->edges[ei].kids[k])) {
        free(dc->edges[ei].kids[k]);
        dc->edgeDirty = true;
        continue;
      }
      dc->edges[ei].kids[w++] = dc->edges[ei].kids[k];
    }
    dc->edges[ei].nkids = w;
    for (int k = 0; k < dc->edges[ei].nkids; k++) {
      int kid = depNodeGet(dc, dc->edges[ei].kids[k]);
      if (kid < 0 || kid == idx) continue;
      depKidAdd(&dc->nodes[idx], kid);
    }
    dc->edges[ei].used = true;
    return true;
  }
  dc->snapIncomplete = true; /* cache meleset — state .d berubah sejak
                                rekaman terakhir: paksa pass rekam */

  size_t len = 0;
  char *buf = depReadWhole(dpath, &len);
  if (!buf) return false;

  /* Gabungkan continuation backslash, proses tiap baris logis. */
  char line[DOTD_MAX_LINE];
  size_t lo = 0;
  const char *p = buf;
  bool isTarget = false;
  for (;;) {
    const char *e = dotdLineEnd(p);
    size_t seg = (size_t)(e - p);
    if (lo + seg > sizeof(line) - 1)
      seg = lo < sizeof(line) - 1 ? sizeof(line) - 1 - lo : 0;
    memcpy(line + lo, p, seg);
    lo += seg;

    if (*e == '\0') break; /* EOF: baris terakhir tanpa '\n' ditangani di bawah */
    if (*e == '\\') {
      p = e + 1; /* buang backslash; '\n' menyusul — baris logis berlanjut */
      continue;
    }

    line[lo] = '\0';
    dotdProcessLine(dc, idx, line, &isTarget);
    lo = 0;
    p = e + 1;
  }
  if (lo > 0) {
    line[lo] = '\0';
    dotdProcessLine(dc, idx, line, &isTarget);
  }

  free(buf);
  edgesRecord(dc, idx, dm);
  return true;
}

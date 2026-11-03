/*
 * cmds_wsstate — fast-state khusus mode workspace. Terpisah dari
 * cmdsFastStateSave/cmdsFastStateValid (yang tetap untuk single project).
 *
 * Penyimpanan: blob per proyek di .satria/caches/state.cache (leaf
 *       `ws.state` fase binary, `src` snapshot sumber, hdr/<hash> semantic header)
 *       dan .satria/caches/lib.cache (fase library) — mode workspace
 *       root-centric (state_path, lihat cmds_internal.h). File lama
 *       .satria/workspace/<proyek>.ws.state[.lib] dan .satria/ws.state tidak
 *       lagi dibaca/ditulis (dibuang legacy sweep).
 * Format (whitespace-delimited, path tanpa spasi):
 *   SATRIAWS3 <cwd>
 *   META <skipped> <target|-> <buildfile>
 *   LIB <path>                 library yang diminta (dicetak saat no-op)
 *   F <mtime_ns> <size> <path> input/output yang dipakai (source, object, closure header,
 *                              target, library, archive embed, ld.cache, Buildfile)
 *   D <mtime_ns> 0 <path>      folder header: penambahan/penghapusan mengubah mtime-nya
 *   X 0 0 <path>               path yang tidak ada saat disimpan (harus tetap tidak ada)
 */
#include "../log/build_report.h" /* buildReportActive: blok lama per proyek dijaga */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compile.h"
#include "../deps.h"
#include "../embed_ld_cache.h"
#include "../portability.h"
#include "../util.h"
#include "cmds_internal.h"
#include "../prof/prof.h" /* profMark: audit overhead ws-faststate */
#include "../cache/state_blob.h"
#include <stdarg.h>

#define WS_STATE_MAGIC "SATRIAWS5"

static void addKind(List *l, char kind, const char *path) {
  if (!path || !*path) return;
  char entry[SATRIA_MAX_PATH * 4];
  snprintf(entry, sizeof(entry), "%c %s", kind, path);
  listAdd(l, entry);
}

typedef struct {
  List *items;
} ClosureCtx;

static void closureCollect(const char *path, void *ctx) {
  addKind(((ClosureCtx *)ctx)->items, 'F', path);
}

static int cmpEntry(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

void wsAddSourceDirs(List *items);


static bool hasSpace(const char *s) { return s && strpbrk(s, " \t\r\n") != NULL; }

/* Tambah teks format ke buffer dinamis (dipakai penulis state blob). */
static void stAppendf(char **buf, size_t *used, size_t *cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) return;
  if (*used + (size_t)need + 1 > *cap) {
    size_t nc = (*used + (size_t)need + 1) * 2;
    if (nc < 256) nc = 256;
    char *nb = realloc(*buf, nc);
    if (!nb) return;
    *buf = nb;
    *cap = nc;
  }
  va_start(ap, fmt);
  vsnprintf(*buf + *used, *cap - *used, fmt, ap);
  va_end(ap);
  *used += (size_t)need;
}


/* Snapshot daftar source per proyek (design/workspace_v1.md §State):
   blob `src` di .satria/caches/state.cache (dulu .satria/workspace/<proyek>.src)
   — dipakai workspace_log sebagai fingerprint
   daftar source (keputusan no-op & skip-write CompDB). Ditulis hanya bila
   isinya berubah; tanpa proyek aktif (jalur lama) dilewati. */
static void wsWriteSrcSnapshot(const List *srcs) {
  const char *proj = cmdsStateWsProject();
  if (!proj || !srcs) return;
  char cwd[WS_PATH_LEN];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  char *buf = NULL;
  size_t used = 0, cap = 0;
  stAppendf(&buf, &used, &cap, "SATRIAWSLIST1 %s\n", cwd);
  for (int i = 0; i < srcs->count; i++) stAppendf(&buf, &used, &cap, "S %s\n", srcs->items[i]);
  char *old = NULL;
  size_t oldn = 0;
  bool same = stateBlobGet(proj, "src", &old, &oldn) && oldn == used && (used == 0 || memcmp(old, buf, used) == 0);
  free(old);
  if (!same) stateBlobPut(proj, "src", buf ? buf : "", used); /* ditulis hanya bila berubah */
  free(buf);
}

/* ===== Reuse hash isi entri fast-state (audit 30/10 — kandidat 4) =========
   wsFastStateSave dulu membaca + hash ULANG seluruh sumber dan closure
   header tiap build hanya untuk menulis kolom content di baris F (1.510 ms
   di workspace 1.003 objek — setelah kandidat 1, bucket terbesar sisa).
   Validasi tidak membutuhkan itu: baris F dianggap segar TANPA hash selama
   (mtime,size) cocok dengan disk (cmdsStateEntryFresh), jadi stamp yang tak
   berubah = isi tak mungkin berubah -> pakai ulang hash tersimpan. Entri yang
   stamp-nya berubah (sumber/header yang disentuh run ini) tetap di-hash ulang
   seperti biasa — bukti sentuh tetap akurat. */
typedef struct {
  char *path;
  long long mtime, size;
  unsigned long long content;
} FsPrevEnt;

typedef struct {
  FsPrevEnt *v;
  size_t n, cap; /* cap pangkat dua; v[i].path == NULL = slot kosong */
} FsPrevTable;

static size_t fsPrevHash(const char *s) {
  size_t h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return h;
}

static bool fsPrevInit(FsPrevTable *t, size_t n) {
  size_t cap = 64;
  while (cap < n * 2 + 16) cap <<= 1;
  t->v = (FsPrevEnt *)calloc(cap, sizeof(FsPrevEnt));
  t->n = 0;
  t->cap = t->v ? cap : 0;
  return t->v != NULL;
}

static void fsPrevFree(FsPrevTable *t) {
  if (t->v)
    for (size_t i = 0; i < t->cap; i++) free(t->v[i].path);
  free(t->v);
  t->v = NULL;
  t->n = t->cap = 0;
}

static void fsPrevGrow(FsPrevTable *t) {
  size_t ncap = t->cap * 2;
  FsPrevEnt *nv = (FsPrevEnt *)calloc(ncap, sizeof(FsPrevEnt));
  if (!nv) return; /* gagal: tabel tetap terpakai apa adanya (tanpa tambah) */
  size_t mask = ncap - 1;
  for (size_t i = 0; i < t->cap; i++) {
    if (!t->v[i].path) continue;
    size_t j = fsPrevHash(t->v[i].path) & mask;
    while (nv[j].path) j = (j + 1) & mask;
    nv[j] = t->v[i];
  }
  free(t->v);
  t->v = nv;
  t->cap = ncap;
}

static void fsPrevAdd(FsPrevTable *t, const char *path, long long mt, long long sz,
                      unsigned long long content) {
  if (!t->cap || !path || !*path) return;
  if ((t->n + 1) * 4 > t->cap * 3) fsPrevGrow(t);
  size_t mask = t->cap - 1;
  size_t j = fsPrevHash(path) & mask;
  while (t->v[j].path) {
    if (strcmp(t->v[j].path, path) == 0) return; /* pertama menang */
    j = (j + 1) & mask;
  }
  char *dup = strdup(path);
  if (!dup) return;
  t->v[j].path = dup;
  t->v[j].mtime = mt;
  t->v[j].size = sz;
  t->v[j].content = content;
  t->n++;
}

static bool fsPrevFind(const FsPrevTable *t, const char *path, long long *mt, long long *sz,
                       unsigned long long *content) {
  if (!t->cap || !path) return false;
  size_t mask = t->cap - 1;
  size_t j = fsPrevHash(path) & mask;
  while (t->v[j].path) {
    if (strcmp(t->v[j].path, path) == 0) {
      *mt = t->v[j].mtime;
      *sz = t->v[j].size;
      *content = t->v[j].content;
      return true;
    }
    j = (j + 1) & mask;
  }
  return false;
}

/* Hash isi: pakai ulang bila stamp (mtime,size) sama dengan catatan lama dan
   catatan itu memuat hash; selain itu hash segar seperti jalur lama. */
static uint64_t fsPrevContent(const FsPrevTable *t, const char *path, long long mt,
                              long long sz) {
  long long om = 0, os = 0;
  unsigned long long oc = 0;
  if (fsPrevFind(t, path, &om, &os, &oc) && om == mt && os == sz && oc)
    return (uint64_t)oc;
  return cmdsStateFileHash(path);
}

/* Muat entri F/I fase library (caches/lib.cache) / fase binary (ws.state). */
static void fsPrevLoadLib(FsPrevTable *t) {
  LibCacheMeta om;
  LibStampRec *ost = NULL;
  int on = 0;
  if (!libCacheLoad(&om, &ost, &on)) return;
  fsPrevInit(t, (size_t)(on > 0 ? on : 0));
  for (int i = 0; i < on; i++)
    if (ost[i].kind == 'F')
      fsPrevAdd(t, ost[i].path, ost[i].mtime, ost[i].size, ost[i].content);
  free(ost);
  libCacheMetaFree(&om);
}

static void fsPrevLoadWsState(FsPrevTable *t, const char *proj) {
  char *tb = NULL;
  size_t tn = 0, tp = 0, lines = 0;
  if (!proj || !stateBlobGet(proj, "ws.state", &tb, &tn)) return;
  for (size_t i = 0; i < tn; i++)
    if (tb[i] == '\n') lines++;
  fsPrevInit(t, lines);
  char line[SATRIA_MAX_PATH * 6];
  while (stateBufGets(tb, tn, &tp, line, sizeof(line))) {
    char k = 0;
    long long mt = 0, sz = 0;
    unsigned long long ct = 0;
    char p[WS_PATH_LEN];
    if (!cmdsParseStateEntry(line, &k, &mt, &sz, &ct, p)) continue;
    if (k == 'F' || k == 'I') fsPrevAdd(t, p, mt, sz, ct);
  }
  free(tb);
}

void wsFastStateSave(struct DepCache *dc, const Config *c, const List *srcs, const char *target,
                     const char *buildfilePath, bool libOnly) {
  if (!dc || !c || !srcs) return;
  char cwd[WS_PATH_LEN];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  cmdsStatMemoSetCwd();
  const char *bf = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  const char *tgt = (!libOnly && target && *target) ? target : "-";
  if (hasSpace(cwd) || hasSpace(bf) || hasSpace(tgt)) return; /* format whitespace-delimited */
  wsWriteSrcSnapshot(srcs); /* snapshot daftar source utk workspace_log */

  List items = {0};
  addKind(&items, 'F', bf);
  if (strcmp(tgt, "-") != 0) addKind(&items, 'O', tgt);

  /* Library yang diminta: dicatat sebagai input (berubah -> rebuild) dan
     sebagai LIB agar status up-to-date dicetak saat no-op. */
  List libs = {0};
  char staticLib[WS_PATH_LEN] = "", sharedLib[WS_PATH_LEN] = "";
  if (c->libRequested) {
    if (c->libStatic) cmdsLibStaticPath(c, staticLib, sizeof(staticLib));
    if (c->libShared) cmdsLibSharedPath(c, sharedLib, sizeof(sharedLib));
  }
  if (staticLib[0]) {
    addKind(&items, 'O', staticLib);
    addKind(&libs, 'L', staticLib);
  }
  if (sharedLib[0]) {
    addKind(&items, 'O', sharedLib);
    addKind(&libs, 'L', sharedLib);
  }

  ClosureCtx ctx = {&items};
  for (int i = 0; i < srcs->count; i++) {
    const char *src = srcs->items[i];
    addKind(&items, 'F', src);
    char obj[WS_PATH_LEN];
    if (objectPathFor(c, src, obj, sizeof(obj))) addKind(&items, 'O', obj);
  }

  /* Union closure: header bersama antar-source dikunjungi sekali saja. */
  depsClosureVisitMany(dc, (const char *const *)srcs->items, srcs->count, closureCollect, &ctx);

  /* Embedded: arsip (input) + header hasil codegen. Object embed kini
     object source biasa — sudah tercatat lewat loop srcs di atas ('F' untuk
     embedded.c, 'O' untuk embedded.o). Verdict ld tidak lagi relevan:
     codegen adalah satu-satunya jalur embed (design/embedded.md). */
  if (c->embCount > 0) {
    char embPath[WS_PATH_LEN + 40];
    snprintf(embPath, sizeof(embPath), "%s/embedded.h", c->outBuildDir);
    addKind(&items, 'F', embPath);
  }
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    if (c->emb[i].archivePath[0]) addKind(&items, 'O', c->emb[i].archivePath);
  }

  /* Library path-form (cdeps lintas-proyek: `library = <dep>/lib/x.a`) —
     input link fase binary: perubahan .a proyek lain harus membatalkan
     fast state agar relink terdeteksi (audit ws-benchmark 2026-10-15).
     Hanya fase binary: .a proyek lain bukan input fase library (kemasan
     hanya dari object milik proyek sendiri). */
  if (!libOnly)
    for (int i = 0; i < c->libraries.count; i++) {
      const char *lib = c->libraries.items[i];
      if (strchr(lib, '/') || strchr(lib, '\\')) addKind(&items, 'I', lib);
    }

  /* Folder sumber: setiap folder yang ditelusuri daftar sumber (termasuk
     subfolder) dicatat sebagai stamp direktori. Penambahan/penghapusan file
     .c/.cpp mengubah mtime folder, sehingga fast path tidak melewatkannya. */
  wsAddSourceDirs(&items);

  /* Jangan jadikan direktori header sebagai input proyek secara keseluruhan.
     Stamp direktori membuat penambahan/penghapusan header membatalkan state
     setiap proyek yang sekadar memiliki -I ke direktori itu, sekalipun tak ada
     translation unit yang mengonsumsi header tersebut. Konsumsi header yang
     nyata dilacak melalui depsClosureVisitMany() (edge dari depfile .d, dengan
     fallback scanner saat depfile belum tersedia); perubahan/hilangnya header
     yang benar-benar dipakai tetap membatalkan konsumen yang bersangkutan.
     Folder stamp hanya tepat untuk discovery source, karena penambahan source
     memang mengubah himpunan translation unit proyek. */

  /* Urutkan dan hilangkan duplikat sekali, sebelum pemeriksaan output/stat.
     Closure dari source berbeda sering memuat header yang sama. */
  if (items.count > 1) {
    qsort(items.items, (size_t)items.count, sizeof(char *), cmpEntry);
    int unique = 0;
    for (int i = 0; i < items.count; i++) {
      if (unique && strcmp(items.items[i], items.items[unique - 1]) == 0) {
        free(items.items[i]);
      } else {
        items.items[unique++] = items.items[i];
      }
    }
    items.count = unique;
  }

  /* Pre-pass: output dan folder build/ dibuang dari memo sebelum ada pembacaan.
     Dengan begitu input yang dibaca dari memo tidak memuat nilai pra-build. */
  cmdsStatMemoForgetOutputs(c->outBuildDir, &items);

  /* Fase library: simpan ke caches/lib.cache (design/startup.md §3) —
     entri meta + stamp terpusat per proyek, bukan .ws.lib.state per proyek. */
  if (libOnly) {
    LibCacheMeta m;
    memset(&m, 0, sizeof(m));
    snprintf(m.cwd, sizeof(m.cwd), "%s", cwd);
    snprintf(m.buildfile, sizeof(m.buildfile), "%s", bf);
    m.bfm = (long long)fsMTimeNs(bf);
    m.bfs = fsFileSize(bf);
    snprintf(m.target, sizeof(m.target), "%s", tgt); /* "-" bila tanpa target */
    m.skipped = srcs->count;
    m.libRequested = c->libRequested;
    snprintf(m.libStaticPath, sizeof(m.libStaticPath), "%s", staticLib);
    snprintf(m.libSharedPath, sizeof(m.libSharedPath), "%s", sharedLib);
    if (libs.count > 0) {
      m.libs = calloc((size_t)libs.count, sizeof(char *));
      if (m.libs) {
        for (int i = 0; i < libs.count; i++) m.libs[i] = strdup(libs.items[i] + 2);
        m.nlibs = libs.count;
      }
    }
    LibStampRec *recs = calloc(items.count ? (size_t)items.count : 1, sizeof(LibStampRec));
    if (recs) {
      FsPrevTable prev = {0};
      fsPrevLoadLib(&prev); /* stamp lama: sumber/header tak tersentuh tanpa hash ulang */
      int nrec = 0;
      for (int i = 0; i < items.count; i++) {
        char kind = items.items[i][0];
        const char *p = items.items[i] + 2;
        int64_t mm = 0;
        long long sz = 0;
        size_t plen = strlen(p);
        bool isObj = plen >= 2 && strcmp(p + plen - 2, ".o") == 0;
        bool exists = kind == 'O' ? (isObj ? memoStampNsSize(p, &mm, &sz) : fsStampNsSize(p, &mm, &sz))
                                  : cmdsStatCached(p, &mm, &sz);
        recs[nrec].kind = !exists ? 'M' : (kind == 'D' ? 'D' : 'F');
        snprintf(recs[nrec].path, sizeof(recs[nrec].path), "%s", p);
        recs[nrec].mtime = (long long)mm;
        recs[nrec].size = sz;
        recs[nrec].content =
            recs[nrec].kind == 'F' ? fsPrevContent(&prev, p, (long long)mm, sz) : 0;
        nrec++;
      }
      fsPrevFree(&prev);
      libCacheSave(&m, recs, nrec);
      free(recs);
    }
    libCacheMetaFree(&m);
    listFree(&items);
    listFree(&libs);
    optsRouterRecord(c, libOnly); /* router: deps + lib (fase library) */
    return;
  }

  const char *proj = cmdsStateWsProject();
  if (!proj) {
    listFree(&items);
    listFree(&libs);
    return;
  }
  FsPrevTable prev = {0};
  fsPrevLoadWsState(&prev, proj); /* stamp lama ws.state: hindari hash ulang */
  char *ob = NULL;
  size_t ou = 0, oc = 0;
  stAppendf(&ob, &ou, &oc, "%s %s\n", WS_STATE_MAGIC, cwd);
  stAppendf(&ob, &ou, &oc, "META %d %s %s\n", srcs->count, tgt, bf);
  for (int i = 0; i < libs.count; i++) stAppendf(&ob, &ou, &oc, "LIB %s\n", libs.items[i] + 2);

  for (int i = 0; i < items.count; i++) {
    char kind = items.items[i][0];
    const char *p = items.items[i] + 2;
    int64_t m = 0;
    long long sz = 0;
    /* Output baru dibangun: stat langsung. Input (source, header, folder)
       tidak berubah selama build, jadi boleh dari memo. */
    /* Output: stat langsung. Input (source, header, folder): dari memo (nilai
       dari validasi/decide yang sudah didapat); pra-pass di atas menjamin tidak ada
       output atau folder build/ yang basi di memo. */
    size_t plen2 = strlen(p);
    bool isObj2 = plen2 >= 2 && strcmp(p + plen2 - 2, ".o") == 0;
    bool exists = kind == 'O' && isObj2 ? memoStampNsSize(p, &m, &sz)
                  : (kind == 'O' || kind == 'I') ? fsStampNsSize(p, &m, &sz) : cmdsStatCached(p, &m, &sz);
    if (!exists) {
      stAppendf(&ob, &ou, &oc, "X 0 0 %s\n", p);
    } else if (kind == 'D') {
      stAppendf(&ob, &ou, &oc, "D %lld 0 %s\n", (long long)m, p);
    } else {
      /* v2: baris F membawa content16hex (FNV-1a isi) — sentuh tanpa
         perubahan isi tetap lolos validasi lewat bukti hash, tanpa rebuild
         (design/startup.md §4). Kompatibel mundur: parser menerima baris
         lama tanpa kolom itu. Hash diambil dari catatan lama bila stamp tak
         berubah (audit 30/10 — kandidat 4); stamp berubah -> hash segar. */
      uint64_t content =
          (kind == 'I' || kind == 'F') ? fsPrevContent(&prev, p, (long long)m, sz) : 0;
      stAppendf(&ob, &ou, &oc, "%c %lld %lld %016llx %s\n",
                kind == 'O' ? 'O' : (kind == 'I' ? 'I' : 'F'), (long long)m, sz,
                (unsigned long long)content, p);
    }
  }
  if (stateBlobPut(proj, "ws.state", ob ? ob : "", ou)) optsRouterRecord(c, libOnly); /* router: deps (+lib/+embed) */
  free(ob);
  fsPrevFree(&prev);
  listFree(&items);
  listFree(&libs);

}

/* Validasi fase library dari caches/lib.cache — cetak identik dengan jalur
   ws.lib.state lama (baris Library perpustakaan + Summary Compiled 0). */
static bool wsFastStateValidLib(const char *buildfilePath, const char *cwd) {
  LibCacheMeta m;
  LibStampRec *st = NULL;
  int nst = 0;
  if (!libCacheLoad(&m, &st, &nst)) return false;
  profMark("fs-load"); /* profil: baca lib.cache (meta + daftar stamp) */
  const char *want = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  if (strcmp(m.cwd, cwd) != 0 || !m.buildfile[0] || strcmp(m.buildfile, want) != 0) {
    libCacheMetaFree(&m);
    free(st);
    return false;
  }
  /* Audit 30/10 (kandidat 5): daftar stamp disimpan urut-hash store, jadi
     entri yang basi bisa berada di ujung dan memaksa ratusan stat sia-sia
     (fs-stamps 690 ms di n=500). Gerbang sudah tahu path mana yang isinya
     berbeda (wsGateStale*) — entri itu dipindah ke depan dan dinilai lebih
     dulu. Murni urutan (swap antar slot): seluruh entri tetap dinilai, keputusan
     akhir identik. Bila path tak ada di state ini, tidak ada yang berubah. */
  for (int s = 0, front = 0; front < nst && s < wsGateStaleCount(); s++) {
    const char *stale = wsGateStalePath(s);
    if (!stale) break;
    for (int i = front; i < nst; i++) {
      if (strcmp(st[i].path, stale) != 0) continue;
      LibStampRec tmp = st[front];
      st[front] = st[i];
      st[i] = tmp;
      front++;
      break;
    }
  }
  bool ok = true;
  for (int i = 0; ok && i < nst; i++) {
    char line[WS_PATH_LEN + 64];
    if (st[i].kind == 'M')
      snprintf(line, sizeof(line), "X 0 0 %s", st[i].path);
    else if (st[i].kind == 'D')
      snprintf(line, sizeof(line), "D %lld 0 %s", st[i].mtime, st[i].path);
    else
      /* Baris F memuat content16hex: sentuh tanpa perubahan isi tetap segar
         lewat bukti hash (bukti sentuh; entri lama content=0 = stamp-only). */
      snprintf(line, sizeof(line), "F %lld %lld %016llx %s", st[i].mtime, st[i].size,
               (unsigned long long)st[i].content, st[i].path);
    ok = cmdsStateEntryFresh(line);
  }
  free(st);
  profMark("fs-stamps"); /* profil: loop stat/hash stamp fase library */
  if (!ok) {
    libCacheMetaFree(&m);
    return false;
  }
  if (!buildReportActive()) { /* report merender blok artefak (Execution summary) */
  for (int i = 0; i < m.nlibs; i++)
    printf("%s Library : %s (up-to-date)\n", repPrefix(), m.libs[i]);
  printf("\n%s Summary :\n", repPrefix());
  for (int i = 0; i < m.nlibs; i++) {
    int64_t lm = -1;
    long long sizeBytes = -1;
    cmdsStatCached(m.libs[i], &lm, &sizeBytes);
    printf("    Library  : %s (%.1fKB)\n", m.libs[i],
           sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0);
  }
  printf("    Compiled : 0\n");
  printf("    Skipped  : %d\n", m.skipped);
  printf("    Status   : Success\n");
  }
  libCacheMetaFree(&m);
  return true;
}

bool wsFastStateValid(const char *buildfilePath, bool libOnly) {
  char cwd[WS_PATH_LEN];
  if (!fsGetCwd(cwd, sizeof(cwd))) return false;
  cmdsStatMemoSetCwd(); /* memo memakai cwd root workspace (konstan antar proyek) */
  /* Router (opts.cache) dibaca lebih dulu: cache yang dirutekan hilang /
     kosong -> batal cepat tanpa memuat state (design/startup.md §4). */
  if (!optsRouterValidate()) return false;
  profMark("fs-router"); /* profil: validasi router opts.cache */
  /* Fase library kini dari caches/lib.cache (design/startup.md §3). */
  if (libOnly) return wsFastStateValidLib(buildfilePath, cwd);
  const char *proj = cmdsStateWsProject();
  char *tb = NULL;
  size_t tn = 0, tp = 0;
  if (!proj || !stateBlobGet(proj, "ws.state", &tb, &tn)) return false;

  const char *want = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  char line[SATRIA_MAX_PATH * 6];
  bool ok = stateBufGets(tb, tn, &tp, line, sizeof(line)) != NULL;
  if (ok) {
    char magic[32], scwd[WS_PATH_LEN];
    ok = sscanf(line, "%31s %" WS_SCAN "s", magic, scwd) == 2 && strcmp(magic, WS_STATE_MAGIC) == 0 &&
         strcmp(scwd, cwd) == 0;
  }
  int skipped = 0;
  char tgt[WS_PATH_LEN] = "", bf[WS_PATH_LEN] = "";
  if (ok) {
    ok = stateBufGets(tb, tn, &tp, line, sizeof(line)) != NULL &&
         sscanf(line, "META %d %" WS_SCAN "s %" WS_SCAN "s", &skipped, tgt, bf) == 3 && strcmp(bf, want) == 0;
  }

  List libs = {0};
  profMark("fs-load"); /* profil: baca blob ws.state + parse header */
  while (ok && stateBufGets(tb, tn, &tp, line, sizeof(line))) {
    char kind = line[0];
    if (kind == 'L') {
      char p[WS_PATH_LEN];
      if (sscanf(line, "LIB %" WS_SCAN "s", p) == 1) addKind(&libs, 'L', p);
      continue;
    }
    /* F/D/X: parse + cocokkan stamp lewat helper bersama (cmds_state.c). */
    if (kind == 'F' || kind == 'O' || kind == 'I' || kind == 'D' || kind == 'X') ok = cmdsStateEntryFresh(line);
  }
  free(tb);
  profMark("fs-stamps"); /* profil: loop stat/hash stamp fase binary */
  if (!ok) {
    listFree(&libs);
    return false;
  }

  if (!buildReportActive()) { /* report merender blok artefak (Execution summary) */
  /* Output no-op: sama dengan jalur single project. */
  if (!libOnly) {
    printf("%s Build with cached state (fingerprint unchanged)\n", repPrefix());
    printf("%s Linking : %s (up-to-date)\n", repPrefix(), tgt);
  }
  for (int i = 0; i < libs.count; i++)
    printf("%s Library : %s (up-to-date)\n", repPrefix(), libs.items[i] + 2);
  printf("\n%s Summary :\n", repPrefix());
  if (!libOnly) {
    int64_t tm = -1;
    long long sizeBytes = -1;
    cmdsStatCached(tgt, &tm, &sizeBytes); /* ukuran dari memo (sudah dicek validasi) */
    printf("    Target   : %s\n", tgt);
    printf("    Size     : %.1fKB\n", sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0);
  }
  for (int i = 0; i < libs.count; i++) {
    int64_t lm = -1;
    long long sizeBytes = -1;
    cmdsStatCached(libs.items[i] + 2, &lm, &sizeBytes);
    printf("    Library  : %s (%.1fKB)\n", libs.items[i] + 2,
           sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0);
  }
  printf("    Compiled : 0\n");
  printf("    Skipped  : %d\n", skipped);
  printf("    Status   : Success\n");
  }
  listFree(&libs);
  return true;
}

/* mtime untuk keputusan decide. Mode workspace: lewat memo (output yang berubah
   dibuang dari memo saat state disimpan). Mode single: stat langsung, perilaku lama. */
int64_t cmdsMTimeDecide(const char *path) {
  if (!wsModeActive()) return fsMTimeNs(path);
  int64_t m = -1;
  long long sz = 0;
  if (cmdsStatCached(path, &m, &sz)) return m;
  return -1;
}

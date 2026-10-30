/* Shared source-discovery state. Build execution state stays mode-specific. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h> /* trace audit stateWalkDirCached */
#include "../portability.h"
#include "../prof/prof.h" /* profMark: sub-fase walk (audit overhead) */
#include "../util.h"
#include "../cache/cache_store.h"
#include "../cache/router.h"
#include "cmds_internal.h"
#include "state.h"

static StateMode currentMode = PROJECT;
static bool sourceCacheEnabled = false;
static void srcCacheReset(void);
static bool hasSpace(const char *s) { return s && strpbrk(s, " \t\r\n") != NULL; }

void stateSetMode(StateMode mode) {
  currentMode = mode;
  sourceCacheEnabled = (mode == WORKSPACE);
  if (mode == WORKSPACE) srcCacheReset();
}
StateMode stateGetMode(void) { return currentMode; }
void wsModeSet(bool on) { stateSetMode(on ? WORKSPACE : PROJECT); }
bool wsModeActive(void) { return stateGetMode() == WORKSPACE; }
void cmdsSingleSourcesCacheBegin(void) {
  srcCacheReset();
  sourceCacheEnabled = true;
}
void cmdsSingleSourcesCacheEnd(void) { sourceCacheEnabled = false; }

/* ==================== langkah 1: cache daftar sumber ====================
 * Penelusuran folder sumber (walkDir) dilakukan setiap fase. Di mode workspace
 * hasilnya disimpan di .satria/sources.cache bersama stamp mtime setiap folder yang
 * ditelusuri. Mtime dicatat SEBELUM folder dibaca, jadi bila ada perubahan
 * selama walk, run berikutnya melihat stamp berbeda dan menelusuri ulang.
 * Format (design/startup.md \xC2\xA7sources.cache):
 *   SATRIASRC2 <cwd>, lalu per record R <dir> <ext>, lalu entri
 *   <alamat> <type> <size> <mtime> <path>
 * <alamat> = FNV-1a 64 atas <path> (csHash), ditulis ke disk dan diverifikasi
 * saat dibaca: alamat tak cocok, entri rusak, atau tabrakan (alamat sama untuk
 * path berbeda) menandai record invalid sehingga dihitung ulang. <type>: D
 * folder (size 0; mtime = stamp, -1 bila folder tidak ada) atau S file sumber
 * (size + mtime saat walk). Validasi kesegaran record tetap memakai stamp
 * folder agar touch tanpa ubah isi tetap no-op; size/mtime S dicatat untuk
 * konsumen fast path (opts.cache). Pointer tidak pernah ditulis ke disk. */

typedef struct {
  char *dir;
  char *ext;
  List stamps; /* "D <mtime> <path>" */
  List files;  /* file cocok ext, urutan walk */
  List meta;   /* "<size> <mtime>" per file, sejajar files */
  bool bad;    /* alamat tak cocok / tabrakan: paksa telusur ulang */
} SrcRec;

static SrcRec *srcRecs = NULL;
static int srcRecCount = 0, srcRecCap = 0;
static bool srcLoaded = false;
static char srcCwd[WS_PATH_LEN];

static SrcRec *srcRecFind(const char *dir, const char *ext) {
  for (int i = 0; i < srcRecCount; i++)
    if (strcmp(srcRecs[i].dir, dir) == 0 && strcmp(srcRecs[i].ext, ext) == 0) return &srcRecs[i];
  return NULL;
}

static SrcRec *srcRecNew(const char *dir, const char *ext) {
  if (srcRecCount == srcRecCap) {
    int next = srcRecCap ? srcRecCap * 2 : 16;
    SrcRec *grown = realloc(srcRecs, (size_t)next * sizeof(SrcRec));
    if (!grown) return NULL;
    srcRecs = grown;
    srcRecCap = next;
  }
  SrcRec *r = &srcRecs[srcRecCount++];
  memset(r, 0, sizeof(*r));
  r->dir = strdup(dir);
  r->ext = strdup(ext);
  return r;
}

/* Pasangan (alamat, path) untuk deteksi tabrakan hash saat memuat. */
typedef struct {
  uint64_t addr;
  int rec;         /* indeks srcRecs */
  const char *path;
} SrcAddrRef;

static int srcAddrCmp(const void *a, const void *b) {
  const SrcAddrRef *x = a, *y = b;
  if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
  return strcmp(x->path, y->path);
}

/* Tabrakan: alamat sama untuk path berbeda -> kedua record invalid. */
static void srcDetectCollisions(void) {
  int total = 0;
  for (int i = 0; i < srcRecCount; i++)
    total += srcRecs[i].files.count + srcRecs[i].stamps.count;
  if (total <= 1) return;
  SrcAddrRef *refs = malloc((size_t)total * sizeof(*refs));
  if (!refs) return;
  int n = 0;
  for (int i = 0; i < srcRecCount; i++) {
    SrcRec *r = &srcRecs[i];
    for (int j = 0; j < r->stamps.count; j++) {
      long long mt = 0;
      char p[WS_PATH_LEN];
      if (sscanf(r->stamps.items[j], "D %lld %" WS_SCAN "s", &mt, p) != 2) continue;
      refs[n].addr = csHash(p);
      refs[n].rec = i;
      refs[n].path = strdup(p);
      if (refs[n].path) n++;
    }
    for (int j = 0; j < r->files.count; j++) {
      refs[n].addr = csHash(r->files.items[j]);
      refs[n].rec = i;
      refs[n].path = strdup(r->files.items[j]);
      if (refs[n].path) n++;
    }
  }
  qsort(refs, (size_t)n, sizeof(*refs), srcAddrCmp);
  for (int i = 1; i < n; i++)
    if (refs[i].addr == refs[i - 1].addr && strcmp(refs[i].path, refs[i - 1].path) != 0) {
      srcRecs[refs[i].rec].bad = true;
      srcRecs[refs[i - 1].rec].bad = true;
    }
  for (int i = 0; i < n; i++) free((void *)refs[i].path);
  free(refs);
}

static void srcLoad(void) {
  if (srcLoaded) return;
  srcLoaded = true;
  if (!fsGetCwd(srcCwd, sizeof(srcCwd))) return;
  FILE *fp = fopen(".satria/sources.cache", "r");
  if (!fp) return;
  char line[MAX_PATH * 6];
  if (fgets(line, sizeof(line), fp)) {
    char magic[32], cwd[WS_PATH_LEN];
    if (sscanf(line, "%31s %" WS_SCAN "s", magic, cwd) != 2 || strcmp(magic, "SATRIASRC2") != 0 ||
        strcmp(cwd, srcCwd) != 0) {
      fclose(fp);
      return; /* cache format lama / milik folder lain: abaikan, telusur ulang */
    }
  }
  SrcRec *cur = NULL;
  while (fgets(line, sizeof(line), fp)) {
    if (line[0] == 'R') {
      char d[WS_PATH_LEN], e[64];
      /* ekstensi kosong ditulis "-" (token sscanf tak boleh kosong) */
      if (sscanf(line, "R %" WS_SCAN "s %63s", d, e) == 2) {
        if (strcmp(e, "-") == 0) e[0] = '\0';
        cur = srcRecNew(d, e);
      }
      continue;
    }
    if (line[0] == 'P' && line[1] == ' ') continue; /* peta fingerprint (wsCacheSave) */
    if (!cur) continue;
    unsigned long long addr = 0;
    char kind = 0, p[WS_PATH_LEN];
    long long sz = 0, mt = 0;
    if (sscanf(line, "%16llx %c %lld %lld %" WS_SCAN "s", &addr, &kind, &sz, &mt, p) == 5 &&
        (kind == 'S' || kind == 'D')) {
      if (addr != csHash(p)) {
        cur->bad = true; /* alamat tak cocok: record dihitung ulang */
        continue;
      }
      if (kind == 'D') {
        char stamp[WS_PATH_LEN + 64];
        snprintf(stamp, sizeof(stamp), "D %lld %s", mt, p);
        listAdd(&cur->stamps, stamp);
      } else {
        char meta[64];
        snprintf(meta, sizeof(meta), "%lld %lld", sz, mt);
        listAdd(&cur->files, p);
        listAdd(&cur->meta, meta);
      }
      continue;
    }
    cur->bad = true; /* entri rusak dalam record: rekalkulasi */
  }
  fclose(fp);
  srcDetectCollisions();
}

static bool srcRecValid(const SrcRec *r) {
  if (r->bad) return false;
  for (int i = 0; i < r->stamps.count; i++) {
    long long mt = 0;
    char p[WS_PATH_LEN];
    if (sscanf(r->stamps.items[i], "D %lld %" WS_SCAN "s", &mt, p) != 2) return false;
    int64_t curM = -1;
    long long curS = -1;
    bool exists = cmdsStatCached(p, &curM, &curS);
    if (mt < 0) {               /* saat disimpan folder tidak ada: valid selama tetap tidak ada */
      if (exists) return false;
      continue;
    }
    if (!exists || curM != (int64_t)mt) return false;
  }
  return true;
}

/*
 * Gagal ke daftar sumber: `.satria` (state buatan satria) dan `.git`
 * (metadata VCS — direktori maupun file gitfile submodule/worktree).
 * mtime `.git` berubah di setiap operasi git; mencatatnya membuat stamp di
 * sources.cache basi terus, memaksa penelusuran ulang, dan stamp direktori ikut
 * masuk daftar input fast state (wsAddSourceDirs) sehingga commit/checkout
 * memicu rebuild yang tidak seharusnya.
 */
static bool srcSkipName(const char *path) {
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  return strcmp(b, ".satria") == 0 || strcmp(b, ".git") == 0;
}

static void srcWalk(SrcRec *r, const char *dir, List *out) {
  int64_t mt = -1;
  long long sz = 0;
  bool ok = fsStampNsSize(dir, &mt, &sz); /* stamp SEBELUM dibaca */
  char stamp[WS_PATH_LEN + 64];
  snprintf(stamp, sizeof(stamp), "D %lld %s", ok ? (long long)mt : -1LL, dir);
  listAdd(&r->stamps, stamp);

  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  size_t elen = strlen(r->ext);
  for (int i = 0; i < files.count; i++) {
    const char *path = files.items[i];
    size_t len = strlen(path);
    if (srcSkipName(path)) continue;
    if (len > elen && strcmp(path + len - elen, r->ext) == 0) {
      int64_t fmt = -1;
      long long fsz = 0;
      /* size + mtime ikut dicatat (fast path opts.cache). File yang hilang di
         tengah walk dibuang: stamp folder berubah, record dihitung ulang. */
      if (!fsStampNsSize(path, &fmt, &fsz)) continue;
      char meta[64];
      snprintf(meta, sizeof(meta), "%lld %lld", (long long)fsz, (long long)fmt);
      listAdd(out, path);
      listAdd(&r->files, path);
      listAdd(&r->meta, meta);
    }
  }
  for (int i = 0; i < dirs.count; i++) {
    /* .satria (state) dan .git (VCS) tak pernah jadi input — lihat srcSkipName. */
    if (srcSkipName(dirs.items[i])) continue;
    srcWalk(r, dirs.items[i], out);
  }
  listFree(&dirs);
  listFree(&files);
}

/* Bandingkan isi .satria/sources.cache dengan daftar baris yang akan ditulis. */
static bool srcFileSame(const List *lines) {
  FILE *fp = fopen(".satria/sources.cache", "r");
  if (!fp) return false;
  char line[WS_PATH_LEN * 2];
  bool same = true;
  int i = 0;
  while (same && fgets(line, sizeof(line), fp)) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = '\0';
    if (line[0] == 'P' && line[1] == ' ' ) continue; /* workspace fingerprint mapping */
    if (i >= lines->count || strcmp(line, lines->items[i]) != 0) same = false;
    i++;
  }
  if (same && i != lines->count) same = false;
  fclose(fp);
  return same;
}

/* Persist discovery records after a per-(dir,ext) upsert.  The P records
 * belong to workspace fingerprinting, not discovery: keep them intact until
 * wsCacheSave replaces them after a successful build.  This avoids destroying
 * the mapping when only one source directory changes. */
static void srcSave(void) {
  List lines = {0};
  char buf[WS_PATH_LEN * 2];
  snprintf(buf, sizeof(buf), "SATRIASRC2 %s", srcCwd);
  listAdd(&lines, buf);
  for (int i = 0; i < srcRecCount; i++) {
    SrcRec *r = &srcRecs[i];
    snprintf(buf, sizeof(buf), "R %s %s", r->dir, r->ext[0] ? r->ext : "-");
    listAdd(&lines, buf);
    for (int j = 0; j < r->stamps.count; j++) {
      long long mt = 0;
      char p[WS_PATH_LEN];
      if (sscanf(r->stamps.items[j], "D %lld %" WS_SCAN "s", &mt, p) != 2) continue;
      snprintf(buf, sizeof(buf), "%016llx D 0 %lld %s", (unsigned long long)csHash(p), mt, p);
      listAdd(&lines, buf);
    }
    for (int j = 0; j < r->files.count && j < r->meta.count; j++) {
      long long sz = 0, mt = 0;
      if (sscanf(r->meta.items[j], "%lld %lld", &sz, &mt) != 2) sz = mt = 0;
      snprintf(buf, sizeof(buf), "%016llx S %lld %lld %s",
               (unsigned long long)csHash(r->files.items[j]), sz, mt, r->files.items[j]);
      listAdd(&lines, buf);
    }
  }
  if (!srcFileSame(&lines)) {
    mkdirs(".satria");
    FILE *old = fopen(".satria/sources.cache", "r");
    FILE *fp = fopen(".satria/sources.cache.tmp", "w");
    bool ok = fp != NULL;
    if (ok) {
      for (int i = 0; i < lines.count && ok; i++)
        ok = fprintf(fp, "%s\n", lines.items[i]) >= 0;
      /* Preserve the fingerprint map while updating only discovery records.
       * Do not carry mappings across unrelated working directories. Magic
       * dibandingkan lewat: upgrade format lama (SATRIASRC1) tak menghapus
       * peta P selama cwd sama. */
      if (old && ok) {
        char line[WS_PATH_LEN * 2];
        bool sameCwd = false;
        if (fgets(line, sizeof(line), old)) {
          char magic[32], cwd[WS_PATH_LEN];
          if (sscanf(line, "%31s %" WS_SCAN "s", magic, cwd) == 2)
            sameCwd = strcmp(cwd, srcCwd) == 0;
        }
        if (sameCwd) {
          while (ok && fgets(line, sizeof(line), old)) {
            if (line[0] == 'P' && line[1] == ' ')
              ok = fputs(line, fp) != EOF;
          }
        }
      }
      if (fclose(fp) != 0) ok = false;
      if (ok && !fsRenameReplace(".satria/sources.cache.tmp", ".satria/sources.cache"))
        ok = false;
      if (!ok) fsRemoveFile(".satria/sources.cache.tmp");
      /* Rute `sources` di opts.cache ikut disegarkan SEKARANG (bukan hanya di
         akir run): re-walk bisa terjadi di tengah run — mis. proyek kemasan
         me-walk arsipnya sebelum proyek lain divalidasi — dan validasi
         proyek berikutnya harus membandingkan stempel terkini, bukan stempel
         akir run sebelumnya (cascade tolak-router; design/startup.md §4).
         Gagal sync aman: gate jadi konservatif. */
      if (ok) optsSourcesRouteSync();
    }
    if (old) fclose(old);
  }
  listFree(&lines);
}

/* Pengganti walkDir di jalur workspace. Di luar mode workspace: walkDir biasa. */
/* Salin stamp folder ("D <mtime> <path>") milik record cache (dir, ext). Dipakai
   freshness arsip: mtime folder berubah bila file ditambah/dihapus. Panggil
   setelah stateWalkDirCached agar record sudah valid/terbaru. */
void stateWalkDirStamps(const char *dir, const char *ext, List *out) {
  if (stateGetMode() != WORKSPACE || !dir || !ext) return;
  srcLoad();
  SrcRec *r = srcRecFind(dir, ext);
  if (!r) return;
  for (int i = 0; i < r->stamps.count; i++) listAdd(out, r->stamps.items[i]);
}

void stateWalkDirCached(const char *dir, const char *ext, List *out) {
  /* Trace audit (SATRIA_PROFILE): durasi + jalur per (dir,ext) — label profMark
     tidak bisa memuat path, jadi dicetak langsung. */
  struct timespec tw0;
  bool trace = profOn();
  if (trace) clock_gettime(CLOCK_MONOTONIC, &tw0);
#define WALK_TRACE(branch)                                                                    \
  do {                                                                                        \
    if (trace) {                                                                              \
      struct timespec tw1;                                                                    \
      clock_gettime(CLOCK_MONOTONIC, &tw1);                                                   \
      double now = tw1.tv_sec * 1000.0 + tw1.tv_nsec / 1e6;                                   \
      double ms = (tw1.tv_sec - tw0.tv_sec) * 1000.0 + (tw1.tv_nsec - tw0.tv_nsec) / 1e6;      \
      fprintf(stderr, "[walk] t=%.0f %s ext=%s -> %s %.2f ms\n", now, dir, ext, branch, ms); \
    }                                                                                         \
  } while (0)
  if ((!sourceCacheEnabled) || !dir || !ext || hasSpace(dir) || hasSpace(ext)) {
    walkDir(dir, ext, out);
    profMark("walk-fallback"); /* profil: jalur tanpa cache */
    WALK_TRACE("fallback");
    return;
  }
  srcLoad();
  SrcRec *r = srcRecFind(dir, ext);
  if (r && srcRecValid(r)) {
    for (int i = 0; i < r->files.count; i++) listAdd(out, r->files.items[i]);
    profMark("walk-hit"); /* profil: record valid, salin dari memori */
    WALK_TRACE("hit");
    return;
  }
  if (r) {
    listFree(&r->stamps);
    listFree(&r->files);
    listFree(&r->meta);
    r->bad = false;
  } else {
    r = srcRecNew(dir, ext);
    if (!r) {
      walkDir(dir, ext, out);
      return;
    }
  }
  srcWalk(r, dir, out);
  srcSave();
  profMark("walk-miss"); /* profil: re-walk + simpan sources.cache */
  WALK_TRACE("miss");
#undef WALK_TRACE
}

static void srcCacheReset(void) {
  for (int i = 0; i < srcRecCount; i++) {
    free(srcRecs[i].dir);
    free(srcRecs[i].ext);
    listFree(&srcRecs[i].stamps);
    listFree(&srcRecs[i].files);
    listFree(&srcRecs[i].meta);
  }
  srcRecCount = 0;
  srcLoaded = false;
}

/* Folder sumber yang ditelusuri (termasuk subfolder) dicatat sebagai D. */
void wsAddSourceDirs(List *items) {
  for (int i = 0; i < srcRecCount; i++) {
    for (int j = 0; j < srcRecs[i].stamps.count; j++) {
      long long mt = 0;
      char p2[WS_PATH_LEN];
      if (sscanf(srcRecs[i].stamps.items[j], "D %lld %" WS_SCAN "s", &mt, p2) == 2)
        { char entry[WS_PATH_LEN + 4]; snprintf(entry, sizeof(entry), "D %s", p2); listAdd(items, entry); }
    }
  }
}


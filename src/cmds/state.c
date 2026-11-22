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

/* Blok arena path. Blok TIDAK pernah dipindah (beda dengan buffer tumbuh),
   jadi files.items tetap valid walau source berikutnya menambah blok baru. */
typedef struct SrcArenaBlk {
  struct SrcArenaBlk *next;
  size_t used, cap;
  char data[1];
} SrcArenaBlk;

typedef struct {
  char *dir;
  char *ext;
  List stamps;      /* "D <mtime> <path>" */
  List files;       /* file cocok ext, urutan walk */
  long long *msize; /* sejajar files: ukuran file (fast path opts.cache) */
  long long *mtime; /* sejajar files: mtime nanodetik */
  int mcount, mcap;
  /* Path file dipadatkan di arena berblok: sources.cache 10k berisi 10k
     path, dan satu strdup per path (10k malloc) lebih mahal daripada menyalin
     byte ke blok 64 KB. files.items menunjuk ke dalam blok-blok ini. */
  SrcArenaBlk *blk, *blkLast;
  bool bad; /* alamat tak cocok / tabrakan: paksa telusur ulang */
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

/* Metadata ukuran/mtime disimpan numerik: string "<size> <mtime>" hanya
   dipakai srcSave, dan membentuknya per source (snprintf + strdup) adalah
   biaya terbesar muat sources.cache pada workspace 10k. */
static bool srcMetaAdd(SrcRec *r, long long sz, long long mt) {
  if (r->mcount == r->mcap) {
    int nc = r->mcap ? r->mcap * 2 : 64;
    long long *ns = realloc(r->msize, (size_t)nc * sizeof(long long));
    if (!ns) return false;
    r->msize = ns;
    long long *nt = realloc(r->mtime, (size_t)nc * sizeof(long long));
    if (!nt) return false;
    r->mtime = nt;
    r->mcap = nc;
  }
  r->msize[r->mcount] = sz;
  r->mtime[r->mcount] = mt;
  r->mcount++;
  return true;
}

static void srcMetaFree(SrcRec *r) {
  free(r->msize);
  free(r->mtime);
  r->msize = r->mtime = NULL;
  r->mcount = r->mcap = 0;
}

/* Tambah pointer ke items List tanpa menyalin string (pemilik: arena r). */
static bool srcListAddRef(List *l, char *s) {
  if (l->count == l->capacity) {
    int next = l->capacity ? l->capacity * 2 : 64;
    char **items = realloc(l->items, (size_t)next * sizeof(*items));
    if (!items) return false;
    l->items = items;
    l->capacity = next;
  }
  l->items[l->count++] = s;
  return true;
}

/* Tambah path file ke arena record + daftar files. */
static bool srcFileAdd(SrcRec *r, const char *path) {
  size_t n = strlen(path) + 1;
  SrcArenaBlk *b = r->blkLast;
  if (!b || b->used + n > b->cap) {
    size_t cap = n > 65536 ? n : 65536;
    SrcArenaBlk *nb = malloc(sizeof(*nb) + cap);
    if (!nb) return false;
    nb->next = NULL;
    nb->used = 0;
    nb->cap = cap;
    if (b)
      b->next = nb;
    else
      r->blk = nb;
    r->blkLast = nb;
    b = nb;
  }
  char *dst = b->data + b->used;
  memcpy(dst, path, n);
  b->used += n;
  return srcListAddRef(&r->files, dst);
}

static void srcFilesFree(SrcRec *r) {
  free(r->files.items);
  r->files.items = NULL;
  r->files.count = r->files.capacity = 0;
  for (SrcArenaBlk *b = r->blk; b;) {
    SrcArenaBlk *next = b->next;
    free(b);
    b = next;
  }
  r->blk = r->blkLast = NULL;
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

/* Tabel tabrakan alamat (open addressing). Tabrakan = alamat sama untuk path
   berbeda; kedua record ditandai invalid. Dibangun langsung saat parse, jadi
   tidak perlu array referensi terpisah + qsort setelahnya. */
typedef struct {
  uint64_t addr;
  const char *path; /* NULL = slot kosong */
  int rec;          /* indeks srcRecs */
} SrcAddrRef;

static void srcCollideAdd(SrcAddrRef *tab, size_t mask, int rec, uint64_t addr,
                          const char *path) {
  size_t h = (size_t)((addr * 0x9E3779B97F4A7C15ULL) & mask);
  for (;;) {
    if (!tab[h].path) {
      tab[h].addr = addr;
      tab[h].path = path;
      tab[h].rec = rec;
      return;
    }
    if (tab[h].addr == addr) {
      if (strcmp(tab[h].path, path) != 0) {
        srcRecs[tab[h].rec].bad = true;
        srcRecs[rec].bad = true;
      }
      return;
    }
    h = (h + 1) & mask;
  }
}

/* Token berikutnya pada baris: NUL-kan ujungnya dan majukan *pp. */
static char *srcToken(char **pp) {
  char *p = *pp;
  while (*p == ' ' || *p == '\t') p++;
  if (!*p) {
    *pp = p;
    return NULL;
  }
  char *start = p;
  while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
  if (*p) *p++ = '\0';
  *pp = p;
  return start;
}

static bool srcHex16(const char *s, uint64_t *out) {
  if (!s) return false;
  uint64_t v = 0;
  for (int i = 0; i < 16; i++) {
    int c = (unsigned char)s[i];
    if (!c) return false;
    int d;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    else
      return false;
    v = (v << 4) | (uint64_t)d;
  }
  *out = v; /* akhir token divalidasi pemanggil (spasi, bukan NUL) */
  return true;
}

/* Angka desimal di tengah baris; kembalikan posisi sesudah digit. */
static const char *srcScanLL(const char *s, long long *out) {
  bool neg = false;
  if (*s == '-') {
    neg = true;
    s++;
  } else if (*s == '+') {
    s++;
  }
  if (*s < '0' || *s > '9') return NULL;
  unsigned long long v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
  *out = neg ? -(long long)v : (long long)v;
  return s;
}

/* Muat sources.cache sekali. Biaya ini BUKAN biaya walk: pemanggil yang
   men-trace memuat lebih dulu (stateSourcesCacheLoad) agar tidak dibebankan
   ke direktori pertama. Jalur cepat: baca per blok berhenti di baris P
   pertama, parse format kanonik tanpa sscanf/token, path di arena berblok,
   metadata numerik, dan tabel tabrakan diisi langsung saat parse. */
static void srcLoad(void) {
  if (srcLoaded) return;
  srcLoaded = true;
  if (!fsGetCwd(srcCwd, sizeof(srcCwd))) return;
  /* trace audit: biaya muat cache dipisah dari [walk], plus rincian
     baca/parse supaya optimasi berikutnya terukur. */
  bool trace = profOn();
  double t0 = 0, tRead = 0, tParse = 0;
  if (trace) t0 = portMonoMs();

  /* Baca per blok besar lalu belah baris di tempat: 40k fgets (3 MB pada
     workspace 10k) menyalin setiap baris dan memanggil I/O per baris. Baris P
     (peta fingerprint, 30k baris) selalu di EKOR file (wsCacheSave menyalin
     record lama lalu menambahkan peta segar; srcSave menulis record lalu
     menyusul P), jadi pembacaan berhenti di awal baris P pertama: hanya
     ~1 MB prefix discovery yang dibaca, bukan seluruh 3 MB. */
  FILE *fp = fopen(".satria/sources.cache", "rb");
  if (!fp) return;
  /* Blok awal 1 MB: prefix discovery workspace 10k ~590 KB, dan di lingkungan
     ber-proot satu panggilan baca besar jauh lebih murah daripada beberapa
     panggilan 256 KB. File lebih besar tetap tumbuh doubling. */
  size_t cap = 1024 * 1024, len = 0, scanned = 0, nlines = 0;
  char *buf = malloc(cap + 1);
  if (!buf) {
    fclose(fp);
    return;
  }
  for (;;) {
    if (len == cap) {
      char *grown = realloc(buf, cap * 2 + 1);
      if (!grown) break;
      buf = grown;
      cap *= 2;
    }
    size_t got = fread(buf + len, 1, cap - len, fp);
    if (got == 0) break;
    len += got;
    buf[len] = '\0';
    size_t from = scanned > 2 ? scanned - 2 : 0; /* -2: '\n' di batas blok */
    bool hit = false;
    for (size_t i = from; i < len; i++) {
      if (buf[i] != '\n') continue;
      nlines++;
      if (i + 2 < len && buf[i + 1] == 'P' && buf[i + 2] == ' ') {
        len = i + 1;
        buf[len] = '\0';
        hit = true;
        break;
      }
    }
    if (hit) break;
    scanned = len;
  }
  fclose(fp);
  char *end = buf + len;
  if (len == 0) {
    free(buf);
    return;
  }
  if (trace) tRead = portMonoMs();

  /* Baris pertama: magic + cwd. */
  char *line = buf;
  char *nl = memchr(line, '\n', (size_t)(end - line));
  if (nl) *nl = '\0';
  if (nl && nl > line && nl[-1] == '\r') nl[-1] = '\0';
  char *q = line;
  char *magic = srcToken(&q);
  char *cwd = srcToken(&q);
  if (!magic || !cwd || strcmp(magic, "SATRIASRC2") != 0 || strcmp(cwd, srcCwd) != 0) {
    free(buf);
    return; /* cache format lama / milik folder lain: abaikan, telusur ulang */
  }

  /* Ukuran tabel = 1,5x jumlah baris (batas atas entri), load factor <= 2/3. */
  SrcAddrRef *ctab = NULL;
  size_t cmask = 0;
  if (nlines > 1) {
    size_t cc = 16;
    while (cc < nlines + nlines / 2 + 1) cc <<= 1;
    ctab = calloc(cc, sizeof(*ctab));
    if (ctab) cmask = cc - 1;
  }
  int nent = 0;
  SrcRec *cur = NULL;
  line = nl ? nl + 1 : end;
  while (line < end) {
    nl = memchr(line, '\n', (size_t)(end - line));
    char *next = nl ? nl + 1 : end;
    if (nl) *nl = '\0';
    if (nl && nl > line && nl[-1] == '\r') nl[-1] = '\0';

    if (line[0] == 'R') {
      char *rq = line + 1;
      char *d = srcToken(&rq);
      char *e = srcToken(&rq);
      /* ekstensi kosong ditulis "-" (token kosong tak bisa ditulis) */
      if (d && e) {
        if (strcmp(e, "-") == 0) e[0] = '\0';
        cur = srcRecNew(d, e);
        if (!cur) break;
      }
      goto nextline;
    }
    if (line[0] == 'P' && line[1] == ' ') goto nextline; /* peta fingerprint (wsCacheSave) */
    if (!cur) goto nextline;
    {
      /* Format kanonik: "<16hex> <S|D> <size> <mtime> <path>", satu spasi
         pemisah (penulis cache selalu kanonik). */
      long long sz = 0, mt = 0;
      uint64_t addr = 0;
      const char *np = NULL;
      if (!srcHex16(line, &addr) || line[16] != ' ' ||
          (line[17] != 'S' && line[17] != 'D') || line[18] != ' ' ||
          !(np = srcScanLL(line + 19, &sz)) || *np != ' ' ||
          !(np = srcScanLL(np + 1, &mt)) || *np != ' ' || !np[1]) {
        cur->bad = true; /* entri rusak dalam record: rekalkulasi */
        goto nextline;
      }
      const char *p = np + 1;
      if (addr != csHash(p)) {
        cur->bad = true; /* alamat tak cocok: record dihitung ulang */
        goto nextline;
      }
      int rec = (int)(cur - srcRecs);
      if (line[17] == 'D') {
        char stamp[WS_PATH_LEN + 64];
        int slen = snprintf(stamp, sizeof(stamp), "D %lld %s", mt, p);
        if (slen < 0 || slen >= (int)sizeof(stamp) || !listAdd(&cur->stamps, stamp))
          goto nextline;
        /* path ada di ujung string stamp yang baru disimpan */
        if (ctab)
          srcCollideAdd(ctab, cmask, rec, addr,
                        cur->stamps.items[cur->stamps.count - 1] + slen - (int)strlen(p));
      } else {
        if (!srcFileAdd(cur, p)) goto nextline;
        srcMetaAdd(cur, sz, mt); /* gagal alokasi: srcSave memakai batas mcount */
        if (ctab) srcCollideAdd(ctab, cmask, rec, addr, cur->files.items[cur->files.count - 1]);
      }
      nent++;
    }
  nextline:
    line = next;
  }
  if (trace) tParse = portMonoMs();
  free(ctab);
  free(buf);
  if (trace && srcRecCount > 0) {
    double tEnd = portMonoMs();
    fprintf(stderr, "[src-load] t=%.0f records=%d entries=%d read=%.1f parse=%.1f total=%.2f ms\n",
            tEnd, srcRecCount, nent, tRead - t0, tParse - tRead, tEnd - t0);
  }
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
      listAdd(out, path);
      if (!srcFileAdd(r, path)) continue;
      srcMetaAdd(r, (long long)fsz, (long long)fmt);
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
    for (int j = 0; j < r->files.count && j < r->mcount; j++) {
      snprintf(buf, sizeof(buf), "%016llx S %lld %lld %s",
               (unsigned long long)csHash(r->files.items[j]), r->msize[j], r->mtime[j],
               r->files.items[j]);
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

/* Muat sources.cache lebih dulu bila trace aktif, supaya biaya muat tidak
   dibebankan ke [collect] milik direktori pertama (lihat srcLoad). */
void stateSourcesCacheLoad(void) {
  if (sourceCacheEnabled) srcLoad();
}

void stateWalkDirCached(const char *dir, const char *ext, List *out) {
  /* Trace audit (SATRIA_PROFILE): durasi + jalur per (dir,ext) — label profMark
     tidak bisa memuat path, jadi dicetak langsung. */
  double tw0 = 0; /* trace: ms monotonic (portMonoMs — portabel MSVC) */
  bool trace = profOn();
  if (trace) tw0 = portMonoMs();
#define WALK_TRACE(branch)                                                                    \
  do {                                                                                        \
    if (trace) {                                                                              \
      double tw1 = portMonoMs();                                                              \
      double now = tw1;                                                                       \
      double ms = tw1 - tw0;                                                                  \
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
  if (trace) tw0 = portMonoMs(); /* muat cache bukan bagian walk ini */
  SrcRec *r = srcRecFind(dir, ext);
  if (r && srcRecValid(r)) {
    for (int i = 0; i < r->files.count; i++) listAdd(out, r->files.items[i]);
    profMark("walk-hit"); /* profil: record valid, salin dari memori */
    WALK_TRACE("hit");
    return;
  }
  if (r) {
    listFree(&r->stamps);
    srcFilesFree(r);
    srcMetaFree(r);
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
    srcFilesFree(&srcRecs[i]);
    srcMetaFree(&srcRecs[i]);
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


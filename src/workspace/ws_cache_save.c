/* ws_cache_save.c — Cache save: menulis fingerprint.cache dan peta P bermetadata
 * di sources.cache sesudah build sukses. Baris P: <alamat> <tipe> <content>
 * <time> <size> <path> — metadata inilah yang dipakai fast path no-op (§4).
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"
#include "../cache/artifact.h"
#include "../cache/router.h"
#include "../cache/state_blob.h"
#include "../prof/prof.h" /* profMark: sub-fase ws-cachesave (audit overhead) */
#include "../commands.h" /* cmdsWsPhaseCompiledGet: audit 4/11 item 1 */
#include <errno.h> /* wsCacheAddDepFile: ENOENT membedakan .d hilang dari gagal baca */
#include <time.h> /* wsNowNs: clock_gettime */

static bool wsCacheAdd(FILE *pf, CsStore *st, uint64_t proj, const char *path, char type);
static bool wsCacheAddSt(FILE *pf, CsStore *st, uint64_t proj, const char *path, char type,
                         struct stat *known);
static bool wsCacheAddEntry(FILE *pf, CsStore *st, uint64_t proj, const char *path, char type,
                            long long mtimeNs, long long fsize);
static void wsCacheDirOf(const char *path, char *out, size_t n);
/* 1 = ok; 0 = file .d tak ada (ENOENT); -1 = gagal baca/parse. Satu fopen
   menggantikan fsFileExists+fopen (dua syscall path-walk per object). */
static int wsCacheAddDepFile(FILE *pf, CsStore *st, uint64_t proj, const char *dfile);
static bool wsCacheAddProjectDeps(FILE *pf, CsStore *st, uint64_t proj, const char *name,
                                  bool compiled);

/* Hash isi dari fingerprint.cache sebelumnya (PERF-01). Dipakai ulang hanya
   bila (time, size) cocok DAN entri tidak racy: time < mtime file cache lama
   (startup.md, pengaman racy timestamp). Selain itu isi di-hash ulang. */
static CsStore *g_prevFp = NULL;
static long long g_prevFpMtime = 0;
/* Audit 4/11 item 1: .o/.d yang direkam dari memo stat gerbang no-op
   (proyek tanpa kompilasi) — untuk profil save-walk. */
static int g_aDSMemo = 0, g_aOMemo = 0;
static void wsCacheSaveBody(const WsModel *m, const char *wsName);
/* Mode selektif (merge per proyek — design/build_report.md O1): indeks proyek
   yang dibangun run ini. NULL/0 = simpanan penuh (perilaku lama). */
static const int *g_selIdx = NULL;
static int g_selN = 0;

/* Audit save-walk (SATRIA_PROFILE): akumulasi ms baca .d / stat header /
   hash header agar komposisi save-walk terpetakan tanpa 43+ penanda.
   Durasi memakai portMonoMs (wrapper monotonic portabel — MSVC tanpa
   CLOCK_MONOTONIC; lihat port/port_time.c). */
static double g_aDRead = 0, g_aHStat = 0, g_aHHash = 0;
static int g_aD = 0, g_aH = 0, g_aHHashed = 0, g_aDSkip = 0;
/* Komposisi save-walk (audit 30/10, kandidat save-walk): stat input (sumber/
   header/dir/Buildfile lewat memo), cek .d (langsung, .d ditulis di tengah
   run), dan stat output (bin/lib/arsip/embedded). Murni pengukuran — tanpa
   SATRIA_PROFILE tidak ada panggilan jam sama sekali. */
static double g_aInMs = 0, g_aDSCheckMs = 0, g_aOutMs = 0;
static int g_aIn = 0, g_aDSCheck = 0, g_aOut = 0;

/* Entri header (H) dari sources.cache SEBELUMNYA — daftar gabungan seluruh
   file .d pada save terakhir. Disalin ulang agar object yang .d-nya TIDAK
   berubah tidak perlu dibuka lagi (audit 30/10: 10.003 fopen+read .d per
   save adalah biaya terbesar jalur inkremental; jalur baca kini hanya untuk
   .d yang digenerate ulang oleh compile di run ini).
   Atribusi proyek lewat g_prevFp: fingerprint.cache menyimpan (proj,file)
   untuk tiap entri, sedangkan baris P di sources.cache tidak memuat proj. */
typedef struct {
  uint64_t file;
  char *path;
} WsPrevH;
static WsPrevH *g_prevH = NULL;
static int g_prevHN = 0, g_prevHCap = 0;

static void wsPrevHFree(void) {
  for (int i = 0; i < g_prevHN; i++) free(g_prevH[i].path);
  free(g_prevH);
  g_prevH = NULL;
  g_prevHN = g_prevHCap = 0;
}

/* P <file> <type> <content> <time> <size> <path> — hanya type H yang
   disimpan; hash isi/stamp diambil ulang dari g_prevFp + stat segar. */
static void wsPrevHLoad(void) {
  FILE *f = fopen(WS_CACHE_SOURCES, "r");
  if (!f) return;
  char line[WS_PATH_LEN + 128];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] != 'P' || line[1] != ' ') continue;
    unsigned long long file = 0, content = 0;
    long long time = 0, size = 0;
    char type = 0;
    int off = 0;
    if (sscanf(line + 2, "%16llx %c %16llx %lld %lld %n", &file, &type, &content,
               &time, &size, &off) < 5 ||
        off <= 0)
      continue;
    if (type != 'H') continue;
    char *path = line + 2 + off;
    size_t pn = strlen(path);
    while (pn && (path[pn - 1] == '\n' || path[pn - 1] == '\r')) path[--pn] = '\0';
    if (!pn) continue;
    /* Satu entri per file: baris P lama boleh punya duplikat (id = hash path),
       dan salinan tanpa dedup membuat jumlahnya berlipat tiap save. */
    bool seen = false;
    for (int i = 0; i < g_prevHN; i++)
      if (g_prevH[i].file == (uint64_t)file) {
        seen = true;
        break;
      }
    if (seen) continue;
    if (g_prevHN == g_prevHCap) {
      int nc = g_prevHCap ? g_prevHCap * 2 : 64;
      WsPrevH *np = (WsPrevH *)realloc(g_prevH, (size_t)nc * sizeof(WsPrevH));
      if (!np) break;
      g_prevH = np;
      g_prevHCap = nc;
    }
    char *dup = strdup(path);
    if (!dup) break;
    g_prevH[g_prevHN].file = (uint64_t)file;
    g_prevH[g_prevHN].path = dup;
    g_prevHN++;
  }
  fclose(f);
}

/* Stamp input (sumber/header/direktori/Buildfile) lewat memo stat: jalur
   fast-state & validasi sudah men-stat path yang sama di run ini, dan input
   tidak ditulis oleh build — audit 29/10 menunjukkan path-walk ~300 µs/stat,
   jadi save-walk membayar statist yang sama 2-3 kali per run. Output ('O')
   tidak memakai memo karena ditulis di tengah run (bisa basi).
   Windows: memo memakai epoch fsStampNsSize (FILETIME) sedangkan gate
   membandingkan dengan wsCacheStampNs (st_mtime) — campur epoch membuat
   stamp fingerprint selalu bentrok, jadi tetap stat langsung di sana. */
static bool wsCacheInputStamp(const char *path, long long *mtimeNs, long long *size) {
#if defined(_WIN32)
  struct stat sb;
  double t0 = portMonoMs();
  bool ok = stat(path, &sb) == 0;
  g_aInMs += portMonoMs() - t0;
  g_aIn++;
  if (!ok) return false;
  *mtimeNs = wsCacheStampNs(path, &sb);
  *size = (long long)sb.st_size;
  return true;
#else
  int64_t m = -1;
  long long s = -1;
  double t0 = portMonoMs();
  bool ok = cmdsStatCached(path, &m, &s);
  g_aInMs += portMonoMs() - t0;
  g_aIn++;
  if (!ok) return false;
  *mtimeNs = (long long)m;
  *size = s;
  return true;
#endif
}

static bool wsCacheAdd(FILE *pf, CsStore *st, uint64_t proj, const char *path, char type) {
  return wsCacheAddSt(pf, st, proj, path, type, NULL);
}

/* known != NULL: stat untuk output 'O' sudah diambil pemanggil pada saat yang sama
   (tanpa penulisan di antaranya) — dipakai ulang, tidak di-stat lagi. */
static bool wsCacheAddSt(FILE *pf, CsStore *st, uint64_t proj, const char *path, char type,
                         struct stat *known) {
  /* Outputs belong to the workspace as a whole: project-specific discovery
     and the final bin/lib scan may encounter the same physical output.
     Store each output path once, while retaining all alias-only outputs. */
  if (type == 'O') proj = csHash("workspace");
  long long mtimeNs = 0, fsize = 0;
  if (type == 'O') {
    struct stat sb;
    bool okO;
    if (known) {
      sb = *known;
      okO = true;
    } else {
      double t0 = portMonoMs();
      okO = stat(path, &sb) == 0;
      g_aOutMs += portMonoMs() - t0;
      g_aOut++;
    }
    if (!okO) return false; /* output: stat segar, jangan memo */
    mtimeNs = wsCacheStampNs(path, &sb);
    fsize = (long long)sb.st_size;
  } else if (!wsCacheInputStamp(path, &mtimeNs, &fsize)) {
    return false;
  }
  return wsCacheAddEntry(pf, st, proj, path, type, mtimeNs, fsize);
}

/* Entri 'O' dari memo stat gerbang no-op (audit 4/11 item 1): proyek tanpa
   kompilasi run ini — .o/.d pasti tak ditulis, stamp gate masih valid.
   Pemanggil wajib menjamin file tak tersentuh sejak gate (flag compiled). */
static bool wsCacheAddOMemo(FILE *pf, CsStore *st, const char *path,
                            long long mtimeNs, long long fsize) {
  return wsCacheAddEntry(pf, st, csHash("workspace"), path, 'O', mtimeNs, fsize);
}

static bool wsCacheAddEntry(FILE *pf, CsStore *st, uint64_t proj, const char *path, char type,
                            long long mtimeNs, long long fsize) {
  CsEntry e = {0}; /* wajib: payload harus NULL — csPut menyalinnya (strdup) */
  e.proj = proj;
  e.file = csHash(path);
  e.time = mtimeNs;
  if (type == 'D') {
    e.size = 0;
    e.content = 0;
  } else {
    e.size = fsize;
    CsEntry prev;
    if (g_prevFp && csGet(g_prevFp, proj, e.file, &prev) && prev.time == e.time &&
        prev.size == e.size && e.time < g_prevFpMtime) {
      e.content = prev.content; /* metadata cocok & tidak racy: isi tidak berubah */
    } else {
      e.content = wsStartupHash(path);
    }
    if (!e.content) return false;
  }
  if (type == 'O') {
    CsEntry existing;
    /* Kunci sudah tercatat dengan nilai identik: duplikat dalam run yang sama
       ATAU entri lama build selektif yang tidak berubah — lewati tanpa baris
       baru (baris P lamanya tetap berlaku lewat aturan merge). Nilai BERBEDA
       (output ditulis ulang run ini) -> timpa: catatan = kenyataan disk. */
    if (csGet(st, proj, e.file, &existing) && existing.content == e.content &&
        existing.time == e.time && existing.size == e.size)
      return true;
  }
  fprintf(pf, "%016llx %c %016llx %lld %lld %s\n", (unsigned long long)e.file, type,
          (unsigned long long)e.content, e.time, e.size, path);
  return csPut(st, &e);
}
/* Direktori induk sebuah path relatif ("a/src/main.c" -> "a/src", "main.c" -> "."). */
static void wsCacheDirOf(const char *path, char *out, size_t n) {
  snprintf(out, n, "%s", path);
  char *slash = strrchr(out, '/');
  if (slash) *slash = '\0';
  else snprintf(out, n, ".");
}
/* Dependensi header dari file .d (hasil -MMD): setiap token sesudah ':' dicatat
   sebagai entri H, divalidasi seperti sumber. 0 = .d tak ada, -1 = gagal. */
static int wsCacheAddDepFile(FILE *pf, CsStore *st, uint64_t proj, const char *dfile) {
  double t0 = portMonoMs();
  FILE *f = fopen(dfile, "rb");
  if (!f) return errno == ENOENT ? 0 : -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return -1;
  }
  char *buf = malloc((size_t)n + 2);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  g_aDRead += portMonoMs() - t0;
  g_aD++;
  buf[got] = '\0';
  for (size_t i = 0; i + 1 < got; i++) /* sambung baris: backslash+newline -> spasi */
    if (buf[i] == '\\' && (buf[i + 1] == '\n' || buf[i + 1] == '\r')) {
      buf[i] = ' ';
      buf[i + 1] = ' ';
    }
  char *colon = strchr(buf, ':');
  bool ok = colon != NULL;
  for (char *tok = ok ? strtok(colon + 1, " \t\r\n") : NULL; ok && tok; tok = strtok(NULL, " \t\r\n")) {
    /* -MP menulis baris target kosong "header:" setelah aturan utama; buang ':'
       agar path-nya sama dengan dependensi yang sudah tercatat. */
    size_t tl = strlen(tok);
    if (tl && tok[tl - 1] == ':') tok[tl - 1] = '\0';
    if (!tok[0]) continue;
    CsEntry probe;
    if (csGet(st, proj, csHash(tok), &probe)) continue; /* sudah tercatat */
    g_aH++;
    double ts0 = portMonoMs();
    long long hm = 0, hsz = 0;
    bool hok = wsCacheInputStamp(tok, &hm, &hsz);
    g_aHStat += portMonoMs() - ts0;
    if (!hok) {
      ok = false;
      break;
    }
    CsEntry e = {0};
    e.proj = proj;
    e.file = csHash(tok);
    e.time = hm;
    e.size = hsz;
    /* Reuse hash isi dari simpanan sebelumnya bila (time, size) cocok dan
       tidak racy — sama seperti wsCacheAdd. Header tak berubah tidak dibaca
       ulang: tiap fopen+read+hash header berbayar ~1.5 ms di mesin dengan
       stat/path-walk mahal (proot/Termux/LSM), ~44 header = ~60 ms/save. */
    CsEntry prev;
    if (g_prevFp && csGet(g_prevFp, proj, e.file, &prev) && prev.time == e.time &&
        prev.size == e.size && e.time < g_prevFpMtime) {
      e.content = prev.content; /* metadata cocok & tidak racy: isi tidak berubah */
    } else {
      double th0 = portMonoMs();
      e.content = wsStartupHash(tok);
      g_aHHash += portMonoMs() - th0;
      g_aHHashed++;
    }
    if (!e.content) {
      ok = false;
      break;
    }
    fprintf(pf, "%016llx H %016llx %lld %lld %s\n", (unsigned long long)e.file,
            (unsigned long long)e.content, e.time, e.size, tok);
    ok = csPut(st, &e);
  }
  free(buf);
  return ok ? 1 : -1;
}
/* Recursively inspect actual compiler objects. Generated embed objects have no
   compiler depfile; track their output and the generated embedded header.
   compiled=false (proyek tanpa kompilasi run ini — audit 4/11 item 1):
   .o/.d pasti tak ditulis, jadi stamp memo gerbang no-op (P-fast) masih
   valid — .d-cek tanpa stat segar, dan isi tak perlu di-hash ulang bila
   entri lama cocok. compiled=true: memo bisa basi (anak fork menulis di
   memorinya sendiri) — stat segar seperti semula. */
static bool wsCacheAddObjectTree(FILE *pf, CsStore *st, uint64_t proj,
                                 const char *dir, const char *root, unsigned depth,
                                 int *objects, bool compiled) {
  if (depth > 64) return false;
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  bool ok = true;
  for (int i = 0; ok && i < files.count; i++) {
    const char *o = files.items[i];
    size_t len = strlen(o);
    if (len < 3 || strcmp(o + len - 2, ".o")) continue;
    (*objects)++;
    char dfile[WS_PATH_LEN];
    snprintf(dfile, sizeof(dfile), "%.*s.d", (int)(len - 2), o);
    /* .d TIDAK berubah sejak save terakhir -> lewati: seluruh entri header
       yang bersumber darinya sudah tersalin di awal wsCacheSaveBody (audit
       30/10 — 10k fopen+read per save murni I/O ulang). Hanya .d yang
       digenerate ulang run ini yang dibaca. Stat LANGSUNG (bukan memo):
       .d ditulis oleh compiler di tengah run sehingga memo bisa basi —
       keputusan lewat/baca ini wajib memakai stamp segar. Pengecualian:
       proyek tanpa kompilasi (compiled=false) — dijamin tidak ada yang
       menulis .o/.d run ini, jadi stamp memo gate dipakai (item 1). */
    int64_t mO = -1;
    long long sO = -1;
    bool okO;
    if (!compiled && cmdsStatCached(o, &mO, &sO)) {
      /* stamp memo gate valid (file tak tersentuh run ini): tanpa stat.
         cmdsStatCached false = ia baru men-stat dan file hilang. */
      g_aOMemo++;
      okO = wsCacheAddOMemo(pf, st, o, (long long)mO, sO);
    } else if (compiled) {
      okO = wsCacheAdd(pf, st, proj, o, 'O'); /* memo basi: stat segar */
    } else {
      okO = false; /* file hilang (memo baru men-stat) */
    }
    if (!okO) {
      ok = false;
      break;
    }
    int64_t mD = -1;
    long long sD = -1;
    int dr;
    struct stat dst;
    bool dstat;
    long long dstamp = 0;
    if (!compiled && cmdsStatCached(dfile, &mD, &sD)) {
      dstat = true;
      dstamp = (long long)mD;
      g_aDSMemo++;
      if (!wsCacheAddOMemo(pf, st, dfile, dstamp, sD)) {
        ok = false;
        break;
      }
    } else if (compiled) {
      double td0 = portMonoMs();
      dstat = stat(dfile, &dst) == 0;
      g_aDSCheckMs += portMonoMs() - td0;
      g_aDSCheck++;
      /* Object dan .d-nya sendiri dicatat sebagai output ('O'). Noop memvalidasi
         stat-nya: object/.d yang hilang (build/ dihapus sebagian) membuat catatan
         tidak valid -> jalur penuh memulihkannya. Tanpa ini noop melaporkan
         'Nothing to build' padahal build/ tidak lengkap. */
      if (dstat) {
        if (!wsCacheAddSt(pf, st, proj, dfile, 'O', &dst)) {
          ok = false;
          break;
        }
      }
    } else {
      dstat = false; /* .d hilang (memo baru men-stat) */
    }
    if (!dstat) {
      dr = 0;
    } else if (dstamp < g_prevFpMtime) {
      g_aDSkip++;
      dr = 1;
    } else {
      dr = wsCacheAddDepFile(pf, st, proj, dfile);
    }
    /* Satu fopen per object: 0 (ENOENT) = .d tak ada -> cabang embedded di
       bawah; -1 (gagal baca/parse) = save batal konservatif. */
    if (dr > 0) ok = true;
    else if (dr < 0) ok = false;
    else {
      char header[WS_PATH_LEN];
      snprintf(header, sizeof(header), "%s/embedded.h", root);
      /* Only generated objects at the root of the project build tree
         may omit .d; missing depfiles for compiled C objects are errors. */
      ok = !strcmp(dir, root) && fsFileExists(header) &&
           wsCacheAdd(pf, st, proj, header, 'O') &&
           wsCacheAdd(pf, st, proj, o, 'O');
    }
  }
  for (int i = 0; ok && i < dirs.count; i++)
    ok = wsCacheAddObjectTree(pf, st, proj, dirs.items[i], root, depth + 1, objects, compiled);
  listFree(&dirs);
  listFree(&files);
  return ok;
}
static bool wsCacheAddProjectDeps(FILE *pf, CsStore *st, uint64_t proj, const char *name,
                                  bool compiled) {
  char dir[WS_PATH_LEN];
  snprintf(dir, sizeof(dir), "build/%s", name);
  int objects = 0;
  /* Proyek tanpa objek C (mis. archive-only) tidak punya dependensi header
     yang perlu dilacak; output-nya tetap dicek lewat entri O. */
  return wsCacheAddObjectTree(pf, st, proj, dir, dir, 0, &objects, compiled);
}
/* Archive-only projects have no C object/.d files. Track the real archive
   inputs recursively, plus the archive output, rather than requiring .o.
   A directory entry detects additions/deletions; file hashes detect edits. */
static bool wsCacheAddArchiveTree(FILE *pf, CsStore *st, uint64_t proj,
                                  const char *dir, unsigned depth) {
  if (depth > 64 || !wsCacheAdd(pf, st, proj, dir, 'D')) return false;
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  bool ok = true;
  for (int i = 0; ok && i < files.count; i++)
    ok = wsCacheAdd(pf, st, proj, files.items[i], 'S');
  for (int i = 0; ok && i < dirs.count; i++) {
    const char *sub = strrchr(dirs.items[i], '/');
    sub = sub ? sub + 1 : dirs.items[i];
    if (!strcmp(sub, "build") || !strcmp(sub, "dist") ||
        !strcmp(sub, ".git") || !strcmp(sub, ".satria") ||
        !strcmp(sub, "self_archive")) continue;
    ok = wsCacheAddArchiveTree(pf, st, proj, dirs.items[i], depth + 1);
  }
  listFree(&dirs);
  listFree(&files);
  return ok;
}

/* Stamp grup untuk metadata P (§4): content = hash konfigurasi + daftar
   sumber (wsProjStamp — sama dengan gate no-op), time = stempel ns save,
   size = jumlah entri grup. */
typedef struct {
  uint64_t proj, content;
  long long time, size;
} WsProjStamp;

static long long wsNowNs(void) {
#if defined(_WIN32)
  return (long long)time(NULL) * 1000000000LL;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

/* Metadata grup di opts.cache memakai stamp yang sama dengan fingerprint:
   router ikut terverifikasi tanpa membuka cache lain (§4). Gagal -> biarkan
   (format lama tetap sah; simpan berikutnya menulis ulang). */
static void wsOptsMetaSave(const WsProjStamp *st, int n) {
  CsStore *o = optsStoreShared();
  if (!o) o = optsStoreRepair();
  if (!o) return;
  for (int i = 0; i < n; i++)
    csProjMetaSet(o, st[i].proj, st[i].content, st[i].time, st[i].size);
  csCommit(o);
}

/* ==== Utilitas merge per proyek (mode selektif, O1) ==== */
typedef struct {
  uint64_t *v;
  int n, cap;
} WsU64Set;
static int wsU64Cmp(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}
static bool wsU64Add(WsU64Set *s, uint64_t x) {
  if (s->n == s->cap) {
    int nc = s->cap ? s->cap * 2 : 64;
    uint64_t *nv = (uint64_t *)realloc(s->v, (size_t)nc * sizeof(uint64_t));
    if (!nv) return false;
    s->v = nv;
    s->cap = nc;
  }
  s->v[s->n++] = x;
  return true;
}
static void wsU64Sort(WsU64Set *s) {
  if (s->n > 1) qsort(s->v, (size_t)s->n, sizeof(uint64_t), wsU64Cmp);
}
static bool wsU64Has(const WsU64Set *s, uint64_t x) {
  return s->n > 0 && bsearch(&x, s->v, (size_t)s->n, sizeof(uint64_t), wsU64Cmp) != NULL;
}
static void wsU64Free(WsU64Set *s) {
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}
/* Kunci 16 heksa di awal baris — format peta P maupun baris map mentah. */
static bool wsCacheKey16(const char *s, uint64_t *out) {
  char *end = NULL;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 16);
  if (errno || end != s + 16) return false;
  *out = (uint64_t)v;
  return true;
}
typedef struct {
  WsU64Set *s;
} WsKeyCtx;
static bool wsCacheKeyStore(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  return wsU64Add(((WsKeyCtx *)ctx)->s, e->file);
}
/* Entri simpanan lama proyek LUAR closure yang kuncinya juga ikut ditulis
   ulang run ini (header bersama) — kumpulkan untuk aturan merge di bawah. */
typedef struct {
  WsU64Set *fresh, *shared;
  const uint64_t *cloH;
  int cloHN;
  uint64_t wsProj;
} WsSharedCtx;
static bool wsCacheKeyShared(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  WsSharedCtx *c = (WsSharedCtx *)ctx;
  if (e->proj == c->wsProj) return true;
  if (c->cloHN && bsearch(&e->proj, c->cloH, (size_t)c->cloHN, sizeof(uint64_t), wsU64Cmp))
    return true;
  if (wsU64Has(c->fresh, e->file)) return wsU64Add(c->shared, e->file);
  return true;
}
/* Kunci pada file peta sementara (baris hasil write run ini = "kunci segar"). */
static bool wsCacheKeyCollectMap(const char *path, WsU64Set *out) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  char line[WS_PATH_LEN + 160];
  bool ok = true;
  while (ok && fgets(line, sizeof(line), f)) {
    uint64_t k;
    if (wsCacheKey16(line, &k)) ok = wsU64Add(out, k);
  }
  fclose(f);
  if (ok) wsU64Sort(out);
  return ok;
}
/* Entri grup closure yang dibuang sebelum walk segar. */
typedef struct {
  uint64_t proj, file;
} WsDelKey;
typedef struct {
  WsDelKey *v;
  int n, cap;
  const uint64_t *cloH;
  int cloHN;
} WsDelCtx;
static bool wsCacheDelCollect(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  WsDelCtx *c = (WsDelCtx *)ctx;
  if (!c->cloHN ||
      !bsearch(&e->proj, c->cloH, (size_t)c->cloHN, sizeof(uint64_t), wsU64Cmp))
    return true;
  if (c->n == c->cap) {
    int nc = c->cap ? c->cap * 2 : 64;
    WsDelKey *nv = (WsDelKey *)realloc(c->v, (size_t)nc * sizeof(WsDelKey));
    if (!nv) return false;
    c->v = nv;
    c->cap = nc;
  }
  c->v[c->n].proj = e->proj;
  c->v[c->n].file = e->file;
  c->n++;
  return true;
}

/* Simpan cache sesudah build sukses. Gagal -> kedua file dibuang (tidak ada cache palsu). */
void wsCacheSave(const WsModel *m, const char *wsName) {
  wsCacheSaveSel(m, wsName, NULL, 0);
}

/* Varian selektif (O1 — merge per proyek): lihat deklarasi di ws_internal.h. */
void wsCacheSaveSel(const WsModel *m, const char *wsName, const int *onlyIdx,
                    int onlyN) {
  struct stat pfs;
  /* Kunci memo stat: sama dengan gerbang no-op (root workspace) supaya stat
     input yang gerbang catat di awal run tidak dibayar ulang di sini
     (audit 30/10 — kandidat save-walk). */
  cmdsStatMemoSetCwd();
  g_prevFp = NULL;
  g_prevFpMtime = 0;
  g_aDRead = g_aHStat = g_aHHash = 0;
  g_aD = g_aH = g_aHHashed = g_aDSkip = 0;
  g_aDSMemo = g_aOMemo = 0;
  g_aInMs = g_aDSCheckMs = g_aOutMs = 0;
  g_aIn = g_aDSCheck = g_aOut = 0;
  wsPrevHFree();
  if (stat(WS_CACHE_FP, &pfs) == 0) {
    g_prevFpMtime = wsCacheStampNs(WS_CACHE_FP, &pfs);
    g_prevFp = csOpen(WS_CACHE_FP);
  }
  wsPrevHLoad(); /* daftar header save sebelumnya (disalin di wsCacheSaveBody) */
  g_selIdx = (onlyIdx && onlyN > 0) ? onlyIdx : NULL;
  g_selN = g_selIdx ? onlyN : 0;
  wsCacheSaveBody(m, wsName);
  g_selIdx = NULL;
  g_selN = 0;
  if (g_prevFp) csClose(g_prevFp);
  g_prevFp = NULL;
  g_prevFpMtime = 0;
  wsPrevHFree();
}

static void wsCacheSaveBody(const WsModel *m, const char *wsName) {
  mkdirs(WS_CACHE_DIR);
  bool sel = g_selN > 0;
  /* Build selektif TANPA simpanan sebelumnya (workspace belum pernah
     dibangun penuh): merge per proyek mustahil — buat fingerprint BARU yang
     hanya memuat entri closure (parsial) dan hapus fp.cover. Gerbang tetap
     menolak run penuh sampai semua proyek model punya stempel grup
     (wsCacheNoopValid: "coverage missing"); simpan penuh berikutnya menulis
     fp.cover. Dengan begitu run penuh PERTAMA setelah build selektif menutup
     cakupan (bila semua proyek sudah tercatat) bisa langsung HIT. */
  if (sel && !g_prevFp) wsCacheCoverWrite(false);
  if (!sel) remove(WS_CACHE_FP); /* mode selektif: muat simpanan lama (merge) */
  char tmpPaths[WS_PATH_LEN + 8];
  snprintf(tmpPaths, sizeof(tmpPaths), "%s.map.tmp", WS_CACHE_SOURCES);
  FILE *pf = fopen(tmpPaths, "w");
  if (!pf) return;
  CsStore *st = csOpen(WS_CACHE_FP); /* penuh: sudah dihapus -> kosong; selektif: muat merge */
  if (!st) {
    fclose(pf);
    remove(tmpPaths);
    return;
  }
  bool ok = true;
  uint64_t wsProj = csHash("workspace");
  uint64_t wKey = csHash(wsName);
  /* Mode selektif (merge per proyek — O1): tandai closure, lalu buang entri
     lamanya — walk segar di bawah menulis ulang sumber/header/direktori
     proyek closure. Entri proyek lain dan grup workspace TIDAK disentuh;
     output yang ikut ter-scan diperbarui lewat pembanding nilai di
     wsCacheAdd. */
  bool clo[WS_MAX_PROJECTS] = {0};
  uint64_t cloH[WS_MAX_PROJECTS];
  int cloHN = 0;
  if (sel) {
    for (int i = 0; i < g_selN; i++) {
      int idx = g_selIdx[i];
      if (idx >= 0 && idx < m->projectCount && !clo[idx]) {
        clo[idx] = true;
        cloH[cloHN++] = csHash(m->projects[idx].name);
      }
    }
    qsort(cloH, (size_t)cloHN, sizeof(uint64_t), wsU64Cmp);
    WsDelCtx del = {0};
    del.cloH = cloH;
    del.cloHN = cloHN;
    bool walked = csForEach(st, wsCacheDelCollect, &del);
    if (!walked) ok = false;
    for (int i = 0; ok && i < del.n; i++)
      if (!csDelete(st, del.v[i].proj, del.v[i].file)) ok = false;
    free(del.v);
  }
  /* Cek keberadaan kunci W dengan variabel bernama (bukan compound literal):
     literal C99 tidak didukung mode-C MSVC lama yang masih menjadi target
     port (design O7). Isinya tidak dipakai — hanya keberadaan entri. */
  CsEntry wsEnt;
  if (sel && csGet(st, wsProj, wKey, &wsEnt)) {
    /* Buildfile.ws TIDAK disentuh build selektif: stempel lama dipertahankan —
       konfigurasi bisa berubah bagi proyek di luar closure, jangan mengklaim
       valid. Baris P lamanya ikut bertahan lewat aturan merge di bawah. */
  } else {
    ok = ok && wsCacheAdd(pf, st, wsProj, wsName, 'W');
  }
  /* Salin entri header lama (audit 30/10): stamp di-stat ulang dari disk
     sehingga perubahan header sejak save terakhir tetap terdeteksi, dan isi
     hanya di-hash ulang bila (time,size) berbeda — aturan yang sama dengan
     jalur baca .d. Header yang sudah hilang dari disk dibuang (tidak lagi
     milik build mana pun); .d kotor yang masih merujuknya tetap dibaca dan
     memakai jalur konservatif lama (save batal) seperti biasa. */
  for (int i = 0; ok && i < g_prevHN; i++) {
    /* Mode selektif: entri header proyek LUAR closure sudah ada di store
       (dimuat dari simpanan) — tidak disalin ulang; stat header pun hanya
       dibayar bila minimal satu proyek closure memilikinya. */
    long long hm = 0, hsz = 0;
    bool statDone = false, statOk = false;
    for (int pi = 0; pi < m->projectCount; pi++) {
      if (sel && !clo[pi]) continue;
      uint64_t proj = csHash(m->projects[pi].name);
      CsEntry prev;
      if (!g_prevFp || !csGet(g_prevFp, proj, g_prevH[i].file, &prev)) continue;
      if (!statDone) {
        statOk = wsCacheInputStamp(g_prevH[i].path, &hm, &hsz);
        statDone = true;
      }
      if (!statOk) break; /* header hilang dari disk: lewati (perilaku lama) */
      /* (proj,file) sudah tersalin save ini -> jangan tulis baris kedua:
         baris P duplikat menggandakan stat di gate P-fast tiap run. */
      CsEntry taken;
      if (csGet(st, proj, g_prevH[i].file, &taken)) continue;
      CsEntry e = {0};
      e.proj = proj;
      e.file = g_prevH[i].file;
      e.time = hm;
      e.size = hsz;
      if (prev.time == hm && prev.size == hsz && hm < g_prevFpMtime) {
        e.content = prev.content; /* metadata cocok & tidak racy: tanpa baca ulang */
      } else {
        e.content = wsStartupHash(g_prevH[i].path);
        if (!e.content) {
          ok = false;
          break;
        }
      }
      fprintf(pf, "%016llx H %016llx %lld %lld %s\n", (unsigned long long)e.file,
              (unsigned long long)e.content, e.time, e.size, g_prevH[i].path);
      ok = csPut(st, &e);
    }
  }
  profMark("save-headers"); /* profil: salin entri H dari sources.cache lama */
  char dirPath[WS_PATH_LEN];
  for (int i = 0; ok && i < m->projectCount; i++) {
    if (sel && !clo[i]) continue; /* luar closure: entri lama dipertahankan */
    const char *name = m->projects[i].name;
    uint64_t proj = csHash(name);
    /* "Ada kompilasi" (audit 4/11 item 1): default konservatif = true (stat
       segar, perilaku lama) bila proyek tak tercatat di tabel pass finish. */
    bool comp = true, cv;
    if (cmdsWsPhaseCompiledGet(name, &cv)) comp = cv;
    List srcs = {0};
    bool archiveOnly = wsProjectArchiveOnly(&m->projects[i]);
    if (archiveOnly) {
      /* The project root and its contents are inputs to the archive. */
      ok = wsCacheAddArchiveTree(pf, st, proj, m->projects[i].root, 0);
      /* Require the generated archive, not an unrelated object file. */
      const char *archiveName = NULL, *archiveDir = NULL;
      for (int k = 0; k < m->projects[i].lines.count; k++) {
        const char *line = m->projects[i].lines.items[k];
        if (!strncmp(line, "archive.name = ", 15)) archiveName = line + 15;
        if (!strncmp(line, "archive.dir = ", 14)) archiveDir = line + 14;
      }
      if (!archiveName || !archiveDir) ok = false;
      if (ok) {
        char archive[WS_PATH_LEN];
        snprintf(archive, sizeof(archive), "%s/%s.tar", archiveDir, archiveName);
        ok = wsCacheAdd(pf, st, proj, archive, 'O');
        if (ok) artifactCacheAdd(archive); /* arsip tanpa kode C -> artifact.cache §9 */
      }
      if (!ok) break;
      continue;
    }
    if (!wsLogLoadSrcs(name, &srcs)) {
      ok = false;
      break;
    }
    for (int j = 0; ok && j < srcs.count; j++) {
      ok = wsCacheAdd(pf, st, proj, srcs.items[j], 'S');
      wsCacheDirOf(srcs.items[j], dirPath, sizeof(dirPath));
      CsEntry probe;
      if (ok && !csGet(st, proj, csHash(dirPath), &probe)) ok = wsCacheAdd(pf, st, proj, dirPath, 'D');
    }
    listFree(&srcs);
    if (ok) ok = wsCacheAddProjectDeps(pf, st, proj, name, comp);
    char out[WS_PATH_LEN];
    snprintf(out, sizeof(out), "bin/%s", name);
    if (ok && fsFileExists(out)) ok = wsCacheAdd(pf, st, proj, out, 'O');
    snprintf(out, sizeof(out), "lib/lib%s.a", name);
    if (ok && fsFileExists(out)) ok = wsCacheAdd(pf, st, proj, out, 'O');
    snprintf(out, sizeof(out), "lib/lib%s.so", name);
    if (ok && fsFileExists(out)) ok = wsCacheAdd(pf, st, proj, out, 'O');
  }
  /* Workspace outputs may be renamed by output aliases (e.g. core -> rupa).
     Record actual root bin/lib files, not only bin/<project-name>. */
  const char *outDirs[] = {"bin", "lib"};
  for (size_t d = 0; ok && d < 2; d++) {
    List dirs = {0}, files = {0};
    fsListDir(outDirs[d], &dirs, &files);
    for (int j = 0; ok && j < files.count; j++)
      ok = wsCacheAdd(pf, st, wsProj, files.items[j], 'O');
    listFree(&dirs);
    listFree(&files);
  }
  if (ok && fsFileExists("compile_commands.json")) ok = wsCacheAdd(pf, st, wsProj, "compile_commands.json", 'O');
  profMark("save-walk"); /* profil: stat+hash seluruh entri (sumber/header/objek/output) */
  if (profOn())
    fprintf(stderr,
            "[save-walk] .d: baca %d file %.1f ms (lewati %d) | header: %d stat %.1f ms, "
            "%d hash ulang %.1f ms | input: %d stat %.1f ms | .d-cek: %d stat %.1f ms | "
            "output: %d stat %.1f ms | memo gate: %d .d, %d O (item 1)\n",
            g_aD, g_aDRead, g_aDSkip, g_aH, g_aHStat, g_aHHashed, g_aHHash, g_aIn,
            g_aInMs, g_aDSCheck, g_aDSCheckMs, g_aOut, g_aOutMs, g_aDSMemo, g_aOMemo);
  /* Metadata grup P (§4): stamp per proyek + grup workspace (content = hash
     Buildfile.ws dari entri W). Disimpan SEBELUM commit agar baris P di
     fingerprint.cache memuat <content> <time> <size>. */
  WsProjStamp stamps[WS_MAX_PROJECTS + 1];
  int sn = 0;
  long long saveNs = wsNowNs();
  if (ok) {
    for (int i = 0; i < m->projectCount; i++) {
      if (sel && !clo[i]) continue; /* luar closure: stempel lama dipertahankan */
      uint64_t proj = csHash(m->projects[i].name);
      size_t cnt = csCountProj(st, proj);
      /* Grup kosong (proyek tanpa entri) TETAP distamp (size 0): stempel =
         penanda "pernah tercatat". Tanpa ini gerbang menolak run penuh
         selamanya untuk proyek tanpa entri (gate mewajibkan stempel).
         Baris P tetap tidak ditulis — tidak ada entri untuk dipetakan. */
      stamps[sn].proj = proj;
      stamps[sn].content = wsProjStamp(m, i);
      stamps[sn].time = saveNs;
      stamps[sn].size = (long long)cnt;
      sn++;
      /* state.cache ikut distamp dengan content/time yang sama (§4 seragam);
         state_blob memakai kunci nama proyek. */
      stateBlobProjMeta(m->projects[i].name, stamps[sn - 1].content, saveNs);
    }
    CsEntry we;
    if (csGet(st, wsProj, csHash(wsName), &we)) {
      stamps[sn].proj = wsProj;
      stamps[sn].content = we.content;
      stamps[sn].time = saveNs;
      stamps[sn].size = (long long)csCountProj(st, wsProj);
      sn++;
    }
    for (int i = 0; i < sn; i++)
      csProjMetaSet(st, stamps[i].proj, stamps[i].content, stamps[i].time, stamps[i].size);
  }
  if (fclose(pf) != 0) ok = false;
  if (ok) ok = csCommit(st);
  /* Tanda format fingerprint v2 (lihat WS_FP_FORMAT_MARK di ws_cache_noop.c). */
  if (ok) {
    FILE *fm = fopen(".satria/caches/fp.format", "w");
    if (fm) {
      fputs("2\n", fm);
      fclose(fm);
    }
    /* Cakupan penuh: simpan penuh mencatat SEMUA proyek model (stempel di
       atas menulis termasuk grup kosong) -> tulis fp.cover. Simpan selektif
       TIDAK menyentuh penanda: merge mempertahankan cakupan lama; fp parsial
       baru sudah menghapusnya di awal wsCacheSaveBody. */
    if (!sel) wsCacheCoverWrite(true);
  }
  profMark("save-commit"); /* profil: tulis fingerprint.cache */
  /* Gabungkan peta P ke sources.cache tanpa mengubah format SATRIASRC1.
     Tulis atomik: cache lama tidak boleh hilang jika penulisan gagal.
     Mode penuh: SEMUA baris P diganti peta segar.
     Mode selektif (merge per proyek — O1): baris lama dipertahankan kecuali
     kuncinya ditulis ulang run ini; kunci segar yang juga dimiliki proyek di
     LUAR closure (header bersama) memakai baris LAMA — P-fast menolak lalu
     jalur penuh menilai proyek luar closure terhadap isi header yang berubah,
     sehingga no-op penuh tidak salah mengklaim valid setelah build selektif
     menyentuh header bersama. */
  if (ok) {
    WsU64Set fresh = {0}, store = {0}, shared = {0}, kept = {0};
    if (sel) {
      ok = wsCacheKeyCollectMap(tmpPaths, &fresh);
      if (ok) {
        WsKeyCtx kc = {&store};
        ok = csForEach(st, wsCacheKeyStore, &kc);
        wsU64Sort(&store);
      }
      if (ok && g_prevFp) {
        WsSharedCtx sc = {&fresh, &shared, cloH, cloHN, wsProj};
        ok = csForEach(g_prevFp, wsCacheKeyShared, &sc);
        wsU64Sort(&shared);
      }
    }
    FILE *src = ok ? fopen(WS_CACHE_SOURCES, "r") : NULL;
    FILE *map = ok ? fopen(tmpPaths, "r") : NULL;
    char combined[WS_PATH_LEN + 16];
    snprintf(combined, sizeof(combined), "%s.merge.tmp", WS_CACHE_SOURCES);
    /* fp parsial baru boleh tanpa sources.cache lama (run pertama workspace):
       hasilnya baris P segar saja — jalur full mode tetap mensyaratkan src. */
    FILE *dst = (map && (src || sel)) ? fopen(combined, "w") : NULL;
    if (!dst) ok = false;
    if (ok) {
      char line[WS_PATH_LEN + 128];
      while (src && fgets(line, sizeof(line), src)) {
        if (line[0] == 'P' && line[1] == ' ') {
          if (!sel) continue; /* mode penuh: baris P datang dari peta segar */
          uint64_t k;
          if (wsCacheKey16(line + 2, &k)) {
            if (!wsU64Has(&store, k)) continue;        /* entri tak ada: buang */
            if (wsU64Has(&shared, k))
              wsU64Add(&kept, k); /* header bersama: baris lama menang */
            else if (wsU64Has(&fresh, k)) continue;    /* ditulis ulang run ini */
          }
        }
        if (fputs(line, dst) == EOF) {
          ok = false;
          break;
        }
      }
      wsU64Sort(&kept);
      while (ok && fgets(line, sizeof(line), map)) {
        if (sel) {
          uint64_t k;
          if (wsCacheKey16(line, &k) && wsU64Has(&kept, k)) continue;
        }
        if (fprintf(dst, "P %s", line) < 0) {
          ok = false;
          break;
        }
      }
    }
    if (src) fclose(src);
    if (map) fclose(map);
    if (dst && fclose(dst) != 0) ok = false;
    if (ok) ok = fsRenameReplace(combined, WS_CACHE_SOURCES);
    if (!ok) remove(combined);
    wsU64Free(&fresh);
    wsU64Free(&store);
    wsU64Free(&shared);
    wsU64Free(&kept);
  }
  profMark("save-merge"); /* profil: gabung baris P ke sources.cache */
  remove(tmpPaths);
  csClose(st);
  if (!ok) {
    remove(tmpPaths);
    if (!sel || !g_prevFp) {
      /* Penuh gagal — buang simpanan tak sah; selektif TANPA simpanan lama
         (fp parsial baru dibuat) juga dibuang supaya tidak menyisakan fp
         setengah jadi tanpa penanda cakupan.
         Jangan hapus sources.cache: indeks discovery dipakai kedua mode. */
      remove(WS_CACHE_FP);
      wsCacheCoverWrite(false);
      wsRouterDrop();
    }
    /* Mode selektif dengan simpanan lama: simpanan TIDAK dibuang — berisi
       entri proyek di luar closure — dan sources.cache tak tersentuh bila
       merge gagal. */
  } else {
    remove(".satria/caches/paths.cache"); /* bersihkan artefak legacy */
    wsRouterRecord();
    profMark("tail-router"); /* profil: tulis ulang router.cache */
    if (sn > 0) wsOptsMetaSave(stamps, sn); /* stamp grup di opts.cache (§4) */
    profMark("tail-opts"); /* profil: stamp grup di opts.cache */
    /* Metadata grup P seragam (§4): deps/embed/lib memakai content/time/skema
       yang sama dengan baris P di fingerprint.cache & opts.cache. Satu
       buka+tulis per file untuk SEMUA grup (batch) — loop per grup dulu
       membuka 4 file × sn kali per run. */
    if (sn > 0) {
      uint64_t projs[WS_MAX_PROJECTS + 1];
      uint64_t contents[WS_MAX_PROJECTS + 1];
      long long times[WS_MAX_PROJECTS + 1];
      for (int i = 0; i < sn; i++) {
        projs[i] = stamps[i].proj;
        contents[i] = stamps[i].content;
        times[i] = stamps[i].time;
      }
      csProjMetaStampN(projs, contents, times, sn);
    }
    profMark("tail-stamp"); /* profil: 4 cache file (deps/embed/fp/lib) */
    /* Grup CompDB global di state.cache (__satria_compdb__) ikut distamp
       dengan content/time grup workspace — penutup terakhir tanpa P meta. */
    if (sn > 0 && stamps[sn - 1].proj == wsProj)
      stateBlobProjMeta(WS_COMPDB_GLOBAL, stamps[sn - 1].content, stamps[sn - 1].time);
    /* Penuh: prune (entri tak lagi dirujuk ikut hilang); selektif: merge —
       pertahankan entri artefak proyek di luar closure (O1). */
    if (sel) artifactCacheSaveMerge();
    else artifactCacheSave(); /* flush metadata artefak/input release (§9) */
    profMark("tail-artifact"); /* profil: artifact.cache */
  }
  profMark("save-tail"); /* profil: router/opts/state.blob/artifact */
}

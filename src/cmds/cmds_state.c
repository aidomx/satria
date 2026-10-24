/*
 * cmds_state — state build lintas-run & command clean
 * (dipindah apa adanya dari commands.c).
 *
 *   - Fingerprint konfigurasi compile/link + set source
 *     (.satria/build.fingerprint): berubah -> rebuild penuh.
 *   - Fast no-op snapshot (.satria/build.state): mtime+size seluruh input;
 *     persis sama -> summary diulang dari cache, build dilewati total.
 *   - cmdClean: hapus artefak build (build dir, file library,
 *     compile_commands.json, deps.cache).
 */
#include "cmds_internal.h"
#include "../log/report_blob.h"
#include "../cache/artifact.h"
#include "../cache/state_blob.h"
#include "../cache/router.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cache/cache_store.h"
#include "../commands.h"
#include "../compdb.h"
#include "../deps.h"
#include "../compile.h"
#include "../embed_ld_cache.h"
#include "../portability.h"
#include "../util.h"

/* ==================== Lokasi file state (state_path) ====================
 *
 * Definisi & semantik ada di cmds_internal.h (bagian cmds_state.c).
 * Mode workspace: file per proyek di .satria/workspace/<proyek>.<leaf>;
 * mode single: .satria/<leaf> relatif cwd. */
static char g_stateWsProj[80] = "";

void cmdsStateSetWsProject(const char *name) {
  if (!name || !*name) {
    g_stateWsProj[0] = '\0';
    return;
  }
  snprintf(g_stateWsProj, sizeof(g_stateWsProj), "%s", name);
}

const char *cmdsStateWsProject(void) { return g_stateWsProj[0] ? g_stateWsProj : NULL; }

void cmdsStatePath(char *out, size_t n, const char *leaf) {
  if (g_stateWsProj[0])
    snprintf(out, n, ".satria/workspace/%s.%s", g_stateWsProj, leaf);
  else
    snprintf(out, n, ".satria/caches/%s", leaf);
}

void cmdsStateMkdirFor(const char *path) {
  char dir[256];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) *slash = '\0';
  mkdirs(dir[0] ? dir : ".satria");
}

/* ==================== Fingerprint build state ==================== */

static uint64_t fpBytes(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t fpString(uint64_t h, const char *s) {
  const unsigned char sep = 0xff;
  if (s) h = fpBytes(h, s, strlen(s));
  return fpBytes(h, &sep, 1);
}

static uint64_t fpList(uint64_t h, const List *list) {
  for (int i = 0; i < list->count; i++)
    h = fpString(h, list->items[i]);
  const unsigned char end = 0xfe;
  return fpBytes(h, &end, 1);
}

void cmdsFingerprintMake(const Config *c, const List *srcs, CmdsBuildFingerprint *fp) {
  fp->compile = 14695981039346656037ULL;
  fp->compile = fpString(fp->compile, c->cc);
  /* std selalu terisi configFinalize sesuai bahasa (gnu11/c++17), jadi
     peralihan bahasa proyek otomatis mengubah fingerprint compile. */
  fp->compile = fpString(fp->compile, c->std);
  fp->compile = fpString(fp->compile, c->target);
  fp->compile = fpString(fp->compile, c->outBuildDir);
  fp->compile = fpList(fp->compile, &c->flags);
  /* defines mempengaruhi hasil kompilasi sama seperti flags (design/flags.md). */
  fp->compile = fpList(fp->compile, &c->defines);
  fp->compile = fpList(fp->compile, &c->headerPublic);
  /* headers.internal: dipantau tanpa -I; perubahan daftarnya ikut memicu build. */
  if (c->headerInternal.count) fp->compile = fpList(fp->compile, &c->headerInternal);
  /* Shared library objects need -fPIC, so this changes compile output. */
  fp->compile = fpString(fp->compile, c->libShared ? "pic" : "no-pic");

  fp->sources = 14695981039346656037ULL;
  for (int i = 0; i < srcs->count; i++) {
    char obj[MAX_PATH];
    fp->sources = fpString(fp->sources, srcs->items[i]);
    if (objectPathFor(c, srcs->items[i], obj, sizeof(obj)))
      fp->sources = fpString(fp->sources, obj);
  }

  fp->link = 14695981039346656037ULL;
  fp->link = fpString(fp->link, c->cc);
  fp->link = fpString(fp->link, c->outBinaryDir);
  fp->link = fpString(fp->link, c->outBinaryName);
  fp->link = fpList(fp->link, &c->libraries);
  fp->link = fpString(fp->link, c->libRequested ? "lib" : "no-lib");
  fp->link = fpString(fp->link, c->libStatic ? "static" : "no-static");
  fp->link = fpString(fp->link, c->libShared ? "shared" : "no-shared");
  fp->link = fpString(fp->link, c->outLibDir);
  fp->link = fpString(fp->link, c->outLibName);
  for (int i = 0; i < c->embCount; i++) {
    fp->link = fpString(fp->link, c->emb[i].enable ? "enabled" : "disabled");
    fp->link = fpString(fp->link, c->emb[i].objectPath);
  }
}

/* Build fingerprint is a metadata record in the shared fingerprint cache.
   Project address differs by execution path, storage format does not. */
#define BUILD_FP_CACHE ".satria/caches/fingerprint.cache"
#define BUILD_FP_KEY "<build-fingerprint-v1>"

bool cmdsFingerprintLoad(CmdsBuildFingerprint *fp) {
  const char *wsp = cmdsStateWsProject();
  if (wsp) {
    char *b = NULL;
    size_t bn = 0;
    if (!stateBlobGet(wsp, "build.fingerprint", &b, &bn)) return false;
    unsigned long long c = 0, l = 0, src = 0;
    bool ok = sscanf(b, "%llx %llx %llx", &c, &l, &src) == 3;
    free(b);
    if (!ok) return false;
    fp->compile = c; fp->link = l; fp->sources = src;
    return true;
  }
  CsStore *st = csOpen(BUILD_FP_CACHE);
  if (!st) return false;
  CsEntry e;
  bool found = csGet(st, cmdsCacheProjAddr(), csHash(BUILD_FP_KEY), &e);
  unsigned long long c = 0, l = 0, s = 0;
  bool ok = found && e.payload && sscanf(e.payload, "%llx %llx %llx", &c, &l, &s) == 3;
  csClose(st);
  if (!ok) return false;
  fp->compile = (uint64_t)c;
  fp->link = (uint64_t)l;
  fp->sources = (uint64_t)s;
  return true;
}

void cmdsFingerprintSave(const CmdsBuildFingerprint *fp, bool libOnly) {
  CmdsBuildFingerprint out = *fp;
  if (libOnly) {
    CmdsBuildFingerprint prev;
    if (cmdsFingerprintLoad(&prev)) {
      out.sources = prev.sources;
      out.link = prev.link;
    }
  }
  const char *wsp = cmdsStateWsProject();
  if (wsp) {
    char txt[80];
    snprintf(txt, sizeof(txt), "%016llx %016llx %016llx\n",
             (unsigned long long)out.compile, (unsigned long long)out.link,
             (unsigned long long)out.sources);
    stateBlobPut(wsp, "build.fingerprint", txt, strlen(txt));
    return;
  }
  mkdirs(".satria/caches");
  CsStore *st = csOpen(BUILD_FP_CACHE);
  if (!st) return;
  char value[80];
  snprintf(value, sizeof(value), "%016llx %016llx %016llx",
           (unsigned long long)out.compile, (unsigned long long)out.link,
           (unsigned long long)out.sources);
  CsEntry e = {cmdsCacheProjAddr(), csHash(BUILD_FP_KEY), 0, 0, 0, value};
  if (csPut(st, &e)) csCommit(st);
  csClose(st);
}

void cmdsFingerprintInvalidate(void) {
  const char *wsp = cmdsStateWsProject();
  if (wsp) { stateBlobDelete(wsp, "build.fingerprint"); return; }
  CsStore *st = csOpen(BUILD_FP_CACHE);
  if (!st) return;
  csDelete(st, cmdsCacheProjAddr(), csHash(BUILD_FP_KEY));
  csCommit(st);
  csClose(st);
}

/* ==================== Fast no-op snapshot ==================== */
/* Dua file state per fase build:
   - build.state     : fase binary (satria / -w fase 2) — target = binary.
   - build.lib.state : fase library workspace (cmdBuildEx libOnly) —
     target = library proyek (atau tanpa target bila proyek tak meminta
     library; state-nya murni "semua input & object masih sama").
   Dengan state fase library, no-op `satria -w` tidak membayar loadConfig +
   scan fingerprint + decide penuh per proyek (miniws: jalur ringan). */
/* FAST5: baris stamp kini membawa hash isi
   (<kind> <mtime> <size> <content> <path>) agar "sentuh tanpa perubahan isi"
   bisa divalidasi (hash sama) lalu di-PATCH, tanpa rebuild (design §4).
   State FAST4 lama ditolak -> cold sekali, lalu ditulis ulang (self-heal). */
#define BUILD_STATE_MAGIC "SATRIAFAST5"

typedef struct {
  char kind;
  char path[MAX_PATH];
  int64_t mtime;
  long long size;
  uint64_t content; /* hash isi (F); 0 = tak terhitung / bukan file */
} FastStamp;

typedef struct {
  char cwd[MAX_PATH];
  char buildfile[MAX_PATH];
  int64_t buildfileMtime;
  long long buildfileSize;
  char target[MAX_PATH * 2];
  int64_t targetMtime;
  long long targetSize;
  int compiled;
  int skipped;
  int count;
  int phase; /* 0 = binary pass, 1 = library pass */
  FastStamp *stamps;
  /* Ringkasan library: no-op berikutnya tetap menampilkan status lib. */
  bool libRequested, libStaticUp, libSharedUp;
  char libStaticPath[MAX_PATH * 2];
  char libSharedPath[MAX_PATH * 2];
  long long libStaticSize, libSharedSize;
} FastState;

static void fastStateFree(FastState *s) {
  free(s->stamps);
  memset(s, 0, sizeof(*s));
}

static bool fastStampAdd(List *paths, const char *path) {
  for (int i = 0; i < paths->count; i++)
    if (strcmp(paths->items[i], path) == 0) return true;
  return listAdd(paths, path);
}

static void fastCollectTree(const char *dir, List *paths) {
  if (!fsDirExists(dir)) return;
  fastStampAdd(paths, dir);
  List dirs = {0}, files = {0};
  listReserve(&dirs, 1024);
  listReserve(&files, 1024);
  fsListDir(dir, &dirs, &files);
  for (int i = 0; i < files.count; i++)
    fastStampAdd(paths, files.items[i]);
  for (int i = 0; i < dirs.count; i++)
    fastCollectTree(dirs.items[i], paths);
  listFree(&dirs);
  listFree(&files);
}

/* Header yang benar-benar dikonsumsi source harus ikut fast-state walaupun
   berada di luar sources/includeDirs/headerInternal. Pakai graph dependensi
   yang sama dengan keputusan incremental (depfile .d bila tersedia, scanner
   sebagai fallback), bukan menelusuri semua direktori header secara global. */
static void fastCollectDepPath(const char *path, void *ctx) {
  List *paths = (List *)ctx;
  if (path && *path) fastStampAdd(paths, path);
}

/* FNV-1a 64 atas isi file (streaming 4KB) — padanan wsStartupHash.
   Dipakai bukti sentuh: stamp beda tapi hash sama -> entri tetap valid dan
   stamp di-PATCH, tanpa rebuild (design/startup.md §4). 0 = tak terbaca. */
static uint64_t fastStateHash(const char *path) {
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

/* Pemapilan fastStateHash untuk validator workspace/lib (cmds_wsstate.c):
   bukti sentuh di jalur incremental lama memakai hash yang sama. */
uint64_t cmdsStateFileHash(const char *path) { return fastStateHash(path); }

/*
 * Pemindai state berbasis buffer (dipakai baik oleh jalur mmap maupun
 * fallback fopen): fscanf-file -> scanf-buffer. Format persis sama dengan
 * fastStateLoad lama; hanya sumbernya yang berbeda.
 *
 * Return true bila seluruh field valid. *endOut = pointer MISALNYA ke
 * karakter setelah last token yang dikonsumsi (dipakai utk sanity check).
 */
static bool fastStateScan(const char *buf, size_t size, FastState *s) {
  memset(s, 0, sizeof(*s));
  /* scanf buffer: buat buffer NUL-terminated. State txt kecil (<~16MB). */
  char *tmp = malloc(size + 1);
  if (!tmp) return false;
  memcpy(tmp, buf, size);
  tmp[size] = '\0';

  char magic[32];
  long long bfm = 0, tm = 0;
  int libReq = 0, libSt = 0, libSh = 0, phase = 0;
  const char *cur = tmp;
  int consumed = 0;
  bool ok =
      sscanf(cur, "%31s", magic) == 1 && strcmp(magic, BUILD_STATE_MAGIC) == 0;
  consumed += magic[0] ? (int)strlen(magic) : 0;
  if (ok) {
    consumed += 1;
    cur += consumed;
    ok = sscanf(
            cur,
            "%1023s %1023s %lld %lld %2047s %lld %lld %d %d %d %d %d %d %d %2047s %2047s %lld %lld%n",
            s->cwd, s->buildfile, &bfm, &s->buildfileSize, s->target, &tm, &s->targetSize,
            &s->compiled, &s->skipped, &s->count, &phase, &libReq, &libSt, &libSh,
            s->libStaticPath, s->libSharedPath, &s->libStaticSize, &s->libSharedSize,
            &consumed) == 18 &&
        s->count >= 0 && s->count <= 200000;
  }
  if (ok) {
    cur += consumed;
    s->buildfileMtime = (int64_t)bfm;
    s->targetMtime = (int64_t)tm;
    s->phase = phase;
    s->libRequested = libReq != 0;
    s->libStaticUp = libSt != 0;
    s->libSharedUp = libSh != 0;
    /* placeholder "-" = varian library tidak aktif (path kosong tidak bisa
       dibaca %s) */
    if (strcmp(s->libStaticPath, "-") == 0) s->libStaticPath[0] = '\0';
    if (strcmp(s->libSharedPath, "-") == 0) s->libSharedPath[0] = '\0';
    s->stamps = calloc((size_t)s->count, sizeof(FastStamp));
    if (s->count && !s->stamps) ok = false;
  }
  for (int i = 0; ok && i < s->count; i++) {
    long long mt = 0, sz = 0;
    unsigned long long content = 0;
    if (sscanf(cur, " %c %lld %lld %16llx %4095s%n", &s->stamps[i].kind, &mt, &sz,
               &content, s->stamps[i].path, &consumed) != 5) {
      fastStateFree(s);
      ok = false;
      break;
    }
    cur += consumed;
    s->stamps[i].mtime = (int64_t)mt;
    s->stamps[i].size = sz;
    s->stamps[i].content = (uint64_t)content;
  }
  free(tmp);
  return ok;
}

static bool fastStateLoadLib(FastState *s); /* definisi di bagian lib.cache */

/* ---- Snapshot fast state single-project di fingerprint.cache ----
   Sama seperti build fingerprint: satu record per cwd (alamat proyek = hash
   cwd) di BUILD_FP_CACHE. Payload record tidak boleh memuat '\n', jadi isi
   snapshot di-escape: '\\' -> "\\\\", '\n' -> "\\n", '\r' -> "\\r". */
static char *fastSnapEscape(const char *s, size_t n, size_t *out) {
  char *o = malloc(n * 2 + 1);
  if (!o) return NULL;
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\\') { o[j++] = '\\'; o[j++] = '\\'; }
    else if (c == '\n') { o[j++] = '\\'; o[j++] = 'n'; }
    else if (c == '\r') { o[j++] = '\\'; o[j++] = 'r'; }
    else o[j++] = c;
  }
  o[j] = '\0';
  *out = j;
  return o;
}

static char *fastSnapUnescape(const char *s, size_t n, size_t *out) {
  char *o = malloc(n + 1);
  if (!o) return NULL;
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n) {
      char d = s[++i];
      o[j++] = d == 'n' ? '\n' : d == 'r' ? '\r' : d;
    } else {
      o[j++] = s[i];
    }
  }
  o[j] = '\0';
  *out = j;
  return o;
}

static uint64_t fastSnapFile(void) { return csHash("build.state"); }

static bool fastSnapGet(char **buf, size_t *len) {
  CsStore *st = csOpen(BUILD_FP_CACHE);
  if (!st) return false;
  CsEntry e;
  bool ok = false;
  if (csGet(st, cmdsCacheProjAddr(), fastSnapFile(), &e) && e.payload && *e.payload) {
    char *u = fastSnapUnescape(e.payload, strlen(e.payload), len);
    if (u) {
      *buf = u;
      ok = true;
    }
  }
  csClose(st);
  return ok;
}

static bool fastSnapPut(const char *buf, size_t len) {
  size_t el = 0;
  char *esc = fastSnapEscape(buf, len, &el);
  if (!esc) return false;
  CsStore *st = csOpen(BUILD_FP_CACHE);
  bool ok = false;
  if (st) {
    CsEntry e = {cmdsCacheProjAddr(), fastSnapFile(), 0, 0, (long long)len, esc};
    ok = csPut(st, &e) && csCommit(st);
    csClose(st);
  }
  free(esc);
  return ok;
}

static void fastSnapDelete(void) {
  CsStore *st = csOpen(BUILD_FP_CACHE);
  if (!st) return;
  csDelete(st, cmdsCacheProjAddr(), fastSnapFile());
  csCommit(st);
  csClose(st);
}

static bool fastStateLoad(FastState *s, bool libOnly) {
  /* Fase library kini dari caches/lib.cache (design/startup.md §3). */
  if (libOnly) return fastStateLoadLib(s);
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return false;
  char *buf = NULL;
  size_t len = 0;
  if (!fastSnapGet(&buf, &len)) return false;
  bool ok = fastStateScan(buf, len, s);
  free(buf);
  return ok;
}

/* Placeholder "-" = target/varian library tidak aktif (path kosong tidak
   bisa dibaca %s). Target "-" sah untuk state fase library proyek tanpa
   library: validasi murni dari stamp input + object. */
static bool fastTargetRecorded(const char *target) {
  return target[0] && strcmp(target, "-") != 0;
}

/* ==================== memo stat per proses ====================
   Validasi fast-state dipanggil per fase (library lalu binary) dalam satu
   proses dan memeriksa daftar stamp yang nyaris identik. Hasil stat disimpan
   di sini supaya fase kedua tidak men-stat ulang path yang sama. Memo hanya
   sah selama tidak ada build di antaranya: cmdBuildEx memanggil
   cmdsStatMemoReset tepat sebelum build dimulai. */
typedef struct {
  char *path;
  bool exists;
  int64_t mtime;
  long long size;
} StatMemoEntry;
static StatMemoEntry *statMemo = NULL;
static size_t statMemoCap = 0, statMemoCount = 0;
/* cwd saat validasi. Path relatif (mis. "src") berarti direktori berbeda di
   tiap proyek, jadi kunci memo harus menyertakan cwd agar tidak tertukar. */
static char statMemoCwd[MAX_PATH * 2];

static uint64_t statMemoHash(const char *s) {
  uint64_t h = 14695981039346656037ULL;
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 1099511628211ULL;
  }
  return h;
}

static void statMemoRehash(size_t newCap) {
  StatMemoEntry *fresh = calloc(newCap, sizeof(StatMemoEntry));
  if (!fresh) return; /* gagal alokasi: lewati memo, pemanggil men-stat langsung */
  StatMemoEntry *old = statMemo;
  size_t oldCap = statMemoCap;
  statMemo = fresh;
  statMemoCap = newCap;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].path) continue;
    size_t j = statMemoHash(old[i].path) & (newCap - 1);
    while (statMemo[j].path) j = (j + 1) & (newCap - 1);
    statMemo[j] = old[i];
  }
  free(old);
}

/* Kunci memo = path absolut ternormalisasi secara leksikal (buang "." dan
   "..", tanpa menyentuh symlink). Dengan begitu "../library/a.h" dari proyek
   core dan dari proyek bade menjadi satu entri, sehingga tidak di-stat dua kali. */
static void memoKeyFor(const char *path, char *out, size_t n) {
  char joined[MAX_PATH * 4];
  if (path[0] == '/' || statMemoCwd[0] == '\0')
    snprintf(joined, sizeof(joined), "%s", path);
  else
    snprintf(joined, sizeof(joined), "%s/%s", statMemoCwd, path);

  char work[MAX_PATH * 4];
  snprintf(work, sizeof(work), "%s", joined);
  const char *seg[256];
  int ns = 0;
  char *savePtr = NULL;
  for (char *tok = strtok_r(work, "/", &savePtr); tok; tok = strtok_r(NULL, "/", &savePtr)) {
    if (strcmp(tok, ".") == 0 || tok[0] == '\0') continue;
    if (strcmp(tok, "..") == 0) {
      if (ns > 0) ns--;
      continue;
    }
    if (ns >= 256) {
      snprintf(out, n, "%s", joined); /* terlalu dalam: pakai path apa adanya */
      return;
    }
    seg[ns++] = tok;
  }
  size_t used = 0;
  out[used++] = '/';
  out[used] = '\0';
  for (int i = 0; i < ns; i++) {
    size_t len = strlen(seg[i]);
    if (used + len + 2 > n) {
      snprintf(out, n, "%s", joined);
      return;
    }
    memcpy(out + used, seg[i], len);
    used += len;
    out[used++] = '/';
    out[used] = '\0';
  }
  if (used > 1) out[used - 1] = '\0'; /* buang '/' terakhir */
}

static bool memoStampNsSize(const char *path, int64_t *mtimeNs, long long *size) {
  char key[MAX_PATH * 4];
  memoKeyFor(path, key, sizeof(key));

  if (statMemoCap == 0 || (statMemoCount + 1) * 2 > statMemoCap)
    statMemoRehash(statMemoCap ? statMemoCap * 2 : 4096);
  if (statMemoCap == 0) return fsStampNsSize(path, mtimeNs, size);

  size_t j = statMemoHash(key) & (statMemoCap - 1);
  while (statMemo[j].path && strcmp(statMemo[j].path, key) != 0)
    j = (j + 1) & (statMemoCap - 1);
  StatMemoEntry *e = &statMemo[j];
  if (e->path) {
    if (!e->exists) return false;
    *mtimeNs = e->mtime;
    *size = e->size;
    return true;
  }
  bool ok = fsStampNsSize(path, mtimeNs, size);
  /* Hanya hasil positif yang disimpan: file yang belum ada bisa dibuat build,
     jadi negatif selalu di-stat ulang. */
  if (ok) {
    char *dup = strdup(key);
    if (dup) {
      e->path = dup;
      e->exists = true;
      e->mtime = *mtimeNs;
      e->size = *size;
      statMemoCount++;
    }
  }
  return ok;
}

/* Memo stat untuk validator workspace (cmds_wsstate.c): kunci dan semantik sama
   dengan validasi single project, sehingga fase library dan binary berbagi hasil. */
/* Kunci memo memakai cwd; pemanggil di luar cmdsFastStateValid (validator ws)
   wajib menyetelnya dulu, jika tidak entri proyek lain bisa terpakai. */
void cmdsStatMemoSetCwd(void) {
  if (!fsGetCwd(statMemoCwd, sizeof(statMemoCwd))) statMemoCwd[0] = '\0';
}

bool cmdsStatCached(const char *path, int64_t *mtimeNs, long long *size) {
  return memoStampNsSize(path, mtimeNs, size);
}

/* Parser baris format state F/D/X (format bersama ws.state dan workspace.state
   lama — lihat header cmds_wsstate.c). Dipakai validator di cmds_wsstate.c
   agar format hidup di SATU tempat. Baris F opsional membawa content16hex
   (bukti sentuh; lihat cmdsParseStateEntry). */
bool cmdsParseStateEntry(const char *line, char *kind, long long *mt, long long *sz,
                         unsigned long long *content, char *path) {
  char k = line[0];
  path[0] = '\0';
  *mt = 0;
  *sz = 0;
  *content = 0;
  if (k == 'X') {
    if (sscanf(line, "X %*d %*d %" WS_SCAN "s", path) != 1) return false;
  } else if (k == 'D') {
    if (sscanf(line, "D %lld %*d %" WS_SCAN "s", mt, path) != 2) return false;
  } else if (k == 'F' || k == 'O' || k == 'I') {
    /* Bentuk lama: F <mt> <sz> <path>. Bentuk baru (bukti sentuh, design §4):
       F <mt> <sz> <content16hex> <path> — path tidak pernah mengandung spasi
       (guard yang sama dengan seluruh format teks state), jadi jumlah token
       membedakan keduanya secara unik. */
    long long m2 = 0, s2 = 0;
    char a[WS_PATH_LEN], b[WS_PATH_LEN];
    int n = sscanf(line, "%*c %lld %lld %" WS_SCAN "s %" WS_SCAN "s", &m2, &s2, a, b);
    if (n == 4) {
      if (sscanf(a, "%16llx", content) != 1) return false;
      snprintf(path, WS_PATH_LEN, "%s", b);
    } else if (n == 3) {
      snprintf(path, WS_PATH_LEN, "%s", a);
    } else {
      return false;
    }
    *mt = m2;
    *sz = s2;
  } else {
    return false;
  }
  *kind = k;
  return true;
}

/* true bila baris F/D/X masih cocok stamp disk saat ini: F = mtime+size sama,
   D = mtime sama, X = path masih tidak ada. False bila parse gagal atau stamp
   beda. Memakai memo stat (cmdsStatCached) — pemanggil validator wajib sudah
   menyetel cwd memo (cmdsStatMemoSetCwd) bila lintas proyek. */
bool cmdsStateEntryFresh(const char *line) {
  char kind = 0;
  long long mt = 0, sz = 0;
  unsigned long long content = 0;
  char p[WS_PATH_LEN];
  int64_t cm = -1;
  long long cs = -1;
  if (!cmdsParseStateEntry(line, &kind, &mt, &sz, &content, p)) return false;
  bool exists = kind == 'I' ? fsStampNsSize(p, &cm, &cs) : cmdsStatCached(p, &cm, &cs);
  if (kind == 'X') return !exists;
  if (!exists) return false;
  if (kind == 'D') return cm == (int64_t)mt;
  if (cm == (int64_t)mt && cs == sz) return true;
  /* Output O tetap stamp-based; tidak hash semua object/output pada setiap
     penyimpanan state. Input library lintas-proyek I menyimpan hash konten:
     perubahan stamp/ukuran yang tidak mengubah byte bukan perubahan input
     linker dan tidak boleh memaksa relink. Format lama tanpa hash konservatif. */
  if (kind == 'O') return false;
  if (kind == 'I' && !content) return false;
  if (content && cs == sz) {
    uint64_t h = fastStateHash(p);
    if (h && h == (uint64_t)content) return true;
  }
  return false;
}

void cmdsStatMemoReset(void) {
  for (size_t i = 0; i < statMemoCap; i++) free(statMemo[i].path);
  free(statMemo);
  statMemo = NULL;
  statMemoCap = 0;
  statMemoCount = 0;
}

/* ==================== caches/lib.cache (design/startup.md §3/§4) ====================
   Fast-state FASE LIBRARY (dulu .satria/build.lib.state untuk single dan
   .satria/workspace/<proyek>.ws.lib.state untuk workspace) kini berbagi
   SATU file terpusat `.satria/caches/lib.cache` (cache_store), dibedakan
   lewat alamat proyek: satu entri meta (payload token k=v) + satu entri per
   input (payload "<kind> <path>"; kind F file / D direktori / M hilang /
   L verdict ld). Fase binary (build.state / ws.state) TETAP memakai format
   lamanya — migrasinya lewat fingerprint.cache (default on; off via
   SATRIA_CACHE_NOOP=0).
   File lama fase library tidak lagi dibaca/ditulis; clean tetap membuangnya. */

/* Kunci entri meta (mustahil tabrakan dengan csHash(path) file nyata). */
static uint64_t libMetaAddr(void) { return csHash("<libmeta>"); }

uint64_t cmdsCacheProjAddr(void) {
  const char *name = cmdsStateWsProject();
  if (name && *name) return csHash(name);
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return 0;
  return csHash(cwd);
}

typedef struct {
  uint64_t *v;
  size_t n, cap;
  uint64_t proj;
} LibKeyCtx;

static bool libKeysCollect(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  LibKeyCtx *ctx = ctxp;
  if (e->proj != ctx->proj) return true;
  if (ctx->n == ctx->cap) {
    size_t nc = ctx->cap ? ctx->cap * 2 : 256;
    uint64_t *nv = realloc(ctx->v, nc * sizeof(uint64_t));
    if (!nv) return false;
    ctx->v = nv;
    ctx->cap = nc;
  }
  ctx->v[ctx->n++] = e->file;
  return true;
}

/* Payload meta: token "k=v" dipisah spasi (path tanpa spasi — guard yang
   sama dengan format teks lama). lib=<path> diulang untuk daftar library. */
static char *libMetaPayload(const LibCacheMeta *m) {
  size_t cap = (strlen(m->cwd) + strlen(m->buildfile) + strlen(m->target) +
                strlen(m->libStaticPath) + strlen(m->libSharedPath)) *
                   2 +
               4096;
  for (int i = 0; i < m->nlibs; i++) cap += strlen(m->libs[i]) + 8;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  snprintf(buf, cap,
           "v1 cwd=%s bf=%s bfm=%lld bfs=%lld tgt=%s tm=%lld ts=%lld "
           "skipped=%d compiled=%d libReq=%d libStUp=%d libShUp=%d "
           "libSt=%s libSh=%s stSz=%lld shSz=%lld",
           m->cwd, m->buildfile, m->bfm, m->bfs, m->target[0] ? m->target : "-", m->tm,
           m->ts, m->skipped, m->compiled, m->libRequested ? 1 : 0, m->libStaticUp ? 1 : 0,
           m->libSharedUp ? 1 : 0, m->libStaticPath[0] ? m->libStaticPath : "-",
           m->libSharedPath[0] ? m->libSharedPath : "-", m->libStaticSize, m->libSharedSize);
  size_t used = strlen(buf);
  for (int i = 0; i < m->nlibs; i++) {
    int w = snprintf(buf + used, cap - used, " lib=%s", m->libs[i]);
    if (w < 0 || (size_t)w >= cap - used) break;
    used += (size_t)w;
  }
  return buf;
}

bool libCacheSave(const LibCacheMeta *m, const LibStampRec *stamps, int n) {
  mkdirs(WS_CACHES_DIR);
  CsStore *st = csOpen(LIB_CACHE_FILE);
  if (!st) {
    remove(LIB_CACHE_FILE); /* rusak: tulis ulang bersih (self-healing) */
    st = csOpen(LIB_CACHE_FILE);
  }
  if (!st) return false;
  uint64_t proj = cmdsCacheProjAddr();

  LibKeyCtx keys = {NULL, 0, 0, proj};
  bool collected = csForEach(st, libKeysCollect, &keys);
  if (collected)
    for (size_t i = 0; i < keys.n; i++) csDelete(st, proj, keys.v[i]);
  free(keys.v);
  if (!collected) {
    csClose(st);
    return false;
  }

  char *meta = libMetaPayload(m);
  if (meta) {
    CsEntry e = {proj, libMetaAddr(), 0, 0, 0, meta};
    csPut(st, &e);
    free(meta);
  }
  for (int i = 0; i < n; i++) {
    /* F membawa content16hex (bukti sentuh, design §4); D/M/L tetap "<kind> <path>". */
    char payload[WS_PATH_LEN + 32];
    if (stamps[i].kind == 'F')
      snprintf(payload, sizeof(payload), "%c %016llx %s", stamps[i].kind,
               (unsigned long long)stamps[i].content, stamps[i].path);
    else
      snprintf(payload, sizeof(payload), "%c %s", stamps[i].kind, stamps[i].path);
    CsEntry e = {proj, csHash(stamps[i].path), 0, stamps[i].mtime, stamps[i].size, payload};
    csPut(st, &e);
  }
  bool ok = csCommit(st);
  csClose(st);
  return ok;
}

typedef struct {
  LibStampRec *v;
  size_t n, cap;
  uint64_t proj;
  uint64_t metaAddr;
} LibStampCtx;

static bool libStampVisit(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  LibStampCtx *ctx = ctxp;
  if (e->proj != ctx->proj || e->file == ctx->metaAddr || !e->payload) return true;
  if (!e->payload[0] || e->payload[1] != ' ') return true;
  char kind = e->payload[0];
  if (kind != 'F' && kind != 'D' && kind != 'M' && kind != 'L') return true;
  if (ctx->n == ctx->cap) {
    size_t nc = ctx->cap ? ctx->cap * 2 : 128;
    LibStampRec *nv = realloc(ctx->v, nc * sizeof(LibStampRec));
    if (!nv) return false;
    ctx->v = nv;
    ctx->cap = nc;
  }
  LibStampRec *r = &ctx->v[ctx->n++];
  r->kind = kind;
  r->content = 0;
  if (kind == 'F') {
    /* F baru: "<kind> <content16hex> <path>"; lama: "<kind> <path>". */
    char a[WS_PATH_LEN], b[WS_PATH_LEN];
    int ntok = sscanf(e->payload + 2, "%" WS_SCAN "s %" WS_SCAN "s", a, b);
    if (ntok == 2 && sscanf(a, "%16llx", &r->content) == 1)
      snprintf(r->path, sizeof(r->path), "%s", b);
    else
      snprintf(r->path, sizeof(r->path), "%s", a);
  } else {
    snprintf(r->path, sizeof(r->path), "%s", e->payload + 2);
  }
  r->mtime = e->time;
  r->size = e->size;
  return true;
}

bool libCacheLoad(LibCacheMeta *m, LibStampRec **stampsOut, int *nOut) {
  memset(m, 0, sizeof(*m));
  *stampsOut = NULL;
  *nOut = 0;
  CsStore *st = csOpen(LIB_CACHE_FILE);
  if (!st) return false;
  uint64_t proj = cmdsCacheProjAddr();
  CsEntry meta;
  if (!csGet(st, proj, libMetaAddr(), &meta) || !meta.payload) {
    csClose(st);
    return false;
  }

  char *copy = strdup(meta.payload);
  if (!copy) {
    csClose(st);
    return false;
  }
  int libsCap = 0;
  for (char *save = NULL, *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
    char *eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = '\0';
    const char *k = tok, *v = eq + 1;
    if (strcmp(k, "cwd") == 0) snprintf(m->cwd, sizeof(m->cwd), "%s", v);
    else if (strcmp(k, "bf") == 0) snprintf(m->buildfile, sizeof(m->buildfile), "%s", v);
    else if (strcmp(k, "bfm") == 0) m->bfm = atoll(v);
    else if (strcmp(k, "bfs") == 0) m->bfs = atoll(v);
    else if (strcmp(k, "tgt") == 0) {
      if (strcmp(v, "-") != 0) snprintf(m->target, sizeof(m->target), "%s", v);
    } else if (strcmp(k, "tm") == 0) m->tm = atoll(v);
    else if (strcmp(k, "ts") == 0) m->ts = atoll(v);
    else if (strcmp(k, "skipped") == 0) m->skipped = atoi(v);
    else if (strcmp(k, "compiled") == 0) m->compiled = atoi(v);
    else if (strcmp(k, "libReq") == 0) m->libRequested = atoi(v) != 0;
    else if (strcmp(k, "libStUp") == 0) m->libStaticUp = atoi(v) != 0;
    else if (strcmp(k, "libShUp") == 0) m->libSharedUp = atoi(v) != 0;
    else if (strcmp(k, "libSt") == 0) {
      if (strcmp(v, "-") != 0) snprintf(m->libStaticPath, sizeof(m->libStaticPath), "%s", v);
    } else if (strcmp(k, "libSh") == 0) {
      if (strcmp(v, "-") != 0) snprintf(m->libSharedPath, sizeof(m->libSharedPath), "%s", v);
    } else if (strcmp(k, "stSz") == 0) m->libStaticSize = atoll(v);
    else if (strcmp(k, "shSz") == 0) m->libSharedSize = atoll(v);
    else if (strcmp(k, "lib") == 0) {
      if (m->nlibs == libsCap) {
        int nc = libsCap ? libsCap * 2 : 4;
        char **nv = realloc(m->libs, (size_t)nc * sizeof(char *));
        if (!nv) break;
        m->libs = nv;
        libsCap = nc;
      }
      m->libs[m->nlibs] = strdup(v);
      if (!m->libs[m->nlibs]) break;
      m->nlibs++;
    }
  }
  free(copy);

  LibStampCtx ctx = {NULL, 0, 0, proj, libMetaAddr()};
  bool walked = csForEach(st, libStampVisit, &ctx);
  csClose(st);
  if (!walked || ctx.n == 0) {
    free(ctx.v);
    libCacheMetaFree(m);
    return false;
  }
  *stampsOut = ctx.v;
  *nOut = (int)ctx.n;
  return true;
}

void libCacheMetaFree(LibCacheMeta *m) {
  if (!m) return;
  for (int i = 0; i < m->nlibs; i++) free(m->libs[i]);
  free(m->libs);
  m->libs = NULL;
  m->nlibs = 0;
}

/* Muat fase library dari caches/lib.cache ke FastState — validasi & cetak
   di bawah tetap sama persis dengan jalur teks lama. */
static bool fastStateLoadLib(FastState *s) {
  LibCacheMeta m;
  LibStampRec *stamps = NULL;
  int n = 0;
  memset(s, 0, sizeof(*s));
  if (!libCacheLoad(&m, &stamps, &n)) return false;
  copyStr(s->cwd, sizeof(s->cwd), m.cwd);
  copyStr(s->buildfile, sizeof(s->buildfile), m.buildfile);
  s->buildfileMtime = m.bfm;
  s->buildfileSize = m.bfs;
  copyStr(s->target, sizeof(s->target), m.target[0] ? m.target : "-");
  s->targetMtime = m.tm;
  s->targetSize = m.ts;
  s->compiled = m.compiled;
  s->skipped = m.skipped;
  s->count = n;
  s->phase = 1;
  s->libRequested = m.libRequested;
  s->libStaticUp = m.libStaticUp;
  s->libSharedUp = m.libSharedUp;
  copyStr(s->libStaticPath, sizeof(s->libStaticPath), m.libStaticPath);
  copyStr(s->libSharedPath, sizeof(s->libSharedPath), m.libSharedPath);
  s->libStaticSize = m.libStaticSize;
  s->libSharedSize = m.libSharedSize;
  s->stamps = calloc((size_t)n, sizeof(FastStamp));
  if (!s->stamps) {
    free(stamps);
    memset(s, 0, sizeof(*s));
    return false;
  }
  for (int i = 0; i < n; i++) {
    s->stamps[i].kind = stamps[i].kind;
    snprintf(s->stamps[i].path, sizeof(s->stamps[i].path), "%s", stamps[i].path);
    s->stamps[i].mtime = stamps[i].mtime;
    s->stamps[i].size = stamps[i].size;
    s->stamps[i].content = stamps[i].content; /* 0 untuk entri lama (kompat mundur) */
  }
  free(stamps);
  return true;
}

/* Tulis ulang build.state dari fast-state tervalidasi — jalur PATCH sentuh
   (mtime berubah, isi sama): perbarui stamp di tempat supaya run berikutnya
   tidak meng-hash ulang. Gagal tulis tidak masalah: run berikutnya cukup
   meng-hash file tersentuh lagi. */
static void fastStateWriteFrom(const FastState *s) {
  size_t cap = 4096;
  for (int i = 0; i < s->count; i++) cap += strlen(s->stamps[i].path) + 96;
  char *buf = malloc(cap);
  if (!buf) return;
  int written = snprintf(buf, cap,
          "%s\n%s %s %lld %lld %s %lld %lld %d %d %d %d %d %d %d %s %s %lld %lld\n",
          BUILD_STATE_MAGIC, s->cwd, s->buildfile, (long long)s->buildfileMtime,
          s->buildfileSize, s->target[0] ? s->target : "-", (long long)s->targetMtime,
          s->targetSize, s->compiled, s->skipped, s->count, s->phase,
          s->libRequested ? 1 : 0, s->libStaticUp ? 1 : 0, s->libSharedUp ? 1 : 0,
          s->libStaticPath[0] ? s->libStaticPath : "-",
          s->libSharedPath[0] ? s->libSharedPath : "-", s->libStaticSize,
          s->libSharedSize);
  bool ok = written > 0 && (size_t)written < cap;
  size_t used = ok ? (size_t)written : 0;
  for (int i = 0; ok && i < s->count; i++) {
    written = snprintf(buf + used, cap - used, "%c %lld %lld %016llx %s\n",
                       s->stamps[i].kind, (long long)s->stamps[i].mtime,
                       s->stamps[i].size, (unsigned long long)s->stamps[i].content,
                       s->stamps[i].path);
    ok = written > 0 && (size_t)written < cap - used;
    if (ok) used += (size_t)written;
  }
  if (ok) {
    fastSnapPut(buf, used);
  }
  free(buf);
}

bool cmdsFastStateValid(const char *buildfilePath, bool libOnly) {
  if (!fsGetCwd(statMemoCwd, sizeof(statMemoCwd))) statMemoCwd[0] = '\0';
  /* Router (opts.cache) dibaca lebih dulu: cache yang dirutekan hilang /
     kosong -> batal cepat tanpa memuat build.state. */
  if (!optsRouterValidate()) return false;
  /* Metadata artefak (design/startup.md §9): file hilang/berubah -> batal. */
  if (!artifactCacheValid()) return false;
  FastState s;
  if (!fastStateLoad(&s, libOnly)) return false;
  if (s.phase != (libOnly ? 1 : 0)) {
    fastStateFree(&s);
    return false;
  }
  char cwd[MAX_PATH];
  int64_t buildfileMtime = -1, targetMtime = -1;
  long long buildfileSize = -1, targetSize = -1;
  bool buildfileStamp = memoStampNsSize(s.buildfile, &buildfileMtime, &buildfileSize);
  bool targetStamp =
      fastTargetRecorded(s.target) ? memoStampNsSize(s.target, &targetMtime, &targetSize) : true;
  bool targetOk = !fastTargetRecorded(s.target) ||
                  (targetStamp && targetMtime == s.targetMtime && targetSize == s.targetSize);
  bool ok =
      fsGetCwd(cwd, sizeof(cwd)) && strcmp(cwd, s.cwd) == 0 &&
      strcmp(buildfilePath && *buildfilePath ? buildfilePath : "Buildfile", s.buildfile) == 0 &&
      buildfileStamp && buildfileMtime == s.buildfileMtime && buildfileSize == s.buildfileSize &&
      targetOk;
  bool patched = false; /* sentuh: stamp di-PATCH setelah seluruh entri lolos */
  for (int i = 0; ok && i < s.count; i++) {
    FastStamp *st = &s.stamps[i];
    int64_t currentMtime = -1;
    long long currentSize = -1;
    if (st->kind == 'L') {
      /* SATRIA_NO_LD sengaja memaksa jalur normal/fallback; jangan biarkan
         fastState menelan override itu. */
      char ldPath[MAX_PATH];
      bool cached = false;
      if (getenv("SATRIA_NO_LD") || !ldCacheResolve("ld", ldPath, sizeof(ldPath)) ||
          !cacheCheckLD(ldPath, &cached)) {
        ok = false;
        break;
      }
      continue;
    }
    bool exists = memoStampNsSize(st->path, &currentMtime, &currentSize);
    if (st->kind == 'M') {
      if (exists) {
        ok = false;
        break;
      }
      continue;
    }
    if (!exists || currentMtime != st->mtime ||
        (st->kind == 'F' && currentSize != st->size)) {
      /* Sentuh: stamp beda tapi ISI sama -> buktikan via hash lalu PATCH
         stamp; tanpa rebuild. Ukuran wajib sama; folder (D) tak punya hash
         -> tetap batal (add/remove folder memang harus rebuild). */
      bool touchOk = false;
      if (st->kind == 'F' && st->content && currentSize == st->size) {
        uint64_t h = fastStateHash(st->path);
        if (h && h == st->content) {
          st->mtime = currentMtime;
          st->size = currentSize;
          patched = true;
          touchOk = true;
        }
      }
      if (!touchOk) {
        ok = false;
        break;
      }
    }
  }
  if (ok && s.phase != 1 && reportBlobHas()) {
    /* No-op binary: putar laporan tersimpan (design §10) — blok STATUS di
       dalamnya adalah pesan no-op. Tanpa laporan tersimpan -> jalur di bawah. */
    reportBlobShow();
  } else if (ok) {
    if (s.phase == 1) {
      /* Fase library: tidak ada link binary — lapor status library saja. */
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("%s Library : %s%s\n", repPrefix(), s.libStaticPath,
                 s.libStaticUp ? " (up-to-date)" : "");
        if (s.libSharedPath[0])
          printf("%s Library : %s%s\n", repPrefix(), s.libSharedPath,
                 s.libSharedUp ? " (up-to-date)" : "");
      }
    } else {
      printf("%s Build with cached state (fingerprint unchanged)\n", repPrefix());
      printf("%s Linking : %s (up-to-date)\n", repPrefix(), s.target);
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("%s Library : %s%s\n", repPrefix(), s.libStaticPath,
                 s.libStaticUp ? " (up-to-date)" : "");
        if (s.libSharedPath[0])
          printf("%s Library : %s%s\n", repPrefix(), s.libSharedPath,
                 s.libSharedUp ? " (up-to-date)" : "");
      }
    }
    printf("\n%s Summary :\n", repPrefix());
    if (s.phase == 1) {
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("    Library  : %s (%.1fKB)\n", s.libStaticPath,
                 s.libStaticSize > 0 ? (double)s.libStaticSize / 1024.0 : 0);
        if (s.libSharedPath[0])
          printf("    Library  : %s (%.1fKB)\n", s.libSharedPath,
                 s.libSharedSize > 0 ? (double)s.libSharedSize / 1024.0 : 0);
      }
    } else {
      printf("    Target   : %s\n", s.target);
      printf("    Size     : %.1fKB\n", s.targetSize > 0 ? (double)s.targetSize / 1024.0 : 0);
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("    Library  : %s (%.1fKB)\n", s.libStaticPath,
                 s.libStaticSize > 0 ? (double)s.libStaticSize / 1024.0 : 0);
        if (s.libSharedPath[0])
          printf("    Library  : %s (%.1fKB)\n", s.libSharedPath,
                 s.libSharedSize > 0 ? (double)s.libSharedSize / 1024.0 : 0);
      }
    }
    printf("    Compiled : %d\n", s.compiled);
    printf("    Skipped  : %d\n", s.skipped);
    printf("    Status   : Success\n");
    /* No-op single menutup dengan pesan yang sama dengan workspace (§10):
       putar laporan tersimpan (STATUS = pesan no-op) atau cetak mandiri. */
    if (s.phase != 1) {
      if (reportBlobHas())
        reportBlobShow();
      else
        printf("STATUS\n  > Nothing to build\n  > Cache valid: %s\n", BUILD_FP_CACHE);
    }
  }
  if (ok && patched) fastStateWriteFrom(&s); /* PATCH sentuh: perbarui stamp */
  fastStateFree(&s);
  return ok;
}

/* Stamp proyek single (paralel `wsProjStamp` workspace, §4): hash isi
   Buildfile + daftar sumber terurut (koleksi sudah kanonik) — dipakai
   sebagai content metadata grup P di opts.cache (optsProjMetaStamp). */
static uint64_t fastProjStamp(const char *bf, const List *srcs) {
  uint64_t h = 1469598103934665603ULL;
  h ^= fastStateHash(bf);
  h *= 1099511628211ULL;
  if (srcs)
    for (int i = 0; i < srcs->count; i++) {
      const unsigned char *p = (const unsigned char *)srcs->items[i];
      for (; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
      }
      h ^= 0xff;
      h *= 1099511628211ULL;
    }
  return h;
}

void cmdsFastStateSave(const Config *c, const List *srcs, const char *target,
                       const char *buildfilePath, bool libOnly) {
  /* Fast path hanya aman bila root build sama dengan cwd. Untuk root khusus,
     gunakan jalur normal sampai tersedia normalisasi path absolut. */
  if (strcmp(c->root, ".") != 0) {
    char key[MAX_PATH];
    if (fsGetCwd(key, sizeof(key))) {
      fastSnapDelete();
    }
    return;
  }
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  const char *bf = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  int64_t bfm = fsMTimeNs(bf);
  long long bfs = fsFileSize(bf);
  /* Fase library proyek tanpa library tidak punya target: simpan "-" —
     validasi murni dari stamp input + object (lihat fastTargetRecorded). */
  char targetBuf[MAX_PATH * 2];
  const char *tgt = target;
  if (!tgt || !tgt[0]) {
    copyStr(targetBuf, sizeof(targetBuf), "-");
    tgt = targetBuf;
  }
  int64_t tm = 0;
  long long ts = 0;
  if (fastTargetRecorded(tgt)) {
    tm = fsMTimeNs(tgt);
    ts = fsFileSize(tgt);
    if (tm < 0 || ts < 0) return;
  }
  if (bfm < 0 || bfs < 0) return;
  /* Format cache is whitespace-delimited; disable it for paths with spaces. */
  if (strpbrk(cwd, " \t\r\n") || strpbrk(bf, " \t\r\n") || strpbrk(tgt, " \t\r\n")) return;

  /* Library: status up-to-date saat build sukses selesai — ditampilkan
     lagi pada no-op berikutnya (dulu informasi ini hilang di fast path). */
  char staticLib[MAX_PATH * 2] = {0}, sharedLib[MAX_PATH * 2] = {0};
  bool staticUp = true, sharedUp = true;
  long long staticSz = 0, sharedSz = 0;
  if (c->libRequested) {
    if (c->libStatic) {
      cmdsLibStaticPath(c, staticLib, sizeof(staticLib));
      staticUp = cmdsLibVariantUpToDate(c, true, srcs);
      long long s1 = fsFileSize(staticLib);
      if (s1 < 0) return; /* lib diwajibkan tapi belum ada — jangan simpan */
      staticSz = s1;
    }
    if (c->libShared) {
      cmdsLibSharedPath(c, sharedLib, sizeof(sharedLib));
      sharedUp = cmdsLibVariantUpToDate(c, false, srcs);
      long long s2 = fsFileSize(sharedLib);
      if (s2 < 0) return;
      sharedSz = s2;
    }
  }

  List paths = {0};
  for (int i = 0; i < c->sources.count; i++)
    fastCollectTree(c->sources.items[i], &paths);
  List incDirs = {0};
  includeDirs(c, &incDirs);
  for (int i = 0; i < incDirs.count; i++) {
    const char *d = incDirs.items[i];
    if (d && *d && strcmp(d, ".") != 0) fastCollectTree(d, &paths);
  }
  listFree(&incDirs);
  /* headers.internal: folder header dipantau sebagai input jalur cepat, tapi
     TIDAK masuk includeDirs (bukan -I), sehingga tidak membayangi header sistem. */
  for (int i = 0; i < c->headerInternal.count; i++) {
    const char *d = c->headerInternal.items[i];
    if (d && *d && d[0] != '-' && strcmp(d, ".") != 0) fastCollectTree(d, &paths);
  }
  /* Direktori di atas hanya discovery kasar. Lengkapi dengan closure header
     aktual tiap translation unit agar include relatif seperti ../shared.h,
     computed include, dan header transitif di luar root tetap membatalkan
     fast-state. depsNew menggunakan cache edges/.d dan fallback scanner. */
  if (srcs && srcs->count > 0) {
    DepCache *deps = depsNew(c);
    if (deps) {
      /* depsNew hanya menginisialisasi konteks; closure tidak menemukan root
         sampai graph source dimuat. Seed tiap source melalui resolver yang
         sama dengan incremental compile (cache .d/edge lebih dulu). */
      for (int i = 0; i < srcs->count; i++)
        (void)depsNewestHeaderMTime(deps, srcs->items[i]);
      depsClosureVisitMany(deps, (const char *const *)srcs->items, srcs->count,
                           fastCollectDepPath, &paths);
      depsFree(deps);
    } else {
      /* Gagal membentuk graph: jangan simpan fast-state yang bisa melewatkan
         header; kembali ke build normal pada run berikutnya. */
      listFree(&paths);
      return;
    }
  }
  /* Include file inputs that are not necessarily inside source/header roots. */
  fastStampAdd(&paths, bf);
  fastStampAdd(&paths, ".satria-version");
  if (compdbEnabled(c) && fsFileExists("compile_commands.json"))
    fastStampAdd(&paths, "compile_commands.json");
  for (int i = 0; i < srcs->count; i++) {
    /* Objek tidak lagi distamp: no-op hanya memeriksa sumber, header, dan
       output akhir (binary/library). Stat per-objek membuat no-op ~2x lebih
       banyak panggilan filesystem tanpa menambah kebenaran output. */
  }
  bool hasEmbedded = false;
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    hasEmbedded = true;
    fastStampAdd(&paths, c->emb[i].objectPath);
    /* embedded.<n>.file adalah input eksternal proyek ini. Tanpa dicatat
       di fast-state, perubahan archive dapat terlewat sebelum cmdBuild()
       sempat menjalankan fase embed/link normal. Arsip yang DIHASILKAN
       (generate) juga dicatat: walkDir koleksi source proyek (jalur
       commands.c) tidak menyentuh folder archiveDir, jadi tanpa ini
       perubahan archive tidak membatalkan fast path. */
    if (c->emb[i].archivePath[0]) fastStampAdd(&paths, c->emb[i].archivePath);
  }
  /* Embedded bergantung pada verdict ld lintas-run. Tandai cache dengan
     kind khusus agar fastState memvalidasi ld.cache -> ld, bukan sekadar
     mtime file cache. Bila cache tidak valid/hilang, fast path gugur dan
     build normal akan mem-probe lalu memperbarui cache. */
  if (hasEmbedded) fastStampAdd(&paths, ldCacheFile());
  /* Library masuk stamp seperti file biasa ('F'): berubah -> fast path
     batal dan jalur normal meng-rebuild varian yang perlu (per-varian). */
  if (staticLib[0]) fastStampAdd(&paths, staticLib);
  if (sharedLib[0]) fastStampAdd(&paths, sharedLib);
  /* Library path-form (mis. `library = ../x/lib/libx.a`) adalah input link:
     perubahan arsip eksternal wajib membatalkan fast state agar relink
     terdeteksi (audit ws-benchmark 2026-10-15). Hanya fase binary — kemasan
     fase library hanya dari object milik proyek sendiri. */
  if (!libOnly)
    for (int i = 0; i < c->libraries.count; i++) {
      const char *lib = c->libraries.items[i];
      if (strchr(lib, '/') || strchr(lib, '\\')) fastStampAdd(&paths, lib);
    }

  FastStamp *stamps = calloc((size_t)paths.count, sizeof(FastStamp));
  if (paths.count && !stamps) {
    listFree(&paths);
    return;
  }
  int count = 0;
  bool safePaths = true;
  for (int i = 0; i < paths.count; i++) {
    const char *path = paths.items[i];
    if (strlen(path) >= sizeof(stamps[count].path) || strpbrk(path, " \t\r\n")) {
      safePaths = false;
      break;
    }
    /* Satu stat per path: mtime + ukuran sekaligus (fsStampNsSize). */
    int64_t mt = 0;
    long long sz = 0;
    if (!fsStampNsSize(path, &mt, &sz)) {
      mt = -1;
      sz = 0;
    }
    stamps[count].kind = strcmp(path, ldCacheFile()) == 0 && hasEmbedded
                             ? 'L'
                             : (mt < 0 ? 'M' : (fsDirExists(path) ? 'D' : 'F'));
    snprintf(stamps[count].path, sizeof(stamps[count].path), "%s", path);
    stamps[count].mtime = mt;
    stamps[count].size = stamps[count].kind == 'F' ? sz : 0;
    /* Hash isi file F: bukti sentuh pada validasi berikutnya (PATCH tanpa
       rebuild). Gagal hash -> 0 -> sentuh diperlakukan sebagai perubahan. */
    stamps[count].content = stamps[count].kind == 'F' ? fastStateHash(path) : 0;
    count++;
  }
  if (!safePaths) {
    free(stamps);
    listFree(&paths);
    return;
  }
  listFree(&paths);
  /* Fase library: simpan ke caches/lib.cache (design/startup.md §3) —
     entri meta + stamp per proyek, bukan file per fase. */
  if (libOnly) {
    LibCacheMeta m;
    memset(&m, 0, sizeof(m));
    snprintf(m.cwd, sizeof(m.cwd), "%s", cwd);
    snprintf(m.buildfile, sizeof(m.buildfile), "%s", bf);
    m.bfm = (long long)bfm;
    m.bfs = bfs;
    snprintf(m.target, sizeof(m.target), "%s", tgt); /* "-" bila tanpa target */
    m.tm = (long long)tm;
    m.ts = ts;
    m.skipped = srcs->count;
    m.compiled = 0;
    m.libRequested = c->libRequested;
    m.libStaticUp = staticUp;
    m.libSharedUp = sharedUp;
    snprintf(m.libStaticPath, sizeof(m.libStaticPath), "%s", staticLib);
    snprintf(m.libSharedPath, sizeof(m.libSharedPath), "%s", sharedLib);
    m.libStaticSize = staticSz;
    m.libSharedSize = sharedSz;
    LibStampRec *recs = calloc(count ? (size_t)count : 1, sizeof(LibStampRec));
    if (recs) {
      for (int i = 0; i < count; i++) {
        recs[i].kind = stamps[i].kind;
        snprintf(recs[i].path, sizeof(recs[i].path), "%s", stamps[i].path);
        recs[i].mtime = (long long)stamps[i].mtime;
        recs[i].size = stamps[i].size;
        recs[i].content = stamps[i].content; /* FNV-1a isi F: bukti sentuh */
      }
      libCacheSave(&m, recs, count);
      free(recs);
    }
    if (optsRouterRecord(c, libOnly)) { /* router: deps + lib (fase library) */
      /* Grup P (§4) seragam: opts + semua cache grup (deps/embed/lib/state)
         memakai content & format identik — jejak lintas-cache & lintas mode. */
      uint64_t content = fastProjStamp(bf, srcs);
      long long t = csNowNs();
      optsProjMetaStamp(cmdsCacheProjAddr(), content); /* grup P (§4) */
      csProjMetaStamp(cmdsCacheProjAddr(), content, t); /* P seragam: deps/embed/lib */
      csProjMetaStampFile(BUILD_FP_CACHE, cmdsCacheProjAddr(), content, t); /* P seragam: fingerprint.cache */
    }
    free(stamps);
    return;
  }
  /* Serialize the unchanged FAST4 payload into a hashed state.cache record. */
  size_t cap = 4096;
  for (int i = 0; i < count; i++) cap += strlen(stamps[i].path) + 96;
  char *buf = malloc(cap);
  if (!buf) { free(stamps); return; }
  int written = snprintf(buf, cap,
          "%s\n%s %s %lld %lld %s %lld %lld %d %d %d %d %d %d %d %s %s %lld %lld\n",
          BUILD_STATE_MAGIC, cwd, bf, (long long)bfm, bfs, tgt, (long long)tm, ts, 0, srcs->count,
          count, libOnly ? 1 : 0, c->libRequested ? 1 : 0, staticUp ? 1 : 0, sharedUp ? 1 : 0,
          staticLib[0] ? staticLib : "-", sharedLib[0] ? sharedLib : "-", staticSz, sharedSz);
  bool ok = written > 0 && (size_t)written < cap;
  size_t used = ok ? (size_t)written : 0;
  for (int i = 0; ok && i < count; i++) {
    written = snprintf(buf + used, cap - used, "%c %lld %lld %016llx %s\n",
                       stamps[i].kind, (long long)stamps[i].mtime, stamps[i].size,
                       (unsigned long long)stamps[i].content, stamps[i].path);
    ok = written > 0 && (size_t)written < cap - used;
    if (ok) used += (size_t)written;
  }
  free(stamps);
  if (ok && fastSnapPut(buf, used) && optsRouterRecord(c, libOnly)) {
    /* Grup P (§4) seragam — lihat catatan di cabang lib di atas. */
    uint64_t content = fastProjStamp(bf, srcs);
    long long t = csNowNs();
    optsProjMetaStamp(cmdsCacheProjAddr(), content);
    csProjMetaStamp(cmdsCacheProjAddr(), content, t);
    csProjMetaStampFile(BUILD_FP_CACHE, cmdsCacheProjAddr(), content, t); /* P seragam: fingerprint.cache */
  }
  free(buf);
}

/* ==================== clean ==================== */

int cmdClean(const char *buildfilePath) {
  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) return 1;
  return cmdsCleanRun(&c);
}

/* Kosongkan ISI folder; folder itu sendiri tetap ada (design/startup.md §6).
   Hanya anak langsung yang dihapus: file lewat fsRemoveFile, subfolder lewat
   fsRemoveTree. Folder tidak ada -> tidak ada yang dilakukan. */
bool cmdsEmptyDir(const char *dir, int *removed) {
  if (!fsDirExists(dir)) return true;
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  bool ok = true;
  for (int i = 0; i < files.count; i++) {
    if (fsRemoveFile(files.items[i]))
      (*removed)++;
    else
      ok = false;
  }
  for (int i = 0; i < dirs.count; i++) {
    if (fsRemoveTree(dirs.items[i]))
      (*removed)++;
    else
      ok = false;
  }
  listFree(&dirs);
  listFree(&files);
  return ok;
}

/* ==================== Pembersihan state lama (audit §Daftar Implementasi) ==
   Jalur tulis lama yang sudah digantikan blob (caches/state.cache) dan store
   (fingerprint.cache) tidak pernah dibaca lagi — file sisa dari versi lama
   dibuang sekali per run dan saat clean agar tidak menumpuk:
     .satria/workspace.state               gerbang no-op ws lama
     .satria/workspace/<p>.<leaf>          state per-proyek lama; leaf lama di
                                           bawah — <p>.Buildfile di folder yang
                                           SAMA adalah CURRENT, jangan disentuh
     .satria/{build.state, build.lib.state, build.fingerprint}   single lama
     .satria/caches/paths.cache            sudah dimigrasi ke sources.cache
   Menghapus path yang tidak ada = tanpa efek; aman dipanggil tiap run. */
static const char *kWsLegacySuffixes[] = {".build.fingerprint", ".ws.state",
                                          ".ws.lib.state",      ".deps.cache",
                                          ".src",               ".hdr"};

void cmdsLegacyStateSweep(void) {
  if (fsDirExists(".satria")) {
    fsRemoveFile(".satria/build.state");
    fsRemoveFile(".satria/build.lib.state");
    fsRemoveFile(".satria/build.fingerprint");
    fsRemoveFile(".satria/caches/paths.cache");
    /* sisa format v0.2.3 dan sebelumnya: log workspace dan daftar sumber lama */
    fsRemoveFile(".satria/workspace_log");
    fsRemoveFile(".satria/ws.src");
  }
  if (!fsDirExists(".satria/workspace")) return;
  List dirs = {0}, files = {0};
  fsListDir(".satria/workspace", &dirs, &files);
  for (int i = 0; i < files.count; i++) {
    const char *p = files.items[i];
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    size_t bl = strlen(base);
    for (size_t k = 0; k < sizeof(kWsLegacySuffixes) / sizeof(kWsLegacySuffixes[0]); k++) {
      size_t ln = strlen(kWsLegacySuffixes[k]);
      if (bl > ln && strcmp(base + bl - ln, kWsLegacySuffixes[k]) == 0) {
        fsRemoveFile(p);
        break;
      }
    }
  }
  for (int i = 0; i < dirs.count; i++) { /* <p>.hdr/ lama (salinan header semantik) */
    const char *p = dirs.items[i];
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    size_t bl = strlen(base);
    if (bl > 4 && strcmp(base + bl - 4, ".hdr") == 0) fsRemoveTree(p);
  }
  /* folder .satria/workspace yang sudah kosong (sisa format lama) ikut dibuang */
  List restDirs = {0}, restFiles = {0};
  fsListDir(".satria/workspace", &restDirs, &restFiles);
  if (restDirs.count == 0 && restFiles.count == 0) fsRemoveTree(".satria/workspace");
  listFree(&restDirs);
  listFree(&restFiles);
  listFree(&dirs);
  listFree(&files);
}

/* Jalankan logika bersih TANPA loadConfig — dipakai jalur clean tunggal
   (cmdClean) dan mode workspace (Config disusun dari Buildfile.ws oleh
   wsBuildConfig; path state mengikuti cmdsStateSetWsProject). */
int cmdsCleanRunEx(const Config *c, bool *anyOut) {
  bool any = false;
  /* bin/, build/, lib/, modules/: yang dibersihkan ISINYA saja. Folder
     dibuat sekali oleh startup dan tetap ada, jadi build berikutnya tidak
     perlu membuatnya ulang. modules/ hanya berisi arsip embed hasil generate
     (dibuat ulang oleh build), bukan input. "." dan path kosong ditolak. */
  if (c->cleanBuildDir) {
    const char *emptied[] = {c->outBuildDir, c->outBinaryDir, c->outLibDir, "modules"};
    for (int i = 0; i < (int)(sizeof(emptied) / sizeof(emptied[0])); i++) {
      const char *d = emptied[i];
      if (!d[0] || strcmp(d, ".") == 0 || !safeRelative(d) || !fsDirExists(d)) continue;
      int removed = 0;
      bool ok = cmdsEmptyDir(d, &removed);
      if (removed || !ok) {
        printf("> Emptied   : %s (%d entri%s)\n", d, removed, ok ? "" : ", sebagian gagal");
        any = true;
      }
    }
  }
  if (c->cleanCompileCommands && fsFileExists("compile_commands.json")) {
    fsRemoveFile("compile_commands.json");
    printf("> Removed   : compile_commands.json\n");
    any = true;
  }
  /* deps.cache tidak dihapus: file dipertahankan dan isinya dikosongkan
     bersama file cache lain di bawah (design/startup.md §6). Snapshot hash
     dependensi kosong -> verifikasi dimulai dari nol pada build berikutnya. */
  /* Isi tiap file cache di .satria/caches dikosongkan (design/startup.md §6)
     — file tetap ada (csOpen menerima file kosong), sehingga jumlah file tidak
     berubah dan router opts.cache tetap menunjuk target yang sah. Router
     sendiri TIDAK disentuh: setelah clean, optsRouterValidate gagal (grup
     proyek hilang dari caches) -> run berikutnya tidak boleh no-op, lalu
     build sukses merekam ulang entri + router. */
  if (fsDirExists(WS_CACHES_DIR)) {
    List cdirs = {0}, cfiles = {0};
    fsListDir(WS_CACHES_DIR, &cdirs, &cfiles);
    int n = 0;
    for (int i = 0; i < cfiles.count; i++) {
      FILE *fp = fopen(cfiles.items[i], "w");
      if (fp) {
        fclose(fp);
        n++;
      }
    }
    listFree(&cdirs);
    listFree(&cfiles);
    if (n) {
      printf("> Emptied   : %s (%d file)\n", WS_CACHES_DIR, n);
      any = true;
    }
  }
  /* Mode workspace: state fast & snapshot source per proyek ikut dibuang
     (root-centric) — clean berarti build berikutnya mulai dari nol. */
  if (cmdsStateWsProject()) {
    static const char *wsLeaves[] = {"ws.state", "src", NULL};
    for (int i = 0; wsLeaves[i]; i++) {
      if (stateBlobExists(cmdsStateWsProject(), wsLeaves[i])) {
        stateBlobDelete(cmdsStateWsProject(), wsLeaves[i]);
        printf("> Removed   : caches/state.cache [%s.%s]\n", cmdsStateWsProject(), wsLeaves[i]);
        any = true;
      }
    }
  }
  /* ld.cache (verdict deteksi GNU ld) TIDAK dihapus: file dan isinya
     dipertahankan. Validasinya sudah memeriksa path, mtime ns, dan ukuran
     binary ld yang sedang di-resolve (embed_ld_cache.c), sehingga verdict
     basi atau dari mesin lain otomatis dibuang dan diprobe ulang. */
  /* Staging packaging (.satria/pack) adalah cache — artefak final di dist/
     TIDAK disentuh (bisa jadi dirujuk embedded.* proyek lain). */
  if (fsDirExists(".satria/pack")) {
    fsRemoveTree(".satria/pack");
    printf("> Removed   : .satria/pack\n");
    any = true;
  }
  /* Sisa state lama (audit): file tulisan versi lama yang sudah digantikan
     blob/store — dibuang juga saat clean (lihat cmdsLegacyStateSweep). */
  cmdsLegacyStateSweep();
  /* startup.state TIDAK dihapus clean: workspaceRun (mode ws) menyinkronkannya
     dengan Buildfile.ws sebelum clean berjalan (design/startup.md §6). */
  if (anyOut) *anyOut = any;
  return 0;
}

int cmdsCleanRun(const Config *c) {
  bool any = false;
  int rc = cmdsCleanRunEx(c, &any);
  if (!any) printf("> Nothing to clean (see Buildfile: clean)\n");
  return rc;
}

/* Buang satu entri memo (kunci = path ternormalisasi, sama seperti insert). */
void cmdsStatMemoForget(const char *path) {
  if (statMemoCap == 0 || !path) return;
  char key[MAX_PATH * 4];
  memoKeyFor(path, key, sizeof(key));
  /* Bangun ulang tabel tanpa kunci itu; open addressing tak punya tombstone. */
  StatMemoEntry *old = statMemo;
  size_t oldCap = statMemoCap;
  statMemo = calloc(oldCap, sizeof(StatMemoEntry));
  if (!statMemo) {
    statMemo = old;
    statMemoCap = oldCap;
    return;
  }
  size_t kept = 0;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].path) continue;
    if (strcmp(old[i].path, key) == 0) {
      free(old[i].path);
      continue;
    }
    size_t j = statMemoHash(old[i].path) & (oldCap - 1);
    while (statMemo[j].path) j = (j + 1) & (oldCap - 1);
    statMemo[j] = old[i];
    kept++;
  }
  free(old);
  statMemoCount = kept;
}

/* Buang semua entri di bawah folder `dir` (relatif cwd), termasuk folder itu. */
void cmdsStatMemoForgetTree(const char *dir) {
  if (statMemoCap == 0 || !dir) return;
  char prefix[MAX_PATH * 4];
  memoKeyFor(dir, prefix, sizeof(prefix));
  size_t pl = strlen(prefix);
  StatMemoEntry *old = statMemo;
  size_t oldCap = statMemoCap;
  statMemo = calloc(oldCap, sizeof(StatMemoEntry));
  if (!statMemo) {
    statMemo = old;
    statMemoCap = oldCap;
    return;
  }
  size_t kept = 0;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].path) continue;
    bool drop = strncmp(old[i].path, prefix, pl) == 0 &&
                (old[i].path[pl] == '\0' || old[i].path[pl] == '/');
    if (drop) {
      free(old[i].path);
      continue;
    }
    size_t j = statMemoHash(old[i].path) & (oldCap - 1);
    while (statMemo[j].path) j = (j + 1) & (oldCap - 1);
    statMemo[j] = old[i];
    kept++;
  }
  free(old);
  statMemoCount = kept;
}

/* Satu rehash untuk invalidasi seluruh output workspace. Jika alokasi gagal,
   reset memo agar tidak ada stamp output basi yang dipakai. */
static int memoKeyCompare(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void cmdsStatMemoForgetOutputs(const char *buildDir, const List *items) {
  if (!statMemoCap) return;
  char prefix[MAX_PATH * 4] = "";
  if (buildDir && *buildDir) memoKeyFor(buildDir, prefix, sizeof(prefix));
  size_t pl = strlen(prefix);
  size_t n = 0;
  for (int i = 0; items && i < items->count; i++)
    if (items->items[i][0] == 'O') n++;
  char **keys = n ? calloc(n, sizeof(*keys)) : NULL;
  if (n && !keys) { cmdsStatMemoReset(); return; }
  size_t used = 0;
  for (int i = 0; items && i < items->count; i++) {
    if (items->items[i][0] != 'O') continue;
    keys[used] = malloc(MAX_PATH * 4);
    if (!keys[used]) {
      for (size_t j = 0; j < used; j++) free(keys[j]);
      free(keys);
      cmdsStatMemoReset();
      return;
    }
    memoKeyFor(items->items[i] + 2, keys[used], MAX_PATH * 4);
    used++;
  }
  if (used > 1) qsort(keys, used, sizeof(*keys), memoKeyCompare);
  StatMemoEntry *fresh = calloc(statMemoCap, sizeof(*fresh));
  if (!fresh) {
    for (size_t j = 0; j < used; j++) free(keys[j]);
    free(keys);
    cmdsStatMemoReset();
    return;
  }
  size_t kept = 0;
  for (size_t i = 0; i < statMemoCap; i++) {
    if (!statMemo[i].path) continue;
    const char *path = statMemo[i].path;
    bool inTree = pl && strncmp(path, prefix, pl) == 0 &&
                  (path[pl] == '\0' || path[pl] == '/');
    bool inOutputs = used && bsearch(&path, keys, used, sizeof(*keys), memoKeyCompare);
    if (inTree || inOutputs) { free(statMemo[i].path); continue; }
    size_t j = statMemoHash(path) & (statMemoCap - 1);
    while (fresh[j].path) j = (j + 1) & (statMemoCap - 1);
    fresh[j] = statMemo[i];
    kept++;
  }
  free(statMemo);
  statMemo = fresh;
  statMemoCount = kept;
  for (size_t j = 0; j < used; j++) free(keys[j]);
  free(keys);
}

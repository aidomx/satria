/*
 * cmds_fast — mesin fast-state: scan snapshot, stamp input,\n * hash, dan penulisan state (entri build.state di fingerprint.cache).
 * Dipisah dari cmds_state.c (audit refactor: berkas kandidat >600 baris).
 * Definisi & semantik lihat cmds_internal.h.
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

#include "../background.h"
#include "../cache/cache_store.h"
#include "../commands.h"
#include "../compdb.h"
#include "../deps.h"
#include "../compile.h"
#include "../embed_ld_cache.h"
#include "../portability.h"
#include "../util.h"

/* ==================== Fast no-op snapshot ==================== */
/* Dua snapshot per fase build (kini keduanya entri cache, bukan file sendiri):
   - `build.state`  : fase binary (satria / -w fase 2), entri di
                      .satria/caches/fingerprint.cache — target = binary.
   - `build.lib.state` (nama entri di lib.cache): fase library workspace
                      (cmdBuildEx libOnly), .satria/caches/lib.cache —
     target = library proyek (atau tanpa target bila proyek tak meminta
     library; state-nya murni "semua input & object masih sama").
   Dengan state fase library, no-op `satria -w` tidak membayar loadConfig +
   scan fingerprint + decide penuh per proyek (miniws: jalur ringan). */
/* Tipe FastState/FastStamp dan BUILD_STATE_MAGIC ada di cmds_internal.h. */

void fastStateFree(FastState *s) {
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
uint64_t fastStateHash(const char *path) {
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

bool fastSnapPut(const char *buf, size_t len) {
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

bool fastStateLoad(FastState *s, bool libOnly) {
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
bool fastTargetRecorded(const char *target) {
  return target[0] && strcmp(target, "-") != 0;
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
    /* Output-dir ada di include path (configFinalize: generated `embedded.h`
       / `version.h` bisa di-#include tanpa headers.each), tapi tetap OUTPUT:
       jangan distamp sebagai input (object di dalamnya sudah sengaja tidak
       distamp — lihat catatan di bawah). Header generated yang memang jadi
       input dicegat closure .d lewat fastCollectDepPath. */
    if (d && *d && strcmp(d, ".") != 0 && strcmp(d, c->outBuildDir) != 0)
      fastCollectTree(d, &paths);
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
  /* Embedded: arsip tetap input proyek. File hasil codegen (embedded.c /
     embedded.h di outBuildDir) ikut distamp — source generated ini berada
     di luar root sumber (fastCollectTree tidak menemukannya); sentuh tanpa
     perubahan isi aman lewat hash konten (regenerasi content-based,
     design/embedded.md). Object embed kini object source biasa dan TIDAK
     distamp, seperti semua object. */
  if (c->embCount > 0) {
    char embPath[MAX_PATH + 40];
    snprintf(embPath, sizeof(embPath), "%s/embedded.c", c->outBuildDir);
    fastStampAdd(&paths, embPath);
    snprintf(embPath, sizeof(embPath), "%s/embedded.h", c->outBuildDir);
    fastStampAdd(&paths, embPath);
  }
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    /* embedded.<n>.file adalah input eksternal proyek ini. Tanpa dicatat
       di fast-state, perubahan archive dapat terlewat sebelum cmdBuild()
       sempat menjalankan fase embed/link normal. Arsip yang DIHASILKAN
       (generate) juga dicatat: walkDir koleksi source proyek (jalur
       commands.c) tidak menyentuh folder archiveDir, jadi tanpa ini
       perubahan archive tidak membatalkan fast path. */
    if (c->emb[i].archivePath[0]) fastStampAdd(&paths, c->emb[i].archivePath);
  }
  /* Verdict ld TIDAK lagi dicatat: codegen adalah satu-satunya jalur embed
     (design/embedded.md) — build tidak lagi peduli ld ada/tidak. Stamp 'L'
     lama pada state lama tetap divalidasi di cmdsFastStateValid dan hilang
     secara alami pada save berikutnya. */
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
    stamps[count].kind = mt < 0 ? 'M' : (fsDirExists(path) ? 'D' : 'F');
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


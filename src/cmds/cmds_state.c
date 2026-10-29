/*
 * cmds_state — state build lintas-run (dipindah dari commands.c).
 * Dulu berisi semuanya; kini dipecah jadi file kembar di modul cmds/:
 *   cmds_state.c    : fingerprint + validasi fast no-op (file ini)
 *   cmds_fast.c     : mesin snapshot (scan, stamp, simpan build.state)
 *   cmds_statmemo.c : memo stat per-proses + invalidasi (forget)
 *   cmds_libcache.c : caches/lib.cache (fase library)
 *   cmds_clean.c    : command clean + sweep state lama
 *
 *   - Fingerprint konfigurasi compile/link + set source (mode single: entri
 *     <build-fingerprint-v1> di .satria/caches/fingerprint.cache; mode
 *     workspace: blob build.fingerprint di .satria/caches/state.cache):
 *     berubah -> rebuild penuh.
 *   - Fast no-op snapshot (mode single: entri `build.state` di
 *     fingerprint.cache; mode workspace: blob ws.state di state.cache):
 *     mtime+size seluruh input; persis sama -> summary diulang dari cache,
 *     build dilewati total. File lama (.satria/build.state dst.) sudah basi
 *     dan dibuang cmdsLegacyStateSweep (cmds_clean.c).
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
/* BUILD_FP_CACHE / BUILD_FP_KEY: ada di cmds_internal.h. */

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

/* Tulis ulang snapshot fast-state (entri `build.state` di fingerprint.cache)
   dari fast-state tervalidasi — jalur PATCH sentuh
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
  cmdsStatMemoSetCwd(); /* memo stat pindah ke cmds_statmemo.c */
  /* Router (opts.cache) dibaca lebih dulu: cache yang dirutekan hilang /
     kosong -> batal cepat tanpa memuat snapshot fast-state. */
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


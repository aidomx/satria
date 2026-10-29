/*
 * cmds_lib — fase library, kompilasi paralel, dan emit header versi
 * (dipindah apa adanya dari commands.c).
 *
 *   - Nama file library per toolchain + cek up-to-date per varian,
 *   - buildLibrary: kemas object source + embedded jadi .a/.lib dan/atau
 *     .so/.dylib/.dll,
 *   - runParallelJobs: baris status per job dicetak parent, tanpa interleaving,
 *   - emitVersionHeader: build/version.h dari .satria-version.
 */
#include "cmds_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../background.h"
#include "../compile.h"
#include "../embed.h"
#include "../log/build_log.h"
#include "../pack/pack.h"
#include "../portability.h"
#include "../util.h"

/* ==================== fase pack (proyek binary + pack.*) ==================== */

/* Wrapper fase normal: library lintas-proyek disertakan pada link shared. */
bool cmdsBuildLibrary(const Config *c, const List *srcs) { return cmdsBuildLibraryEx(c, srcs, true); }

/*
 * Proyek binary dengan pack.*: summary build tidak lengkap tanpa status
 * kemasan, jadi packRun dipanggil DI DALAM kedua jalur sukses cmdBuild —
 * sebelum summary, setelah fingerprint disimpan (gagal kemasan tidak
 * memicu kompilasi ulang; fast state dilewati untuk proyek pack karena
 * artefak kemasan divalidasi mtime oleh packRun tiap run).
 */
bool cmdsPackAfterBinary(const Config *c, bool *fastStateSaved, const List *srcs,
                         const char *target, const char *buildfilePath) {
  if (!c->pack.requested) {
    *fastStateSaved = false; /* pemanggil menyimpan state seperti biasa */
    return true;
  }
  if (!packRun(c)) return false; /* gagal kemasan: state tidak direkam */
  if (!wsModeActive()) cmdsFastStateSave(c, srcs, target, buildfilePath, false);
  *fastStateSaved = true;
  return true;
}

/* Proyek kemasan murni (output.binary = false): embedded dulu (arsip
   untuk proyek lain), lalu packaging, lalu summary ringkas. */
int cmdsPackOnlyProject(const Config *c) {
  bool anyRebuilt = false;
  if (c->embCount > 0) {
    mkdirs(c->outBuildDir);
    if (!buildEmbeddedArchivesEx(c, &anyRebuilt)) return 1;
  }
  if (!packRun(c)) return 1;
  if (packLastRebuilt()) anyRebuilt = true;
  printf("\n%s Summary :\n", repPrefix());
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    printf("    Archive  : %s%s\n", c->emb[i].archivePath,
           fsFileExists(c->emb[i].archivePath) ? "" : " (missing)");
  }
  /* No-op: tidak ada arsip/paket yang dibangun ulang (design/build_report.md). */
  if (!anyRebuilt) printf("    Skipped  : true\n");
  printf("    Status   : Success\n");
  return 0;
}

/* Nama file library sesuai toolchain: lib<name>.a / <name>.lib (statis). */
void cmdsLibStaticPath(const Config *c, char *out, size_t n) {
#ifdef _WIN32
  if (compilerIsMSVC(c))
    snprintf(out, n, "%s/%s.lib", c->outLibDir, c->outLibName);
  else
    snprintf(out, n, "%s/lib%s.a", c->outLibDir, c->outLibName);
#else
  (void)c;
  snprintf(out, n, "%s/lib%s.a", c->outLibDir, c->outLibName);
#endif
}

/* Nama file library dinamis: lib<name>.so / .dylib / <name>.dll. */
void cmdsLibSharedPath(const Config *c, char *out, size_t n) {
#if defined(_WIN32)
  (void)c;
  snprintf(out, n, "%s/%s.dll", c->outLibDir, c->outLibName);
#elif defined(__APPLE__)
  (void)c;
  snprintf(out, n, "%s/lib%s.dylib", c->outLibDir, c->outLibName);
#else
  (void)c;
  snprintf(out, n, "%s/lib%s.so", c->outLibDir, c->outLibName);
#endif
}

/*
 * Kompilasi paralel (satria -jN, mirip make). Baris status tiap job dicetak
 * oleh parent tepat setelah job selesai — satu printf utuh per baris, jadi
 * tidak ada interleaving antar job. Return false bila ada yang gagal atau
 * build di-interupsi Ctrl+C.
 */
typedef struct {
  char src[MAX_PATH]; /* source yang dikompilasi slot ini */
  double start;       /* untuk durasi per job */
  int idx;            /* indeks source di srcs (untuk okOut) */
} JobSlot;

bool cmdsRunParallelJobs(const Config *c, const char *inc, const char *wf, const List *srcs,
                         char *objPath, size_t objCap, int jobs, int *outCompiled,
                         int *outFailed, int *outInterrupted, bool quiet, bool *okOut) {
  JobSlot *slots = calloc((size_t)jobs, sizeof(JobSlot));
  ProcHandle *handles = calloc((size_t)jobs, sizeof(ProcHandle));
  if (!slots || !handles) {
    free(slots);
    free(handles);
    return false;
  }

  /* -fPIC untuk library shared (GNU/Clang) — setara compileLibraryOne. */
#ifndef _WIN32
  char *picwf = (c->libShared && !compilerIsMSVC(c)) ? picWarningFlags(c) : NULL;
  const char *wfUse = picwf ? picwf : wf;
#else
  const char *wfUse = wf;
#endif

  int total = srcs->count;
  int compiled = 0, failed = 0, interrupted = 0;
  int nextSrc = 0, active = 0;

  while (nextSrc < total || active > 0) {
    /* isi slot kosong dengan job baru (berhenti juga saat Ctrl+C) */
    while (active < jobs && nextSrc < total && !procInterrupted()) {
      const char *src = srcs->items[nextSrc];
      if (!objectPathFor(c, src, objPath, objCap)) {
        nextSrc++;
        continue;
      }
      mkparent(objPath);
      char *cmd = compileCmd(c, inc, wfUse, src, objPath);
      bool started = procStartCapture(cmd, &handles[active]);
      free(cmd);
      if (!started) {
        fprintf(stderr, "satria: cannot start compiler for %s\n", src);
        failed++;
        nextSrc++;
        continue;
      }
      snprintf(slots[active].src, sizeof(slots[active].src), "%s", src);
      slots[active].start = nowSeconds();
      slots[active].idx = nextSrc;
      nextSrc++;
      active++;
    }
    if (active == 0) break;

    ProcHandle *fin = NULL;
    int w = procWaitAny(handles, active, &fin);
    if (w < 0) break; /* tidak ada job tersisa (atau wait gagal) */

    double dt = nowSeconds() - slots[w].start;
    if (okOut) okOut[slots[w].idx] = fin->ok;
    compiled++;
    bgProgress(5 + (compiled * 80) / total, "Compiling: %s", slots[w].src); /* §3B */
    const char *tag = fin->interrupted ? "INT" : (fin->ok ? "OK" : "FAIL");
    if (!quiet) {
      printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, tag, dt, slots[w].src);
      fflush(stdout);
    }
    /* Output compiler (warning/error) dicetak UTUH setelah baris progress:
       stream yang sama -> urutan terjaga dan blok tidak bisa tercampur job
       lain yang selesai bersamaan (anti-interleaving). */
    cmdsEmitCapture(fin);
    if (!fin->ok) {
      if (fin->interrupted)
        interrupted++;
      else
        failed++;
    }

    /* kompakkan: pindahkan slot terakhir ke slot yang baru kosong */
    handles[w] = handles[active - 1];
    slots[w] = slots[active - 1];
    active--;
  }

  /* Tangkapan job yang berhenti tanpa sempat dicetak (break saat Ctrl+C /
     wait gagal) dibuang supaya file sementara tidak tertinggal. */
  for (int i = 0; i < active; i++) {
    size_t n = 0;
    free(procCaptureTake(&handles[i], &n));
  }
  free(slots);
  free(handles);
#ifndef _WIN32
  free(picwf);
#endif
  *outCompiled = compiled;
  *outFailed = failed;
  *outInterrupted = interrupted;
  return failed == 0 && interrupted == 0;
}

void cmdsEmitCapture(ProcHandle *h) {
  size_t n = 0;
  char *buf = procCaptureTake(h, &n);
  if (!buf) return;
  fwrite(buf, 1, n, stdout);
  if (buf[n - 1] != '\n') fputc('\n', stdout);
  fflush(stdout);
  buildLogCapture(buf, n); /* relay stderr tidak melihat output ini */
  free(buf);
}

/*
 * Fase library: kumpulkan object hasil kompilasi (source + embedded),
 * kemas statis (ar / lib) dan/atau link shared (-shared / /LD).
 * Object perantara TIDAK dihapus — tetap dipakai link binary dan
 * agar build incremental berikutnya tidak kompilasi ulang total.
 *
 * linkPathLibs=false (fase library workspace): entri library ber-path
 * (artifact proyek lain, mis. ../ruka/lib/libruka.a) DILEWATI pada link
 * shared — .a proyek lain belum tentu ada saat fase library berjalan dan
 * .so tidak perlu inline kode proyek lain. Library sistem (-lm, -lssl)
 * tetap disertakan.
 */
bool cmdsBuildLibraryEx(const Config *c, const List *srcs, bool linkPathLibs) {
  if (!c->libRequested) return true;

  /* Jalur no-op: hindari membangun daftar object, alokasi string, dan mkdir
     bila kedua varian library sudah mutakhir. */
  bool staticUpToDate = !c->libStatic || cmdsLibVariantUpToDate(c, true, srcs);
  bool sharedUpToDate = !c->libShared || cmdsLibVariantUpToDate(c, false, srcs);
  if (staticUpToDate && sharedUpToDate) {
    char target[MAX_PATH * 2];
    if (c->libStatic) {
      cmdsLibStaticPath(c, target, sizeof(target));
      printf("%s Library : %s (up-to-date)\n", repPrefix(), target);
    }
    if (c->libShared) {
      cmdsLibSharedPath(c, target, sizeof(target));
      printf("%s Library : %s (up-to-date)\n", repPrefix(), target);
    }
    return true;
  }

  List objs = {0};
  char obj[MAX_PATH];
  for (int i = 0; i < srcs->count; i++) {
    /* exclude: source tidak ikut DIKEMAS ke library (mis. main.c milik
       binary) — tetap dikompilasi untuk executable. Object embed kini
       object source biasa (embedded.c ikut srcs) — tanpa perlakuan khusus. */
    if (excludedSource(c, srcs->items[i])) continue;
    if (objectPathFor(c, srcs->items[i], obj, sizeof(obj))) listAdd(&objs, obj);
  }

  if (objs.count == 0) {
    fprintf(stderr, "satria: library requested but no object files found\n");
    listFree(&objs);
    return false;
  }

  mkdirs(c->outLibDir);

  bool staticDone = true, sharedDone = true;
  char target[MAX_PATH * 2];

  if (c->libStatic) {
    cmdsLibStaticPath(c, target, sizeof(target));
    if (staticUpToDate) {
      printf("%s Library : %s (up-to-date)\n", repPrefix(), target);
    } else {
      size_t n = 64;
      for (int i = 0; i < objs.count; i++)
        n += strlen(objs.items[i]) + 3;
      char *cmd = malloc(n);
      if (compilerIsMSVC(c))
        snprintf(cmd, n, "lib /nologo /OUT:%s", target);
      else
        snprintf(cmd, n, "ar rcs %s", target);
      for (int i = 0; i < objs.count; i++) {
        strcat(cmd, " ");
        strcat(cmd, objs.items[i]);
      }
      printf("%s Library : %s\n", repPrefix(), target);
      staticDone = runCmd(cmd);
      if (!staticDone) fprintf(stderr, "satria: static library command: %s\n", cmd);
      free(cmd);
      if (!staticDone) fprintf(stderr, "satria: static library build failed\n");
    }
  }

  if (c->libShared) {
    cmdsLibSharedPath(c, target, sizeof(target));
    if (sharedUpToDate) {
      printf("%s Library : %s (up-to-date)\n", repPrefix(), target);
      listFree(&objs);
      return staticDone; /* .so dilewati; hasil .a tetap dihormati */
    }
    size_t n = strlen(c->cc) + strlen(target) + 96;
    for (int i = 0; i < objs.count; i++)
      n += strlen(objs.items[i]) + 3;
    for (int i = 0; i < c->libraries.count; i++)
      n += strlen(c->libraries.items[i]) + 8;
    char *cmd = malloc(n);
    if (compilerIsMSVC(c)) {
      snprintf(cmd, n, "cl /nologo /LD");
      for (int i = 0; i < objs.count; i++) {
        strcat(cmd, " ");
        strcat(cmd, objs.items[i]);
      }
      strcat(cmd, " /link /nologo /INCREMENTAL:NO /OUT:");
      strcat(cmd, target);
      for (int i = 0; i < c->libraries.count; i++) {
        const char *lib = c->libraries.items[i];
        if (!linkPathLibs && (strchr(lib, '/') || strchr(lib, '\\'))) continue;
        if (lib[0] == '-') lib++;
        if (lib[0] == 'l') lib++;
        strcat(cmd, " ");
        strcat(cmd, lib);
        strcat(cmd, ".lib");
      }
    } else {
      /* Driver C (mis. compiler = gcc eksplisit) tetap harus bisa me-link
         object C++: tambahkan -lstdc++ saat proyek berbahasa C++. */
      snprintf(cmd, n, c->langCpp && !compilerIsCppDriver(c) ? "%s -shared -lstdc++"
                                                             : "%s -shared",
               c->cc);
      for (int i = 0; i < objs.count; i++) {
        strcat(cmd, " ");
        strcat(cmd, objs.items[i]);
      }
      strcat(cmd, " -o ");
      strcat(cmd, target);
      for (int i = 0; i < c->libraries.count; i++) {
        const char *lib = c->libraries.items[i];
        /* Fase library workspace: artifact proyek lain dilewati (belum
           tentu ada); simbol lintas diselesaikan saat link binary. */
        if (!linkPathLibs && (strchr(lib, '/') || strchr(lib, '\\'))) continue;
        strcat(cmd, " ");
        /* Explicit workspace artifact paths (e.g. ../ruka/lib/libruka.a)
           must be passed directly to the linker. Only logical/system
           library names use -l. */
        if (strchr(lib, '/') || strchr(lib, '\\')) {
          strcat(cmd, lib);
        } else {
          if (lib[0] == '-' || lib[0] == 'l')
            strcat(cmd, "-");
          else
            strcat(cmd, "-l");
          strcat(cmd, lib);
        }
      }
    }
    printf("%s Library : %s\n", repPrefix(), target);
    sharedDone = runCmd(cmd);
    if (!sharedDone) fprintf(stderr, "satria: shared library command: %s\n", cmd);
    free(cmd);
    if (!sharedDone) fprintf(stderr, "satria: shared library build failed\n");
  }

  listFree(&objs);
  return staticDone && sharedDone;
}

/* True bila SATU varian library (statis/shared) sudah ada dan lebih baru
   daripada seluruh object inputnya — varian itu boleh dilewati. Dipakai
   per varian di cmdsBuildLibrary: menghapus .so saja tidak meng-rebuild .a. */
bool cmdsLibVariantUpToDate(const Config *c, bool isStatic, const List *srcs) {
  char obj[MAX_PATH];
  char target[MAX_PATH * 2];

  if (isStatic)
    cmdsLibStaticPath(c, target, sizeof(target));
  else
    cmdsLibSharedPath(c, target, sizeof(target));

  int64_t mtime = fsMTimeNs(target);
  if (mtime < 0) return false; /* target belum ada */

  for (int i = 0; i < srcs->count; i++) {
    if (excludedSource(c, srcs->items[i])) continue;
    if (!objectPathFor(c, srcs->items[i], obj, sizeof(obj))) continue;
    int64_t om = fsMTimeNs(obj);
    if (om < 0 || om > mtime) return false;
  }
  return true;
}

/*
 * Terbitkan build/version.h dari .satria-version (root project). Bila file
 * itu ada, project yang menambahkan build ke headers bisa memakai
 * SATRIA_VERSION_EMBEDDED — versi ikut ter-embed ke binary saat build, bukan
 * dibaca ulang saat runtime, sehingga `satria version` tetap benar di mana
 * pun binary dijalankan (tidak tergantung path lokal/repo). Header hanya
 * ditulis ulang saat isinya berubah; perubahan memaksa kompilasi ulang.
 */
bool cmdsEmitVersionHeader(const Config *c, bool *changed) {
  if (!fsFileExists(".satria-version")) return true;

  FILE *rf = fopen(".satria-version", "rb");
  if (!rf) return true;
  char raw[64] = {0};
  size_t n = fread(raw, 1, sizeof(raw) - 1, rf);
  fclose(rf);
  while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r' || raw[n - 1] == ' ' || raw[n - 1] == '\t'))
    raw[--n] = '\0';
  char *s = raw;
  while (*s == ' ' || *s == '\t')
    s++;
  if (!*s) return true; /* kosong: jangan terbitkan apa pun */

  char body[192];
  snprintf(body, sizeof(body),
           "/* Auto-generated by satria from .satria-version. Do not edit. */\n"
           "#define SATRIA_VERSION_EMBEDDED \"%s\"\n",
           s);

  char path[MAX_PATH + 16];
  snprintf(path, sizeof(path), "%s/version.h", c->outBuildDir);
  mkparent(path);

  /* lewati penulisan ulang bila tidak berubah agar mtime tetap stabil */
  char existing[192] = {0};
  FILE *ef = fopen(path, "rb");
  if (ef) {
    size_t en = fread(existing, 1, sizeof(existing) - 1, ef);
    fclose(ef);
    if (en == strlen(body) && memcmp(existing, body, en) == 0) return true;
  }

  /* Mode BINER: penulisan teks mengubah \n menjadi \r\n di Windows sehingga
     perbandingan biner berikutnya selalu beda dan versi dianggap berubah —
     memaksa recompile penuh di setiap run (gagal inkremental build-manual). */
  FILE *wf = fopen(path, "wb");
  if (!wf) {
    fprintf(stderr, "satria: cannot write %s\n", path);
    return false;
  }
  fwrite(body, 1, strlen(body), wf);
  fclose(wf);
  if (changed) *changed = true;
  return true;
}

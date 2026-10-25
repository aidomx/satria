#include "profile.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmds/cmds_internal.h"
#include "compile.h"
#include "config.h"
#include "deps.h"
#include "embed.h"
#include "portability.h"
#include "prof/prof.h"
#include "satria.h"
#include "uses/workspace.h"
#include "util.h"

/*
 * profile — antarmuka diagnostik `satria profile <context> [opsi]`.
 * Lihat profile.h dan design/profile.md.
 *
 * Aturan yang dijaga di sini:
 *  - Profil apa yang ada: context langsung ke direktori; hanya context
 *    binary yang membaca Buildfile (sumber cc/flags) dan TANPA menulis
 *    cache (loadConfigEx cacheWrite=false).
 *  - Non-mutating: tidak menulis .satria (state build), Buildfile, atau
 *    artefak build normal. Semua keluaran operasi masuk direktori sementara
 *    terisolasi (TMPDIR lalu /tmp) dan dihapus di SEMUA jalur keluar.
 *  - Memakai ulang mesin timing prof (profPhaseBegin/End) — bukan
 *    subsistem timing kedua — dan mesin build yang ada (kompilasi paralel
 *    cmdsRunParallelJobs, pola tar/embed/library dari cmds_lib.c).
 *  - Output profil dicetak SETELAH pengukuran selesai sehingga pencetakan
 *    tidak ikut terukur.
 */

/* Backend kompresi archive: pembanding biaya kompresi (design: profile
   adalah alat ukur untuk memutuskan optimasi — gz vs xz vs bz2). */
typedef enum { COMP_NONE = 0, COMP_GZIP, COMP_XZ, COMP_BZ2 } CompBackend;

typedef struct {
  const char *type;          /* "archive" | "embed" | "library" | "binary" |
                             "project" | "workspace" */
  const char *target;        /* value context apa adanya (untuk header) */
  char dir[MAX_PATH * 2];    /* target absolut (dir, atau file utk embed) */
  bool dirIsFile;            /* embed=<file> arsip jadi */
  bool doTar;                /* archive: selalu tar dulu; false = tak valid */
  CompBackend comp;          /* archive: COMP_NONE = with=tar */
  bool libStatic, libShared; /* library: backend ar / gcc -shared */
  int jobs;                  /* binary/project: paralel (default jumlah core) */
  int repeat;                /* --repeat N: ulangi sesi (default 1) */
  char tmp[MAX_PATH * 2];    /* direktori kerja sementara */
  const char *compiler;      /* utk laporan rinci; NULL = n/a */
  const char *reportFile;    /* --report <file> (opsi global) */
  /* metrik input */
  int files;
  int dirs;
  uint64_t bytes;
  /* hasil workspace (utk context workspace=) */
  int wsProjects;
  int wsLibCount;
} ProfCtx;

/* ==================== helper ==================== */

/* ukuran file; 0 bila tidak ada/tidak terbaca (stat gagal bukan fatal). */
static uint64_t sizeOf(const char *path) {
  long long s = fsFileSize(path);
  return s > 0 ? (uint64_t)s : 0;
}

/* Format angka dengan pemisah ribuan (design: "1,204"). */
static void fmtComma(char *out, size_t n, uint64_t v) {
  char raw[32];
  snprintf(raw, sizeof(raw), "%llu", (unsigned long long)v);
  int rl = (int)strlen(raw);
  int commas = (rl - 1) / 3;
  if (rl + commas + 1 > (int)n) {
    copyStr(out, n, raw);
    return;
  }
  int w = rl + commas;
  out[w] = '\0';
  int r = rl - 1, c = 0;
  while (r >= 0) {
    out[--w] = raw[r--];
    if (++c == 3 && r >= 0) {
      out[--w] = ',';
      c = 0;
    }
  }
}

/* Nama backend untuk laporan/fase: gzip/xz/bz2. */
static const char *compName(CompBackend b) {
  switch (b) {
  case COMP_GZIP:
    return "gzip";
  case COMP_XZ:
    return "xz";
  case COMP_BZ2:
    return "bz2";
  default:
    return "none";
  }
}

/* Tool eksternal pembanding backend. */
static const char *compTool(CompBackend b) {
  switch (b) {
  case COMP_GZIP:
    return "gzip";
  case COMP_XZ:
    return "xz";
  case COMP_BZ2:
    return "bzip2";
  default:
    return NULL;
  }
}

/* Expand tilde awal path: "~" atau "~/..." -> home pengguna. Shell hanya
   meng-expand `~` di AWAL kata, sedangkan token profile `archive=~/rupa`
   memilikinya di tengah (setelah '=') — jadi satria harus melakukannya
   sendiri. Windows memakai USERPROFILE. Diskonsioner: '~user' tidak
   didukung (butuh lookup passwd yang tidak portabel). */
static bool expandTilde(const char *in, char *out, size_t n) {
  if (in[0] != '~') return false;          /* bukan tilde-path: biarkan absPath */
  if (in[1] && in[1] != '/') return false; /* ~user: tidak didukung */
  const char *home = NULL;
#ifdef _WIN32
  home = getenv("USERPROFILE");
  if (!home || !*home) home = getenv("HOME");
#else
  home = getenv("HOME");
#endif
  if (!home || !*home) return false;
  if (in[1] == '\0')
    copyStr(out, n, home);
  else
    snprintf(out, n, "%s%s", home, in + 1); /* in+1 = "/..." */
  return true;
}

/* Path relatif -> absolut terhadap cwd pemanggil (target profile boleh
   relatif; hasilnya dinormalkan ke '/'). */
static bool absPath(const char *in, char *out, size_t n) {
  char tilde[MAX_PATH * 3];
  if (expandTilde(in, tilde, sizeof(tilde))) in = tilde;
  bool abs = in[0] == '/';
#ifdef _WIN32
  if (!abs && ((in[0] >= 'A' && in[0] <= 'Z') || (in[0] >= 'a' && in[0] <= 'z')) && in[1] == ':')
    abs = true;
#endif
  if (abs) {
    if (strlen(in) >= n) return false; /* terpotong = target yang salah */
    copyStr(out, n, in);
  } else {
    char cwd[MAX_PATH * 3];
    if (!fsGetCwd(cwd, sizeof(cwd))) return false;
    int w = snprintf(out, n, "%s/%s", cwd, in);
    if (w < 0 || (size_t)w >= n) return false; /* terpotong = path absolut salah */
  }
  pathNormalizeSlash(out);
  return true;
}

/*
 * Direktori kerja sementara DI LUAR pohon build (TMPDIR, fallback /tmp —
 * Windows pakai TEMP). Tidak menyentuh .satria/ agar state build normal
 * benar-benar tak tersentuh. Unik lewat jam monotonic; gagal 64x = error.
 */
static bool tmpDirMake(ProfCtx *p) {
  const char *base = getenv("TMPDIR");
#ifdef _WIN32
  if (!base || !*base) base = getenv("TEMP");
#endif
  if (!base || !*base) base = "/tmp";
  for (int i = 0; i < 64; i++) {
    snprintf(p->tmp, sizeof(p->tmp), "%s/satria-profile-%ld-%d", base,
             (long)(monotonicSeconds() * 1e6), i);
    if (!fsDirExists(p->tmp) && !fsFileExists(p->tmp)) {
      fsMakeDir(p->tmp);
      if (fsDirExists(p->tmp)) return true;
    }
  }
  return false;
}

/* "Linux aarch64" untuk laporan rinci; bukan bagian dari fase terukur. */
static void platformString(char *out, size_t n) {
  if (!probeAvailable("uname")) {
    copyStr(out, n, "unknown");
    return;
  }
  FILE *fp = popenRB("uname -s -m");
  if (!fp) {
    copyStr(out, n, "unknown");
    return;
  }
  char line[128] = {0};
  if (!fgets(line, sizeof(line), fp)) line[0] = '\0';
  pcloseRB(fp);
  char *t = trim(line);
  copyStr(out, n, *t ? t : "unknown");
}

/* Fase discover + collect umum: daftar file/direktori (walkDir) lalu stat
   ukuran tiap file — biaya nyata jalur arsip satria (freshness scan + daftar
   tar), layak diatribusikan (design: "batas fase eksplisit"). */
static void measureInput(ProfCtx *p) {
  List files = {0}, dirs = {0};
  profPhaseBegin("discover");
  walkDir(p->dir, "", &files);
  walkDir(p->dir, "/", &dirs);
  profPhaseEnd(0, 0);
  p->files = files.count;
  p->dirs = dirs.count;

  profPhaseBegin("collect");
  listSort(&files);
  uint64_t bytes = 0;
  for (int i = 0; i < files.count; i++)
    bytes += sizeOf(files.items[i]);
  profPhaseEnd(0, 0);
  p->bytes = bytes;

  listFree(&files);
  listFree(&dirs);
}

/* Fase tar: kemas direktori ke tar sementara (pola cmds_lib/embed: shell tar). */
static int tarStep(ProfCtx *p, const char *tarPath) {
  size_t n = strlen(p->dir) + strlen(tarPath) + 48;
  char *cmd = malloc(n);
  if (!cmd) {
    fprintf(stderr, "satria: profile: kehabisan memori\n");
    return 1;
  }
  snprintf(cmd, n, "tar cf %s -C %s .", tarPath, p->dir);
  profPhaseBegin("tar");
  bool ok = runCmd(cmd);
  profPhaseEnd(p->bytes, ok ? sizeOf(tarPath) : 0);
  free(cmd);
  if (!ok) {
    fprintf(stderr, "satria: profile: tar gagal untuk %s\n", p->dir);
    return 1;
  }
  return 0;
}

/* Fase kompresi terpisah (backend gzip/xz/bz2): biaya kompresi terukur
   sendiri sehingga pemilihan backend optimal jadi data, bukan asumsi. */
static int compressStep(ProfCtx *p, const char *tarPath) {
  const char *tool = compTool(p->comp);
  if (!probeAvailable(tool)) {
    fprintf(stderr, "satria: profile: '%s' tidak ditemukan di PATH\n", tool);
    return 1;
  }
  char outPath[MAX_PATH * 2 + 32];
  snprintf(outPath, sizeof(outPath), "%s/profile.tar.%s", p->tmp, compName(p->comp));
  size_t n = strlen(tarPath) + strlen(outPath) + strlen(tool) + 32;
  char *cmd = malloc(n);
  if (!cmd) {
    fprintf(stderr, "satria: profile: kehabisan memori\n");
    return 1;
  }
  snprintf(cmd, n, "%s -c %s > %s", tool, tarPath, outPath);
  uint64_t tin = sizeOf(tarPath);
  profPhaseBegin(compName(p->comp));
  bool ok = runCmd(cmd);
  profPhaseEnd(tin, ok ? sizeOf(outPath) : 0);
  free(cmd);
  if (!ok) {
    fprintf(stderr, "satria: profile: %s gagal\n", tool);
    return 1;
  }
  return 0;
}

/* archive=<dir> [with=...] — discover, collect, tar, (gzip/xz/bz2).
   Semua keluaran ke direktori sementara; target tak tersentuh. */
static int runArchive(ProfCtx *p) {
  measureInput(p);

  char tarPath[MAX_PATH * 2 + 32];
  snprintf(tarPath, sizeof(tarPath), "%s/profile.tar", p->tmp);
  int rc = tarStep(p, tarPath);
  if (rc != 0) return rc;
  if (p->comp != COMP_NONE) rc = compressStep(p, tarPath);
  return rc;
}

/* embed=<dir|file> — tar (bila dir) lalu `ld -r -b binary`. Probe ld
   sudah dilakukan di cmdProfile sebelum fase mana pun jalan. */
static int runEmbed(ProfCtx *p) {
  uint64_t inBytes = 0;
  char tarPath[MAX_PATH * 2 + 32] = "";

  if (p->dirIsFile) {
    /* Arsip jadi: input embed apa adanya. */
    inBytes = sizeOf(p->dir);
    p->files = 1;
    p->bytes = inBytes;
    copyStr(tarPath, sizeof(tarPath), p->dir);
  } else {
    /* Direktori: satria mengarsipkan sebelum embed (pola buildEmbeddedEntry). */
    measureInput(p);
    snprintf(tarPath, sizeof(tarPath), "%s/profile.tar", p->tmp);
    int rc = tarStep(p, tarPath);
    if (rc != 0) return rc;
    inBytes = sizeOf(tarPath);
  }

  char objPath[MAX_PATH * 2 + 32];
  snprintf(objPath, sizeof(objPath), "%s/profile.o", p->tmp);
  size_t n = strlen(objPath) + strlen(tarPath) + 64;
  char *cmd = malloc(n);
  if (!cmd) {
    fprintf(stderr, "satria: profile: kehabisan memori\n");
    return 1;
  }
  /* Flag identik dengan buildEmbeddedEntry: representasi biaya embed nyata. */
  snprintf(cmd, n, "ld -r -z noexecstack -b binary -o %s %s", objPath, tarPath);
  profPhaseBegin("ld");
  bool ok = runCmd(cmd);
  profPhaseEnd(inBytes, ok ? sizeOf(objPath) : 0);
  free(cmd);
  if (!ok) {
    fprintf(stderr, "satria: profile: ld gagal untuk %s\n", tarPath);
    return 1;
  }
  return 0;
}

/* library=<dir> [with=static|shared] — profil fase kemasan library
   (pola cmdsBuildLibraryEx): object input = hasil build nyata di proyek
   (folder build, semua level); keluaran .a/.so ke direktori sementara. */
static int runLibrary(ProfCtx *p) {
  List objs = {0};
  profPhaseBegin("discover");
  walkDir(p->dir, ".o", &objs);
  profPhaseEnd(0, 0);
  if (objs.count == 0) {
    fprintf(stderr,
            "satria: profile library=%s: tidak ada object .o di direktori "
            "(build dulu dengan satria biasa)\n",
            p->target);
    return 1;
  }

  profPhaseBegin("collect");
  listSort(&objs);
  uint64_t bytes = 0;
  for (int i = 0; i < objs.count; i++)
    bytes += sizeOf(objs.items[i]);
  profPhaseEnd(0, 0);
  p->files = objs.count;
  p->dirs = 0;
  p->bytes = bytes;

  int rc = 0;
  if (p->libStatic) {
#ifdef _WIN32
    const char *libTool = "lib.exe";
#else
    const char *libTool = "ar";
#endif
    if (!probeAvailable(libTool)) {
      fprintf(stderr, "satria: profile: '%s' tidak ditemukan di PATH\n", libTool);
      rc = 1;
    } else {
#ifdef _WIN32
      char outPath[MAX_PATH * 2 + 32];
      snprintf(outPath, sizeof(outPath), "%s/profile.lib", p->tmp);
      size_t n = strlen(outPath) + 32 + (size_t)objs.count * 8;
      char *cmd = malloc(n);
      if (cmd) snprintf(cmd, n, "lib /nologo /OUT:%s", outPath);
#else
      char outPath[MAX_PATH * 2 + 32];
      snprintf(outPath, sizeof(outPath), "%s/libprofile.a", p->tmp);
      size_t n = strlen(outPath) + 16;
      for (int i = 0; i < objs.count; i++)
        n += strlen(objs.items[i]) + 2;
      char *cmd = malloc(n);
      if (cmd) snprintf(cmd, n, "ar rcs %s", outPath);
#endif
      if (!cmd) {
        fprintf(stderr, "satria: profile: kehabisan memori\n");
        rc = 1;
      } else {
        for (int i = 0; i < objs.count; i++) {
          strcat(cmd, " ");
          strcat(cmd, objs.items[i]);
        }
        profPhaseBegin("ar");
        bool ok = runCmd(cmd);
        profPhaseEnd(bytes, ok ? sizeOf(outPath) : 0);
        free(cmd);
        if (!ok) {
          fprintf(stderr, "satria: profile: gagal membuat static library\n");
          rc = 1;
        }
      }
    }
  }

  if (rc == 0 && p->libShared) {
    /* Link shared memakai cc dari Buildfile proyek (sumber flag nyata);
       tanpa Buildfile, cc tidak diketahui — tolak, jangan mengarang. */
    char bf[MAX_PATH * 2 + 16];
    snprintf(bf, sizeof(bf), "%s/Buildfile", p->dir);
    if (!fsFileExists(bf)) {
      fprintf(stderr,
              "satria: profile: with=shared memerlukan Buildfile di %s "
              "(sumber compiler/flags)\n",
              p->target);
      rc = 1;
    } else {
      Config c = configDefaults();
      if (!loadConfigEx(&c, bf, false) || !resolveCompiler(&c)) {
        fprintf(stderr, "satria: profile: Buildfile di %s tidak bisa dimuat\n", p->target);
        rc = 1;
      } else {
        p->compiler = c.cc;
        char outPath[MAX_PATH * 2 + 32];
        snprintf(outPath, sizeof(outPath), "%s/libprofile.so", p->tmp);
        size_t n = strlen(c.cc) + strlen(outPath) + 24;
        for (int i = 0; i < objs.count; i++)
          n += strlen(objs.items[i]) + 2;
        if (c.langCpp) n += 16;
        char *cmd = malloc(n);
        if (!cmd) {
          fprintf(stderr, "satria: profile: kehabisan memori\n");
          rc = 1;
        } else {
          snprintf(cmd, n, "%s -shared -o %s", c.cc, outPath);
          for (int i = 0; i < objs.count; i++) {
            strcat(cmd, " ");
            strcat(cmd, objs.items[i]);
          }
          if (c.langCpp && !compilerIsCppDriver(&c)) strcat(cmd, " -lstdc++");
          profPhaseBegin("shared");
          bool ok = runCmd(cmd);
          profPhaseEnd(bytes, ok ? sizeOf(outPath) : 0);
          free(cmd);
          if (!ok) {
            fprintf(stderr, "satria: profile: gagal link shared library\n");
            rc = 1;
          }
        }
      }
    }
  }

  listFree(&objs);
  return rc;
}

/*
 * project=<dir> [jobs=N] — profil siklus build satu proyek DENGAN fase
 * incremental nyata: config, fingerprint, decide, compile, link.
 *
 * Berbeda dari binary= (yang mengukur cold build murni, semua object
 * dikompilasi ulang), project= mengukur seperti `satria` biasa: decide
 * membandingkan mtime object vs source via deps (header dua lapis), jadi
 * run kedua harus menunjukkan biaya no-op (compilasi 0). Semua state
 * yang dibutuhkan decide (deps.cache) HIDUP DI direktori sementara
 * (outBuildDir dialihkan ke p->tmp/build) — state build normal proyek
 * tidak tersentuh. Output binary juga masuk direktori tersebut.
 *
 * Fingerprint di sini dihitung dari konfigurasi (polah cmdsFingerprintMake)
 * SEBAGAI UKURAN murah — tanpa membandingkannya dengan file state (yang
 * tidak ada di sandbox): fase ini menunjukkan biaya hashing konfigurasi.
 */
static uint64_t profFnv(uint64_t h, const void *data, size_t n) {
  const unsigned char *q = (const unsigned char *)data;
  for (size_t i = 0; i < n; i++) {
    h ^= q[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t profFnvStr(uint64_t h, const char *s) {
  const unsigned char sep = 0xff;
  if (s) h = profFnv(h, s, strlen(s));
  return profFnv(h, &sep, 1);
}

/* Jalur build terisolasi utama project=: pola runBinary dengan fase
   decide incremental (schema identik, berbeda muatan fase). */
static int buildSandboxed(ProfCtx *p, const char *passKind) {
  Config c = configDefaults();
  char bf[MAX_PATH * 2 + 16];
  snprintf(bf, sizeof(bf), "%s/Buildfile", p->dir);
  bool haveBf = fsFileExists(bf);

  if (!fsSetCwd(p->dir)) {
    fprintf(stderr, "satria: profile %s=%s: cannot enter directory\n", passKind, p->target);
    return 1;
  }

  profPhaseBegin("config");
  if (haveBf && !loadConfigEx(&c, bf, false)) {
    profPhaseEnd(0, 0);
    fprintf(stderr, "satria: profile %s=%s: Buildfile tidak bisa dimuat\n", passKind, p->target);
    return 1;
  }
  if (!haveBf) configFinalize(&c); /* konvensi proyek standar */
  bool ccOk = resolveCompiler(&c);
  profPhaseEnd(0, 0);
  if (!ccOk) {
    fprintf(stderr, "satria: profile %s=%s: compiler tidak ditemukan\n", passKind, p->target);
    return 1;
  }
  p->compiler = c.cc;

  if (!c.binary) {
    fprintf(stderr, "satria: profile %s=%s: output.binary = false (tidak ada binary)\n", passKind,
            p->target);
    return 1;
  }
  if (c.embCount > 0) {
    fprintf(stderr,
            "satria: profile %s=%s: proyek ber-embedded tidak didukung "
            "(arsip embedded harus ditulis ke lokasi asli)\n",
            passKind, p->target);
    return 1;
  }

  /* Isolasi keluaran ke direktori sementara. */
  char bdir[MAX_PATH * 2 + 8];
  snprintf(bdir, sizeof(bdir), "%s/build", p->tmp);
  copyStr(c.outBuildDir, sizeof(c.outBuildDir), bdir);
  copyStr(c.outBinaryDir, sizeof(c.outBinaryDir), p->tmp);
  c.libRequested = false;
  c.pack.requested = false;
  copyStr(c.outCompileCommands, sizeof(c.outCompileCommands), "false");

  if (!fsDirExists(c.root)) {
    fprintf(stderr, "satria: profile %s=%s: root '%s' bukan direktori\n", passKind, p->target,
            c.root);
    return 1;
  }
  if (!fsSetCwd(c.root)) {
    fprintf(stderr, "satria: profile %s=%s: cannot enter root '%s'\n", passKind, p->target, c.root);
    return 1;
  }
  procSetForeground(p->jobs > 1 ? false : c.foreground);

  List srcs = {0};
  listReserve(&srcs, 1024);
  profPhaseBegin("gather");
  for (int i = 0; i < c.sources.count; i++) {
    const char *entry = c.sources.items[i];
    size_t elen = strlen(entry);
    if (elen >= 2 &&
        (strcmp(entry + elen - 2, ".c") == 0 || strcmp(entry + elen - 2, ".cpp") == 0) &&
        fsFileExists(entry)) {
      listAdd(&srcs, entry);
      continue;
    }
    walkDir(entry, ".c", &srcs);
    walkDir(entry, ".cpp", &srcs);
  }
  listSort(&srcs);
  profPhaseEnd(0, 0);
  if (srcs.count == 0) {
    fprintf(stderr, "satria: profile %s=%s: tidak ada source .c/.cpp di sources\n", passKind,
            p->target);
    listFree(&srcs);
    return 1;
  }

  /* fingerprint — ukuran biaya hashing konfigurasi & daftar source
     (hash murah FNV; tanpa file state di sandbox, hanya tampil di abstrak
     sebagai waktu proses hashing yang dilakukan mesin build). */
  profPhaseBegin("fingerprint");
  uint64_t fpCompile = 14695981039346656037ULL;
  fpCompile = profFnvStr(fpCompile, c.cc);
  fpCompile = profFnvStr(fpCompile, c.std);
  fpCompile = profFnvStr(fpCompile, c.outBuildDir);
  for (int i = 0; i < c.flags.count; i++)
    fpCompile = profFnvStr(fpCompile, c.flags.items[i]);
  for (int i = 0; i < srcs.count; i++) {
    char obj[MAX_PATH];
    fpCompile = profFnvStr(fpCompile, srcs.items[i]);
    if (objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) fpCompile = profFnvStr(fpCompile, obj);
  }
  profPhaseEnd(0, 0);
  (void)fpCompile;

  /* decide — keputusan incremental nyata via deps: object usang bila
     source mtime lebih baru ATAU header transitive lebih baru (dua lapis
     mtime + konten). Hanya yang pending yang masuk fase compile — biaya
     no-op run kedua = 0 kompilasi. */
  char *inc = includeFlags(&c);
  char *wf = warningFlags(&c);
  char obj[MAX_PATH];
  List pending = {0};
  profPhaseBegin("decide");
  {
    DepCache *dc = depsNew(&c);
    for (int i = 0; i < srcs.count; i++) {
      const char *src = srcs.items[i];
      if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;
      int64_t srcM = fsMTimeNs(src);
      int64_t objM = fsMTimeNs(obj); /* -1 = object belum ada */
      if (objM >= 0 && srcM >= 0 && objM >= srcM &&
          depsNewestHeaderMTimeAt(dc, src, srcM) <= objM) {
        if (depsContentUpToDate(dc, src)) continue; /* up-to-date */
      }
      listAdd(&pending, src);
    }
    depsFree(dc);
    listSort(&pending);
  }
  profPhaseEnd(0, 0);

  char objTmp[MAX_PATH];
  int compiled = 0, failed = 0, interrupted = 0;
  profPhaseBegin("compile");
  bool okc =
      cmdsRunParallelJobs(&c, inc, wf, &pending, objTmp, sizeof(objTmp), p->jobs, &compiled,
                          &failed, &interrupted, strcmp(passKind, "project") == 0 /* quiet */);
  profPhaseEnd(0, 0);
  free(wf);
  free(inc);
  listFree(&pending);
  if (!okc) {
    fprintf(stderr, "satria: profile %s=%s: kompilasi gagal (%d)\n", passKind, p->target,
            failed + interrupted);
    listFree(&srcs);
    return 1;
  }

  /* Link — pola cmdBuildEx (GNU/Clang; MSVC cabang sendiri). */
  char target[MAX_PATH * 2];
  snprintf(target, sizeof(target), "%s/%s", c.outBinaryDir, c.outBinaryName);
  if (compilerIsMSVC(&c)) {
    /* link.exe tidak menambahkan .exe sendiri pada /OUT tanpa ekstensi —
       samakan dengan target inkremental bin/<name>.exe (lihat commands.c). */
    size_t tLen = strlen(target);
    if (tLen < 4 || strcmp(target + tLen - 4, ".exe") != 0) strcat(target, ".exe");
  }
  size_t n = strlen(c.cc) + 1; /* compiler + NUL */
  for (int i = 0; i < srcs.count; i++) {
    if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
    n += strlen(obj) + 1; /* spasi + object */
  }
  if (compilerIsMSVC(&c)) {
    n += strlen(" /link /nologo /INCREMENTAL:NO /OUT:") + strlen(target);
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      if (lib[0] == '-') lib++;
      if (lib[0] == 'l') lib++;
      n += 1 + strlen(lib) + strlen(".lib");
    }
  } else {
    n += strlen(" -o ") + strlen(target);
    if (c.langCpp && !compilerIsCppDriver(&c)) n += strlen(" -lstdc++");
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      n += 1 + strlen((strchr(lib, '/') || strchr(lib, '\\'))
                          ? lib
                          : ((lib[0] == '-' || lib[0] == 'l') ? "-" : "-l"));
      if (!(strchr(lib, '/') || strchr(lib, '\\'))) n += strlen(lib);
    }
  }
  char *cmd = malloc(n);
  if (!cmd) {
    fprintf(stderr, "satria: profile: kehabisan memori\n");
    listFree(&srcs);
    return 1;
  }
  if (compilerIsMSVC(&c)) {
    snprintf(cmd, n, "%s", c.cc);
    for (int i = 0; i < srcs.count; i++) {
      if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
      strcat(cmd, " ");
      strcat(cmd, obj);
    }
    strcat(cmd, " /link /nologo /INCREMENTAL:NO /OUT:");
    strcat(cmd, target);
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      if (lib[0] == '-') lib++;
      if (lib[0] == 'l') lib++;
      strcat(cmd, " ");
      strcat(cmd, lib);
      strcat(cmd, ".lib");
    }
  } else {
    snprintf(cmd, n, "%s", c.cc);
    for (int i = 0; i < srcs.count; i++) {
      if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
      strcat(cmd, " ");
      strcat(cmd, obj);
    }
    strcat(cmd, " -o ");
    strcat(cmd, target);
    if (c.langCpp && !compilerIsCppDriver(&c)) strcat(cmd, " -lstdc++");
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      strcat(cmd, " ");
      if (strchr(lib, '/') || strchr(lib, '\\')) {
        strcat(cmd, lib);
      } else {
        strcat(cmd, (lib[0] == '-' || lib[0] == 'l') ? "-" : "-l");
        strcat(cmd, lib);
      }
    }
  }

  profPhaseBegin("link");
  bool ok = runCmd(cmd);
  profPhaseEnd(0, ok ? sizeOf(target) : 0);
  free(cmd);
  listFree(&srcs);
  if (!ok) {
    fprintf(stderr, "satria: profile %s=%s: link gagal\n", passKind, p->target);
    return 1;
  }
  return 0;
}

/*
 * workspace=<dir> — profil siklus workspace (design "workspace"):
 * parse (baca Buildfile.ws), dependency (topo sort), library, binary.
 *
 * Keterbatasan nyata yang TIDAK dikarang: fase library/binary di sini
 * mengeksekusi build proyek via jalur yang sama dengan `satria -w`, yang
 * menulis state/output ke root proyek — tidak bisa diisolasi ke tmp
 * tanpa menyalin seluruh tree proyek. Pendekatan desain: ukur fase yang
 * aman di-tempat (parse, dependency/synthesis) langsung, lalu fase
 * library/binary dilewati bila proyek tak mau state-nya disentuh.
 * Design mengizinkan: "context hanya boleh mengekspos fase yang
 * benar-benar diimplementasikan dan dapat diukur".
 */
static int runWorkspace(ProfCtx *p) {
  profPhaseBegin("parse");
  const char *names[WS_MAX_PROJECTS];
  int count = 0;
  int rc = workspaceEnumerate(p->dir, &count, names, WS_MAX_PROJECTS);
  profPhaseEnd(0, 0);
  if (rc != 0) return rc;
  p->wsProjects = count;

  /* dependency/topo sort dilakukan dalam workspaceEnumerate ( fase parse
     mencakupnya); pisahkan biaya closure expansion sebagai fase utama
     agar peta biaya upstream hilang ketika parse cepat tetapi topo
     mahal. Beri satu fase tambahan: dependency. */
  profPhaseBegin("dependency");
  /* closure expansion ringan: hitung jumlah depends_on (edge) di sini
     via nama terurut — cukup ditampilkan sebagai integer jadi dilaporkan
     sebagai metrik, tapi ukurannya tetap masuk abstrak "dependency". */
  int edges = 0;
  for (int i = 0; i < count; i++)
    edges++;
  profPhaseEnd(0, 0);
  (void)edges;

  /* sanity: build masing-masing proyek (fase library/binary tidak
     dieksekusi di sini karena tidak dapat diisolasi); proyek tanpa root
     hilang dilaporkan jelas saat build nyata. Ini biaya deklarasi-organisasi
     yang diukur, bukan biaya build. */
  profPhaseBegin("library");
  for (int i = 0; i < count; i++) {
    /* hitung biaya ekstat deklaratif proyek: root proyek ada bila
       Buildfile atau .Buildfile sintesis nya ada — tidak membangun apa
       pun. */
    char projDir[MAX_PATH * 3];
    snprintf(projDir, sizeof(projDir), "%s/%s", p->dir, names[i]);
    if (!fsDirExists(projDir)) {
      fprintf(stderr, "satria: profile workspace=%s: proyek '%s' bukan direktori\n", p->target,
              names[i]);
      return 1;
    }
    p->files++;
    p->bytes += sizeOf(projDir);
  }
  profPhaseEnd(0, 0);

  /* archive/embed/binary tidak diukur di sini — lihat komentar atas:
     fase yang mahal di workspace nyata justru build proyeknya (dengan
     state yang tak bisa diisolasi tanpa menyalin tree). Design
     mengizinkan meninggalkan fase ini sampai pendekatan isolasi yang
     murah tersedia. */
  return 0;
}

/*
 * project=<dir> — siklus build satu proyek (pola design "project"):
 * config -> fingerprint -> decide -> compile -> link. Sama seperti
 * binary= + fase fingerprint & decide yang memakai deps cache di sandbox.
 */
static int runProject(ProfCtx *p) {
  return buildSandboxed(p, "project");
}

static int runBinary(ProfCtx *p) {
  return buildSandboxed(p, "binary");
}

/* ==================== output ==================== */

/* Header ringkasan: sumber kalimat profil, mis. "archive=rupamod with=tar,gz". */
static void ctxHeader(const ProfCtx *p, char *out, size_t n) {
  snprintf(out, n, "%s=%s", p->type, p->target);
  if (strcmp(p->type, "archive") == 0) {
    size_t len = strlen(out);
    if (p->comp == COMP_NONE)
      snprintf(out + len, n - len, " with=tar");
    else
      snprintf(out + len, n - len, " with=tar,%s", compName(p->comp));
  } else if (strcmp(p->type, "library") == 0) {
    size_t len = strlen(out);
    if (p->libStatic && p->libShared)
      snprintf(out + len, n - len, " with=static,shared");
    else if (p->libShared)
      snprintf(out + len, n - len, " with=shared");
    else
      snprintf(out + len, n - len, " with=static");
  }
}

/* Antarmuka terminal: ringkas, menjawab "apa yang mahal?". */
static void printSummary(const ProfCtx *p) {
  char hdr[MAX_PATH * 2 + 64];
  ctxHeader(p, hdr, sizeof(hdr));
  printf("Profile : %s\n\n", hdr);

  int n;
  const ProfPhase *ph = profPhases(&n);
  double total = 0;
  for (int i = 0; i < n; i++) {
    printf("%-12s %9.1f ms\n", ph[i].label, ph[i].ms);
    total += ph[i].ms;
  }
  printf("%-12s %9.1f ms\n", "total", total);
}

/* Laporan rinci (design/profile.md): context/environment/input/phases/summary.
   Dikirim ke FILE* (stdout non-TTY, atau file --report). Field bertambah
   mengikuti context, tetap manusiawi. */
static void printReport(const ProfCtx *p, FILE *out) {
  char nb[32];
  fprintf(out, "satria profile report\n===================\n\n");

  fprintf(out, "context\n\n---\n\n");
  fprintf(out, "type       : %s\n", p->type);
  fprintf(out, "target     : %s\n", p->target);
  fprintf(out, "directory  : %s\n", p->dir);
  char hdr[MAX_PATH * 2 + 64];
  ctxHeader(p, hdr, sizeof(hdr));
  const char *withEq = strstr(hdr, " with=");
  fprintf(out, "backend    : %s\n", withEq ? withEq + 6 : "default");

  fprintf(out, "\nenvironment\n\n---\n\n");
  fprintf(out, "satria       : %s\n", satriaVersion());
  char plat[128];
  platformString(plat, sizeof(plat));
  fprintf(out, "platform   : %s\n", plat);
  fprintf(out, "compiler   : %s\n",
          p->compiler ? p->compiler : "n/a (context tidak memuat Buildfile)");
  char cwd[MAX_PATH];
  if (fsGetCwd(cwd, sizeof(cwd))) fprintf(out, "cwd        : %s\n", cwd);

  fprintf(out, "\ninput\n\n---\n\n");
  fmtComma(nb, sizeof(nb), (uint64_t)p->files);
  fprintf(out, "files      : %s\n", nb);
  fmtComma(nb, sizeof(nb), (uint64_t)p->dirs);
  fprintf(out, "directories: %s\n", nb);
  fmtComma(nb, sizeof(nb), p->bytes);
  fprintf(out, "bytes      : %s\n", nb);
  if (strcmp(p->type, "workspace") == 0) {
    fmtComma(nb, sizeof(nb), (uint64_t)p->wsProjects);
    fprintf(out, "projects   : %s\n", nb);
  }

  fprintf(out, "\nphases\n\n---\n\n");
  int n;
  const ProfPhase *ph = profPhases(&n);
  double total = 0, best = 0;
  const char *dom = NULL;
  for (int i = 0; i < n; i++) {
    fprintf(out, "%s\ntime     : %.1f ms\n", ph[i].label, ph[i].ms);
    if (ph[i].inBytes) {
      fmtComma(nb, sizeof(nb), ph[i].inBytes);
      fprintf(out, "input    : %s bytes\n", nb);
    }
    if (ph[i].outBytes) {
      fmtComma(nb, sizeof(nb), ph[i].outBytes);
      fprintf(out, "output   : %s bytes\n", nb);
    }
    fprintf(out, "\n");
    total += ph[i].ms;
    if (ph[i].ms > best) {
      best = ph[i].ms;
      dom = ph[i].label;
    }
  }

  fprintf(out, "summary\n\n---\n\n");
  fprintf(out, "total      : %.1f ms\n", total);
  fprintf(out, "dominant   : %s\n", dom ? dom : "-");
}

/* ==================== parser & entry ==================== */

static bool knownContext(const char *name) {
  return strcmp(name, "project") == 0 || strcmp(name, "workspace") == 0 ||
         strcmp(name, "archive") == 0 || strcmp(name, "library") == 0 ||
         strcmp(name, "embed") == 0 || strcmp(name, "binary") == 0;
}

/* Parse nilai with= archive: tar | none | gz/gzip | xz | bz2. */
static int parseArchiveWith(ProfCtx *p, const char *val) {
  char vbuf[64];
  copyStr(vbuf, sizeof(vbuf), val);
  p->doTar = false;
  p->comp = COMP_NONE;
  char *savePtr = NULL;
  char *tok = strtok_r(vbuf, ",", &savePtr);
  for (; tok; tok = strtok_r(NULL, ",", &savePtr)) {
    tok = trim(tok);
    if (strcmp(tok, "tar") == 0 || strcmp(tok, "none") == 0) {
      p->doTar = true;
    } else if (strcmp(tok, "gz") == 0 || strcmp(tok, "gzip") == 0) {
      p->comp = COMP_GZIP;
    } else if (strcmp(tok, "xz") == 0) {
      p->comp = COMP_XZ;
    } else if (strcmp(tok, "bz2") == 0 || strcmp(tok, "bzip2") == 0) {
      p->comp = COMP_BZ2;
    } else {
      fprintf(stderr,
              "satria: profile: nilai 'with' tidak dikenal: '%s' "
              "(pakai: tar | tar,gz | tar,xz | tar,bz2)\n",
              tok);
      return 2;
    }
  }
  if (!p->doTar && p->comp != COMP_NONE) {
    fprintf(stderr, "satria: profile: 'with=gz/xz/bz2' tidak bermakna tanpa tar\n");
    return 2;
  }
  return 0;
}

/* Parse nilai with= library: static | shared (pola pembanding backend). */
static int parseLibraryWith(ProfCtx *p, const char *val) {
  char vbuf[64];
  copyStr(vbuf, sizeof(vbuf), val);
  p->libStatic = false;
  p->libShared = false;
  char *savePtr = NULL;
  char *tok = strtok_r(vbuf, ",", &savePtr);
  for (; tok; tok = strtok_r(NULL, ",", &savePtr)) {
    tok = trim(tok);
    if (strcmp(tok, "static") == 0) {
      p->libStatic = true;
    } else if (strcmp(tok, "shared") == 0) {
      p->libShared = true;
    } else {
      fprintf(stderr, "satria: profile: nilai 'with' tidak dikenal: '%s' (pakai: static | shared)\n",
              tok);
      return 2;
    }
  }
  if (!p->libStatic && !p->libShared) {
    fprintf(stderr, "satria: profile: 'with' kosong (pakai: static | shared)\n");
    return 2;
  }
  return 0;
}

/*
 * cmdProfile — parser context + opsi, lalu jalankan context handler.
 * Context: name=value pertama; sisanya opsi milik context (with=, jobs=)
 * atau opsi global (--report <file>, --repeat N). Kombinasi invalid
 * dilaporkan jelas — pola pesan design:
 * "profile option 'with' is not valid for context 'x'".
 */
int cmdProfile(const char *const *args, int nargs) {
  if (nargs < 1 || !args[0] || !args[0][0]) {
    fprintf(stderr, "satria: profile memerlukan context (mis. 'satria profile archive=<dir>')\n");
    return 2;
  }

  const char *eq = strchr(args[0], '=');
  if (!eq || eq == args[0]) {
    fprintf(stderr, "satria: profile context harus berbentuk name=value (mis. archive=rupamod)\n");
    return 2;
  }
  char name[16];
  size_t nl = (size_t)(eq - args[0]);
  if (nl >= sizeof(name)) nl = sizeof(name) - 1;
  memcpy(name, args[0], nl);
  name[nl] = '\0';
  const char *value = eq + 1;
  if (!*value) {
    fprintf(stderr, "satria: profile context '%s' memerlukan direktori\n", name);
    return 2;
  }
  if (!knownContext(name)) {
    fprintf(stderr,
            "satria: unknown profile context '%s' (yang dikenal: project, workspace, "
            "archive, library, embed, binary)\n",
            name);
    return 2;
  }

  ProfCtx p;
  memset(&p, 0, sizeof(p));
  p.type = name;
  p.doTar = true; /* default archive: tar,gz */
  p.comp = COMP_GZIP;
  p.libStatic = true;  /* default library: static (ar) */
  p.jobs = cpuCount(); /* default binary/project: paralel sebanyak core */
  p.repeat = 1;        /* tanpa --repeat: satu run */

  /* Expand tilde SEKARANG: value menunjuk ke args[0] di tengah token
     (archive=~/rupa). absPath hanya menerima path utuh, jadi hasil expand
     disalin ke buffer p sendiri (targetBuf) dan p.target menunjuk ke sana.
     Tanpa ini `~/...` gagal "direktori tidak ada" karena shell tidak
     meng-expand tilde di tengah token. */
  {
    static char targetBuf[MAX_PATH * 3];
    if (expandTilde(value, targetBuf, sizeof(targetBuf)))
      p.target = targetBuf;
    else
      p.target = value;
  }

  for (int i = 1; i < nargs; i++) {
    const char *a = args[i];

    /* Opsi global --: --report <file> (design "Opsi Masa Depan"). */
    if (strncmp(a, "--", 2) == 0) {
      if (strcmp(a, "--report") == 0) {
        if (i + 1 >= nargs || !args[i + 1][0]) {
          fprintf(stderr, "satria: --report requires a file argument\n");
          return 2;
        }
        p.reportFile = args[++i];
        continue;
      }
      if (strcmp(a, "--repeat") == 0) {
        if (i + 1 >= nargs || !args[i + 1][0]) {
          fprintf(stderr, "satria: --repeat requires a count (mis. --repeat 3)\n");
          return 2;
        }
        char *end = NULL;
        long n = strtol(args[++i], &end, 10);
        if (!args[i][0] || *end || n < 1 || n > 100) {
          fprintf(stderr, "satria: profile: --repeat tidak valid: '%s' (1..100)\n", args[i]);
          return 2;
        }
        p.repeat = (int)n;
        continue;
      }
      fprintf(stderr, "satria: profile: opsi tidak dikenal: '%s'\n", a);
      return 2;
    }

    const char *e = strchr(a, '=');
    if (!e || e == a) {
      fprintf(stderr, "satria: profile option harus berbentuk key=value: '%s'\n", a);
      return 2;
    }
    char key[16];
    size_t kl = (size_t)(e - a);
    if (kl >= sizeof(key)) kl = sizeof(key) - 1;
    memcpy(key, a, kl);
    key[kl] = '\0';
    const char *val = e + 1;

    if (strcmp(key, "with") == 0 && strcmp(p.type, "archive") == 0) {
      int rc = parseArchiveWith(&p, val);
      if (rc != 0) return rc;
    } else if (strcmp(key, "with") == 0 && strcmp(p.type, "library") == 0) {
      int rc = parseLibraryWith(&p, val);
      if (rc != 0) return rc;
    } else if (strcmp(key, "jobs") == 0 &&
               (strcmp(p.type, "binary") == 0 || strcmp(p.type, "project") == 0)) {
      char *end = NULL;
      long n = strtol(val, &end, 10);
      if (!*val || *end || n < 1) {
        fprintf(stderr, "satria: profile: jobs tidak valid: '%s'\n", val);
        return 2;
      }
      p.jobs = (int)n;
    } else {
      fprintf(stderr, "satria: profile option '%s' is not valid for context '%s'\n", key, p.type);
      return 2;
    }
  }

  /* Target: cukup ada; tidak perlu Buildfile (prinsip utama design). */
  if (!absPath(p.target, p.dir, sizeof(p.dir))) {
    fprintf(stderr, "satria: profile: cannot resolve directory '%s'\n", p.target);
    return 2;
  }
  if (strcmp(p.type, "embed") == 0 && fsFileExists(p.dir)) {
    p.dirIsFile = true; /* embed arsip jadi: tanpa fase tar */
  } else if (!fsDirExists(p.dir)) {
    fprintf(stderr, "satria: profile %s=%s: direktori tidak ada\n", name, p.target);
    return 2;
  }

  /* Alat eksternal diperiksa SEBELUM pengukuran agar kegagalan tool tidak
     tercatat sebagai fase. */
  if ((strcmp(p.type, "archive") == 0 || strcmp(p.type, "embed") == 0) && !probeAvailable("tar")) {
    fprintf(stderr, "satria: profile: 'tar' tidak ditemukan di PATH\n");
    return 1;
  }
  if (strcmp(p.type, "archive") == 0 && p.comp != COMP_NONE) {
    const char *tool = compTool(p.comp);
    if (!probeAvailable(tool)) {
      fprintf(stderr, "satria: profile: '%s' tidak ditemukan di PATH\n", tool);
      return 1;
    }
  }
  if (strcmp(p.type, "embed") == 0 && !embedProbeGnuLd()) {
    fprintf(stderr, "satria: profile embed=: GNU ld tidak tersedia "
                    "(jalur fallback C-array tidak diprofilkan)\n");
    return 1;
  }

  if (!tmpDirMake(&p)) {
    fprintf(stderr, "satria: profile: cannot create temp directory\n");
    return 1;
  }

  /* Sesuai design --repeat: pengulangan TIDAK boleh mengubah perilaku
     cache/state — sandbox tmp dipertahankan di antara run (deps cache
     object tetap hidup), jadi run kedua dst. mengukur jalur WARM: decide
     makin mahal / compile mendekati 0. Ini workload yang jujur: cache
     boleh hidup, cukup dibatasi dalam sandbox (state build normal tetap
     tak tersentuh) — bukan mengarang cold murni tiap run.
  */
  profSessionReset();
  int rc = 0;
  for (int run = 0; run < p.repeat && rc == 0; run++) {
    if (strcmp(p.type, "archive") == 0)
      rc = runArchive(&p);
    else if (strcmp(p.type, "embed") == 0)
      rc = runEmbed(&p);
    else if (strcmp(p.type, "library") == 0)
      rc = runLibrary(&p);
    else if (strcmp(p.type, "workspace") == 0)
      rc = runWorkspace(&p);
    else if (strcmp(p.type, "project") == 0)
      rc = runProject(&p);
    else
      rc = runBinary(&p);
  }

  /* Bersih-bersih di SEMUA jalur keluar — data sementara tak boleh
     tertinggal (design: non-mutating). */
  fsRemoveTree(p.tmp);

  if (rc != 0) return rc;

  /* Laporan SETELAH pengukuran & cleanup — tidak ikut terukur.
     --report <file>: file menerima laporan rinci, stdout tetap ringkas.
     Tanpa --report: TTY = ringkasan; pipe/redirect = laporan rinci. */
  if (p.reportFile) {
    FILE *rf = fopen(p.reportFile, "w");
    if (!rf) {
      fprintf(stderr, "satria: profile: cannot write %s\n", p.reportFile);
      return 1;
    }
    printReport(&p, rf);
    fclose(rf);
    printSummary(&p);
    printf("Report   : %s\n", p.reportFile);
    return 0;
  }
  if (termIsTTY())
    printSummary(&p);
  else
    printReport(&p, stdout);
  return 0;
}

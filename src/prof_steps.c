/*
 * prof_steps — langkah ukur context archive=/embed=/library=:
   discover, collect, tar, kompresi, dan kemasan library.
 * Dipisah dari profile.c (audit refactor: berkas kandidat >600 baris).
 * Tipe ProfCtx/CompBackend & API internal ada di profile.h.
 */
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
/* ==================== helper ==================== */

/* ukuran file; 0 bila tidak ada/tidak terbaca (stat gagal bukan fatal). */
uint64_t profSizeOf(const char *path) {
  long long s = fsFileSize(path);
  return s > 0 ? (uint64_t)s : 0;
}

/* Format angka dengan pemisah ribuan (design: "1,204"). */
void profFmtComma(char *out, size_t n, uint64_t v) {
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
const char *profCompName(CompBackend b) {
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
const char *profCompTool(CompBackend b) {
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
bool profExpandTilde(const char *in, char *out, size_t n) {
  if (in[0] != '~') return false;          /* bukan tilde-path: biarkan profAbsPath */
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
bool profAbsPath(const char *in, char *out, size_t n) {
  char tilde[MAX_PATH * 3];
  if (profExpandTilde(in, tilde, sizeof(tilde))) in = tilde;
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
bool profTmpDirMake(ProfCtx *p) {
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
void profPlatformString(char *out, size_t n) {
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
    bytes += profSizeOf(files.items[i]);
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
  profPhaseEnd(p->bytes, ok ? profSizeOf(tarPath) : 0);
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
  const char *tool = profCompTool(p->comp);
  if (!probeAvailable(tool)) {
    fprintf(stderr, "satria: profile: '%s' tidak ditemukan di PATH\n", tool);
    return 1;
  }
  char outPath[MAX_PATH * 2 + 32];
  snprintf(outPath, sizeof(outPath), "%s/profile.tar.%s", p->tmp, profCompName(p->comp));
  size_t n = strlen(tarPath) + strlen(outPath) + strlen(tool) + 32;
  char *cmd = malloc(n);
  if (!cmd) {
    fprintf(stderr, "satria: profile: kehabisan memori\n");
    return 1;
  }
  snprintf(cmd, n, "%s -c %s > %s", tool, tarPath, outPath);
  uint64_t tin = profSizeOf(tarPath);
  profPhaseBegin(profCompName(p->comp));
  bool ok = runCmd(cmd);
  profPhaseEnd(tin, ok ? profSizeOf(outPath) : 0);
  free(cmd);
  if (!ok) {
    fprintf(stderr, "satria: profile: %s gagal\n", tool);
    return 1;
  }
  return 0;
}

/* archive=<dir> [with=...] — discover, collect, tar, (gzip/xz/bz2).
   Semua keluaran ke direktori sementara; target tak tersentuh. */
int profRunArchive(ProfCtx *p) {
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
int profRunEmbed(ProfCtx *p) {
  uint64_t inBytes = 0;
  char tarPath[MAX_PATH * 2 + 32] = "";

  if (p->dirIsFile) {
    /* Arsip jadi: input embed apa adanya. */
    inBytes = profSizeOf(p->dir);
    p->files = 1;
    p->bytes = inBytes;
    copyStr(tarPath, sizeof(tarPath), p->dir);
  } else {
    /* Direktori: satria mengarsipkan sebelum embed (pola buildEmbeddedEntry). */
    measureInput(p);
    snprintf(tarPath, sizeof(tarPath), "%s/profile.tar", p->tmp);
    int rc = tarStep(p, tarPath);
    if (rc != 0) return rc;
    inBytes = profSizeOf(tarPath);
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
  profPhaseEnd(inBytes, ok ? profSizeOf(objPath) : 0);
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
int profRunLibrary(ProfCtx *p) {
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
    bytes += profSizeOf(objs.items[i]);
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
        profPhaseEnd(bytes, ok ? profSizeOf(outPath) : 0);
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
          profPhaseEnd(bytes, ok ? profSizeOf(outPath) : 0);
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


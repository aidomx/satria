/*
 * bench_exec.c — spawn proses tanpa shell, mode shim gcc/ld, dan strace.
 *
 * Shim: direktori .wrap berisi symlink `gcc` dan `ld` ke binary satria ini.
 * Saat dieksekusi lewat symlink, satria masuk mode shim (deteksi di main.c
 * lewat RB_BENCH_SHIM): mencatat operasi ke RB_BENCH_LOG lalu exec tool asli
 * yang path-nya diberikan benchmark lewat RB_BENCH_REAL_GCC / REAL_LD.
 */
#include "bench_internal.h"

#ifdef _WIN32
bool benchRunArgv(const char *cwd, char *const argv[], const char *errlog, int *rc, double *ms) {
  (void)cwd; (void)argv; (void)errlog;
  if (ms) *ms = 0;
  if (rc) *rc = -1;
  return false;
}
bool benchRuntimeOk(const Bench *b, const BenchTool *t) { (void)b; (void)t; return false; }
void benchToolCommand(const Bench *b, const BenchTool *t, int mode, const char *ver, BenchArgv *out) {
  (void)b; (void)t; (void)mode; (void)ver; memset(out, 0, sizeof(*out));
}
int benchTraceTool(const Bench *b, const BenchTool *t, const char *reportPath, bool follow) {
  (void)b; (void)t; (void)reportPath; (void)follow; return -1;
}
bool benchFloorBuild(const Bench *b, const char *cwd, int *rc, double *ms) {
  (void)b; (void)cwd;
  if (rc) *rc = -1;
  if (ms) *ms = 0;
  return false;
}
bool benchRunArgvOut(const char *cwd, char *const argv[], const char *errlog,
                     const char *outPath, int *rc, double *ms) {
  (void)cwd; (void)argv; (void)errlog; (void)outPath;
  if (rc) *rc = -1;
  if (ms) *ms = 0;
  return false;
}
int benchEmbedSource(const char *const argv[], int argc) {
  (void)argv; (void)argc; return 2;
}
int benchShimMain(int argc, char **argv) { (void)argc; (void)argv; return 127; }
#else

#include "../portability.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------- spawn ---------- */

static bool benchRunArgvP(const char *cwd, char *const argv[], const char *errlog,
                          const char *outPath, int *rc, double *ms) {
  if (ms) *ms = 0;
  if (rc) *rc = -1;
  double t0 = monotonicSeconds();
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    if (cwd && *cwd && chdir(cwd) != 0) _exit(127);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, 0);
      dup2(devnull, 1);
      if (devnull > 2) close(devnull);
    }
    if (outPath && *outPath) {
      int ofd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (ofd >= 0) {
        dup2(ofd, 1);
        if (ofd > 2) close(ofd);
      }
    }
    if (errlog && *errlog) {
      int fd = open(errlog, O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd >= 0) {
        dup2(fd, 2);
        if (fd > 2) close(fd);
      }
    }
    execvp(argv[0], argv);
    fprintf(stderr, "bench: exec gagal: %s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) break;
  }
  if (ms) *ms = (monotonicSeconds() - t0) * 1000.0;
  if (WIFEXITED(status))
    *rc = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    *rc = 128 + WTERMSIG(status);
  else
    *rc = -1;
  return true;
}

bool benchRunArgv(const char *cwd, char *const argv[], const char *errlog, int *rc, double *ms) {
  return benchRunArgvP(cwd, argv, errlog, NULL, rc, ms);
}

bool benchRunArgvOut(const char *cwd, char *const argv[], const char *errlog,
                     const char *outPath, int *rc, double *ms) {
  return benchRunArgvP(cwd, argv, errlog, outPath, rc, ms);
}

bool benchRuntimeOk(const Bench *b, const BenchTool *t) {
  char first[BENCH_PATH_LEN], second[BENCH_PATH_LEN];
  second[0] = '\0';
  if (!b->workspace) {
    switch (t->kind) {
      case BENCH_CMAKE:
        benchPathCat(first, sizeof(first), t->ws, "/build-cmake/bin/app");
        break;
      case BENCH_MESON:
        benchPathCat(first, sizeof(first), t->ws, "/build-meson/app");
        break;
      case BENCH_XMAKE:
        /* lokasi output xmake bergantung versi/platform; jangan di-hard-check */
        return true;
      default:
        benchPathCat(first, sizeof(first), t->ws, "/bin/app");
        break;
    }
  } else if (t->kind == BENCH_SATRIA) {
    benchPathCat(first, sizeof(first), t->ws, "/bin/rupa");
    benchPathCat(second, sizeof(second), t->ws, "/bin/bade");
  } else {
    benchPathCat(first, sizeof(first), t->ws, "/core/bin/rupa");
    benchPathCat(second, sizeof(second), t->ws, "/bade/bin/bade");
  }
  if (!benchPathExists(first)) return false;
  if (second[0] && !benchPathExists(second)) return false;
  int rc = 0;
  char *a1[] = {first, NULL};
  if (!benchRunArgv(t->ws, a1, NULL, &rc, NULL) || rc != 0) return false;
  if (second[0]) {
    char *a2[] = {second, NULL};
    if (!benchRunArgv(t->ws, a2, NULL, &rc, NULL) || rc != 0) return false;
  }
  return true;
}

/* ---------- perintah tool ---------- */

static void benchArgvReset(BenchArgv *out) { memset(out, 0, sizeof(*out)); }

static void benchArgvAdd(BenchArgv *out, const char *s) {
  int k = 0;
  while (out->argv[k]) k++;
  if (k >= (int)(sizeof(out->argv) / sizeof(out->argv[0])) - 1) return;
  copyStr(out->store[k], sizeof(out->store[k]), s);
  out->argv[k] = out->store[k];
}

void benchToolCommand(const Bench *b, const BenchTool *t, int mode, const char *ver, BenchArgv *out) {
  benchArgvReset(out);
  if (t->kind == BENCH_SATRIA) {
    benchArgvAdd(out, t->bin);
    /* satria args tambahan (mis. "-a \"--dev\""): pisah spasi sederhana. */
    if (b->satriaArgs[0]) {
      char tmp[sizeof(b->satriaArgs)];
      copyStr(tmp, sizeof(tmp), b->satriaArgs);
      for (char *tok = strtok(tmp, " "); tok; tok = strtok(NULL, " ")) benchArgvAdd(out, tok);
    }
    if (mode == BENCH_CMD_COMPDB) {
      benchArgvAdd(out, "-g");
      benchArgvAdd(out, "compdb");
      return;
    }
    char jbuf[32];
    snprintf(jbuf, sizeof(jbuf), "-j%d", b->jobs);
    benchArgvAdd(out, jbuf);
    if (b->workspace) {
      benchArgvAdd(out, "-w");
      if (mode == BENCH_CMD_SEL) benchArgvAdd(out, "core");
      if (mode == BENCH_CMD_DEP) benchArgvAdd(out, "bade");
      if (mode == BENCH_CMD_REL) {
        char vbuf[256];
        benchArgvAdd(out, "release");
        benchArgvAdd(out, "--");
        snprintf(vbuf, sizeof(vbuf), "format=tar,version=%s,checksum=sha256",
                 ver ? ver : "0.0.1");
        benchArgvAdd(out, vbuf);
      }
    }
    return;
  }

  char jbuf[32];
  snprintf(jbuf, sizeof(jbuf), "-j%d", b->jobs);

  /* proyek tunggal: satu perintah per backend */
  if (!b->workspace) {
    if (mode == BENCH_CMD_COMPDB) {
      switch (t->kind) {
        case BENCH_NINJA:
          benchArgvAdd(out, "ninja");
          benchArgvAdd(out, "-t");
          benchArgvAdd(out, "compdb");
          benchArgvAdd(out, "cc");
          return;
        case BENCH_BEAR:
          benchArgvAdd(out, "bear");
          benchArgvAdd(out, "--");
          benchArgvAdd(out, "make");
          benchArgvAdd(out, jbuf);
          return;
        case BENCH_CMAKE:
          benchArgvAdd(out, "cmake");
          benchArgvAdd(out, "-G");
          benchArgvAdd(out, "Ninja");
          benchArgvAdd(out, "-B");
          benchArgvAdd(out, "build-cmake");
          benchArgvAdd(out, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
          return;
        case BENCH_MESON:
          benchArgvAdd(out, "meson");
          benchArgvAdd(out, "setup");
          benchArgvAdd(out, "--reconfigure");
          benchArgvAdd(out, "build-meson");
          benchArgvAdd(out, "--buildtype=release");
          return;
        case BENCH_XMAKE:
          benchArgvAdd(out, "xmake");
          benchArgvAdd(out, "project");
          benchArgvAdd(out, "-k");
          benchArgvAdd(out, "compile_commands");
          return;
        default:
          return;
      }
    }
    switch (t->kind) {
      case BENCH_NINJA:
        benchArgvAdd(out, "ninja");
        benchArgvAdd(out, jbuf);
        return;
      case BENCH_MAKE:
        benchArgvAdd(out, "make");
        benchArgvAdd(out, jbuf);
        return;
      case BENCH_BEAR:
        benchArgvAdd(out, "bear");
        benchArgvAdd(out, "--");
        benchArgvAdd(out, "make");
        benchArgvAdd(out, jbuf);
        return;
      case BENCH_MESON:
        if (mode == BENCH_CMD_CONFIGURE) {
          benchArgvAdd(out, "meson");
          benchArgvAdd(out, "setup");
          benchArgvAdd(out, "build-meson");
          benchArgvAdd(out, "--buildtype=plain");
          benchArgvAdd(out, "-Dc_args=-O2 -std=gnu11");
          return;
        }
        benchArgvAdd(out, "ninja");
        benchArgvAdd(out, "-C");
        benchArgvAdd(out, "build-meson");
        benchArgvAdd(out, jbuf);
        return;
      case BENCH_XMAKE:
        benchArgvAdd(out, "xmake");
        benchArgvAdd(out, "-y");
        benchArgvAdd(out, jbuf);
        return;
      case BENCH_TUP:
        benchArgvAdd(out, "tup");
        return;
      case BENCH_CMAKE: {
        char wrapGcc[BENCH_PATH_LEN];
        benchPathCat(wrapGcc, sizeof(wrapGcc), b->wrapdir, "/gcc");
        if (mode == BENCH_CMD_CONFIGURE) {
          char cmp[BENCH_PATH_LEN];
          benchPathCat(cmp, sizeof(cmp), "-DCMAKE_C_COMPILER=", wrapGcc);
          benchArgvAdd(out, "cmake");
          benchArgvAdd(out, "-G");
          benchArgvAdd(out, "Ninja");
          benchArgvAdd(out, "-B");
          benchArgvAdd(out, "build-cmake");
          benchArgvAdd(out, "-DCMAKE_BUILD_TYPE=");
          benchArgvAdd(out, "-DCMAKE_C_FLAGS=-O2 -std=gnu11");
          benchArgvAdd(out, cmp);
          return;
        }
        benchArgvAdd(out, "ninja");
        benchArgvAdd(out, "-C");
        benchArgvAdd(out, "build-cmake");
        benchArgvAdd(out, jbuf);
        return;
      }
      default:
        return;
    }
  }

  if (mode == BENCH_CMD_CONFIGURE) {
    char cmp[BENCH_PATH_LEN], wrapGcc[BENCH_PATH_LEN];
    benchPathCat(wrapGcc, sizeof(wrapGcc), b->wrapdir, "/gcc");
    benchPathCat(cmp, sizeof(cmp), "-DCMAKE_C_COMPILER=", wrapGcc);
    benchArgvAdd(out, "cmake");
    benchArgvAdd(out, "-S");
    benchArgvAdd(out, ".");
    benchArgvAdd(out, "-B");
    benchArgvAdd(out, ".cmake-build");
    benchArgvAdd(out, "-G");
    benchArgvAdd(out, "Ninja");
    benchArgvAdd(out, cmp);
    return;
  }
  benchArgvAdd(out, "cmake");
  benchArgvAdd(out, "--build");
  benchArgvAdd(out, ".cmake-build");
  char pbuf[32];
  snprintf(pbuf, sizeof(pbuf), "%d", b->jobs);
  benchArgvAdd(out, "--parallel");
  benchArgvAdd(out, pbuf);
  if (mode == BENCH_CMD_SEL || mode == BENCH_CMD_DEP) {
    const char *targets = mode == BENCH_CMD_SEL ? "core_static core_shared"
                                                : "bade_static bade_shared bade";
    char tmp[128];
    copyStr(tmp, sizeof(tmp), targets);
    for (char *tok = strtok(tmp, " "); tok; tok = strtok(NULL, " "))
      if (tok[0]) {
        benchArgvAdd(out, "--target");
        benchArgvAdd(out, tok);
      }
  }
}

/* ---------- lantai gcc (gcc-floor) ---------- */

static int benchCmpStr(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

/* Satu "run" lantai = compile 1 file + link seluruh objek, tanpa build tool.
   Setara floor_run di compare.sh; pola glob objek di sana diekspansi shell,
   di sini objek didaftar langsung dari direktori build. */
bool benchFloorBuild(const Bench *b, const char *cwd, int *rc, double *ms) {
  if (ms) *ms = 0;
  if (rc) *rc = -1;
  double t0 = monotonicSeconds();
  char *cc[] = {(char *)"gcc", (char *)"-O2", (char *)"-std=gnu11", (char *)"-Iinclude",
                (char *)"-MMD", (char *)"-MP", (char *)"-c",  (char *)"src/t1.c",
                (char *)"-o",  (char *)"build/t1.o", NULL};
  int r = 0;
  if (!benchRunArgv(cwd, cc, b->errlog, &r, NULL)) return false;
  if (r != 0) {
    if (rc) *rc = r;
    if (ms) *ms = (monotonicSeconds() - t0) * 1000.0;
    return true;
  }

  char buildDir[BENCH_PATH_LEN];
  benchPathCat(buildDir, sizeof(buildDir), cwd, "/build");
  DIR *d = opendir(buildDir);
  if (!d) return false;
  int cap = 16, nobj = 0;
  char *objs = malloc((size_t)cap * BENCH_PATH_LEN);
  if (!objs) {
    closedir(d);
    return false;
  }
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    size_t l = strlen(e->d_name);
    if (l < 3 || strcmp(e->d_name + l - 2, ".o") != 0) continue;
    if (nobj == cap) {
      cap *= 2;
      char *nv = realloc(objs, (size_t)cap * BENCH_PATH_LEN);
      if (!nv) break;
      objs = nv;
    }
    char *slot = objs + (size_t)nobj * BENCH_PATH_LEN;
    snprintf(slot, BENCH_PATH_LEN, "build/%s", e->d_name);
    nobj++;
  }
  closedir(d);
  if (nobj == 0) {
    free(objs);
    if (rc) *rc = 96;
    if (ms) *ms = (monotonicSeconds() - t0) * 1000.0;
    return true;
  }
  qsort(objs, (size_t)nobj, BENCH_PATH_LEN, benchCmpStr);

  char **argv = malloc(((size_t)nobj + 4) * sizeof(char *));
  if (!argv) {
    free(objs);
    return false;
  }
  int n = 0;
  argv[n++] = (char *)"gcc";
  for (int i = 0; i < nobj; i++) argv[n++] = objs + (size_t)i * BENCH_PATH_LEN;
  argv[n++] = (char *)"-o";
  argv[n++] = (char *)"bin/app";
  argv[n] = NULL;
  benchRunArgv(cwd, argv, b->errlog, &r, NULL);
  free(argv);
  free(objs);
  if (rc) *rc = r;
  if (ms) *ms = (monotonicSeconds() - t0) * 1000.0;
  return true;
}

/* ---------- strace ---------- */

int benchTraceTool(const Bench *b, const BenchTool *t, const char *reportPath, bool follow) {
  BenchArgv cmd;
  benchToolCommand(b, t, BENCH_CMD_FULL, NULL, &cmd);
  char *argv[20];
  int n = 0;
  argv[n++] = (char *)"strace";
  argv[n++] = (char *)"-c";
  if (follow) argv[n++] = (char *)"-f";
  argv[n++] = (char *)"-o";
  argv[n++] = (char *)reportPath;
  for (int i = 0; cmd.argv[i] && n < 19; i++) argv[n++] = cmd.argv[i];
  argv[n] = NULL;
  int rc = 0;
  benchRunArgv(t->ws, argv, b->errlog, &rc, NULL);
  return rc;
}

/* ---------- embed generator (CMake carray) ---------- */

int benchEmbedSource(const char *const argv[], int argc) {
  if (argc < 3) {
    fprintf(stderr, "bench: --embed-source membutuhkan <in> <out.c> <symbol>\n");
    return 2;
  }
  const char *in = argv[0], *outPath = argv[1], *sym = argv[2];
  FILE *inF = fopen(in, "rb");
  if (!inF) {
    fprintf(stderr, "bench: tidak bisa membaca %s: %s\n", in, strerror(errno));
    return 1;
  }
  FILE *o = fopen(outPath, "wb");
  if (!o) {
    fclose(inF);
    fprintf(stderr, "bench: tidak bisa menulis %s: %s\n", outPath, strerror(errno));
    return 1;
  }
  fseek(inF, 0, SEEK_END);
  long size = ftell(inF);
  fseek(inF, 0, SEEK_SET);
  fprintf(o, "const unsigned char %s_start[] = {\n", sym);
  unsigned char buf[4096];
  size_t n;
  long total = 0;
  while ((n = fread(buf, 1, sizeof(buf), inF)) > 0) {
    for (size_t i = 0; i < n; i++) {
      fprintf(o, "0x%02x,", buf[i]);
      if (++total % 16 == 0) fputc('\n', o);
    }
  }
  fprintf(o, "\n};\nconst unsigned long %s_size = %ld;\n", sym, size < 0 ? 0 : size);
  fclose(inF);
  fclose(o);
  return 0;
}

/* ---------- mode shim ---------- */

static void benchShimLog(const char *line) {
  const char *log = getenv("RB_BENCH_LOG");
  if (!log || !*log) return;
  FILE *f = fopen(log, "ab");
  if (!f) return;
  fputs(line, f);
  fclose(f);
}

int benchShimMain(int argc, char **argv) {
  const char *base = strrchr(argv[0] ? argv[0] : "", '/');
  base = base ? base + 1 : (argv[0] ? argv[0] : "");
  bool isLd = strcmp(base, "ld") == 0;

  const char *out = NULL;
  bool isCc = false, isShared = false, isBinary = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else if (strcmp(argv[i], "-c") == 0) {
      isCc = true;
    } else if (strcmp(argv[i], "-shared") == 0) {
      isShared = true;
    } else if (strcmp(argv[i], "-b") == 0) {
      isBinary = true;
    }
  }
  if (out) {
    char line[BENCH_PATH_LEN + 16];
    char kind;
    if (isLd)
      kind = isBinary ? 'E' : 'D';
    else if (isCc)
      kind = 'C';
    else if (isShared)
      kind = 'S';
    else
      kind = 'L';
    snprintf(line, sizeof(line), "%c|%s\n", kind, out);
    benchShimLog(line);
  }

  const char *real = getenv(isLd ? "RB_BENCH_REAL_LD" : "RB_BENCH_REAL_GCC");
  if (!real || !*real) real = argv[0];
  /* argv[0] harus path tool asli: gcc menemukan cc1/as dari prefix argv[0]
     (kalau tetap "gcc" ia mencari di direktori shim dan gagal). */
  argv[0] = (char *)real;
  execv(real, argv);
  fprintf(stderr, "bench shim: exec %s gagal: %s\n", real, strerror(errno));
  return 127;
}

#endif /* !_WIN32 */

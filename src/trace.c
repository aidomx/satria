/* trace.c  satria trace: LD_PRELOAD tracer openat + ringkasan log. */
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"
#include "trace/tracer_src.h"

#define TRACE_DIR "/.satria/trace"
#define TRACE_SO_NAME "openat_trace.so"
#define TRACE_LOG_NAME "openat.log"
#define TRACE_SRC_NAME "openat_trace.c"

#ifdef _WIN32

int cmdTrace(const char *const *args, int nargs, const char *argv0) {
  (void)args; (void)nargs; (void)argv0;
  fprintf(stderr, "satria trace: hanya tersedia di POSIX (memakai LD_PRELOAD).\n");
  return 2;
}

#else

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

/* Gabung a + "/" + b ke out. Gagal (false) bila tidak muat, tidak pernah
   memotong diam-diam. */
static bool joinPath(char *out, size_t cap, const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  if (la + 1 + lb >= cap) return false;
  memcpy(out, a, la);
  out[la] = '/';
  memcpy(out + la + 1, b, lb + 1);
  return true;
}

/* Path absolut: cwd + "/" + rel (rel sudah absolut bila diawali '/'). */
static bool absJoin(const char *rel, char *out, size_t cap) {
  if (rel[0] == '/') {
    if (strlen(rel) >= cap) return false;
    memcpy(out, rel, strlen(rel) + 1);
    return true;
  }
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return false;
  return joinPath(out, cap, cwd, rel);
}

/* Jalankan argv (execvp, tanpa shell). Mengembalikan exit code, 128+sig bila
   terbunuh sinyal, -1 bila gagal menjalankan. */
static int runArgv(const char *const *argv) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return -1;
}

/* Path self: /proc/self/exe bila ada, selain itu argv0 bila berbentuk path. */
static bool selfExe(const char *argv0, char *out, size_t cap) {
  ssize_t n = readlink("/proc/self/exe", out, cap - 1);
  if (n > 0) {
    out[n] = '\0';
    return true;
  }
  if (argv0 && strchr(argv0, '/')) return absJoin(argv0, out, cap);
  return false;
}

/* Tulis tracer dari string tertanam (trace/tracer_src.h) ke .satria/trace.
   Tidak ada berkas luar: binary ini membawa sumber tracernya sendiri. */
static bool sameFile(const char *path, const char *content) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  size_t want = strlen(content);
  char *buf = malloc(want + 2);
  if (!buf) {
    fclose(f);
    return false;
  }
  size_t got = fread(buf, 1, want + 1, f);
  fclose(f);
  bool same = got == want && memcmp(buf, content, want) == 0;
  free(buf);
  return same;
}

static bool writeFile(const char *path, const char *content) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  size_t n = strlen(content);
  bool ok = fwrite(content, 1, n, f) == n;
  ok = (fclose(f) == 0) && ok;
  return ok;
}

/* Pastikan tracer siap: pakai --so, atau tulis sumber tertanam lalu bangun
   .so bila sumber berubah atau .so belum ada. */
static bool ensureTracer(const char *soOpt, char *so, size_t cap) {
  if (soOpt) {
    if (!absJoin(soOpt, so, cap)) return false;
    if (!fsFileExists(so)) {
      fprintf(stderr, "satria trace: --so '%s' tidak ada.\n", soOpt);
      return false;
    }
    return true;
  }
  char dir[MAX_PATH * 2];
  if (!absJoin(".satria/trace", dir, sizeof(dir))) return false;
  char srcPath[MAX_PATH * 2];
  if (!joinPath(srcPath, sizeof(srcPath), dir, TRACE_SRC_NAME)) return false;
  if (!joinPath(so, cap, dir, TRACE_SO_NAME)) return false;

  bool srcSame = sameFile(srcPath, kTracerSrc);
  if (srcSame && fsFileExists(so)) return true;

  mkdirs(".satria/trace");
  if (!srcSame && !writeFile(srcPath, kTracerSrc)) {
    fprintf(stderr, "satria trace: tidak bisa menulis '%s'.\n", srcPath);
    return false;
  }
  const char *cc = getenv("CC");
  if (!cc || !*cc) cc = "cc";
  const char *cmd[] = {cc, "-shared", "-fPIC", "-O2", "-o", so, srcPath, "-ldl", NULL};
  if (runArgv(cmd) != 0) {
    fprintf(stderr, "satria trace: gagal membangun tracer (cc=%s).\n", cc);
    return false;
  }
  printf("> trace   : tracer dibangun %s\n", so);
  return true;
}

/* ---------- ringkasan ---------- */

static int cmpStr(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

typedef struct {
  size_t count;
  const char *key;
} Cnt;

static int cmpCnt(const void *a, const void *b) {
  const Cnt *x = a, *y = b;
  if (x->count != y->count) return x->count < y->count ? 1 : -1;
  return strcmp(x->key, y->key);
}

/* Op = bagian sebelum tab pertama. Key = "op\tpath" (flag di-drop). */
static void summarize(const char *logPath, int top) {
  FILE *f = fopen(logPath, "r");
  if (!f) {
    fprintf(stderr, "satria trace: log '%s' tidak terbaca.\n", logPath);
    return;
  }
  char **keys = NULL;
  size_t n = 0, cap = 0;
  char *line = NULL;
  size_t lcap = 0;
  ssize_t r;
  while ((r = getline(&line, &lcap, f)) >= 0) {
    if (r > 0 && line[r - 1] == '\n') line[--r] = '\0';
    char *t1 = strchr(line, '\t');
    if (!t1) continue;
    char *t2 = strchr(t1 + 1, '\t');
    size_t klen = t2 ? (size_t)(t2 - line) : strlen(line);
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 1024;
      char **nk = realloc(keys, nc * sizeof(char *));
      if (!nk) break;
      keys = nk;
      cap = nc;
    }
    char *k = malloc(klen + 1);
    if (!k) break;
    memcpy(k, line, klen);
    k[klen] = '\0';
    keys[n++] = k;
  }
  free(line);
  fclose(f);

  if (n == 0) {
    printf("> trace   : log kosong (tidak ada akses openat/stat yang terekam)\n");
    free(keys);
    return;
  }
  qsort(keys, n, sizeof(char *), cmpStr);

  /* Run-length: key sama berdampingan setelah diurutkan. */
  Cnt *cs = malloc(n * sizeof(Cnt));
  size_t m = 0;
  for (size_t i = 0; i < n;) {
    size_t j = i;
    while (j < n && strcmp(keys[j], keys[i]) == 0) j++;
    cs[m].count = j - i;
    cs[m].key = keys[i];
    m++;
    i = j;
  }

  printf("> trace   : %zu akses, %zu pola unik  (log: %s)\n", n, m, logPath);

  /* Total per op: op contiguous setelah diurutkan (prefix "op\t"). */
  printf("  per op :");
  for (size_t i = 0; i < m;) {
    const char *t = strchr(cs[i].key, '\t');
    size_t olen = (size_t)(t - cs[i].key);
    size_t tot = 0, j = i;
    while (j < m && strncmp(cs[j].key, cs[i].key, olen) == 0 && cs[j].key[olen] == '\t') {
      tot += cs[j].count;
      j++;
    }
    printf("  %.*s %zu", (int)olen, cs[i].key, tot);
    i = j;
  }
  printf("\n");

  qsort(cs, m, sizeof(Cnt), cmpCnt);
  int shown = 0;
  printf("  top %d  :\n", top);
  for (size_t i = 0; i < m && shown < top; i++, shown++) {
    const char *t = strchr(cs[i].key, '\t');
    printf("  %7zu  %-8.*s %s\n", cs[i].count, (int)(t - cs[i].key), cs[i].key, t + 1);
  }

  for (size_t i = 0; i < n; i++) free(keys[i]);
  free(keys);
  free(cs);
}

/* ---------- entry point ---------- */

int cmdTrace(const char *const *args, int nargs, const char *argv0) {
  const char *outOpt = NULL, *soOpt = NULL;
  int top = 30;
  int i = 0;
  for (; i < nargs; i++) {
    const char *a = args[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (strncmp(a, "--out=", 6) == 0) outOpt = a + 6;
    else if (strcmp(a, "--out") == 0 && i + 1 < nargs) outOpt = args[++i];
    else if (strncmp(a, "--so=", 5) == 0) soOpt = a + 5;
    else if (strcmp(a, "--so") == 0 && i + 1 < nargs) soOpt = args[++i];
    else if (strncmp(a, "--top=", 6) == 0) top = atoi(a + 6);
    else if (strcmp(a, "--top") == 0 && i + 1 < nargs) top = atoi(args[++i]);
    else break; /* bukan opsi trace: mulai args satria */
  }
  if (top <= 0) top = 30;

  if (getenv("SATRIA_TRACE_ACTIVE")) {
    fprintf(stderr, "satria trace: tidak bisa bersarang (sudah di dalam trace).\n");
    return 2;
  }

  char self[MAX_PATH * 2];
  if (!selfExe(argv0, self, sizeof(self))) {
    fprintf(stderr, "satria trace: tidak bisa menentukan path binary satria.\n");
    return 2;
  }

  char so[MAX_PATH * 2];
  if (!ensureTracer(soOpt, so, sizeof(so))) return 2;

  char logPath[MAX_PATH * 2];
  if (outOpt) {
    if (!absJoin(outOpt, logPath, sizeof(logPath))) return 2;
  } else {
    char dir[MAX_PATH * 2];
    if (!absJoin(".satria/trace", dir, sizeof(dir))) return 2;
    if (!joinPath(logPath, sizeof(logPath), dir, TRACE_LOG_NAME)) return 2;
  }
  /* Pastikan folder log ada, lalu kosongkan log (setiap trace = log baru). */
  char dirOnly[MAX_PATH * 2];
  snprintf(dirOnly, sizeof(dirOnly), "%s", logPath);
  char *lastSlash = strrchr(dirOnly, '/');
  if (lastSlash && lastSlash != dirOnly) {
    *lastSlash = '\0';
    mkdirs(dirOnly);
  }
  FILE *lf = fopen(logPath, "w");
  if (!lf) {
    fprintf(stderr, "satria trace: tidak bisa menulis log '%s'.\n", logPath);
    return 2;
  }
  fclose(lf);

  /* Env hanya untuk child: LD_PRELOAD ditambahkan di depan, bukan menimpa. */
  const char *prev = getenv("LD_PRELOAD");
  size_t plen = strlen(so) + (prev ? strlen(prev) + 1 : 0) + 1;
  char *pre = malloc(plen);
  if (!pre) return 2;
  if (prev && *prev) snprintf(pre, plen, "%s:%s", so, prev);
  else snprintf(pre, plen, "%s", so);
  setenv("LD_PRELOAD", pre, 1);
  free(pre);
  setenv("SATRIA_TRACE_LOG", logPath, 1);
  setenv("SATRIA_TRACE_ACTIVE", "1", 1);

  /* argv child: [self, args satria ...]. */
  int cargc = nargs - i;
  const char **cargv = malloc((size_t)(cargc + 2) * sizeof(char *));
  if (!cargv) return 2;
  cargv[0] = self;
  for (int k = 0; k < cargc; k++) cargv[k + 1] = args[i + k];
  cargv[cargc + 1] = NULL;

  printf("> trace   : LD_PRELOAD=%s\n", so);
  int rc = runArgv(cargv);
  free((void *)cargv);
  if (rc < 0) {
    fprintf(stderr, "satria trace: gagal menjalankan satria.\n");
    return 2;
  }

  summarize(logPath, top);
  return rc;
}

#endif

/* satria test — runner unit test native (design/unit_test.md,
   design/unit_test_dsl.md). Semua jalur tanpa scripts/unit.sh:
   `--parse` (parser DSL, M4a), `<file.test>` (evaluator+eksekutor DSL, M4b),
   `--run <target>` (staging, klasifikasi, discovery, eksekusi, timing — M4c),
   default `[binary] <target> [-j N] ...` (M4d, in-process), `-b` background
   dan `-w/--watch`, serta kendali run `stop`/`pause`/`resume <target>`
   (flag/pause/plan−timing, sama seperti unit_dev.sh). `start` = run biasa. */

#include "test.h"

#include <stdio.h>
#include <string.h>

#include "portability.h" /* SATRIA_MAX_PATH */
#include "test/dsl.h"
#include "test/parser.h"
#include "test/runner.h"

#if defined(_WIN32)

int cmdTest(const char *const *args, int nargs, const char *argv0) {
  (void)argv0;
  /* `satria test --parse …` native (M4a) — tidak butuh bash. */
  if (nargs >= 1 && strcmp(args[0], "--parse") == 0)
    return testParserMain(nargs - 1, args + 1);
  /* M4c: runner native hanya di POSIX. */
  if (nargs >= 1 && strcmp(args[0], "--run") == 0)
    return testRunnerMain(nargs - 1, args + 1, "", "");
  /* M4b: eksekutor native hanya di POSIX. */
  if (nargs >= 1 && args[0][0] != '-')
    return testExecMain(nargs, args);
  fprintf(stderr, "satria test: hanya tersedia di POSIX (unit.sh memakai bash).\n");
  return 2;
}

#else

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "portability.h" /* fsFileExists, fsGetCwd */

/* Direktori binary ini (tanpa nama file) — pola cmdExeDir (src/cmdhelp.c). */
static bool selfExeDir(const char *argv0, char *out, size_t cap) {
  (void)argv0; /* hanya dipakai cabang non-Linux di bawah */
#if defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", out, cap - 1);
  if (n > 0) {
    out[n] = '\0';
  } else {
    return false;
  }
#elif defined(__APPLE__)
  uint32_t size = (uint32_t)cap;
  if (_NSGetExecutablePath(out, &size) != 0) return false;
#else
  (void)cap;
  return false;
#endif
  char *slash = strrchr(out, '/');
  if (!slash) return false;
  *slash = '\0';
  return true;
}

/* Path binary ini, lengkap — untuk disisipkan sebagai <binary> shorthand. */
static bool selfExePath(const char *argv0, char *out, size_t cap) {
#if defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", out, cap - 1);
  if (n > 0) {
    out[n] = '\0';
    return true;
  }
#elif defined(__APPLE__)
  uint32_t size = (uint32_t)cap;
  if (_NSGetExecutablePath(out, &size) == 0) return true;
#endif
  (void)argv0;
  (void)cap;
  return false;
}

/* True bila a jelas adalah binary unit.sh: berisi '/' DAN dapat dieksekusi
   (relatif cwd, atau relatif root repo — agar jalan dari subdirektori). */
static bool looksLikeBinary(const char *a, const char *root) {
  if (!strchr(a, '/')) return false;
  if (a[0] == '/') return access(a, X_OK) == 0;
  if (access(a, X_OK) == 0) return true;
  char p[SATRIA_MAX_PATH];
  if ((size_t)snprintf(p, sizeof(p), "%s/%s", root, a) >= sizeof(p)) return false;
  return access(p, X_OK) == 0;
}

/* Root repo: cwd bila scripts/unit.sh ada di sana, else parent exe dir
   (bin/satria → repo). Keluarkan path skrip (out) DAN root repo (rootOut) —
   direktori pemilik scripts/ — untuk heuristik path binary relatif-repo. */
static bool unitScriptPath(const char *argv0, char *out, size_t cap, char *rootOut,
                           size_t rcap) {
  char cwd[SATRIA_MAX_PATH];
  if (fsGetCwd(cwd, sizeof(cwd)) && fsFileExists("scripts/unit.sh")) {
    if ((size_t)snprintf(out, cap, "%s/scripts/unit.sh", cwd) >= cap) return false;
    if ((size_t)snprintf(rootOut, rcap, "%s", cwd) >= rcap) return false;
    return true;
  }
  char dir[SATRIA_MAX_PATH];
  if (!selfExeDir(argv0, dir, sizeof(dir))) return false;
  if ((size_t)snprintf(rootOut, rcap, "%s/..", dir) >= rcap) return false;
  if ((size_t)snprintf(out, cap, "%s/scripts/unit.sh", rootOut) >= cap) return false;
  return true;
}

/* Berkas tes DSL: nama berakhiran .test atau .t. */
static bool isDslTestFile(const char *a) {
  size_t n = strlen(a);
  return (n > 5 && strcmp(a + n - 5, ".test") == 0) ||
         (n > 2 && strcmp(a + n - 2, ".t") == 0);
}

/* Kendali run native (M4d): stop/pause/resume <target>. */
static bool isControlMode(const char *a) {
  return strcmp(a, "stop") == 0 || strcmp(a, "pause") == 0 ||
         strcmp(a, "resume") == 0;
}


/* Root repo untuk runner native: cwd bila punya tests/unit, selain itu
   parent direktori binary (bin/satria → repo) — tidak lagi bergantung pada
   keberadaan scripts/unit.sh. */
static bool repoRoot(const char *argv0, char *out, size_t cap) {
  char cwd[SATRIA_MAX_PATH];
  if (fsGetCwd(cwd, sizeof(cwd)) && fsDirExists("tests/unit")) {
    if ((size_t)snprintf(out, cap, "%s", cwd) >= cap) return false;
    return true;
  }
  char dir[SATRIA_MAX_PATH];
  if (!selfExeDir(argv0, dir, sizeof(dir))) return false;
  if ((size_t)snprintf(out, cap, "%s/..", dir) >= cap) return false;
  return true;
}

int cmdTest(const char *const *args, int nargs, const char *argv0) {
  /* `satria test --parse …` native (M4a): parser DSL, tanpa delegasi bash. */
  if (nargs >= 1 && strcmp(args[0], "--parse") == 0)
    return testParserMain(nargs - 1, args + 1);
  /* M4b: `satria test <file.test>` mengeksekusi DSL native (tanpa bash). */
  if (nargs >= 1 && args[0][0] != '-' && isDslTestFile(args[0]))
    return testExecMain(nargs, args);

  char script[SATRIA_MAX_PATH], root[SATRIA_MAX_PATH];
  /* M4c/M4d: runner native butuh root repo (tests/) dan binary satria ini
     sendiri (dipin ke staging, dipakai menjalankan tes). */
  if (nargs >= 1 && strcmp(args[0], "--run") == 0) {
    char self[SATRIA_MAX_PATH];
    if (!repoRoot(argv0, root, sizeof(root)))
      if (!unitScriptPath(argv0, script, sizeof(script), root, sizeof(root)))
        copyStr(root, sizeof(root), ".");
    if (!selfExePath(argv0, self, sizeof(self))) copyStr(self, sizeof(self), argv0);
    return testRunnerMain(nargs - 1, args + 1, root, self);
  }
  /* Kendali run native: stop/pause/resume <target>. */
  if (nargs >= 1 && isControlMode(args[0])) {
    if (nargs < 2) {
      fprintf(stderr, "pemakaian: satria test %s <target>\n", args[0]);
      return 2;
    }
    if (nargs > 2) {
      fprintf(stderr, "Error: %s hanya menerima <target>\n", args[0]);
      return 2;
    }
    if (!repoRoot(argv0, root, sizeof(root))) copyStr(root, sizeof(root), ".");
    return testRunnerControl(args[0], args[1], root);
  }

  /* M4d: `satria test [binary] <target> [...]` in-process. `start` setara
     run biasa. */
  int off = (nargs >= 1 && strcmp(args[0], "start") == 0) ? 1 : 0;
  char self[SATRIA_MAX_PATH];
  if (!repoRoot(argv0, root, sizeof(root))) copyStr(root, sizeof(root), ".");
  if (!selfExePath(argv0, self, sizeof(self))) copyStr(self, sizeof(self), argv0);
  /* `satria test bin/satria miniws` tetap diterima: binary eksplisit
     dilewati — runner selalu memakai binary ini sendiri. */
  if (nargs - off >= 2 && looksLikeBinary(args[off], root)) off++;
  if (nargs == off) {
    fprintf(stderr, "pemakaian: satria test [binary] <target> "
                    "[-j N] [-s tes[,tes...]] [-o log] [--clean] | -b | -w "
                    "[target] | stop|pause|resume <target>\n");
    return 2;
  }
  return testRunnerMain(nargs - off, args + off, root, self);
}

#endif

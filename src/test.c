/* satria test — delegasi ke scripts/unit.sh (design/unit_test.md).
   Tidak mem-parse argumen unit.sh sama sekali: satria hanya menemukan root
   repo, lalu exec `bash <root>/scripts/unit.sh <args…>`. Porting runner ke C
   adalah milestone M4 (design/unit_test_dsl.md). */

#include "test.h"

#include <stdio.h>
#include <string.h>

#include "portability.h" /* SATRIA_MAX_PATH */

#if defined(_WIN32)

int cmdTest(const char *const *args, int nargs, const char *argv0) {
  (void)args; (void)nargs; (void)argv0;
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
  (void)argv0;
  (void)cap;
  return false;
#endif
  char *slash = strrchr(out, '/');
  if (!slash) return false;
  *slash = '\0';
  return true;
}

/* Root repo: cwd bila scripts/unit.sh ada di sana, else parent exe dir
   (bin/satria → repo). Path absolut dipertahankan agar `satria test` jalan
   dari subdirektori mana pun. */
static bool unitScriptPath(const char *argv0, char *out, size_t cap) {
  if (fsFileExists("scripts/unit.sh")) {
    char cwd[SATRIA_MAX_PATH];
    if (!fsGetCwd(cwd, sizeof(cwd))) return false;
    if ((size_t)snprintf(out, cap, "%s/scripts/unit.sh", cwd) >= cap) return false;
    return true;
  }
  char dir[SATRIA_MAX_PATH];
  if (!selfExeDir(argv0, dir, sizeof(dir))) return false;
  char root[SATRIA_MAX_PATH];
  if ((size_t)snprintf(root, sizeof(root), "%s/..", dir) >= sizeof(root)) return false;
  if ((size_t)snprintf(out, cap, "%s/scripts/unit.sh", root) >= cap) return false;
  return true;
}

int cmdTest(const char *const *args, int nargs, const char *argv0) {
  char script[SATRIA_MAX_PATH];
  if (!unitScriptPath(argv0, script, sizeof(script)) || !fsFileExists(script)) {
    fprintf(stderr, "satria test: scripts/unit.sh tidak ditemukan (bukan root repo satria?).\n");
    return 2;
  }
  /* exec bash langsung: tanpa shell, tanpa quoting — argumen verbatim. */
  const char **argv = malloc((size_t)(nargs + 3) * sizeof(*argv));
  if (!argv) {
    fprintf(stderr, "satria test: kehabisan memori.\n");
    return 2;
  }
  argv[0] = "bash";
  argv[1] = script;
  for (int i = 0; i < nargs; i++) argv[i + 2] = args[i];
  argv[nargs + 2] = NULL;

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "satria test: fork gagal.\n");
    free(argv);
    return 2;
  }
  if (pid == 0) {
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }
  free(argv); /* anak sudah punya salinannya sendiri */
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) {
      fprintf(stderr, "satria test: waitpid gagal.\n");
      return 2;
    }
  }
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return 2;
}

#endif

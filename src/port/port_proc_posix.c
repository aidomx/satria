/*
 * port_proc_posix — lapisan PROSES (POSIX) dari portability.
 * Pasangan port_proc.c (blok Windows); dipisah dari port_proc.c
 * (audit refactor: berkas kandidat >600 baris). Deklarasi publik di
 * portability.h.
 */
#include "../portability.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h> /* _isatty */
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h> /* open/O_* — redirect output child ke file tangkapan */
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

#ifndef _WIN32

/* ========== POSIX: Proses ========== */

bool termIsTTY(void) { return isatty(1) == 1; }

static bool probeAvailableScan(const char *exe) {
  if (!exe || !*exe) return false;
  if (exe[0] == '/') return fsFileExists(exe);

  /* Eksplisit menghindari system("command -v ...") — system() menelan
     exit code pada beberapa libc, dan "command" bukan binary biasa. */
  const char *pathvar = getenv("PATH");
  if (!pathvar) pathvar = "/usr/bin:/bin";
  const char *p = pathvar;
  while (*p) {
    const char *colon = strchr(p, ':');
    size_t len = colon ? (size_t)(colon - p) : strlen(p);
    if (len > 0 && len < MAX_PATH) {
      char dir[MAX_PATH];
      memcpy(dir, p, len);
      dir[len] = '\0';
      char full[MAX_PATH + 128];
      snprintf(full, sizeof(full), "%s/%s", dir, exe);
      if (fsFileExists(full)) return true;
    }
    if (!colon) break;
    p = colon + 1;
  }
  return false;
}

/* Resolusi nama program ke path absolut, sekali per proses dan di-cache. Spawn
   memakai path ini: posix_spawnp mencoba execve di setiap direktori PATH (puluhan
   percobaan gagal per spawn, mahal di proot); dengan path absolut cukup satu. */
static struct {
  char *name;
  char *full; /* NULL = tidak ditemukan di PATH */
} spawnCache[32];
static int spawnCacheCount = 0;

static const char *resolveSpawnExe(const char *name) {
  for (int i = 0; i < spawnCacheCount; i++)
    if (strcmp(spawnCache[i].name, name) == 0) return spawnCache[i].full;
  char *found = NULL;
  const char *pathvar = getenv("PATH");
  if (!pathvar) pathvar = "/usr/bin:/bin";
  const char *p = pathvar;
  while (*p && !found) {
    const char *colon = strchr(p, ':');
    size_t len = colon ? (size_t)(colon - p) : strlen(p);
    if (len > 0 && len < MAX_PATH) {
      char full[MAX_PATH * 2];
      snprintf(full, sizeof(full), "%.*s/%s", (int)len, p, name);
      if (access(full, X_OK) == 0) found = strdup(full);
    }
    if (!colon) break;
    p = colon + 1;
  }
  if (spawnCacheCount < (int)(sizeof(spawnCache) / sizeof(spawnCache[0]))) {
    spawnCache[spawnCacheCount].name = strdup(name);
    spawnCache[spawnCacheCount].full = found;
    if (spawnCache[spawnCacheCount].name) spawnCacheCount++;
  }
  return found;
}

/* Cache hasil probe per proses: PATH tidak berubah selama satu run, sedangkan
   compile.c dan embed.c memanggil probe yang sama berulang kali (tiap fase). */
static struct {
  char *exe;
  bool ok;
} probeCache[32];
static int probeCacheCount = 0;

bool probeAvailable(const char *exe) {
  if (!exe || !*exe) return false;
  for (int i = 0; i < probeCacheCount; i++)
    if (strcmp(probeCache[i].exe, exe) == 0) return probeCache[i].ok;
  bool ok = probeAvailableScan(exe);
  if (probeCacheCount < (int)(sizeof(probeCache) / sizeof(probeCache[0]))) {
    probeCache[probeCacheCount].exe = strdup(exe);
    if (probeCache[probeCacheCount].exe) {
      probeCache[probeCacheCount].ok = ok;
      probeCacheCount++;
    }
  }
  return ok;
}

/*
 * Ctrl+C, mode foreground, dan job paralel.
 *
 * foreground=true (default): child (shell/compiler) dibalikkan ke SIG_DFL
 *   untuk SIGINT/SIGQUIT lalu dibiarkan berbagi process group dengan satria,
 *   sehingga Ctrl+C dari terminal menghentikan semuanya sekaligus.
 *
 * foreground=false: satria memasang handler SIGINT sendiri (child tidak
 *   ikut menerima Ctrl+C dari terminal). Ctrl+C ditandai, diteruskan ke
 *   process group child secara eksplisit, dan build dibatalkan dengan rapi
 *   (procRun mengembalikan PROC_RUN_INTERRUPTED).
 *
 * Job paralel (-jN): tiap child diletakkan di process group sendiri
 * (setpgid(0,0)), sehingga forward sinyal dari satria terarah per proses.
 */

#define JOB_MAX 4096
static volatile sig_atomic_t g_wantInterrupt = 0;
static bool g_foregroundMode = true;
static volatile sig_atomic_t g_jobCount = 0;
static pid_t g_jobs[JOB_MAX];

static void procForwardToJobs(void) {
  for (int i = 0; i < g_jobCount; i++) {
    pid_t pid = g_jobs[i];
    if (pid > 1) kill(-pid, SIGINT); /* grup child: forward terarah */
  }
}

static void procSigintHandler(int sig) {
  (void)sig;
  g_wantInterrupt = 1;
  procForwardToJobs();
}

/* Handler kosong untuk mode klasik (foreground): hanya memutus waitpid()
   dengan EINTR; Ctrl+C ditangani terminal langsung ke process group. */
static void procSigintIgnore(int sig) {
  (void)sig;
  /* Ctrl+C tetap diterima terminal langsung ke grup (child ikut mati), tetapi
     flag interupsi juga diset agar satria berhenti setelah job berjalan selesai,
     walau SIGINT hanya sampai ke satria. */
  g_wantInterrupt = 1;
}

bool procSetForeground(bool foreground) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);

  g_wantInterrupt = 0;
  g_foregroundMode = foreground;

  if (foreground) {
    /* Terminal yang mengelola Ctrl+C: kembali ke SIG_DFL, buang job lama. */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGINT, &sa, NULL);
    g_jobCount = 0;
    return true;
  }

  sa.sa_handler = procSigintHandler;
  sigaction(SIGINT, &sa, NULL);
  return true;
}

bool procInterrupted(void) { return g_wantInterrupt != 0; }

/* File sementara untuk tangkapan output child (TMPDIR, fallback /tmp). */
bool procCaptureTemp(char *pathOut, size_t n) {
  if (!pathOut || n < 32) return false;
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char tmpl[MAX_PATH + 32];
  if (snprintf(tmpl, sizeof(tmpl), "%s/satria-cap-XXXXXX", tmp) >= (int)sizeof(tmpl))
    return false;
  int fd = mkstemp(tmpl);
  if (fd < 0) return false;
  close(fd);
  if (strlen(tmpl) + 1 > n) {
    remove(tmpl);
    return false;
  }
  strcpy(pathOut, tmpl);
  return true;
}

/*
 * Jalankan command langsung (tanpa `/bin/sh -c`) bila command "polos": hanya
 * kata dipisah spasi, tanpa quote/variabel/glob/redirect/pipe — persis bentuk
 * command kompilasi & link yang disusun satria. Menghemat SATU exec per job
 * (shell) dan memakai posix_spawn (vfork-style, tanpa menyalin page table
 * proses induk); keduanya mahal di proot/Termux. Return pid, atau -1 bila
 * command butuh shell / spawn gagal — pemanggil jatuh ke jalur fork+sh lama
 * sehingga perilaku tidak berubah untuk kasus tepi.
 */
static bool cmdIsPlain(const char *cmd) {
  for (const char *p = cmd; *p; p++) {
    switch (*p) {
      case '"': case '\'': case '\\': case '$': case '`': case ';': case '&': case '|':
      case '<': case '>': case '(': case ')': case '*': case '?': case '[': case ']':
      case '{': case '}': case '~': case '#': case '!': case '\n': case '\r':
        return false;
      default:
        break;
    }
  }
  return true;
}

static pid_t spawnDirect(const char *cmd, bool ownGroup, const char *capPath) {
  if (!cmdIsPlain(cmd)) return -1;

  /* Tangkapan output child: fd 2 -> file, lalu fd 1 -> dup fd 2 (satu open
     file description -> offset bersama, aman bila keduanya menulis).
     Gagal menyiapkan file actions -> return -1 supaya pemanggil jatuh ke
     jalur fork+sh yang meredirect sendiri. */
  posix_spawn_file_actions_t fa;
  bool useFa = false;
  if (capPath && posix_spawn_file_actions_init(&fa) == 0) {
    if (posix_spawn_file_actions_addopen(&fa, 2, capPath, O_WRONLY | O_CREAT | O_TRUNC,
                                         0644) == 0 &&
        posix_spawn_file_actions_adddup2(&fa, 2, 1) == 0)
      useFa = true;
    else
      posix_spawn_file_actions_destroy(&fa);
  }
  if (capPath && !useFa) return -1;

  size_t len = strlen(cmd);
  char *buf = malloc(len + 1);
  char **argv = malloc((len / 2 + 2) * sizeof(char *));
  if (!buf || !argv) {
    free(buf);
    free(argv);
    return -1;
  }
  memcpy(buf, cmd, len + 1);

  int argc = 0;
  for (char *p = buf; *p;) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = '\0';
  }
  argv[argc] = NULL;

  pid_t pid = -1;
  /* token pertama berbentuk VAR=nilai = assignment milik shell */
  if (argc > 0 && !strchr(argv[0], '=')) {
    posix_spawnattr_t at;
    if (posix_spawnattr_init(&at) == 0) {
      sigset_t defs;
      sigemptyset(&defs);
      sigaddset(&defs, SIGINT);
      sigaddset(&defs, SIGQUIT);
      sigaddset(&defs, SIGHUP);
      sigaddset(&defs, SIGTERM);
      short flags = POSIX_SPAWN_SETSIGDEF; /* setara reset SIG_DFL di child lama */
      posix_spawnattr_setsigdefault(&at, &defs);
      if (ownGroup) {
        flags |= POSIX_SPAWN_SETPGROUP; /* setara setpgid(0, 0) */
        posix_spawnattr_setpgroup(&at, 0);
      }
      posix_spawnattr_setflags(&at, flags);
      pid_t np;
      const char *full = strchr(argv[0], '/') ? NULL : resolveSpawnExe(argv[0]);
      posix_spawn_file_actions_t *fap = useFa ? &fa : NULL;
      int rc = full ? posix_spawn(&np, full, fap, &at, argv, environ)
                    : posix_spawnp(&np, argv[0], fap, &at, argv, environ);
      if (rc == 0) pid = np;
      posix_spawnattr_destroy(&at);
    }
  }
  if (useFa) posix_spawn_file_actions_destroy(&fa);
  free(argv);
  free(buf);
  return pid;
}

static bool procStartImpl(const char *cmd, ProcHandle *out, bool capture) {
  if (!cmd || !*cmd || !out) return false;

  out->capPath[0] = '\0';
  if (capture && !procCaptureTemp(out->capPath, sizeof(out->capPath))) capture = false;
  const char *cap = capture ? out->capPath : NULL;

  /* Job paralel selalu di process group sendiri. */
  pid_t pid = spawnDirect(cmd, true, cap);
  if (pid < 0) {
    pid = fork();
    if (pid < 0) {
      if (capture) {
        remove(out->capPath);
        out->capPath[0] = '\0';
      }
      return false;
    }

    if (pid == 0) {
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_handler = SIG_DFL;
      sigemptyset(&sa.sa_mask);
      sigaction(SIGINT, &sa, NULL);
      sigaction(SIGQUIT, &sa, NULL);
      sigaction(SIGHUP, &sa, NULL);
      sigaction(SIGTERM, &sa, NULL);

      setpgid(0, 0); /* grup sendiri: forward sinyal dari satria terarah */

      if (cap) { /* jalur shell: redirect output child ke file tangkapan */
        int cf = open(cap, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (cf >= 0) {
          dup2(cf, 2);
          dup2(cf, 1);
          if (cf > 2) close(cf);
        }
      }

      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
    }

    /* Race setpgid: anak bisa exec lebih dulu; lakukan juga dari parent
       (EACCES setelah exec diabaikan secara diam). */
    setpgid(pid, pid);
  }

  out->pid = pid;
  out->finished = false;
  out->ok = false;
  out->interrupted = false;

  if (g_jobCount < JOB_MAX) g_jobs[g_jobCount++] = pid;
  return true;
}

bool procStart(const char *cmd, ProcHandle *out) { return procStartImpl(cmd, out, false); }

bool procStartCapture(const char *cmd, ProcHandle *out) { return procStartImpl(cmd, out, true); }

int procWaitAny(ProcHandle *handles, int count, ProcHandle **finished) {
  if (finished) *finished = NULL;
  if (!handles || count <= 0) return -1;

  for (;;) {
    int alive = 0;
    for (int i = 0; i < count; i++)
      if (!handles[i].finished) alive++;
    if (alive == 0) return -1;

    int status;
    pid_t pid = wait(&status);
    if (pid < 0) {
      if (errno == EINTR) {
        /* Ctrl+C: forward eksplisit ke grup tiap child yang masih jalan —
           child di grup terpisah tidak menerima Ctrl+C dari terminal. */
        if (g_wantInterrupt)
          for (int i = 0; i < count; i++)
            if (!handles[i].finished && handles[i].pid > 1) kill(-handles[i].pid, SIGINT);
        continue;
      }
      return -1;
    }

    for (int i = 0; i < count; i++) {
      if (handles[i].finished || handles[i].pid != pid) continue;

      handles[i].finished = true;
      handles[i].ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      handles[i].interrupted = g_wantInterrupt && !handles[i].ok;
      if (finished) *finished = &handles[i];

      for (int j = 0; j < g_jobCount; j++) {
        if (g_jobs[j] == pid) {
          g_jobs[j] = g_jobs[--g_jobCount];
          break;
        }
      }
      return i;
    }
    /* pid bukan milik batch ini — abaikan dan lanjut menunggu */
  }
}

void procStopAll(ProcHandle *handles, int count) {
  if (!handles || count <= 0) return;

  for (int i = 0; i < count; i++) {
    if (handles[i].finished || handles[i].pid <= 1) continue;
    kill(-handles[i].pid, SIGINT);
  }

  for (int i = 0; i < count; i++) {
    if (handles[i].finished) continue;
    int status;
    while (waitpid(handles[i].pid, &status, 0) < 0 && errno == EINTR) {}
    handles[i].finished = true;
    handles[i].ok = false;
    handles[i].interrupted = true;
  }
  /* File tangkapan job yang berhenti tanpa sempat dicetak dibuang di sini. */
  for (int i = 0; i < count; i++) {
    if (handles[i].capPath[0]) {
      remove(handles[i].capPath);
      handles[i].capPath[0] = '\0';
    }
  }
  g_jobCount = 0;
}

int cpuCount(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
}

bool procRun(const char *cmd) {
  if (!cmd || !*cmd) return false;

  bool separateGroup = !g_foregroundMode;
  pid_t pid = spawnDirect(cmd, separateGroup, NULL);
  if (pid < 0) pid = fork();
  if (pid < 0) return false;

  if (pid == 0) {
    /* system() membuat signal SIGINT/SIGQUIT diabaikan saat shell berjalan.
       Child build harus kembali ke default agar Ctrl+C — langsung dari
       terminal saat foreground, atau forward dari satria saat background —
       benar-benar menghentikan shell/compiler. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    if (separateGroup) setpgid(0, 0);

    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }

  if (separateGroup) {
    /* Race setpgid: lakukan juga dari parent (EACCES setelah exec diam-
       diabaikan), lalu daftarkan agar Ctrl+C diteruskan seketika. */
    setpgid(pid, pid);
    if (g_jobCount < JOB_MAX) g_jobs[g_jobCount++] = pid;
  }

  struct sigaction ignoreInt, oldInt, oldQuit;
  memset(&ignoreInt, 0, sizeof(ignoreInt));
  sigemptyset(&ignoreInt.sa_mask);

  if (separateGroup) {
    /* foreground=false: child di grup terpisah, satria tetap hidup menerima
       Ctrl+C lewat handler procSigintHandler. */
    ignoreInt.sa_handler = procSigintHandler;
    sigaction(SIGINT, &ignoreInt, &oldInt);
    sigaction(SIGQUIT, &ignoreInt, &oldQuit);
  } else {
    /* Klasik (foreground): child berbagi process group dan menerima Ctrl+C
       langsung dari terminal; parent cukup tetap hidup selama menunggu. */
    ignoreInt.sa_handler = procSigintIgnore;
    sigaction(SIGINT, &ignoreInt, &oldInt);
    sigaction(SIGQUIT, &ignoreInt, &oldQuit);
  }

  int status;
  for (;;) {
    if (waitpid(pid, &status, 0) >= 0) break;
    if (errno == EINTR) continue;
    sigaction(SIGINT, &oldInt, NULL);
    sigaction(SIGQUIT, &oldQuit, NULL);
    return false;
  }

  sigaction(SIGINT, &oldInt, NULL);
  sigaction(SIGQUIT, &oldQuit, NULL);

  if (separateGroup) {
    for (int j = 0; j < g_jobCount; j++) {
      if (g_jobs[j] == pid) {
        g_jobs[j] = g_jobs[--g_jobCount];
        break;
      }
    }
    kill(-pid, SIGINT); /* jaga-jaga bila forward dari handler terlewat */
    if (g_wantInterrupt) return PROC_RUN_INTERRUPTED;
  }

  if (WIFEXITED(status)) return WEXITSTATUS(status) == 0;
  if (WIFSIGNALED(status)) {
    /* Mode klasik: child mati oleh Ctrl+C grup terminal (SIGINT/SIGQUIT) —
       tandai interrupted agar pemanggil berhenti, bukan lanjut kompilasi. */
    int sig = WTERMSIG(status);
    if (sig == SIGINT || sig == SIGQUIT) {
      g_wantInterrupt = 1;
      return PROC_RUN_INTERRUPTED;
    }
    return false;
  }
  return false;
}

double monotonicSeconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#endif /* !_WIN32 */

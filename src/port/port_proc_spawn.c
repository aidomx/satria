/*
 * port_proc_spawn — primitif lintas-platform: procCaptureTake (ambil output
 * child) dan empat primitif background.c (procSelfPid, procAlive,
 * procSleepMs, procSpawnDetached) — kedua blok platform dipertahankan
 * apa adanya. Dipisah dari port_proc.c (audit refactor: berkas kandidat
 * >600 baris). Deklarasi publik di portability.h.
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


/*
 * Ambil isi tangkapan child (stdout+stderr) sebagai buffer malloc, hapus file
 * sementara, dan kosongkan capPath. NULL bila tidak ada tangkapan / kosong —
 * pemanggil cukup free() bila tidak NULL. Dipakai compile loop agar output
 * compiler dicetak parent UTUH setelah baris progress (urutan terjaga,
 * anti-interleaving) dan bisa disalin ke build.log.
 */
char *procCaptureTake(ProcHandle *h, size_t *outLen) {
  if (outLen) *outLen = 0;
  if (!h || !h->capPath[0]) return NULL;
  FILE *f = fopen(h->capPath, "rb");
  char *buf = NULL;
  size_t len = 0, cap = 0;
  if (f) {
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
      if (len + n + 1 > cap) {
        size_t ncap = cap ? cap * 2 : 8192;
        while (ncap < len + n + 1) ncap *= 2;
        char *nb = realloc(buf, ncap);
        if (!nb) break;
        buf = nb;
        cap = ncap;
      }
      memcpy(buf + len, tmp, n);
      len += n;
    }
    fclose(f);
  }
  remove(h->capPath);
  h->capPath[0] = '\0';
  if (!buf || len == 0) {
    free(buf);
    return NULL;
  }
  buf[len] = '\0';
  if (outLen) *outLen = len;
  return buf;
}

/*
 * ==================== Background build (design/background.md) =================
 * Empat primitif yang dipakai background.c: PID diri sendiri, cek hidup
 * proses, tidur milidetik, dan spawn child terpisah dengan stdout+stderr
 * diarahkan ke file log (§2 Fork/Spawn + §2.3 Redirection).
 */

#ifdef _WIN32

int procSelfPid(void) { return (int)GetCurrentProcessId(); }

bool procAlive(int pid) {
  if (pid <= 0) return false;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (!h) return false;
  DWORD code = 0;
  bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
}

void procSleepMs(int ms) { Sleep((DWORD)ms); }

/* _putenv("k=v") menyetel; _putenv("k=") MENGHAPUS (semantik MSVCRT). */
void procEnvSet(const char *name, const char *value) {
  char pair[1024];
  if (value)
    snprintf(pair, sizeof(pair), "%s=%s", name, value);
  else
    snprintf(pair, sizeof(pair), "%s=", name);
  _putenv(pair);
}

/* Kutip satu argumen untuk CreateProcess ( '"' -> \" ). [malloc] */
static char *procQuoteArg(const char *s) {
  size_t len = strlen(s);
  char *q = malloc(len * 2 + 3);
  if (!q) return NULL;
  char *d = q;
  *d++ = '"';
  for (const char *p = s; *p; p++) {
    if (*p == '"') *d++ = '\\';
    *d++ = *p;
  }
  *d++ = '"';
  *d = '\0';
  return q;
}

bool procSpawnDetached(char *const argv[], const char *logPath, int *outPid) {
  if (!argv || !argv[0] || !logPath) return false;

  /* Command line gabungan — CreateProcess memodifikasi buffer sendiri. */
  size_t cap = 1024;
  for (int i = 0; argv[i]; i++) cap += strlen(argv[i]) * 2 + 4;
  char *cmdline = malloc(cap);
  if (!cmdline) return false;
  cmdline[0] = '\0';
  bool qok = true;
  for (int i = 0; argv[i] && qok; i++) {
    char *q = procQuoteArg(argv[i]);
    if (!q) { qok = false; break; }
    if (i) strncat(cmdline, " ", cap - strlen(cmdline) - 1);
    strncat(cmdline, q, cap - strlen(cmdline) - 1);
    free(q);
  }
  if (!qok) { free(cmdline); return false; }

  /* Log ditimpa tiap build background (§4.3). Handle HARUS inheritable
     (bInheritHandle=TRUE): CreateProcess dengan STARTF_USESTDHANDLES hanya
     mewariskan handle yang bisa diwarisi — kalau tidak, stdout/stderr child
     invalid dan background.log kosong. */
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;
  HANDLE hLog = CreateFileA(logPath, GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &sa, OPEN_EXISTING, 0, NULL);
  bool ok = false;
  if (hLog != INVALID_HANDLE_VALUE) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    /* DETACHED_PROCESS: tanpa jendela konsol baru & tidak mewarisi konsol
       induk — prompt terminal langsung kembali setelah parent keluar. */
    if (CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                       DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si,
                       &pi)) {
      if (outPid) *outPid = (int)pi.dwProcessId;
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      ok = true;
    }
    CloseHandle(hLog);
  }
  if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
  free(cmdline);
  return ok;
}

#else /* POSIX */

int procSelfPid(void) { return (int)getpid(); }

bool procAlive(int pid) {
  if (pid <= 0) return false;
  if (kill(pid, 0) == 0) return true;
  return errno == EPERM; /* proses ada, tapi milik user lain */
}

void procSleepMs(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

void procEnvSet(const char *name, const char *value) {
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
}

bool procSpawnDetached(char *const argv[], const char *logPath, int *outPid) {
  if (!argv || !argv[0] || !logPath) return false;
  fflush(NULL); /* buffer stdout induk tidak boleh ikut terduplikasi fork */
  /* SIGHUP diabaikan SEBELUM fork: bila induk keluar lebih dulu (session leader
     di pty), kernel mengirim SIGHUP ke grup foreground dan child yang belum
     sempat setsid() ikut mati tanpa jejak. SIG_IGN diwarisi child dan bertahan
     setelah exec. Induk memulihkan handler lamanya. */
  struct sigaction ign, oldHup;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigaction(SIGHUP, &ign, &oldHup);
  pid_t pid = fork();
  if (pid != 0) sigaction(SIGHUP, &oldHup, NULL); /* induk / gagal fork */
  if (pid < 0) return false;
  if (pid == 0) {
    /* Child: sesi baru + redirect — induk bebas keluar tanpa menunggu. */
    setsid();
    int fd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      if (fd > STDERR_FILENO) close(fd);
    }
    int in = open("/dev/null", O_RDONLY);
    if (in >= 0) {
      dup2(in, STDIN_FILENO);
      if (in > STDIN_FILENO) close(in);
    }
    execvp(argv[0], argv);
    _exit(127); /* exec gagal — log tetap menerima jejk bila perlu */
  }
  if (outPid) *outPid = (int)pid;
  return true;
}

#endif /* !_WIN32 */

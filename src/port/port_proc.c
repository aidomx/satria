/*
 * port_proc — lapisan PROSES dari portability (blok WINDOWS).
 *
 * Dipindah apa adanya dari portability.c supaya file tetap modular:
 *   - deteksi executable di PATH (probeAvailable),
 *   - Ctrl+C / mode foreground / registry child,
 *   - spawn shell (procRun) dan job paralel (procStart/procWaitAny/procStopAll),
 *   - cpuCount & monotonicSeconds.
 * Blok POSIX ada di port_proc_posix.c; primitif lintas-platform
 * (procCaptureTake + background) di port_proc_spawn.c.
 * Deklarasi publik tetap di portability.h.
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

#ifdef _WIN32

/* ========== Windows: Proses ========== */

bool probeAvailable(const char *exe) {
  if (!exe || !*exe) return false;
  if (strchr(exe, '/') || strchr(exe, '\\')) return fsFileExists(exe);

  /* PATH lengkap di Windows; mencoba sufiks .exe/.bat/.cmd.
     Eksekusi tetap lewat cmd.exe (procRun), jadi "cl" dari Developer
     Command Prompt ikut terdeteksi lewat PATH lingkungan itu. */
  static const char *kSuffixes[] = {"", ".exe", ".bat", ".cmd", NULL};
  const char *pathvar = getenv("PATH");
  if (!pathvar) return false;
  const char *p = pathvar;
  while (*p) {
    const char *semi = strchr(p, ';');
    size_t len = semi ? (size_t)(semi - p) : strlen(p);
    if (len > 0 && len < MAX_PATH) {
      char dir[MAX_PATH];
      memcpy(dir, p, len);
      dir[len] = '\0';
      /* buang tanda kutip yang kadang menyelimuti entri PATH Windows */
      if (dir[0] == '"') {
        size_t dl = strlen(dir);
        if (dl && dir[dl - 1] == '"') {
          dir[dl - 1] = '\0';
          memmove(dir, dir + 1, dl - 1);
        }
      }
      for (int s = 0; kSuffixes[s]; s++) {
        char full[MAX_PATH + 128];
        snprintf(full, sizeof(full), "%s/%s%s", dir, exe, kSuffixes[s]);
        if (fsFileExists(full)) return true;
      }
    }
    if (!semi) break;
    p = semi + 1;
  }
  return false;
}

/* ---------- Status Ctrl+C & registry child ---------- */

static volatile bool g_wantInterrupt = false;
static bool g_foregroundMode = true;
static bool g_handlerAttached = false;

/* Registry pid child yang masih berjalan — dibaca console handler (dari
   thread lain) untuk meneruskan CTRL_BREAK ke process group child. */
#define PROC_MAX_TRACKED 256
static CRITICAL_SECTION g_cs;
static bool g_csInit = false;
static DWORD g_pids[PROC_MAX_TRACKED];
static int g_pidCount = 0;

static void procEnsureCS(void) {
  if (!g_csInit) {
    InitializeCriticalSection(&g_cs);
    g_csInit = true;
  }
}

static void procRegisterPid(DWORD pid) {
  if (!pid) return;
  procEnsureCS();
  EnterCriticalSection(&g_cs);
  if (g_pidCount < PROC_MAX_TRACKED) g_pids[g_pidCount++] = pid;
  LeaveCriticalSection(&g_cs);
}

static void procUnregisterPid(DWORD pid) {
  procEnsureCS();
  EnterCriticalSection(&g_cs);
  for (int i = 0; i < g_pidCount; i++) {
    if (g_pids[i] == pid) {
      g_pids[i] = g_pids[--g_pidCount];
      break;
    }
  }
  LeaveCriticalSection(&g_cs);
}

static BOOL WINAPI procConsoleHandler(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
  g_wantInterrupt = true;
  /* CREATE_NEW_PROCESS_GROUP membuat compiler terpisah dari grup satria,
     jadi terminal tidak menghentikannya langsung; teruskan CTRL_BREAK
     eksplisit ke setiap process group child yang masih berjalan. */
  procEnsureCS();
  EnterCriticalSection(&g_cs);
  DWORD pids[PROC_MAX_TRACKED];
  int n = g_pidCount;
  for (int i = 0; i < n; i++) pids[i] = g_pids[i];
  LeaveCriticalSection(&g_cs);
  for (int i = 0; i < n; i++)
    if (pids[i]) GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pids[i]);
  return TRUE;
}

static void procEnsureHandler(void) {
  if (g_handlerAttached) return;
  procEnsureCS();
  SetConsoleCtrlHandler(procConsoleHandler, TRUE);
  g_handlerAttached = true;
}

bool procSetForeground(bool foreground) {
  g_foregroundMode = foreground;
  g_wantInterrupt = false;
  procEnsureHandler();
  return true;
}

bool procInterrupted(void) { return g_wantInterrupt; }

/* Susun command line "cmd.exe /d /s /c <cmd>" — dipakai procRun & procStart. */
static bool procBuildShellCommand(const char *cmd, char *command, size_t n) {
  char shell[MAX_PATH];
  DWORD len = GetEnvironmentVariableA("COMSPEC", shell, sizeof(shell));
  if (len == 0 || len >= sizeof(shell)) strcpy(shell, "cmd.exe");
  int written = snprintf(command, n, "\"%s\" /d /s /c \"%s\"", shell, cmd);
  return written >= 0 && (size_t)written < n;
}

/* File sementara untuk tangkapan output child (dir TEMP user). */
bool procCaptureTemp(char *pathOut, size_t n) {
  if (!pathOut || n < MAX_PATH) return false;
  char dir[MAX_PATH + 1];
  DWORD len = GetTempPathA((DWORD)sizeof(dir), dir);
  if (len == 0 || len >= sizeof(dir)) return false;
  char file[MAX_PATH];
  if (GetTempFileNameA(dir, "sat", 0, file) == 0) return false;
  pathNormalizeSlash(file);
  if (strlen(file) + 1 > n) {
    DeleteFileA(file);
    return false;
  }
  strcpy(pathOut, file);
  return true;
}

/* Buat proses lewat cmd.exe di process group terpisah (dilacak registry).
   capPath != NULL: stdout+stderr child diarahkan ke file tangkapan. */
static bool procCreate(const char *cmd, PROCESS_INFORMATION *pi, const char *capPath) {
  STARTUPINFOA si;
  /* Buffer command shell DIPROSES dinamis: tautan self-host (69+ object,
     ~1.7KB perintah) melampaui batas lama MAX_PATH*4 (1040 byte) sehingga
     pembangunan perintah gagal dan link dilaporkan "link failed" diam-diam.
     Batas absolut tetap 32767 karakter (limit CreateProcessA). */
  size_t need = strlen(cmd) + MAX_PATH + 64;
  if (need > 32767) return false;
  char *command = malloc(need);
  if (!command) return false;
  if (!procBuildShellCommand(cmd, command, need)) {
    free(command);
    return false;
  }

  memset(&si, 0, sizeof(si));
  memset(pi, 0, sizeof(*pi));
  si.cb = sizeof(si);

  HANDLE hCap = NULL;
  if (capPath) {
    /* Handle inheritable agar lolos bInheritHandles; parent menutupnya
       setelah CreateProcess (child memegang salinan). */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    hCap = CreateFileA(capPath, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (hCap == INVALID_HANDLE_VALUE) {
      free(command);
      return false;
    }
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hCap;
    si.hStdError = hCap;
  }

  procEnsureHandler();
  if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, NULL, NULL,
                      &si, pi)) {
    DWORD err = GetLastError();
    if (hCap) CloseHandle(hCap);
    free(command);
    fprintf(stderr, "satria: gagal menjalankan perintah (CreateProcess error %lu)\n",
            (unsigned long)err);
    return false;
  }
  free(command);
  if (hCap) CloseHandle(hCap);
  procRegisterPid(pi->dwProcessId);
  return true;
}

bool procRun(const char *cmd) {
  if (!cmd || !*cmd) return false;

  PROCESS_INFORMATION pi;
  if (!procCreate(cmd, &pi, NULL)) return false;

  DWORD wait = WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 1;
  if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exitCode);

  procUnregisterPid(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return wait == WAIT_OBJECT_0 && exitCode == 0;
}

bool procStart(const char *cmd, ProcHandle *out) {
  if (!cmd || !*cmd || !out) return false;

  out->capPath[0] = '\0';
  PROCESS_INFORMATION pi;
  if (!procCreate(cmd, &pi, NULL)) return false;
  CloseHandle(pi.hThread);

  out->pid = pi.dwProcessId;
  out->hProcess = (void *)pi.hProcess;
  out->finished = false;
  out->ok = false;
  out->interrupted = false;
  return true;
}

bool procStartCapture(const char *cmd, ProcHandle *out) {
  if (!cmd || !*cmd || !out) return false;

  out->capPath[0] = '\0';
  if (!procCaptureTemp(out->capPath, sizeof(out->capPath))) return procStart(cmd, out);

  PROCESS_INFORMATION pi;
  if (!procCreate(cmd, &pi, out->capPath)) {
    out->capPath[0] = '\0';
    return false;
  }
  CloseHandle(pi.hThread);

  out->pid = pi.dwProcessId;
  out->hProcess = (void *)pi.hProcess;
  out->finished = false;
  out->ok = false;
  out->interrupted = false;
  return true;
}

int procWaitAny(ProcHandle *handles, int count, ProcHandle **finished) {
  if (finished) *finished = NULL;
  if (!handles || count <= 0) return -1;

  for (;;) {
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    int idx[MAXIMUM_WAIT_OBJECTS];
    int n = 0;
    for (int i = 0; i < count && n < MAXIMUM_WAIT_OBJECTS; i++) {
      if (handles[i].finished) continue;
      hs[n] = (HANDLE)handles[i].hProcess;
      idx[n] = i;
      n++;
    }
    if (n == 0) return -1;

    DWORD w = WaitForMultipleObjects((DWORD)n, hs, FALSE, INFINITE);
    /* WAIT_OBJECT_0 == 0 dan w unsigned: batas bawah sudah implisit. */
    if (w >= WAIT_OBJECT_0 + (DWORD)n) {
      if (w == WAIT_FAILED) return -1;
      continue; /* WAIT_ABANDONED_0 dsb: tunggu lagi */
    }

    int i = idx[w - WAIT_OBJECT_0];
    DWORD exitCode = 1;
    GetExitCodeProcess((HANDLE)handles[i].hProcess, &exitCode);
    procUnregisterPid(handles[i].pid);
    CloseHandle((HANDLE)handles[i].hProcess);
    handles[i].hProcess = NULL;
    handles[i].finished = true;
    handles[i].ok = exitCode == 0;
    handles[i].interrupted = g_wantInterrupt && exitCode != 0;
    if (finished) *finished = &handles[i];
    return i;
  }
}

void procStopAll(ProcHandle *handles, int count) {
  if (!handles || count <= 0) return;

  for (int i = 0; i < count; i++) {
    if (handles[i].finished || !handles[i].pid) continue;
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, handles[i].pid);
  }

  for (int i = 0; i < count; i++) {
    if (handles[i].finished) continue;
    WaitForSingleObject((HANDLE)handles[i].hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess((HANDLE)handles[i].hProcess, &exitCode);
    procUnregisterPid(handles[i].pid);
    CloseHandle((HANDLE)handles[i].hProcess);
    handles[i].hProcess = NULL;
    handles[i].finished = true;
    handles[i].ok = exitCode == 0;
    handles[i].interrupted = true;
  }
  /* File tangkapan job yang berhenti tanpa sempat dicetak dibuang di sini. */
  for (int i = 0; i < count; i++) {
    if (handles[i].capPath[0]) {
      remove(handles[i].capPath);
      handles[i].capPath[0] = '\0';
    }
  }
}

bool termIsTTY(void) { return _isatty(_fileno(stdout)) != 0; }

double monotonicSeconds(void) {
  LARGE_INTEGER freq, counter;
  if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter)) return 0.0;
  return (double)counter.QuadPart / (double)freq.QuadPart;
}

int cpuCount(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

#endif /* _WIN32 */

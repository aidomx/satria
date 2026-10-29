#include "build_log.h"
#include <stdio.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
/* Reader is portable; POSIX stream relay below is not enabled on Windows. */
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
static pid_t relay = -1;
static int saved_err = -1;
static int log_active = 0;

static int diagnostic(const char *s) {
  return strstr(s, "warning:") || strstr(s, "error:") ||
         strstr(s, "fatal error:") || strstr(s, "undefined reference") ||
         strstr(s, "FAILED") || strstr(s, "failed") ||
         strstr(s, "Error ") || strstr(s, "ld:");
}
static void relayRun(int fd, int terminal) {
  char buf[4096], line[8192];
  size_t used = 0;
  FILE *log = NULL;
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    size_t pos = 0;
    while (pos < (size_t)n) {
      ssize_t k = write(terminal, buf + pos, (size_t)n - pos);
      if (k < 0 && errno == EINTR) continue;
      if (k <= 0) break;
      pos += (size_t)k;
    }
    for (ssize_t i = 0; i < n; ++i) {
      char c = buf[i];
      if (used + 1 < sizeof(line)) line[used++] = c;
      if (c == '\n' || used + 1 >= sizeof(line)) {
        line[used] = 0;
        if (diagnostic(line)) {
          if (!log) {
            mkdir(".satria", 0777);
            log = fopen(".satria/build.log", "a");
          }
          if (log) fwrite(line, 1, used, log);
        }
        used = 0;
      }
    }
  }
  if (used) {
    line[used] = 0;
    if (diagnostic(line)) {
      if (!log) { mkdir(".satria", 0777); log = fopen(".satria/build.log", "a"); }
      if (log) fwrite(line, 1, used, log);
    }
  }
  if (log) fclose(log);
  close(fd);
  close(terminal);
  _exit(0);
}
#endif
/* Keputusan relay ada di pemanggil: workspace memakai WsModel
 * (commonLines startup.state); single-project memakai Config.logEnabled yang
 * dipulihkan cfgCacheLoad dari .satria/startup.state. Tidak ada pembacaan
 * Buildfile langsung di sini (design/startup.md §3). */
int buildLogShow(void) {
  FILE *f = fopen(".satria/build.log", "rb");
  if (!f) { fprintf(stderr, "satria: build.log belum tersedia\n"); return 1; }
  char buf[8192]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) != 0) fwrite(buf, 1, n, stdout);
  int err = ferror(f);
  fclose(f);
  return err ? 1 : 0;
}
void buildLogBegin(void) {
#ifndef _WIN32
  if (log_active) return;
  int p[2];
  if (pipe(p) != 0) return;
  int original = dup(STDERR_FILENO);
  if (original < 0) { close(p[0]); close(p[1]); return; }
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) { close(original); close(p[0]); close(p[1]); return; }
  if (pid == 0) {
    close(p[1]);
    /* Lihat report_blob.c: relay mengabaikan SIGINT agar tidak mati di tengah build. */
    signal(SIGINT, SIG_IGN);
    relayRun(p[0], original);
  }
  close(p[0]);
  if (dup2(p[1], STDERR_FILENO) < 0) {
    close(p[1]); close(original); return;
  }
  close(p[1]);
  saved_err = original;
  relay = pid;
  log_active = 1;
#endif
}
void buildLogEnd(int status) {
#ifndef _WIN32
  if (!log_active) return;
  fflush(stderr);
  dup2(saved_err, STDERR_FILENO);
  close(saved_err);
  saved_err = -1;
  int st;
  while (waitpid(relay, &st, 0) < 0 && errno == EINTR) {}
  relay = -1;
  log_active = 0;
#endif
  if (status != 0) {
    FILE *f;
#ifndef _WIN32
    mkdir(".satria", 0777);
#endif
    f = fopen(".satria/build.log", "a");
    if (f) { fprintf(f, "satria: build failed (exit status %d)\n", status); fclose(f); }
  }
}

void buildLogCapture(const char *buf, size_t n) {
#ifndef _WIN32
  /* log_active diwarisi anak workspace dari induk (buildLogBegin dipanggil
     sebelum fork batch) — gate-nya identik dengan relay stderr. */
  if (!buf || !n || !log_active) return;
  size_t i = 0;
  FILE *out = NULL;
  while (i < n) {
    size_t eol = i;
    while (eol < n && buf[eol] != '\n') eol++;
    size_t len = eol - i + (eol < n ? 1 : 0); /* sertakan \n bila ada */
    if (len == 0) break;
    size_t chk = len < 8191 ? len : 8191;
    char line[8192];
    memcpy(line, buf + i, chk);
    line[chk] = '\0';
    if (diagnostic(line)) {
      if (!out) {
        mkdir(".satria", 0777);
        out = fopen(".satria/build.log", "a");
      }
      if (out) fwrite(buf + i, 1, len, out);
    }
    i += len;
  }
  if (out) fclose(out);
#else
  (void)buf;
  (void)n;
#endif
}

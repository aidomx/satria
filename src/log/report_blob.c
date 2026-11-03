/* report_blob.c — lihat report_blob.h.
   stdout build di-tee (fork + pipe; anak menyalin ke terminal asli DAN tmp),
   lalu dikondensasi ke format terstruktur (design/startup.md §10) — BUKAN
   keluaran mentah dan bukan baris per fase:

     STATUS
       > Nothing to build
       > Cache valid: .satria/caches/fingerprint.cache
     LAST BUILD SUMMARY
       Total time : 1.41s
       Archives   : 1 created
       Libraries  : 4 generated (2 static, 2 shared)
       Binaries   : 2 linked
       Status     : Success

   Agregat dihitung dari record EKSPLISIT build report (buildReportAgg) —
   parser teks rbCondense lama dihapus (design §9: "Hapus agregasi berbasis
   parsing teks setelah record eksplisit tersedia"). Konsekuensinya ringkasan
   mengikuti format Build Report yang memang dicetak run itu.

   Pada no-op file diputar ulang (reportBlobShow): blok STATUS di dalamnya
   adalah pesan no-op — seragam di kedua mode. Workspace menyetel jalur cache
   lewat reportBlobCachePath(WS_CACHE_FP); single-project memakai default
   fingerprint.cache. POSIX saja — Windows dilewati (tanpa tee; sama seperti relay
   build.log). */
#include "report_blob.h"
#include "build_report.h"    /* buildReportAgg — agregat eksplisit (§9) */
#include "../portability.h" /* fsRenameReplace */
#include "../util.h"        /* mkdirs */
#include <stdio.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* struct stat, stat(): juga dipakai jalur Windows */
#include <time.h>

#define RB_FILE ".satria/report.blob"
#define RB_TMP ".satria/report.blob.tmp"

static int rbActive = 0;
static char rbCache[256] = ".satria/caches/fingerprint.cache";
static double rbT0ms; /* portMonoMs: monotonic portabel (ms) */

void reportBlobCachePath(const char *p) {
  if (p && *p) snprintf(rbCache, sizeof(rbCache), "%s", p);
}

#ifdef _WIN32
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t rbRelay = -1;
static int rbSaved = -1;

/* Anak: baca pipe -> tulis ke stdout asli (terminal) + file tmp. */
static void rbRelayLoop(int rd, int term, int file) {
  char buf[4096];
  ssize_t n;
  while ((n = read(rd, buf, sizeof(buf))) > 0) {
    ssize_t w1 = write(term, buf, (size_t)n);
    (void)w1; /* terminal tertutup: tetap lanjut ke file */
    ssize_t w2 = write(file, buf, (size_t)n);
    (void)w2;
  }
  close(rd);
  close(term);
  close(file);
  _exit(0);
}
#endif

/* ---- Kondensasi dari record eksplisit (design/build_report.md §9) ----
   Tidak ada parsing teks: agregat datang dari buildReportAgg(). Panggil
   hanya bila build report aktif (pemilik lifecycle: cmdBuildEx / wsRunCore
   memanggil buildReportBegin sebelum reportBlobBegin). */
static bool rbCondenseReport(FILE *out, double secs) {
  int nArch = 0, nLibStatic = 0, nLibShared = 0, nBin = 0, nBinUp = 0;
  if (!buildReportAgg(&nArch, &nLibStatic, &nLibShared, &nBin, &nBinUp))
    return false;
  int libs = nLibStatic + nLibShared;
  fprintf(out, "STATUS\n------\n");
  fprintf(out, "Build        : Nothing to build\n");
  fprintf(out, "Cache valid  : %s\n", rbCache);
  fprintf(out, "\n");
  {
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&now));
    fprintf(out, "LAST BUILD SUMMARY\n------------------\n");
    fprintf(out, "Datetime   : %s\n", when);
  }
  fprintf(out, "Total time : %.2fs\n", secs);
  fprintf(out, "Archives   : %d created\n", nArch);
  fprintf(out, "Libraries  : %d generated (%d static, %d shared)\n", libs,
          nLibStatic, nLibShared);
  fprintf(out, "Binaries   : %d linked, %d up-to-date\n", nBin, nBinUp);
  fprintf(out, "Status     : Success\n"); /* file hanya ditulis bila run sukses */
  return true;
}

/* Tulis ringkasan dari record ke .sum lalu ganti report.blob (atomik).
   Dipakai POSIX dan Windows; tidak bergantung pada output yang ditangkap. */
static bool rbSaveSummary(double secs) {
  char sum[sizeof(RB_TMP) + 8];
  snprintf(sum, sizeof(sum), "%s.sum", RB_TMP);
  bool wrote = false;
  FILE *o = fopen(sum, "w");
  if (o) {
    wrote = rbCondenseReport(o, secs);
    if (fclose(o) != 0) wrote = false;
  }
  if (wrote && fsRenameReplace(sum, RB_FILE)) return true;
  remove(sum);
  return false;
}

void reportBlobBegin(void) {
  if (rbActive) return;
#ifdef _WIN32
  /* Windows tanpa tee: blob dibentuk dari record saat Finish (lihat rbSaveSummary). */
  mkdirs(".satria");
  rbT0ms = portMonoMs();
  rbActive = 1;
  return;
#else
  mkdirs(".satria");
  remove(RB_TMP);
  fflush(stdout);
  rbT0ms = portMonoMs(); /* Total time di LAST BUILD SUMMARY */
  int p[2];
  if (pipe(p) != 0) return;
  int orig = dup(STDOUT_FILENO);
  if (orig < 0) {
    close(p[0]);
    close(p[1]);
    return;
  }
  int tf = open(RB_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (tf < 0) {
    close(p[0]);
    close(p[1]);
    close(orig);
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    close(orig);
    close(tf);
    remove(RB_TMP);
    return;
  }
  if (pid == 0) {
    close(p[1]); /* anak: baca p[0], tulis orig + tf */
    /* Ctrl+C mengenai seluruh grup proses. Relay mengabaikannya agar tetap hidup
       sampai satria selesai; kalau relay mati, satria menulis ke pipe tertutup
       dan kena SIGPIPE sebelum sempat menyimpan kemajuan build. */
    signal(SIGINT, SIG_IGN);
    rbRelayLoop(p[0], orig, tf);
  }
  close(p[0]);
  close(tf);
  if (dup2(p[1], STDOUT_FILENO) < 0) {
    close(p[1]);
    close(orig);
    return;
  }
  close(p[1]);
  /* Jalur stdout kini pipe: line-buffer agar laporan muncul real-time. */
  setvbuf(stdout, NULL, _IOLBF, 0);
  rbSaved = orig;
  rbRelay = pid;
  rbActive = 1;
#endif
}

bool reportBlobFinish(bool save) {
#ifdef _WIN32
  if (!rbActive) return false;
  rbActive = 0;
  if (!save) return false; /* run gagal: report.blob lama dipertahankan */
  return rbSaveSummary((portMonoMs() - rbT0ms) / 1000.0);
#else
  if (!rbActive) return false;
  fflush(stdout);
  dup2(rbSaved, STDOUT_FILENO);
  close(rbSaved);
  rbSaved = -1;
  int st;
  while (waitpid(rbRelay, &st, 0) < 0 && errno == EINTR) {
  }
  rbRelay = -1;
  rbActive = 0;
  if (save) {
    struct stat sb;
    if (stat(RB_TMP, &sb) == 0 && sb.st_size > 0) {
      double secs = (portMonoMs() - rbT0ms) / 1000.0;
      /* Ringkasan dari record eksplisit (9), lihat rbSaveSummary. */
      if (rbSaveSummary(secs)) {
        remove(RB_TMP);
        return true;
      }
    }
  }
  remove(RB_TMP); /* run gagal / kosong / report tak aktif: laporan lama dipertahankan */
#endif
  return false;
}

void reportBlobShow(void) {
  FILE *f = fopen(RB_FILE, "rb");
  if (!f) return;
  struct stat sb;
  if (stat(RB_FILE, &sb) != 0 || sb.st_size == 0) { /* tanpa isi: jangan cetak header kosong */
    fclose(f);
    return;
  }
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) != 0)
    fwrite(buf, 1, n, stdout);
  fclose(f);
  fflush(stdout);
}

bool reportBlobHas(void) {
  struct stat sb;
  return stat(RB_FILE, &sb) == 0 && sb.st_size > 0;
}

/* Salin isi report.blob (untuk disimpan ke state). false bila tak ada/kosong. */
bool reportBlobRead(char **buf, size_t *len) {
  FILE *f = fopen(RB_FILE, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
  long n = ftell(f);
  if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
  char *b = malloc((size_t)n + 1);
  if (!b) { fclose(f); return false; }
  size_t got = fread(b, 1, (size_t)n, f);
  fclose(f);
  if (got == 0) { free(b); return false; }
  b[got] = '\0';
  *buf = b;
  *len = got;
  return true;
}

/* Pulihkan report.blob dari salinan di state: tulis tmp lalu rename (atomik). */
bool reportBlobRestore(const char *buf, size_t len) {
  if (!buf || len == 0) return false;
  FILE *f = fopen(RB_TMP, "wb");
  if (!f) return false;
  bool ok = fwrite(buf, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  if (ok && fsRenameReplace(RB_TMP, RB_FILE)) return true;
  remove(RB_TMP);
  return false;
}
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

   Pada no-op file diputar ulang (reportBlobShow): blok STATUS di dalamnya
   adalah pesan no-op — seragam di kedua mode. Workspace menyetel jalur cache
   lewat reportBlobCachePath(WS_CACHE_FP); single-project memakai default
   fingerprint.cache. POSIX saja — Windows dilewati (tanpa tee; sama seperti relay
   build.log). */
#include "report_blob.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h> /* struct stat, stat(): juga dipakai jalur Windows */
#include "../portability.h" /* fsRenameReplace */
#include "../util.h" /* mkdirs */

#define RB_FILE ".satria/report.blob"
#define RB_TMP ".satria/report.blob.tmp"

static int rbActive = 0;
static char rbCache[256] = ".satria/caches/fingerprint.cache";
#ifndef _WIN32
static struct timespec rbT0;
#endif

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

/* ---- Kondensasi agregat (design/startup.md §10) ----
   Keluaran mentah run SUKSES diringkas menjadi satu blok STATUS + satu blok
   LAST BUILD SUMMARY lintas proyek, dihitung PER BLOK ringkasan (aman untuk
   mode single yang tanpa baris "Fase"):
     - Archive : -> Archives; Target : -> Binaries;
     - Library : -> statis (.a) / bersama (.so) — dibuang bila blok punya
       Target (fase binary hanya mencetak "up-to-date").
     - Status  : Success bila semua blok Success.
   Baris progres per file, "Build with", "Embedded", "Linking", dan CompDB
   dibuang — no-op menampilkan ringkasan padat, bukan keluaran basi rinci. */
typedef struct {
  int nArch, nLibStatic, nLibShared, nBin;
  int blocks, bad;
} RbAgg;

typedef struct {
  int libStatic, libShared, hasArchive, hasTarget, hasStatus, statusOk;
} RbBlk;

#ifndef _WIN32 /* helper kondensasi: hanya dipakai rbCondense (POSIX) */
static void rbBlkReset(RbBlk *b) {
  b->libStatic = b->libShared = b->hasArchive = b->hasTarget = b->hasStatus = b->statusOk = 0;
}

static void rbBlkFlush(RbBlk *b, RbAgg *a) {
  if (b->hasStatus || b->hasArchive || b->hasTarget || b->libStatic || b->libShared) {
    a->blocks++;
    if (!b->hasStatus || !b->statusOk) a->bad++;
    if (b->hasArchive)
      a->nArch++;
    else if (b->hasTarget) {
      /* binary dihitung saat parse lapangan "    Target" — lihat rbCondense
         (phase-run workspace tidak mencetak baris live Linking) */
    } else { /* blok fase library: baris "up-to-date" binary tidak dihitung */
      a->nLibStatic += b->libStatic;
      a->nLibShared += b->libShared;
    }
  }
  rbBlkReset(b);
}

static void rbCopy(char *dst, size_t n, const char *src) {
  snprintf(dst, n, "%.*s", (int)(n - 1), src);
}

static bool rbHasSuffix(const char *s, const char *suf) {
  size_t n = strlen(s), m = strlen(suf);
  return n >= m && strcmp(s + n - m, suf) == 0;
}

/* Baris lapangan laporan memakai repPrefix(): "> " (UTF-8) atau "| "
   (non-UTF-8) — keduanya diterima. Mengembalikan nilai sesudah ":" pada
   baris yang diawali <prefix> <key>, NULL bila bukan lapangan itu. */
static const char *rbRepField(const char *line, const char *key) {
  if ((line[0] != '>' && line[0] != '|') || line[1] != ' ') return NULL;
  const char *p = line + 2;
  size_t kl = strlen(key);
  if (strncmp(p, key, kl) != 0) return NULL;
  p += kl;
  while (*p == ' ') p++;
  if (*p != ':') return NULL;
  p++;
  while (*p == ' ') p++;
  return p;
}

#endif
#ifndef _WIN32
static void rbCondense(const char *in, FILE *out, RbAgg *a, double secs) {
  FILE *f = fopen(in, "r");
  if (!f) return;
  char line[4096];
  RbBlk b;
  rbBlkReset(&b);
  /* Status link terakhir sejak awal bagian (Project/Fase): false = link
     nyata, true = "(up-to-date)". Phase-run workspace MENCETAK "    Target"
     pada link nyata TANPA baris live "Linking", jadi binary dihitung dari
     lapangan Target yang TANPA penanda up-to-date — bukan dari baris live
     (yang juga bergantung prefix "> "/"| " tergantung locale). */
  bool linkUp = false;
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    const char *fld;
    if ((fld = rbRepField(line, "Project")) != NULL ||
        (fld = rbRepField(line, "Fase")) != NULL) {
      rbBlkFlush(&b, a);
      linkUp = false; /* bagian baru: status link di-reset */
    } else if ((fld = rbRepField(line, "Summary")) != NULL) {
      (void)fld;
      rbBlkFlush(&b, a);
    } else if ((fld = rbRepField(line, "Linking")) != NULL) {
      /* rekam status: "(up-to-date)" -> link TIDAK dihitung nanti */
      linkUp = strstr(fld, "(up-to-date)") != NULL;
    } else if (strncmp(line, "    ", 4) == 0) {
      const char *k = line + 4;
      const char *v = strchr(k, ':');
      if (!v) continue;
      char key[32];
      size_t kl = (size_t)(v - k);
      while (kl && k[kl - 1] == ' ') kl--;
      if (kl >= sizeof(key)) continue;
      memcpy(key, k, kl);
      key[kl] = '\0';
      v++;
      while (*v == ' ') v++;
      char val[256];
      rbCopy(val, sizeof(val), v);
      char *up = strstr(val, " ("); /* buang "(up-to-date)" dsb. */
      if (up) *up = '\0';
      if (strcmp(key, "Status") == 0) {
        b.hasStatus = 1;
        b.statusOk = strcmp(v, "Success") == 0;
      } else if (strcmp(key, "Library") == 0) {
        if (rbHasSuffix(val, ".a")) b.libStatic++;
        else if (rbHasSuffix(val, ".so")) b.libShared++;
      } else if (strcmp(key, "Archive") == 0) {
        b.hasArchive = 1;
      } else if (strcmp(key, "Target") == 0) {
        b.hasTarget = 1;
        if (!linkUp) a->nBin++; /* link nyata tanpa penanda up-to-date */
      }
    }
  }
  fclose(f);
  rbBlkFlush(&b, a);
  int libs = a->nLibStatic + a->nLibShared;
  fprintf(out, "STATUS\n");
  fprintf(out, "  > Nothing to build\n");
  fprintf(out, "  > Cache valid: %s\n", rbCache);
  fprintf(out, "\n");
  {
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&now));
    fprintf(out, "LAST BUILD SUMMARY\n");
    fprintf(out, "  Last       : %s\n", when);
  }
  fprintf(out, "  Total time : %.2fs\n", secs);
  fprintf(out, "  Archives   : %d created\n", a->nArch);
  fprintf(out, "  Libraries  : %d generated (%d static, %d shared)\n", libs,
          a->nLibStatic, a->nLibShared);
  fprintf(out, "  Binaries   : %d linked\n", a->nBin);
  fprintf(out, "  Status     : %s\n", (a->blocks && !a->bad) ? "Success" : "Failed");
}
#endif

void reportBlobBegin(void) {
  if (rbActive) return;
#ifdef _WIN32
  return; /* tanpa tee di Windows: report.blob tidak ditulis (lihat build_log) */
#else
  mkdirs(".satria");
  remove(RB_TMP);
  fflush(stdout);
  clock_gettime(CLOCK_MONOTONIC, &rbT0); /* Total time di LAST BUILD SUMMARY */
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
  (void)save;
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
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      double secs = (double)(t1.tv_sec - rbT0.tv_sec) +
                    (double)(t1.tv_nsec - rbT0.tv_nsec) / 1e9;
      /* Simpan ringkasan terstruktur, bukan keluaran mentah (§10). */
      char sum[sizeof(RB_TMP) + 8];
      snprintf(sum, sizeof(sum), "%s.sum", RB_TMP);
      FILE *o = fopen(sum, "w");
      if (o) {
        RbAgg agg = {0};
        rbCondense(RB_TMP, o, &agg, secs);
        bool ok = (fclose(o) == 0) && agg.blocks > 0; /* tanpa blok: laporan lama dipertahankan */
        if (ok && fsRenameReplace(sum, RB_FILE)) {
          remove(RB_TMP);
          return true;
        }
      }
      remove(sum);
    }
  }
  remove(RB_TMP); /* run gagal / kosong: laporan lama dipertahankan */
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
  while ((n = fread(buf, 1, sizeof(buf), f)) != 0) fwrite(buf, 1, n, stdout);
  fclose(f);
  fflush(stdout);
}

bool reportBlobHas(void) {
  struct stat sb;
  return stat(RB_FILE, &sb) == 0 && sb.st_size > 0;
}

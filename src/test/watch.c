/* watch.c — watcher unit test native (M4d, design/unit_test_dsl.md).
   Padanan C scripts/lib/unit_watcher.sh: memantau kemajuan run di area staging
   (targets/<target>/log) — read-only, tanpa menyentuh repo.

     header → entri tes selesai (dari timing.tsv) → baris process live (TTY)
     → fase prepare build snap (TTY) → Paused → Summary, lalu keluar.

   Non-TTY mencetak alur teks yang sama tanpa baris process/prepare live. */

#include "watch.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsl.h"

#define WBUF (SATRIA_MAX_PATH * 2 + 512)

static char w_stage[SATRIA_MAX_PATH];
static char w_log[WBUF];       /* -o, kosong = hanya stdout */
static char w_watchFile[WBUF + 64]; /* registri watcher (dihapus saat keluar) */
static int w_interval = 1;

/* ==================== util ==================== */

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "Error: ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

/* Cetak ke stdout dan (bila -o) append ke log watcher. */
static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
  if (!w_log[0]) return;
  FILE *fp = fopen(w_log, "a");
  if (!fp) return;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  fclose(fp);
}

static void joinPath(char *out, size_t n, const char *a, const char *b) {
  if ((size_t)snprintf(out, n, "%s/%s", a, b) >= n) die("path terlalu panjang: %s/%s", a, b);
}

static long long nowMs(void) {
  return (long long)wallClockMs(); 
}

static bool alivePid(long pid) {
  if (pid <= 0) return false;
  return procAlive((int)pid);
  //return kill((pid_t)pid, 0) == 0;
}

/* PID di berkas (0 bila tidak ada/tidak sah). */
static long pidFile(const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp) return 0;
  long v = 0;
  if (fscanf(fp, "%ld", &v) != 1) v = 0;
  fclose(fp);
  return v > 0 ? v : 0;
}

/* Nilai KEY=... di berkas env. */
static bool fileValue(const char *path, const char *key, char *out, size_t n) {
  FILE *fp = fopen(path, "r");
  if (!fp) return false;
  char line[8192];
  size_t kl = strlen(key);
  bool found = false;
  while (fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
      copyStr(out, n, line + kl + 1);
      found = true;
      break;
    }
  }
  fclose(fp);
  return found;
}

static long long treeBytes(const char *path) {
  if (!fsDirExists(path)) return fsFileExists(path) ? fsFileSize(path) : 0;
  List files = {0};
  walkDir(path, "", &files);
  long long total = 0;
  for (int i = 0; i < files.count; i++) total += fsFileSize(files.items[i]);
  listFree(&files);
  return total;
}

static void fmtBytes(long long b, char *out, size_t n) {
  if (b >= 1048576)
    snprintf(out, n, "%.1fMB", (double)b / 1048576.0);
  else
    snprintf(out, n, "%.1fKB", (double)b / 1024.0);
}

/* " 12.3KB" bila path ada, kosong bila tidak (padanan size_of bash). */
static void sizeOf(const char *path, char *out, size_t n) {
  out[0] = '\0';
  if (!fsDirExists(path) && !fsFileExists(path)) return;
  char b[32];
  fmtBytes(treeBytes(path), b, sizeof(b));
  snprintf(out, n, " %s", b);
}

/* Total byte artefak build di folder snap (pembilang persen prepare). */
static long long snapBytes(const char *dir) {
  static const char *art[] = {"build", "bin", "lib", "dist", "modules", ".satria"};
  long long total = 0;
  for (size_t i = 0; i < sizeof(art) / sizeof(art[0]); i++) {
    char p[WBUF];
    joinPath(p, sizeof(p), dir, art[i]);
    if (fsDirExists(p)) total += treeBytes(p);
  }
  return total;
}

static void fmtHms(long long sec, char *out, size_t n) {
  if (sec < 0) sec = 0;
  snprintf(out, n, "%02lld:%02lld:%02lld", sec / 3600, (sec % 3600) / 60, sec % 60);
}

/* ==================== registry & sinyal ==================== */

static void wipeWatch(void) {
  if (w_watchFile[0]) fsRemoveFile(w_watchFile);
}

static void onWatchSignal(int sig) {
  (void)sig;
  wipeWatch();
  exit(130);
}

/* Bersihkan entri watcher mati; kumpulkan watcher lain dan targetnya. */
static int initRegistry(const char *watchDir, List *otherPids, List *otherTargets) {
  mkdirs(watchDir);
  List dirs = {0}, files = {0};
  fsListDir(watchDir, &dirs, &files);
  listFree(&dirs);
  long self = (long)procSelfPid();
  for (int i = 0; i < files.count; i++) {
    const char *b = strrchr(files.items[i], '/');
    b = b ? b + 1 : files.items[i];
    char num[64];
    copyStr(num, sizeof(num), b);
    char *end = NULL;
    long pid = strtol(num, &end, 10);
    if (!num[0] || !end || *end) {
      fsRemoveFile(files.items[i]);
      continue;
    }
    if (pid == self) continue;
    if (!alivePid(pid)) {
      fsRemoveFile(files.items[i]);
      continue;
    }
    listAdd(otherPids, num);
    char tgt[256] = "";
    FILE *fp = fopen(files.items[i], "r");
    if (fp) {
      if (!fgets(tgt, sizeof(tgt), fp)) tgt[0] = '\0';
      fclose(fp);
      tgt[strcspn(tgt, "\r\n")] = '\0';
    }
    if (tgt[0]) {
      char pair[300];
      snprintf(pair, sizeof(pair), "%ld:%s", pid, tgt);
      listAdd(otherTargets, pair);
    }
  }
  listFree(&files);
  return otherPids->count;
}

/* Bersihkan registri build ($STAGE/snap/<pid>.pid) milik proses mati. */
static void cleanSnapRegistry(const char *stage) {
  char snapDir[WBUF];
  joinPath(snapDir, sizeof(snapDir), stage, "snap");
  if (!fsDirExists(snapDir)) return;
  List dirs = {0}, files = {0};
  fsListDir(snapDir, &dirs, &files);
  listFree(&dirs);
  long self = (long)procSelfPid();
  for (int i = 0; i < files.count; i++) {
    const char *b = strrchr(files.items[i], '/');
    b = b ? b + 1 : files.items[i];
    char num[32];
    copyStr(num, sizeof(num), b);
    char *dot = strrchr(num, '.');
    if (dot) *dot = '\0';
    char *end = NULL;
    long pid = strtol(num, &end, 10);
    if (!num[0] || !end || *end || pid == self) {
      if (!num[0] || (end && *end)) fsRemoveFile(files.items[i]);
      continue;
    }
    if (!alivePid(pid)) fsRemoveFile(files.items[i]);
  }
  listFree(&files);
}

/* ==================== data run ==================== */

static void loadNames(const char *plan, const char *tim, const char *envp, List *names) {
  names->count = 0;
  FILE *fp = fopen(plan, "r");
  if (fp) {
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
      char *t1 = strchr(line, '\t');
      if (!t1) continue;
      char *name = t1 + 1;
      char *t2 = strchr(name, '\t');
      if (t2) *t2 = '\0';
      name[strcspn(name, "\r\n")] = '\0';
      if (*name) listAdd(names, name);
    }
    fclose(fp);
  }
  if (!names->count) {
    fp = fopen(tim, "r");
    if (fp) {
      char line[8192];
      while (fgets(line, sizeof(line), fp)) {
        char key[512];
        copyStr(key, sizeof(key), line);
        char *tab = strchr(key, '\t');
        if (!tab) continue;
        *tab = '\0';
        if (!key[0] || strcmp(key, "operasi") == 0 || strcmp(key, "snap-build") == 0) continue;
        listAdd(names, key);
      }
      fclose(fp);
    }
  }
  if (!names->count) {
    char a[32] = "0", b[32] = "0";
    fileValue(envp, "RUN_TESTS_COLD", a, sizeof(a));
    fileValue(envp, "RUN_TESTS_SNAP", b, sizeof(b));
    long n = strtol(a, NULL, 10) + strtol(b, NULL, 10);
    for (long i = 0; i < n; i++) {
      char num[32];
      snprintf(num, sizeof(num), "%ld", i + 1);
      listAdd(names, num);
    }
  }
}

/* Hasil tes dari timing.tsv: p f pre ms. */
static bool resultOf(const char *tim, const char *name, int *p, int *f, int *pre, long long *ms) {
  FILE *fp = fopen(tim, "r");
  if (!fp) return false;
  char line[8192];
  bool ok = false;
  while (fgets(line, sizeof(line), fp)) {
    char key[512];
    copyStr(key, sizeof(key), line);
    char *tab = strchr(key, '\t');
    if (!tab) continue;
    *tab = '\0';
    if (strcmp(key, name) != 0) continue;
    long long msV = 0;
    int pV = 0, fV = 0, qV = 0;
    if (sscanf(tab + 1, "%*[^\t]\t%lld\t%d\t%d\t%d", &msV, &pV, &fV, &qV) == 4) {
      *ms = msV;
      *p = pV;
      *f = fV;
      *pre = qV;
      ok = true;
    }
    break;
  }
  fclose(fp);
  return ok;
}

/* Durasi pembanding (%) dari timing.prev.tsv. */
static long long prevMs(const char *prev, const char *name) {
  FILE *fp = fopen(prev, "r");
  if (!fp) return 0;
  char line[8192];
  long long ms = 0;
  while (fgets(line, sizeof(line), fp)) {
    char key[512];
    copyStr(key, sizeof(key), line);
    char *tab = strchr(key, '\t');
    if (!tab) continue;
    *tab = '\0';
    if (strcmp(key, name) != 0) continue;
    long long v = 0;
    if (sscanf(tab + 1, "%*[^\t]\t%lld", &v) == 1) ms = v;
    break;
  }
  fclose(fp);
  return ms;
}

/* Nama tes yang sedang berjalan dan waktu mulainya (@epoch_ms di log runner). */
static void currentTest(const char *logp, char *name, size_t n, long long *startMs) {
  name[0] = '\0';
  *startMs = 0;
  FILE *fp = fopen(logp, "r");
  if (!fp) return;
  char line[8192];
  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "> Running ", 10) != 0) {
      if (strncmp(line, "> Selesai ", 10) == 0) name[0] = '\0';
      continue;
    }
    char tok[512];
    if (sscanf(line + 10, "%511s", tok) != 1) continue;
    char *dot = strrchr(tok, '.');
    if (dot && (strcmp(dot, ".sh") == 0 || strcmp(dot, ".test") == 0 || strcmp(dot, ".t") == 0))
      *dot = '\0';
    copyStr(name, n, tok);
    char *at = strrchr(line, '@');
    *startMs = at ? strtoll(at + 1, NULL, 10) : 0;
  }
  fclose(fp);
}

/* ==================== state watcher ==================== */

typedef struct {
  char target[256];
  char dir[WBUF];  /* stage/targets/<target> */
  char ldir[WBUF]; /* .../log */
  char envp[WBUF], plan[WBUF], tim[WBUF], prev[WBUF], logp[WBUF], pidp[WBUF];
  int n;
  List names;

  char runKey[128];
  int active;
  int ended;
  int paused;

  int emitK;
  int liveOn;
  char liveName[256];
  long long liveMs;
  char live100[256];

  int execShown, prepDrawn, prepDone, prepShown;
  long long prepT0, prepPrev, prepMs, prepPid;
  int prepCold;

  int pausedShown, sumPrinted;
  int tty;
  int waitUnknown;
} W;

static W w;

static void prepareTick(void);

static void loadState(void) {
  /* Anti-stale (design §9.1): run.env yang lebih tua dari run.pid bukan cerita
     run ini; tanpa run.pid dan tanpa bg.pid segar, tunggu dulu. */
  if (!w.runKey[0] && !fsFileExists(w.pidp)) {
    char bgp[WBUF];
    joinPath(bgp, sizeof(bgp), w.ldir, "bg.pid");
    bool fresh = fsFileExists(bgp) && (!fsFileExists(w.envp) || fsMTimeNs(bgp) > fsMTimeNs(w.envp));
    if (!fresh && w.waitUnknown <= 2) {
      w.waitUnknown++;
      return;
    }
  }
  w.waitUnknown = 0;

  if (!fsFileExists(w.envp)) {
    /* Run belum menulis env: mungkin fase prepare build snap. */
    if (w.prepDone != 2) prepareTick();
    return;
  }

  long rpid = pidFile(w.pidp);
  if (!rpid) {
    char bgp[WBUF];
    joinPath(bgp, sizeof(bgp), w.ldir, "bg.pid");
    long bpid = pidFile(bgp);
    if (bpid && alivePid(bpid) && fsFileExists(bgp) &&
        (!fsFileExists(w.envp) || fsMTimeNs(bgp) > fsMTimeNs(w.envp)))
      rpid = bpid;
  }
  w.active = alivePid(rpid);
  if (w.active && fsFileExists(w.pidp) && fsMTimeNs(w.envp) < fsMTimeNs(w.pidp)) return;

  char started[128] = "";
  fileValue(w.envp, "RUN_STARTED", started, sizeof(started));
  if (strcmp(started, w.runKey) != 0) {
    copyStr(w.runKey, sizeof(w.runKey), started);
    w.emitK = 0;
    w.liveOn = 0;
    w.liveName[0] = '\0';
    w.live100[0] = '\0';
    w.pausedShown = 0;
  }

  loadNames(w.plan, w.tim, w.envp, &w.names);
  w.n = w.names.count;
  if (!w.n) {
    char a[32] = "0", b[32] = "0";
    fileValue(w.envp, "RUN_TESTS_COLD", a, sizeof(a));
    fileValue(w.envp, "RUN_TESTS_SNAP", b, sizeof(b));
    w.n = (int)(strtol(a, NULL, 10) + strtol(b, NULL, 10));
  }

  char v[32] = "";
  w.ended = fileValue(w.envp, "RUN_ENDED", v, sizeof(v));
  w.paused = fileValue(w.envp, "RUN_PAUSED", v, sizeof(v)) && strcmp(v, "1") == 0;

  if (w.prepShown && w.prepDone == 1) w.prepDone = 2;
}

static void entryStr(char *out, size_t cap, int idx, int n, const char *name, const char *p,
                     const char *f, const char *q, const char *t) {
  snprintf(out, cap, "[%d/%d] %s\n* PASS: %s FAIL: %s PRE: %s TIME: %s\n", idx, n, name, p, f, q,
           t);
}

/* Baris process: durasi + persen (timing.prev) + ukuran folder work. */
static void procText(char *out, size_t cap) {
  char cur[256] = "";
  long long startMs = 0;
  currentTest(w.logp, cur, sizeof(cur), &startMs);
  long long tms = nowMs() - (startMs > 0 ? startMs : nowMs());
  if (tms < 0) tms = 0;
  if (tms < w.liveMs) tms = w.liveMs;
  w.liveMs = tms;
  char pctx[32] = "";
  long long pm = prevMs(w.prev, cur);
  if (cur[0] && pm > 0) {
    long long pct = tms * 100 / pm;
    if (pct > 99) pct = 99;
    snprintf(pctx, sizeof(pctx), " %lld%%", pct);
  }
  char sz[64];
  char workDir[WBUF];
  joinPath(workDir, sizeof(workDir), w.dir, "work");
  sizeOf(workDir, sz, sizeof(sz));
  snprintf(out, cap, "* process: %lld ms%s%s", tms, pctx, sz);
}

/* ==================== fase prepare ==================== */

static void execLine(void) {
  if (w.execShown) return;
  if (w.tty && w.prepDone == 1) {
    long long tms = w.prepMs;
    char sz[64];
    char snapDir[WBUF];
    joinPath(snapDir, sizeof(snapDir), w.dir, "snap");
    sizeOf(snapDir, sz, sizeof(sz));
    char hms[32];
    fmtHms(tms / 1000, hms, sizeof(hms));
    if (!w.prepDrawn) {
      printf("\n# Prepare\n");
      w.prepDrawn = 1;
    }
    printf("\r\033[2K√ %s%s\n", hms, sz);
  }
  printf("\n# Execution test\n");
  if (w_log[0]) {
    FILE *fp = fopen(w_log, "a");
    if (fp) {
      fputs("# Execution test\n", fp);
      fclose(fp);
    }
  }
  w.execShown = 1;
  w.prepDone = 2;
}

static void prepareTick(void) {
  long rpid = pidFile(w.pidp);
  if (!rpid) {
    char bgp[WBUF];
    joinPath(bgp, sizeof(bgp), w.ldir, "bg.pid");
    long bpid = pidFile(bgp);
    if (bpid && alivePid(bpid) && fsFileExists(bgp) &&
        (!fsFileExists(w.envp) || fsMTimeNs(bgp) > fsMTimeNs(w.envp)))
      rpid = bpid;
  }
  if (!rpid || !alivePid(rpid)) {
    if (w.prepShown && w.prepDone != 2) {
      if (w.tty) printf("\r\033[2K");
      const char *msg = "# Prepare : terhenti (run berhenti sebelum run.env)\n";
      printf("%s", msg);
      if (w_log[0]) {
        FILE *fp = fopen(w_log, "a");
        if (fp) {
          fputs(msg, fp);
          fclose(fp);
        }
      }
      wipeWatch();
      exit(1);
    }
    return;
  }
  if ((long long)rpid != w.prepPid) {
    w.prepPid = rpid;
    w.prepT0 = 0;
    w.prepMs = 0;
    w.prepPrev = 0;
    w.prepCold = 0;
    w.prepShown = 0;
  }
  char reg[WBUF];
  snprintf(reg, sizeof(reg), "%s/snap/%ld.pid", w_stage, rpid);
  FILE *fp = fopen(reg, "r");
  if (fp) {
    char tn[256] = "", md[32] = "";
    long long pv = 0;
    if (fscanf(fp, "%255s %31s %lld", tn, md, &pv) >= 2) {
      w.prepPrev = pv > 0 ? pv : 0;
      w.prepCold = strcmp(md, "cold") == 0;
      if (strcmp(tn, w.target) != 0) {
        w.prepCold = 0;
        w.prepPrev = 0;
      }
    }
    fclose(fp);
  }
  if (w.prepT0 == 0) {
    int64_t mt = fsMTimeNs(w.pidp);
    w.prepT0 = mt >= 0 ? (long long)(mt / 1000000) : nowMs();
  }
  if (w.prepT0 > nowMs()) w.prepT0 = nowMs();
  w.prepDone = 1;
  w.prepShown = 1;

  if (!w.tty) return;
  if (!w.prepDrawn) {
    printf("\n# Prepare\n");
    w.prepDrawn = 1;
  }
  long long tms = nowMs() - w.prepT0;
  if (tms < 0) tms = 0;
  if (tms < w.prepMs) tms = w.prepMs;
  w.prepMs = tms;
  char snapDir[WBUF];
  joinPath(snapDir, sizeof(snapDir), w.dir, "snap");
  char text[128];
  long long cur = snapBytes(snapDir);
  if (w.prepCold && w.prepPrev > 0 && cur > 0) {
    long long pct = cur * 100 / w.prepPrev;
    if (pct > 99) pct = 99;
    snprintf(text, sizeof(text), "* build: %lld%%", pct);
  } else {
    char sz[64];
    sizeOf(snapDir, sz, sizeof(sz));
    snprintf(text, sizeof(text), "* build:%s", sz);
  }
  printf("\r\033[2K%s", text);
  fflush(stdout);
}

/* ==================== render ==================== */

/* 0 = lanjut, 1 = selesai (exit 0). */
static void printSummary(void) {
  long long done = 0;
  for (int i = 0; i < w.names.count; i++) {
    int p, f, q;
    long long ms;
    if (resultOf(w.tim, w.names.items[i], &p, &f, &q, &ms)) done++;
  }
  long long sp = 0, sf = 0, sq = 0;
  FILE *fp = fopen(w.tim, "r");
  if (fp) {
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
      char key[512];
      copyStr(key, sizeof(key), line);
      char *tab = strchr(key, '\t');
      if (!tab) continue;
      *tab = '\0';
      if (!key[0] || strcmp(key, "operasi") == 0 || strcmp(key, "snap-build") == 0) continue;
      int p = 0, f = 0, q = 0;
      if (sscanf(tab + 1, "%*[^\t]\t%*[^\t]\t%d\t%d\t%d", &p, &f, &q) == 3) {
        sp += p;
        sf += f;
        sq += q;
      }
    }
    fclose(fp);
  }
  char msTxt[32] = "0";
  fileValue(w.envp, "RUN_ELAPSED_MS", msTxt, sizeof(msTxt));
  long long elapsed = strtoll(msTxt, NULL, 10);
  char hms[32];
  fmtHms(elapsed / 1000, hms, sizeof(hms));
  char buf[512];
  snprintf(buf, sizeof(buf),
           "\n# Summary\n* %lld/%d selesai, total %s\n* PASS: %lld FAIL: %lld PRE: %lld\n", done,
           w.n, hms, sp, sf, sq);
  printf("%s", buf);
  if (w_log[0]) {
    FILE *lf = fopen(w_log, "a");
    if (lf) {
      fputs(buf, lf);
      fclose(lf);
    }
  }
  w.sumPrinted = 1;
}

/* 0 = lanjut, 2 = summary tercetak (keluar setelah jeda). */
static int renderRun(void) {
  execLine();

  if (w.tty) {
    /* Entri selesai + baris process live (rewrite di tempat). */
    char cur[256] = "";
    long long startMs = 0;
    if (!w.ended && w.active) currentTest(w.logp, cur, sizeof(cur), &startMs);
    int want = !w.ended && w.active && cur[0];

    /* 100% untuk tes yang baru selesai sebelum entri berikutnya. */
    bool hold = false;
    if (!w.ended && w.liveOn && w.liveName[0] && cur[0] == '\0' &&
        w.emitK < w.names.count && strcmp(w.live100, w.liveName) != 0) {
      int p, f, q;
      long long ms;
      if (resultOf(w.tim, w.liveName, &p, &f, &q, &ms)) {
        if (ms < w.liveMs) ms = w.liveMs;
        char sz[64], workDir[WBUF];
        joinPath(workDir, sizeof(workDir), w.dir, "work");
        sizeOf(workDir, sz, sizeof(sz));
        printf("\r\033[2K* process: %lld ms 100%%%s", ms, sz);
        copyStr(w.live100, sizeof(w.live100), w.liveName);
        hold = true;
      }
    }
    bool newEntry = false;
    for (int k = w.emitK; k < w.names.count; k++) {
      int p, f, q;
      long long ms;
      if (resultOf(w.tim, w.names.items[k], &p, &f, &q, &ms)) {
        newEntry = true;
        break;
      }
      if (w.ended) {
        newEntry = true;
        break;
      }
      break;
    }
    if (!hold && (newEntry || strcmp(w.liveName, want ? cur : "") != 0)) {
      if (w.liveOn) printf("\r\033[2K\033[1A\r\033[2K");
      w.liveOn = 0;
      w.liveName[0] = '\0';
      while (w.emitK < w.names.count) {
        const char *name = w.names.items[w.emitK];
        int p, f, q;
        long long ms;
        char ps[16], fs[16], qs[16], ts[32];
        if (resultOf(w.tim, name, &p, &f, &q, &ms)) {
          snprintf(ps, sizeof(ps), "%d", p);
          snprintf(fs, sizeof(fs), "%d", f);
          snprintf(qs, sizeof(qs), "%d", q);
          snprintf(ts, sizeof(ts), "%lld ms", ms);
        } else if (w.ended) {
          copyStr(ps, sizeof(ps), "-");
          copyStr(fs, sizeof(fs), "-");
          copyStr(qs, sizeof(qs), "-");
          copyStr(ts, sizeof(ts), "-");
        } else {
          break;
        }
        char e[1024];
        entryStr(e, sizeof(e), w.emitK + 1, w.n, name, ps, fs, qs, ts);
        fputs(e, stdout);
        w.emitK++;
      }
      if (want) {
        char proc[256];
        procText(proc, sizeof(proc));
        printf("[%d/%d] %s\n%s", w.emitK + 1, w.n, cur, proc);
        w.liveOn = 1;
        copyStr(w.liveName, sizeof(w.liveName), cur);
      }
      fflush(stdout);
    } else if (w.liveOn) {
      char proc[256];
      procText(proc, sizeof(proc));
      printf("\r\033[2K%s", proc);
      fflush(stdout);
    }
  } else {
    /* Non-TTY: alur teks tanpa baris process. */
    while (w.emitK < w.names.count) {
      const char *name = w.names.items[w.emitK];
      int p, f, q;
      long long ms;
      char ps[16], fs[16], qs[16], ts[32];
      if (resultOf(w.tim, name, &p, &f, &q, &ms)) {
        snprintf(ps, sizeof(ps), "%d", p);
        snprintf(fs, sizeof(fs), "%d", f);
        snprintf(qs, sizeof(qs), "%d", q);
        snprintf(ts, sizeof(ts), "%lld ms", ms);
      } else if (w.ended) {
        copyStr(ps, sizeof(ps), "-");
        copyStr(fs, sizeof(fs), "-");
        copyStr(qs, sizeof(qs), "-");
        copyStr(ts, sizeof(ts), "-");
      } else {
        break;
      }
      char e[1024];
      entryStr(e, sizeof(e), w.emitK + 1, w.n, name, ps, fs, qs, ts);
      fputs(e, stdout);
      if (w_log[0]) {
        FILE *lf = fopen(w_log, "a");
        if (lf) {
          fputs(e, lf);
          fclose(lf);
        }
      }
      w.emitK++;
    }
    fflush(stdout);
  }

  /* Paused: run berhenti terkendali tanpa RUN_ENDED. */
  if (!w.ended && !w.active && w.paused && !w.pausedShown) {
    w.pausedShown = 1;
    int done = 0;
    for (int i = 0; i < w.names.count; i++) {
      int p, f, q;
      long long ms;
      if (resultOf(w.tim, w.names.items[i], &p, &f, &q, &ms)) done++;
    }
    char msg[512];
    snprintf(msg, sizeof(msg),
             "\n# Paused : %d/%d selesai, sisa %d — lanjutkan dengan: unit_dev.sh resume %s\n",
             done, w.n, w.n - done, w.target);
    printf("%s", msg);
    if (w_log[0]) {
      FILE *lf = fopen(w_log, "a");
      if (lf) {
        fputs(msg, lf);
        fclose(lf);
      }
    }
  }

  if (w.ended && !w.sumPrinted) {
    printSummary();
    return 2;
  }
  return 0;
}

/* ==================== main ==================== */

static void detectTarget(void) {
  char targets[WBUF];
  joinPath(targets, sizeof(targets), w_stage, "targets");
  if (!fsDirExists(targets)) die("belum ada run di staging: %s (jalankan run dulu)", targets);
  List dirs = {0}, files = {0};
  fsListDir(targets, &dirs, &files);
  listFree(&files);
  List active = {0}, known = {0};
  for (int i = 0; i < dirs.count; i++) {
    char pidp[WBUF], envp[WBUF];
    joinPath(pidp, sizeof(pidp), dirs.items[i], "log/run.pid");
    if (alivePid(pidFile(pidp))) listAdd(&active, dirs.items[i]);
    joinPath(envp, sizeof(envp), dirs.items[i], "log/run.env");
    if (fsFileExists(envp)) listAdd(&known, dirs.items[i]);
  }
  listFree(&dirs);
  if (active.count > 1) {
    char list[512] = "";
    for (int i = 0; i < active.count; i++) {
      char b[256];
      copyStr(b, sizeof(b), strrchr(active.items[i], '/') ? strrchr(active.items[i], '/') + 1
                                                         : active.items[i]);
      if (i) strncat(list, ",", sizeof(list) - strlen(list) - 1);
      strncat(list, b, sizeof(list) - strlen(list) - 1);
    }
    die("ada beberapa run aktif (%s): sebutkan yang mau dipantau, mis. -w %s", list,
        strrchr(active.items[0], '/') ? strrchr(active.items[0], '/') + 1 : active.items[0]);
  }
  if (active.count == 0) {
    char list[512] = "";
    for (int i = 0; i < known.count; i++) {
      const char *b = strrchr(known.items[i], '/');
      b = b ? b + 1 : known.items[i];
      if (i) strncat(list, ",", sizeof(list) - strlen(list) - 1);
      strncat(list, b, sizeof(list) - strlen(list) - 1);
    }
    if (known.count)
      die("tidak ada run aktif: sebutkan target yang mau dipantau (%s)", list);
    die("tidak ada run aktif: sebutkan target yang mau dipantau");
  }
  copyStr(w.target, sizeof(w.target),
          strrchr(active.items[0], '/') ? strrchr(active.items[0], '/') + 1 : active.items[0]);
  listFree(&active);
  listFree(&known);
}

int testWatchMain(const char *target, const char *optLog, const char *stage) {
  copyStr(w_stage, sizeof(w_stage), stage);
  copyStr(w_log, sizeof(w_log), optLog ? optLog : "");
  memset(&w, 0, sizeof(w));

  char targets[WBUF];
  joinPath(targets, sizeof(targets), w_stage, "targets");
  if (!fsDirExists(targets)) die("belum ada run di staging: %s (jalankan run dulu)", targets);

  char tn[256];
  copyStr(tn, sizeof(tn), target ? target : "");
  if (strchr(tn, '/') || tn[0] == '.') die("nama target tidak valid: %s", tn);

  const char *iv = getenv("UNIT_WATCH_INTERVAL");
  if (iv && *iv) {
    char *end = NULL;
    long v = strtol(iv, &end, 10);
    if (!end || *end || v <= 0) die("UNIT_WATCH_INTERVAL harus bilangan bulat");
    w_interval = (int)v;
  }

  memset(&w, 0, sizeof(w));
  copyStr(w.target, sizeof(w.target), tn);
  if (!w.target[0]) detectTarget();

  joinPath(w.dir, sizeof(w.dir), targets, w.target);
  joinPath(w.ldir, sizeof(w.ldir), w.dir, "log");
  joinPath(w.envp, sizeof(w.envp), w.ldir, "run.env");
  joinPath(w.plan, sizeof(w.plan), w.ldir, "plan.tsv");
  joinPath(w.tim, sizeof(w.tim), w.ldir, "timing.tsv");
  joinPath(w.prev, sizeof(w.prev), w.ldir, "timing.prev.tsv");
  joinPath(w.logp, sizeof(w.logp), w.ldir, "unit.log");
  joinPath(w.pidp, sizeof(w.pidp), w.ldir, "run.pid");
  w.tty = termIsTTY();

  char watchDir[WBUF];
  joinPath(watchDir, sizeof(watchDir), w_stage, "watch");
  List others = {0}, otherTargets = {0};
  int nwatch = initRegistry(watchDir, &others, &otherTargets);
  snprintf(w_watchFile, sizeof(w_watchFile), "%s/%ld", watchDir, (long)procSelfPid());
  FILE *wf = fopen(w_watchFile, "w");
  if (wf) {
    fprintf(wf, "%s\n", w.target);
    fclose(wf);
  }
  cleanSnapRegistry(w_stage);

  signal(SIGINT, onWatchSignal);
  signal(SIGTERM, onWatchSignal);
#ifdef SIGHUP
  signal(SIGHUP, onWatchSignal);
#endif

  if (nwatch > 0) {
    char list[512] = "";
    for (int i = 0; i < others.count; i++) {
      if (i) strncat(list, " ", sizeof(list) - strlen(list) - 1);
      strncat(list, others.items[i], sizeof(list) - strlen(list) - 1);
    }
    say("* Watcher lain aktif: %d (%s)\n", nwatch, list);
  }
  listFree(&others);
  listFree(&otherTargets);

  char pidTxt[32] = "";
  long runPid = pidFile(w.pidp);
  if (runPid > 0) snprintf(pidTxt, sizeof(pidTxt), " - %ld", runPid);
  say("# Watch unit test\n");
  say("* PID      : %ld\n", (long)procSelfPid());
  say("* Staging  : %s\n", targets);
  say("* Interval : %ds, Ctrl-C untuk berhenti\n", w_interval);
  say("* Target   : %s%s\n", w.target, pidTxt);
  if (!tn[0]) say("* Dipilih otomatis: satu-satunya run aktif\n");

  for (;;) {
    loadState();
    if (fsFileExists(w.envp)) {
      int r = renderRun();
      if (r == 2) {
        procSleepMs(2);
        wipeWatch();
        listFree(&w.names);
        return 0;
      }
      if (r == 1) {
        wipeWatch();
        listFree(&w.names);
        return 0;
      }
    }
    procSleepMs(w_interval * 1000);
  }
}

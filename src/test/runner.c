/* runner.c — runner unit test native (M4c, design/unit_test_dsl.md).
   Padanan C jalur foreground scripts/lib/unit_runner.sh:

     staging tests/unit (common/, lib/, classes.conf + test.state)
       → klasifikasi classes.conf (cold/snap) + validasi discovery
       → fixture target tests/<nama> (tanpa artefak build)
       → snapshot cold sekali untuk tes [snap]
       → discovery compdb (satria -w -g compdb → sources.tsv)   → tes .test/.t lewat eksekutor DSL C (`satria test <file>`), tes .sh lewat sh
   → log per tes, timing.tsv, plan.tsv, run.env, ringkasan.

   CLI: `satria test --run <target> [-j N] [-s tes[,tes...]] [-o log] [--clean]`.
   Belum termasuk (M4d): background/pause/resume, watcher, dan pemanggilan
   in-process — runner ini memanggil binary yang di-pin sebagai child. */

#include "runner.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsl.h"
#include "watch.h"

#if defined(_WIN32)

int testRunnerMain(int argc, const char *const *argv, const char *root, const char *bin) {
  (void)argc;
  (void)argv;
  (void)root;
  (void)bin;
  fprintf(stderr, "satria test --run: runner native hanya di POSIX.\n");
  return 2;
}

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define XBUF (SATRIA_MAX_PATH * 2 + 512)

/* run.pid yang harus dihapus bila run background dihentikan. */
static char g_runPidPath[XBUF];

static void onBgSignal(int sig) {
  if (g_runPidPath[0]) unlink(g_runPidPath);
  signal(sig, SIG_DFL);
  kill(0, sig); /* ikut hentikan anak (tes) di process group yang sama */
}

typedef struct {
  char root[SATRIA_MAX_PATH];
  char tests[SATRIA_MAX_PATH];
  char tname[256];
  char target[SATRIA_MAX_PATH];
  char stage[SATRIA_MAX_PATH];
  char t[SATRIA_MAX_PATH];
  char logdir[SATRIA_MAX_PATH];
  char log[XBUF];
  char fix[SATRIA_MAX_PATH];
  char snapd[SATRIA_MAX_PATH];
  char work[SATRIA_MAX_PATH];
  char tmpd[SATRIA_MAX_PATH];
  char disc[SATRIA_MAX_PATH];
  char pin[XBUF];
  char wrap[XBUF];
  char binId[17];
  char jobsArg[32];
  char sleepLog[SATRIA_MAX_PATH];
  int jobs;
  int bg;
  List cold, snap; /* urutan classes.conf */
  int pass, fail, pre, total;
  List results;
} Ctx;

static Ctx g;
static FILE *g_log;

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

static void sayf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  if (g_log) {
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
  }
  fflush(stdout);
}

static const char *baseName(const char *p) {
  const char *b = strrchr(p, '/');
  return b ? b + 1 : p;
}

static void joinPath(char *out, size_t n, const char *a, const char *b) {
  if ((size_t)snprintf(out, n, "%s/%s", a, b) >= n)
    die("path terlalu panjang: %s/%s", a, b);
}

static void copyRaw(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (!in) die("gagal membaca %s", src);
  mkparent(dst);
  FILE *out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    die("gagal menulis %s", dst);
  }
  char buf[65536];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
    if (fwrite(buf, 1, got, out) != got) die("gagal menulis %s", dst);
  if (ferror(in)) die("gagal membaca %s", src);
  fclose(in);
  fclose(out);
  unsigned mode = fsGetMode(src);
  if (mode) fsSetMode(dst, mode);
}

static void copyTree(const char *src, const char *dst) {
  mkdirs(dst);
  List dirs = {0}, files = {0};
  fsListDir(src, &dirs, &files);
  for (int i = 0; i < files.count; i++) {
    char out[XBUF];
    joinPath(out, sizeof(out), dst, baseName(files.items[i]));
    copyRaw(files.items[i], out);
  }
  for (int i = 0; i < dirs.count; i++) {
    char out[XBUF];
    joinPath(out, sizeof(out), dst, baseName(dirs.items[i]));
    copyTree(dirs.items[i], out);
  }
  listFree(&dirs);
  listFree(&files);
}

static bool listHas(const List *l, const char *s) {
  for (int i = 0; i < l->count; i++)
    if (strcmp(l->items[i], s) == 0) return true;
  return false;
}

static void listAddUnique(List *l, const char *s) {
  if (!listHas(l, s)) listAdd(l, s);
}

/* -s/--select menerima daftar dipisah koma: "cache_noop,version_alias". */
static void listAddCsv(List *l, const char *csv) {
  char buf[512];
  copyStr(buf, sizeof(buf), csv);
  char *p = buf;
  while (p) {
    char *c = strchr(p, ',');
    if (c) *c = '\0';
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) p[--n] = '\0';
    if (*p) listAdd(l, p);
    p = c ? c + 1 : NULL;
  }
}

static void writeText(const char *path, const char *text) {
  mkparent(path);
  FILE *fp = fopen(path, "wb");
  if (!fp) die("gagal menulis %s", path);
  size_t n = strlen(text);
  if (fwrite(text, 1, n, fp) != n) die("gagal menulis %s", path);
  fclose(fp);
}

static void trimLine(char *s) {
  char *p = s;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
    s[--n] = '\0';
}

static uint64_t fnvBytes(uint64_t h, const void *p, size_t n) {
  const unsigned char *b = p;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t fnvStr(uint64_t h, const char *s) { return fnvBytes(h, s, strlen(s)); }

/* Sidik jari pohon: path relatif + ukuran + mtime. Cukup untuk test.state
   dan identitas snapshot; bukan checksum isi. */
static uint64_t treeStamp(const char *dir) {
  uint64_t h = 1469598103934665603ULL;
  List files = {0};
  walkDir(dir, "", &files);
  listSort(&files);
  size_t rootLen = strlen(dir);
  for (int i = 0; i < files.count; i++) {
    const char *rel = files.items[i] + rootLen;
    if (*rel == '/') rel++;
    h = fnvStr(h, rel);
    h = fnvBytes(h, &(long long){fsFileSize(files.items[i])}, sizeof(long long));
    h = fnvBytes(h, &(int64_t){fsMTimeNs(files.items[i])}, sizeof(int64_t));
  }
  listFree(&files);
  return h;
}

/* Sidik jari isi pohon (path relatif + byte berkas) — identitas snapshot harus
   stabil antar run; mtime fixture berubah setiap kali disalin. */
static uint64_t treeContentStamp(const char *dir) {
  uint64_t h = 1469598103934665603ULL;
  List files = {0};
  walkDir(dir, "", &files);
  listSort(&files);
  size_t rootLen = strlen(dir);
  for (int i = 0; i < files.count; i++) {
    const char *rel = files.items[i] + rootLen;
    if (*rel == '/') rel++;
    h = fnvStr(h, rel);
    size_t n = 0;
    char *data = dslReadFile(files.items[i], &n);
    if (!data) continue;
    h = fnvBytes(h, &(uint64_t){n}, sizeof(uint64_t));
    h = fnvBytes(h, data, n);
    free(data);
  }
  listFree(&files);
  return h;
}

static uint64_t fileStamp(const char *path) {
  uint64_t h = 1469598103934665603ULL;
  h = fnvStr(h, path);
  h = fnvBytes(h, &(long long){fsFileSize(path)}, sizeof(long long));
  h = fnvBytes(h, &(int64_t){fsMTimeNs(path)}, sizeof(int64_t));
  return h;
}

static bool readPidFile(const char *path, pid_t *pid) {
  FILE *fp = fopen(path, "r");
  if (!fp) return false;
  long v = 0;
  bool ok = fscanf(fp, "%ld", &v) == 1;
  fclose(fp);
  if (ok) *pid = (pid_t)v;
  return ok;
}

/* ==================== staging ==================== */

static void staging(void) {
  char commonSrc[XBUF], libSrc[XBUF], classesSrc[XBUF];
  joinPath(commonSrc, sizeof(commonSrc), g.tests, "common");
  joinPath(libSrc, sizeof(libSrc), g.tests, "lib");
  joinPath(classesSrc, sizeof(classesSrc), g.tests, "classes.conf");
  if (!fsDirExists(commonSrc)) die("tests/unit/common tidak ada: %s", commonSrc);
  if (!fsFileExists(classesSrc)) die("classes.conf tidak ada: %s", classesSrc);

  mkdirs(g.stage);
  char bindir[XBUF];
  joinPath(bindir, sizeof(bindir), g.stage, "bin");
  mkdirs(bindir);

  uint64_t fp = 1469598103934665603ULL;
  fp = fnvBytes(fp, &(uint64_t){treeStamp(commonSrc)}, sizeof(uint64_t));
  fp = fnvBytes(fp, &(uint64_t){treeStamp(libSrc)}, sizeof(uint64_t));
  fp = fnvBytes(fp, &(uint64_t){fileStamp(classesSrc)}, sizeof(uint64_t));

  char state[XBUF], stateTxt[XBUF] = "";
  joinPath(state, sizeof(state), g.stage, "test.state");
  size_t sl = 0;
  char *old = dslReadFile(state, &sl);
  if (old) {
    copyStr(stateTxt, sizeof(stateTxt), old);
    free(old);
  }
  char want[64];
  snprintf(want, sizeof(want), "SRC_FINGERPRINT=%016llx", (unsigned long long)fp);
  char commonDst[XBUF], libDst[XBUF], classesDst[XBUF];
  joinPath(commonDst, sizeof(commonDst), g.stage, "common");
  joinPath(libDst, sizeof(libDst), g.stage, "lib");
  joinPath(classesDst, sizeof(classesDst), g.stage, "classes.conf");

  if (strstr(stateTxt, want) && fsDirExists(commonDst) && fsDirExists(libDst)) {
    sayf("> Staging : sama dengan test.state (tidak disalin ulang)\n");
  } else {
    sayf("> Staging : sinkronkan common/, lib/, classes.conf\n");
    fsRemoveTree(commonDst);
    fsRemoveTree(libDst);
    copyTree(commonSrc, commonDst);
    copyTree(libSrc, libDst);
    copyRaw(classesSrc, classesDst);
    char buf[XBUF];
    snprintf(buf, sizeof(buf),
             "TEST_STATE_VERSION=1\nSRC_FINGERPRINT=%016llx\nFILES:\n",
             (unsigned long long)fp);
    writeText(state, buf);
  }

  /* Binary yang di-pin: identitas dari path+ukuran+mtime. */
  uint64_t bh = fileStamp(g.pin[0] ? g.pin : "");
  (void)bh;
  char binAbs[XBUF];
  copyStr(binAbs, sizeof(binAbs), g.pin);
  uint64_t id = 1469598103934665603ULL;
  id = fnvStr(id, binAbs);
  id = fnvBytes(id, &(long long){fsFileSize(binAbs)}, sizeof(long long));
  id = fnvBytes(id, &(int64_t){fsMTimeNs(binAbs)}, sizeof(int64_t));
  snprintf(g.binId, sizeof(g.binId), "%016llx", (unsigned long long)id);
  char pin[XBUF + 64];
  snprintf(pin, sizeof(pin), "%s/satria-%.8s", bindir, g.binId);
  if (!fsFileExists(pin)) {
    char tmp[XBUF + 80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", pin);
    copyRaw(binAbs, tmp);
    fsRemoveFile(pin);
    if (rename(tmp, pin) != 0) die("gagal memindahkan binary ke %s", pin);
  }
  fsSetMode(pin, 0755);
  copyStr(g.pin, sizeof(g.pin), pin);

  /* Wrapper -j untuk tes .sh lama (path binary + jobs). */
  joinPath(g.wrap, sizeof(g.wrap), g.t, "bin/satria-j");
  char wtext[XBUF + 128];
  snprintf(wtext, sizeof(wtext), "#!/bin/sh\nexec \"%s\" %s \"$@\"\n", g.pin, g.jobsArg);
  writeText(g.wrap, wtext);
  fsSetMode(g.wrap, 0755);

  /* Shim sleep: mencatat durasi tunggu per tes (work_ms = total - tunggu). */
  char shim[XBUF];
  joinPath(shim, sizeof(shim), g.stage, "shim");
  mkdirs(shim);
  const char *realSleep = fsFileExists("/bin/sleep") ? "/bin/sleep" : "/usr/bin/sleep";
  if (!fsFileExists(realSleep)) die("sleep tidak ditemukan");
  char stext[XBUF];
  snprintf(stext, sizeof(stext),
           "#!/bin/sh\n[ -n \"$UNIT_SLEEP_LOG\" ] && echo \"${UNIT_TEST_NAME:-?} $1\" >> "
           "\"$UNIT_SLEEP_LOG\"\nexec \"%s\" \"$@\"\n",
           realSleep);
  char shimPath[XBUF];
  joinPath(shimPath, sizeof(shimPath), shim, "sleep");
  writeText(shimPath, stext);
  fsSetMode(shimPath, 0755);
  const char *oldPath = getenv("PATH");
  char newPath[8192];
  snprintf(newPath, sizeof(newPath), "%s:%s", shim, oldPath ? oldPath : "/usr/bin:/bin");
  setenv("PATH", newPath, 1);
  setenv("UNIT_REAL_SLEEP", realSleep, 1);
  joinPath(g.sleepLog, sizeof(g.sleepLog), g.logdir, "sleep.log");
  setenv("UNIT_SLEEP_LOG", g.sleepLog, 1);
  setenv("UNIT_JOBS", g.jobsArg + 2, 1);
}

/* ==================== klasifikasi & discovery ==================== */

static char *readAll(const char *path) {
  size_t n = 0;
  return dslReadFile(path, &n);
}

static void validateClasses(void) {
  char path[XBUF];
  joinPath(path, sizeof(path), g.stage, "classes.conf");
  char *txt = readAll(path);
  if (!txt) die("classes.conf tidak ada: %s", path);

  int sec = 0; /* 0 = luar bagian, 1 = cold, 2 = snap */
  for (char *line = txt; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    trimLine(line);
    if (*line) {
      if (strcmp(line, "[cold]") == 0) {
        sec = 1;
      } else if (strcmp(line, "[snap]") == 0) {
        sec = 2;
      } else if (line[0] == '[') {
        die("classes.conf: bagian tidak dikenal: %s", line);
      } else if (!sec) {
        die("classes.conf: entri '%s' di luar bagian", line);
      } else {
        char f[XBUF];
        snprintf(f, sizeof(f), "%s/common/%s.sh", g.stage, line);
        bool have = fsFileExists(f);
        if (!have) {
          snprintf(f, sizeof(f), "%s/common/%s.test", g.stage, line);
          have = fsFileExists(f);
        }
        if (!have) {
          snprintf(f, sizeof(f), "%s/common/%s.t", g.stage, line);
          have = fsFileExists(f);
        }
        if (!have) die("classes.conf: '%s' tidak punya berkas common/%s.sh|test|t", line, line);
        listAdd(sec == 1 ? &g.cold : &g.snap, line);
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  free(txt);

  /* Discovery: .test/.t lebih dulu (nama sama menang), lalu .sh yang belum
     punya padanan DSL; urutan nama seperti glob bash. */
  List dsl = {0}, sh = {0}, all = {0};
  char common[XBUF];
  joinPath(common, sizeof(common), g.stage, "common");
  List dirs = {0}, files = {0};
  fsListDir(common, &dirs, &files);
  for (int i = 0; i < files.count; i++) {
    const char *b = baseName(files.items[i]);
    size_t n = strlen(b);
    char name[256];
    if (n > 5 && strcmp(b + n - 5, ".test") == 0) {
      snprintf(name, sizeof(name), "%.*s", (int)(n - 5), b);
      listAddUnique(&dsl, name);
    } else if (n > 2 && strcmp(b + n - 2, ".t") == 0) {
      snprintf(name, sizeof(name), "%.*s", (int)(n - 2), b);
      listAddUnique(&dsl, name);
    } else if (n > 3 && strcmp(b + n - 3, ".sh") == 0) {
      snprintf(name, sizeof(name), "%.*s", (int)(n - 3), b);
      listAddUnique(&sh, name);
    }
  }
  listFree(&dirs);
  listFree(&files);
  listSort(&dsl);
  listSort(&sh);
  for (int i = 0; i < dsl.count; i++) listAdd(&all, dsl.items[i]);
  for (int i = 0; i < sh.count; i++)
    if (!listHas(&dsl, sh.items[i])) listAdd(&all, sh.items[i]);

  for (int i = 0; i < all.count; i++) {
    int n = 0;
    for (int j = 0; j < g.cold.count; j++)
      if (strcmp(g.cold.items[j], all.items[i]) == 0) n++;
    for (int j = 0; j < g.snap.count; j++)
      if (strcmp(g.snap.items[j], all.items[i]) == 0) n++;
    if (n != 1)
      die("tes '%s' belum diklasifikasikan (atau terdaftar ganda) di classes.conf",
          all.items[i]);
  }

  /* Guard [snap] untuk tes .sh: tidak boleh menghapus state build. */
  for (int i = 0; i < g.snap.count; i++) {
    char f[XBUF];
    snprintf(f, sizeof(f), "%s/common/%s.sh", g.stage, g.snap.items[i]);
    if (!fsFileExists(f)) continue;
    char *s = readAll(f);
    if (!s) continue;
    bool hit = false;
    for (char *line = s; line && *line;) {
      char *nl = strchr(line, '\n');
      if (nl) *nl = '\0';
      if (strstr(line, "rm -rf") &&
          (strstr(line, ".satria") || strstr(line, "/build") || strstr(line, "/bin") ||
           strstr(line, "/lib")))
        hit = true;
      line = nl ? nl + 1 : NULL;
    }
    free(s);
    if (hit)
      die("tes [snap] '%s' menghapus state build. Pindahkan ke [cold] atau tulis ulang.",
          g.snap.items[i]);
  }

  listFree(&dsl);
  listFree(&sh);
  listFree(&all);
  sayf("> Klasifikasi : %d cold, %d snap\n", g.cold.count, g.snap.count);
}

static void selectTests(const List *sel) {
  if (!sel->count) return;
  List nc = {0}, ns = {0};
  for (int i = 0; i < sel->count; i++) {
    const char *s = sel->items[i];
    bool ok = listHas(&g.cold, s) || listHas(&g.snap, s);
    if (!ok)
      die("tes '-t %s' tidak ada di tests/unit/common atau tidak terdaftar di classes.conf", s);
    if (listHas(&g.cold, s)) listAddUnique(&nc, s);
    else listAddUnique(&ns, s);
  }
  listFree(&g.cold);
  listFree(&g.snap);
  g.cold = nc;
  g.snap = ns;
  char list[1024] = "";
  for (int i = 0; i < sel->count; i++) {
    if (i) copyStr(list + strlen(list), sizeof(list) - strlen(list), " ");
    copyStr(list + strlen(list), sizeof(list) - strlen(list), sel->items[i]);
  }
  sayf("> Seleksi : %d tes (-t: %s), cold %d, snap %d\n", sel->count, list, g.cold.count,
       g.snap.count);
}

/* ==================== fixture & snapshot ==================== */

static void prepareFixture(void) {
  fsRemoveTree(g.fix);
  testCopyFixtureTree(g.target, g.fix);
  if (!fsDirExists(g.fix)) die("gagal menyalin fixture %s", g.target);
}

static int spawnCaptured(char *const argv[], const char *cwd, const char *logPath) {
  pid_t pid = fork();
  if (pid < 0) die("fork gagal: %s", strerror(errno));
  if (pid == 0) {
    if (cwd && chdir(cwd) != 0) _exit(126);
    int fd = open(logPath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) _exit(125);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    if (strchr(argv[0], '/'))
      execv(argv[0], argv);
    else
      execvp(argv[0], argv);
    _exit(127);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) die("waitpid gagal: %s", strerror(errno));
  }
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return 2;
}

static void teeLog(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return;
  char buf[8192];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
    fwrite(buf, 1, got, stdout);
    if (g_log) fwrite(buf, 1, got, g_log);
  }
  fclose(fp);
  fflush(stdout);
  if (g_log) fflush(g_log);
}

/* Total byte artefak build (pembilang persen prepare di watcher). */
static long long artifactBytes(const char *dir) {
  static const char *art[] = {"build", "bin", "lib", "dist", "modules", ".satria"};
  long long total = 0;
  for (size_t i = 0; i < sizeof(art) / sizeof(art[0]); i++) {
    char p[XBUF];
    joinPath(p, sizeof(p), dir, art[i]);
    if (!fsDirExists(p)) continue;
    List files = {0};
    walkDir(p, "", &files);
    for (int k = 0; k < files.count; k++) total += fsFileSize(files.items[k]);
    listFree(&files);
  }
  return total;
}

static void buildSnap(uint64_t fixHash) {
  if (!g.snap.count) return;
  char envp[XBUF];
  joinPath(envp, sizeof(envp), g.snapd, "snap.env");
  uint64_t id = 1469598103934665603ULL;
  id = fnvStr(id, g.tname);
  id = fnvStr(id, g.binId);
  id = fnvBytes(id, &fixHash, sizeof(fixHash));
  char idTxt[64];
  snprintf(idTxt, sizeof(idTxt), "SNAP_ID=%016llx", (unsigned long long)id);

  char *old = readAll(envp);
  bool match = old && strstr(old, idTxt);
  free(old);
  if (match) {
    sayf("> Snap : identitas cocok, tidak dibangun ulang\n");
    return;
  }

  sayf("> Snap : build cold sekali\n");
  long long prevBytes = artifactBytes(g.snapd);
  char snapDir[XBUF], snapReg[XBUF + 64];
  joinPath(snapDir, sizeof(snapDir), g.stage, "snap");
  mkdirs(snapDir);
  snprintf(snapReg, sizeof(snapReg), "%s/%d.pid", snapDir, (int)getpid());
  char regTxt[320];
  snprintf(regTxt, sizeof(regTxt), "%s cold %lld\n", g.tname, prevBytes);
  writeText(snapReg, regTxt);
  fsRemoveTree(g.snapd);
  copyTree(g.fix, g.snapd);
  char logp[XBUF];
  joinPath(logp, sizeof(logp), g.logdir, "snap-build.log");
  char *argv[] = {g.pin, g.jobsArg, "-w", NULL};
  long long t0 = (long long)portMonoMs();
  int rc = spawnCaptured(argv, g.snapd, logp);
  long long secs = ((long long)portMonoMs() - t0) / 1000;
  fsRemoveFile(snapReg);
  if (rc != 0) die("build snap gagal (rc=%d), lihat %s", rc, logp);
  char buf[256];
  snprintf(buf, sizeof(buf), "%s\nSNAP_BUILT_S=%lld\n", idTxt, secs);
  writeText(envp, buf);
  sayf("> Snap : selesai dalam %lld s\n", secs);
}

static void discovery(void) {
  mkdirs(g.disc);
  char tsv[XBUF];
  joinPath(tsv, sizeof(tsv), g.disc, "sources.tsv");
  char discw[XBUF];
  joinPath(discw, sizeof(discw), g.t, "discw");
  fsRemoveTree(discw);
  copyTree(g.fix, discw);
  char logp[XBUF];
  joinPath(logp, sizeof(logp), g.logdir, "discovery.log");
  char *argv[] = {g.pin, g.jobsArg, "-w", "-g", "compdb", NULL};
  int rc = spawnCaptured(argv, discw, logp);
  if (rc != 0) die("discovery compdb gagal, lihat %s", logp);
  char json[XBUF];
  joinPath(json, sizeof(json), discw, "compile_commands.json");
  if (!testCompdbToTsv(json, tsv)) die("discovery: compdb tanpa entri (lihat %s)", logp);

  FILE *fp = fopen(tsv, "r");
  if (!fp) die("discovery: gagal membaca %s", tsv);
  List projs = {0};
  int lines = 0;
  char line[4096];
  while (fgets(line, sizeof(line), fp)) {
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = '\0';
    listAddUnique(&projs, line);
    lines++;
  }
  fclose(fp);
  if (!lines) die("discovery: compdb tanpa entri (lihat %s)", logp);
  sayf("> Discovery : %d proyek, %d unit kompilasi (-w -g compdb)\n", projs.count, lines);
  listFree(&projs);
  fsRemoveTree(discw);
  setenv("UNIT_DISCOVERY", g.disc, 1);
}

/* ==================== plan, timing, run.env ==================== */

/* timing.prev.tsv = gabungan prev lama + timing run terakhir (run terakhir
   menang). Denominator persen watcher; run -s yang memilih subset tidak
   menyempitkan cakupan. */
static void mergeTimingPrev(const char *tim, const char *prev) {
  List keys = {0}, lines = {0};
  const char *src[2] = {prev, tim};
  for (int s = 0; s < 2; s++) {
    FILE *fp = fopen(src[s], "r");
    if (!fp) continue;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
      char *tab = strchr(line, '\t');
      if (!tab) continue;
      size_t kl = (size_t)(tab - line);
      if (kl >= 500) continue;
      char key[512];
      memcpy(key, line, kl);
      key[kl] = '\0';
      if (!key[0] || strcmp(key, "operasi") == 0) continue;
      size_t ll = strlen(line);
      char *dup = malloc(ll + 1);
      if (!dup) continue;
      memcpy(dup, line, ll + 1);
      int idx = -1;
      for (int i = 0; i < keys.count; i++)
        if (strcmp(keys.items[i], key) == 0) {
          idx = i;
          break;
        }
      if (idx >= 0) {
        free(lines.items[idx]);
        lines.items[idx] = dup;
      } else {
        listAdd(&keys, key);
        listAdd(&lines, dup);
        free(dup);
      }
    }
    fclose(fp);
  }
  if (lines.count) {
    FILE *out = fopen(prev, "w");
    if (out) {
      fputs("operasi\tfase\tms\tpass\tfail\tpre\tsleep_ms\twork_ms\n", out);
      for (int i = 0; i < lines.count; i++) fputs(lines.items[i], out);
      fclose(out);
    }
  }
  listFree(&keys);
  listFree(&lines);
}

static void writePlan(void) {
  char plan[XBUF];
  joinPath(plan, sizeof(plan), g.logdir, "plan.tsv");
  FILE *fp = fopen(plan, "w");
  if (!fp) die("gagal menulis %s", plan);
  int n = 0;
  for (int i = 0; i < g.cold.count; i++) fprintf(fp, "%d\t%s\tcold\n", ++n, g.cold.items[i]);
  for (int i = 0; i < g.snap.count; i++) fprintf(fp, "%d\t%s\tsnap\n", ++n, g.snap.items[i]);
  fclose(fp);

  char timing[XBUF];
  joinPath(timing, sizeof(timing), g.logdir, "timing.tsv");
  char prevTim[XBUF];
  joinPath(prevTim, sizeof(prevTim), g.logdir, "timing.prev.tsv");
  mergeTimingPrev(timing, prevTim);
  fp = fopen(timing, "w");
  if (!fp) die("gagal menulis %s", timing);
  fprintf(fp, "operasi\tfase\tms\tpass\tfail\tpre\tsleep_ms\twork_ms\n");
  char envp[XBUF];
  joinPath(envp, sizeof(envp), g.snapd, "snap.env");
  char *env = readAll(envp);
  if (env) {
    char *p = strstr(env, "SNAP_BUILT_S=");
    if (p) {
      long secs = strtol(p + strlen("SNAP_BUILT_S="), NULL, 10);
      fprintf(fp, "snap-build\tsnap\t%ld\t-\t-\t-\t-\t-\n", secs * 1000);
    }
    free(env);
  }
  fclose(fp);

  char runenv[XBUF];
  joinPath(runenv, sizeof(runenv), g.logdir, "run.env");
  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char stamp[64];
  strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  fp = fopen(runenv, "w");
  if (!fp) die("gagal menulis %s", runenv);
  fprintf(fp,
          "RUN_STARTED=%s\nRUN_BINARY=%s\nRUN_ORIGINAL=%s\nRUN_BINARY_SHA=%s\n"
          "RUN_PINNED=%s\nRUN_TARGET=%s\nRUN_TARGET_SRC=%s\nRUN_JOBS=%d\n"
          "RUN_BACKGROUND=%d\nRUN_SNAP_ID=\nRUN_TESTS_COLD=%d\nRUN_TESTS_SNAP=%d\n"
          "RUN_LOG=%s\n",
          stamp, g.pin, g.pin, g.binId, g.pin, g.tname, g.target, g.jobs, g.bg,
          g.cold.count, g.snap.count, g.log);
  fclose(fp);
}

/* ==================== eksekusi ==================== */

typedef struct {
  char name[256];
  char mode[16];
  long long ms, sleepMs, workMs;
  int pass, fail, pre;
} Row;

static Row rows[512];
static int nrows;

static void resetWork(const char *mode) {
  fsRemoveTree(g.work);
  copyTree(g.fix, g.work);
  if (strcmp(mode, "snap") == 0) {
    const char *art[] = {".satria", "bin", "lib", "build", "modules", "dist",
                         "compile_commands.json"};
    for (size_t i = 0; i < sizeof(art) / sizeof(art[0]); i++) {
      char src[XBUF], dst[XBUF];
      joinPath(src, sizeof(src), g.snapd, art[i]);
      if (!fsFileExists(src)) continue;
      joinPath(dst, sizeof(dst), g.work, art[i]);
      copyTree(src, dst);
    }
  }
}

static long long sleepTotal(void) {
  FILE *fp = fopen(g.sleepLog, "r");
  if (!fp) return 0;
  double total = 0;
  char name[256];
  double v;
  while (fscanf(fp, "%255s %lf", name, &v) == 2) total += v;
  fclose(fp);
  return (long long)(total * 1000.0);
}

static int countPrefix(const char *path, const char *prefix, size_t pl) {
  FILE *fp = fopen(path, "r");
  if (!fp) return 0;
  char line[8192];
  int n = 0;
  while (fgets(line, sizeof(line), fp))
    if (strncmp(line, prefix, pl) == 0) n++;
  fclose(fp);
  return n;
}

static void runTest(const char *name, const char *mode, bool dsl) {
  g.total++;
  long long epoch = (long long)(time(NULL)) * 1000;
  char ext[8] = ".sh";
  if (dsl) {
    char t[XBUF];
    snprintf(t, sizeof(t), "%s/common/%s.test", g.stage, name);
    copyStr(ext, sizeof(ext), fsFileExists(t) ? ".test" : ".t");
  }
  sayf("> Running %s%s [%s]... @%lld\n", name, ext, mode, epoch);

  long long t0 = (long long)portMonoMs();
  resetWork(mode);
  fsRemoveTree(g.tmpd);
  mkdirs(g.tmpd);
  FILE *sf = fopen(g.sleepLog, "w");
  if (sf) fclose(sf);

  char testPath[XBUF], logp[XBUF];
  joinPath(logp, sizeof(logp), g.logdir, name);
  snprintf(testPath, sizeof(testPath), "%s/common/%s%s", g.stage, name, ext);
  size_t ll = strlen(logp);
  snprintf(logp + ll, sizeof(logp) - ll, ".log");

  setenv("UNIT_TEST_NAME", name, 1);
  setenv("UNIT_CLASS", mode, 1);
  int rc;
  if (dsl) {
    char *argv[] = {g.pin, "test", testPath, NULL};
    rc = spawnCaptured(argv, g.work, logp);
  } else {
    char *argv[] = {"sh", testPath, g.wrap, g.fix, NULL};
    rc = spawnCaptured(argv, g.work, logp);
  }
  (void)rc;

  long long t1 = (long long)portMonoMs();
  teeLog(logp);
  long long ms = t1 - t0;
  long long sleepMs = sleepTotal();
  long long workMs = ms - sleepMs;
  if (workMs < 0) workMs = 0;

  int p = countPrefix(logp, "PASS", 4);
  int f = countPrefix(logp, "FAIL", 4);
  int pre = countPrefix(logp, "PRECONDITION", 12);
  g.pass += p;
  g.fail += f;
  g.pre += pre;

  char timing[XBUF];
  joinPath(timing, sizeof(timing), g.logdir, "timing.tsv");
  FILE *tf = fopen(timing, "a");
  if (tf) {
    fprintf(tf, "%s\t%s\t%lld\t%d\t%d\t%d\t%lld\t%lld\n", name, mode, ms, p, f, pre, sleepMs,
            workMs);
    fclose(tf);
  }
  if (nrows < (int)(sizeof(rows) / sizeof(rows[0]))) {
    Row *r = &rows[nrows++];
    copyStr(r->name, sizeof(r->name), name);
    copyStr(r->mode, sizeof(r->mode), mode);
    r->ms = ms;
    r->sleepMs = sleepMs;
    r->workMs = workMs;
    r->pass = p;
    r->fail = f;
    r->pre = pre;
  }
  sayf("> Selesai %s [%s] %lld.%03llds\n", name, mode, ms / 1000, ms % 1000);

  char result[300];
  if (f > 0)
    snprintf(result, sizeof(result), "FAIL %s (%d)", name, f);
  else if (pre > 0)
    snprintf(result, sizeof(result), "PRE %s (%d)", name, pre);
  else
    snprintf(result, sizeof(result), "OK   %s (%d)", name, p);
  listAdd(&g.results, result);
}

static int cmpWork(const void *a, const void *b) {
  const Row *x = a, *y = b;
  return (x->workMs < y->workMs) - (x->workMs > y->workMs);
}

static int runAll(void) {
  long long t0 = (long long)portMonoMs();
  for (int i = 0; i < g.cold.count; i++) {
    char testPath[XBUF];
    snprintf(testPath, sizeof(testPath), "%s/common/%s.test", g.stage, g.cold.items[i]);
    bool dsl = fsFileExists(testPath);
    if (!dsl) {
      snprintf(testPath, sizeof(testPath), "%s/common/%s.t", g.stage, g.cold.items[i]);
      dsl = fsFileExists(testPath);
    }
    runTest(g.cold.items[i], "cold", dsl);
  }
  for (int i = 0; i < g.snap.count; i++) {
    char testPath[XBUF];
    snprintf(testPath, sizeof(testPath), "%s/common/%s.test", g.stage, g.snap.items[i]);
    bool dsl = fsFileExists(testPath);
    if (!dsl) {
      snprintf(testPath, sizeof(testPath), "%s/common/%s.t", g.stage, g.snap.items[i]);
      dsl = fsFileExists(testPath);
    }
    runTest(g.snap.items[i], "snap", dsl);
  }

  long long elapsed = (long long)portMonoMs() - t0;
  /* RUN_ENDED/RUN_ELAPSED_MS: penanda selesai untuk watcher. */
  {
    char runenv[XBUF];
    joinPath(runenv, sizeof(runenv), g.logdir, "run.env");
    FILE *rf = fopen(runenv, "a");
    if (rf) {
      time_t now = time(NULL);
      struct tm tmv;
      gmtime_r(&now, &tmv);
      char stamp[64];
      strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tmv);
      fprintf(rf, "RUN_ENDED=%s\nRUN_ELAPSED_MS=%lld\n", stamp, elapsed);
      fclose(rf);
    }
  }
  sayf("==========================================\n");
  sayf("Ringkasan: PASS=%d FAIL=%d PRE=%d tes=%d target=%s\n", g.pass, g.fail, g.pre, g.total,
       g.tname);
  for (int i = 0; i < g.results.count; i++) sayf("  %s\n", g.results.items[i]);
  sayf("Waktu total : %lld.%03lld s\n", elapsed / 1000, elapsed % 1000);
  sayf("5 tes terlama (kerja tanpa tunggu):\n");
  Row sorted[512];
  int ns = nrows;
  memcpy(sorted, rows, (size_t)ns * sizeof(Row));
  qsort(sorted, (size_t)ns, sizeof(Row), cmpWork);
  for (int i = ns - 1, k = 0; i >= 0 && k < 5; i--, k++)
    sayf("  %-30s %-5s kerja %7.3f s  tunggu %6.3f s  total %7.3f s\n", sorted[i].name,
         sorted[i].mode, sorted[i].workMs / 1000.0, sorted[i].sleepMs / 1000.0,
         sorted[i].ms / 1000.0);
  char timing[XBUF];
  joinPath(timing, sizeof(timing), g.logdir, "timing.tsv");
  sayf("Timing : %s\n", timing);
  sayf("Log    : %s\n", g.log);
  return g.fail == 0 ? 0 : 1;
}

/* ==================== CLI ==================== */

static void absPath(const char *in, char *out, size_t n) {
  if (in[0] == '/') {
    copyStr(out, n, in);
    return;
  }
  char cwd[SATRIA_MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) die("tidak dapat membaca cwd");
  joinPath(out, n, cwd, in);
}

int testRunnerMain(int argc, const char *const *argv, const char *root, const char *bin) {
  const char *target = NULL, *optLog = NULL;
  List sel = {0};
  int jobs = 2;
  bool clean = false, bg = false, watch = false;
  for (int i = 0; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-j") == 0) {
      if (++i >= argc) die("-j memerlukan angka");
      jobs = atoi(argv[i]);
      if (jobs <= 0) die("-j harus bilangan bulat > 0");
    } else if (strncmp(a, "-j", 2) == 0 && a[2]) {
      jobs = atoi(a + 2);
      if (jobs <= 0) die("-j harus bilangan bulat > 0");
    } else if (strcmp(a, "-s") == 0 || strcmp(a, "--select") == 0) {
      if (++i >= argc) die("-s/--select memerlukan nama tes");
      listAddCsv(&sel, argv[i]);
    } else if (strcmp(a, "-o") == 0) {
      if (++i >= argc) die("-o memerlukan path log");
      optLog = argv[i];
    } else if (strcmp(a, "--clean") == 0) {
      clean = true;
    } else if (strcmp(a, "-b") == 0) {
      bg = true;
    } else if (strcmp(a, "-w") == 0 || strcmp(a, "--watch") == 0) {
      watch = true;
    } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      printf("pemakaian: satria test --run <target> [-j N] [-s tes[,tes...]] [-o log] "
             "[--clean] [-b] | -w [target]\n");
      listFree(&sel);
      return 0;
    } else if (a[0] == '-') {
      die("opsi tidak dikenal: %s", a);
    } else if (!target) {
      target = a;
    } else {
      die("argumen berlebih: %s", a);
    }
  }
  /* Watcher (M4d): read-only, target opsional. */
  if (watch) {
    if (bg) die("-w hanya menampilkan status; jangan digabung dengan -b");
    if (clean) die("-w dan --clean tidak bisa digabung");
    if (sel.count) die("-w tidak menerima -s/--select (watcher tidak memilih tes)");
    const char *stageEnv = getenv("UNIT_STAGE");
    char stagePath[SATRIA_MAX_PATH];
    copyStr(stagePath, sizeof(stagePath),
            (stageEnv && *stageEnv) ? stageEnv : "/tmp/satria-unit");
    listFree(&sel);
    return testWatchMain(target ? target : "", optLog ? optLog : "", stagePath);
  }
  if (!target) {
    listFree(&sel);
    die("perlu <target>");
  }
  if (!*target || strchr(target, '/') || target[0] == '.') die("nama target tidak valid: %s", target);
  if (!*bin || access(bin, X_OK) != 0)
    die("binary tidak ditemukan atau tidak dapat dieksekusi: %s", bin);

  memset(&g, 0, sizeof(g));
  copyStr(g.tname, sizeof(g.tname), target);
  copyStr(g.root, sizeof(g.root), root);
  joinPath(g.tests, sizeof(g.tests), g.root, "tests/unit");
  joinPath(g.target, sizeof(g.target), g.root, "tests");
  size_t tl = strlen(g.target);
  snprintf(g.target + tl, sizeof(g.target) - tl, "/%s", target);
  if (!fsDirExists(g.target)) die("target tidak ditemukan: tests/%s", target);

  const char *stageEnv = getenv("UNIT_STAGE");
  copyStr(g.stage, sizeof(g.stage), (stageEnv && *stageEnv) ? stageEnv : "/tmp/satria-unit");
  joinPath(g.t, sizeof(g.t), g.stage, "targets");
  tl = strlen(g.t);
  snprintf(g.t + tl, sizeof(g.t) - tl, "/%s", target);
  joinPath(g.logdir, sizeof(g.logdir), g.t, "log");
  if (clean) {
    fsRemoveTree(g.t);
    printf("dihapus: %s\n", g.t);
    listFree(&sel);
    return 0;
  }
  mkdirs(g.logdir);

  char self[XBUF];
  absPath(bin, self, sizeof(self));
  copyStr(g.pin, sizeof(g.pin), self);
  copyStr(g.log, sizeof(g.log), optLog ? optLog : "");
  if (!g.log[0]) joinPath(g.log, sizeof(g.log), g.logdir, "unit.log");
  mkparent(g.log);

  char pidPath[XBUF];
  joinPath(pidPath, sizeof(pidPath), g.logdir, "run.pid");
  pid_t oldPid;
  if (readPidFile(pidPath, &oldPid) && oldPid > 0 && oldPid != getpid() &&
      kill(oldPid, 0) == 0)
    die("run lain masih aktif pada target %s (PID %d). Hentikan dulu: kill %d", target,
        (int)oldPid, (int)oldPid);

  /* -b: parent memvalidasi lalu fork; anak menulis run.pid sendiri dan
     melepas terminal (setsid), parent mencetak PID lalu keluar 0. */
  if (bg && !getenv("UNIT_V1_CHILD")) {
    pid_t pid = fork();
    if (pid < 0) die("fork gagal: %s", strerror(errno));
    if (pid > 0) {
      char bgp[XBUF], txt[32];
      joinPath(bgp, sizeof(bgp), g.logdir, "bg.pid");
      snprintf(txt, sizeof(txt), "%d\n", (int)pid);
      writeText(bgp, txt);
      printf("Berjalan di background\n  PID : %d\n  Log : %s\n  Stop: kill %d\n", (int)pid,
             g.log, (int)pid);
      return 0;
    }
    setsid();
    signal(SIGHUP, SIG_IGN);
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      if (fd > 2) close(fd);
    }
    setenv("UNIT_V1_CHILD", "1", 1);
  }

  g_log = fopen(g.log, "w");
  if (!g_log) die("gagal membuka log %s", g.log);

  char pidTxt[32];
  snprintf(pidTxt, sizeof(pidTxt), "%d\n", (int)getpid());
  writeText(pidPath, pidTxt);
  if (bg) {
    copyStr(g_runPidPath, sizeof(g_runPidPath), pidPath);
    signal(SIGTERM, onBgSignal);
    signal(SIGINT, onBgSignal);
    signal(SIGHUP, onBgSignal);
  }

  g.jobs = jobs;
  g.bg = bg ? 1 : 0;
  snprintf(g.jobsArg, sizeof(g.jobsArg), "-j%d", jobs);
  joinPath(g.fix, sizeof(g.fix), g.t, "fixture");
  joinPath(g.snapd, sizeof(g.snapd), g.t, "snap");
  joinPath(g.work, sizeof(g.work), g.t, "work");
  joinPath(g.tmpd, sizeof(g.tmpd), g.t, "t");
  joinPath(g.disc, sizeof(g.disc), g.t, "discovery");

  setenv("UNIT_REPO", g.root, 1);
  setenv("UNIT_TMP", g.tmpd, 1);
  setenv("UNIT_BASE", g.t, 1);
  setenv("UNIT_FIXTURE", g.fix, 1);
  setenv("UNIT_DISCOVERY", g.disc, 1);
  setenv("UNIT_SLEEP_LOG", g.sleepLog, 1);
  char cmdTxt[XBUF];
  joinPath(cmdTxt, sizeof(cmdTxt), g.root, "src/cmd.txt");
  setenv("SATRIA_CMD", cmdTxt, 1);
  setenv("SATRIA", g.pin, 1);
  setenv("NO_COLOR", "1", 1);

  staging();
  setenv("SATRIA", g.pin, 1);
  validateClasses();
  selectTests(&sel);
  listFree(&sel);
  prepareFixture();
  uint64_t fixHash = treeContentStamp(g.fix);
  buildSnap(fixHash);
  discovery();
  writePlan();

  int rc = runAll();
  fsRemoveFile(pidPath);
  if (g_log) fclose(g_log);
  listFree(&g.cold);
  listFree(&g.snap);
  listFree(&g.results);
  return rc;
}

#endif /* _WIN32 */

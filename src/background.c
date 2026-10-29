/*
 * background.c — implementasi design/background.md (build di latar belakang
 * + watcher `satria watch`).
 *
 * Mekanisme inti (§2):
 *   1. `background = true` terdeteksi di Buildfile (single) atau Buildfile.ws
 *      (workspace) -> bgSpawn meluncurkan child proses memakai argv asli
 *      (design §2.1 Fork/Spawn) dengan env SATRIA_BG_CHILD.
 *   2. Child segera menulis PID-nya (design §2.2 PID Tracking); induk juga
 *      menulis PID hasil spawn supaya tidak ada jendela tanpa pengaman
 *      ganda-build setelah induk keluar.
 *   3. Redirection (§2.3): stdout+stderr child -> .satria/background.log,
 *      stdin -> /dev/null (NUL) — dikerjakan procSpawnDetached.
 *   4. Induk keluar rc 0 (§2.4); prompt terminal kembali.
 *   5. Child menutup run dengan .satria/background.status (success/failed)
 *      LALU menghapus PID (§4.3 Teardown) — urutan ini membuat "status ada"
 *      menjadi penanda final bagi watcher, walau PID sempat terdaur-ulang.
 *
 * Path di-absolutkan saat pertama dipakai (setelah chdir -f, sebelum
 * fsSetCwd(c.root) di dalam build) supaya perpindahan cwd di tengah build
 * tidak mengacaukan penulisan status/PID child.
 */
#include "background.h"
#include "startup.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "portability.h"
#include "util.h"

#define BG_MARKER "SATRIA_BG_CHILD"
#define BG_LOG ".satria/background.log"
#define BG_STATUS ".satria/background.status"
#define BG_PROGRESS ".satria/background.progress"
#define BG_STARTUP ".satria/background.startup"
#define BG_PID_SINGLE ".satria/build.pid"
#define BG_PID_WS ".satria/workspace.pid"

#define BG_MAX_ARGS 128
#define BG_ACT_MAX 512
#define BG_POLL_MS 250

/* ==================== state ==================== */

static const char *g_argv[BG_MAX_ARGS + 1]; /* salinan pointer argv, NULL-akhir */
static char g_abs0[MAX_PATH * 2];            /* argv[0] absolut (bila path relatif) */
static bool g_child = false;
static BgMode g_mode = BG_SINGLE;

/* ==================== path (di-absolutkan sekali) ==================== */

static void bgAbs(char *path, size_t n) {
  if (!path[0] || path[0] == '/' || path[0] == '\\' || path[1] == ':') return;
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  char buf[MAX_PATH * 2];
  snprintf(buf, sizeof(buf), "%s/%s", cwd, path);
  pathNormalizeSlash(buf);
  /* Tolak path yang tidak muat, jangan ambil versi yang sudah terpotong
     (PID/log/status akan ditulis ke lokasi yang salah). */
  size_t len = strlen(buf);
  if (len >= n) return;
  memcpy(path, buf, len + 1);
}

static const char *bgPidPath(BgMode mode) {
  static char path[2][MAX_PATH];
  static bool init[2];
  int i = mode == BG_WS ? 1 : 0;
  if (!init[i]) {
    snprintf(path[i], MAX_PATH, "%s", i ? BG_PID_WS : BG_PID_SINGLE);
    bgAbs(path[i], MAX_PATH);
    init[i] = true;
  }
  return path[i];
}

static const char *bgSimplePath(const char *rel) {
  /* Empat file non-PID; rel selalu konstan -> cache per pointer rel. */
  static const char *rels[4];
  static char vals[4][MAX_PATH];
  static int n;
  for (int i = 0; i < n; i++)
    if (rels[i] == rel) return vals[i];
  if (n >= 4) return rel;
  snprintf(vals[n], MAX_PATH, "%s", rel);
  bgAbs(vals[n], MAX_PATH);
  rels[n] = rel;
  return vals[n++];
}

static const char *bgLogPath(void) { return bgSimplePath(BG_LOG); }
static const char *bgStatusPath(void) { return bgSimplePath(BG_STATUS); }
static const char *bgProgressPath(void) { return bgSimplePath(BG_PROGRESS); }
static const char *bgStartupPath(void) { return bgSimplePath(BG_STARTUP); }

void bgStartupRecord(bool want) {
  const char *path = bgStartupPath();
  if (want && startupRecordHas()) {
    mkdirs(".satria");
    startupRecordSave(path);
    return;
  }
  /* Run tanpa startup (warm / fase tidak dijalankan): sisa rekaman run lama
     dibuang supaya `watch` tidak me-replay blok startup yang basi. */
  if (fsFileExists(path)) fsRemoveFile(path);
}

/* ==================== akses file kecil ==================== */

static bool bgReadPid(const char *path, int *out) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  int pid = 0;
  bool ok = fscanf(f, "%d", &pid) == 1 && pid > 0;
  fclose(f);
  if (ok) *out = pid;
  return ok;
}

static void bgWritePid(const char *path, int pid) {
  mkdirs(".satria");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "%d\n", pid);
  fclose(f);
}

/* Tulis progress ATOMIK: file unik per proses lalu fsRenameReplace — aman
   terhadap penulis paralel (anak fork wsRunBatch ikut menulis progress). */
static void bgWriteProgress(const char *path, int pct, const char *activity) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  char tmp[MAX_PATH + 32];
  snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)procSelfPid());
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  fprintf(f, "%d|%s\n", pct, activity);
  fclose(f);
  if (!fsRenameReplace(tmp, path)) fsRemoveFile(tmp);
}

/* ==================== init & identitas ==================== */

void bgNoteArgv(int argc, const char *argv[]) {
  /* Penanda child: baca SEKALI lalu buang — compiler children yang di-spawn
     build tidak boleh mewarisi penanda (nested satria tidak ikut Background). */
  const char *marker = getenv(BG_MARKER);
  if (marker && *marker && strcmp(marker, "0") != 0) g_child = true;
  procEnvSet(BG_MARKER, NULL);

  int n = argc;
  if (n < 0) n = 0;
  if (n > BG_MAX_ARGS) n = BG_MAX_ARGS;
  for (int i = 0; i < n; i++) g_argv[i] = argv[i];
  g_argv[n] = NULL;

  /* argv[0] path eksplisit (./bin/satria, bin\satria.exe): absolutkan SEBELUM
     bootstrapApply chdir -f, agar child tidak mencari binary relatif terhadap
     direktori Buildfile. Nama polos (PATH) dibiarkan apa adanya. */
  if (n > 0 && argv[0] && (strchr(argv[0], '/') || strchr(argv[0], '\\')) &&
      argv[0][0] != '/' && argv[0][0] != '\\' && argv[0][1] != ':') {
    char cwd[MAX_PATH];
    if (fsGetCwd(cwd, sizeof(cwd))) {
      snprintf(g_abs0, sizeof(g_abs0), "%s/%s", cwd, argv[0]);
      pathNormalizeSlash(g_abs0);
      g_argv[0] = g_abs0;
    }
  }
}

bool bgIsChild(void) { return g_child; }

/* ==================== gerbang (edge case §4.1 & §4.4) ==================== */

/* true bila ada build background yang masih berjalan; membersihkan berkas
   PID sisa (proses sudah mati) di sepanjang jalan. */
static bool bgOtherRunning(BgMode mode) {
  if (g_child) return false; /* diri sendiri: tidak diblokir */
  int pid = 0;
  if (bgReadPid(bgPidPath(mode), &pid) && procAlive(pid)) return true;
  if (fsFileExists(bgPidPath(mode))) fsRemoveFile(bgPidPath(mode));
  return false;
}

int bgBuildGate(BgMode mode) {
  int pid = 0;
  if (!bgOtherRunning(mode)) return -1;
  bgReadPid(bgPidPath(mode), &pid);
  fprintf(stderr,
          "Error: Background build is already running (PID: %d). "
          "Use 'satria watch' to monitor.\n",
          pid);
  return 2;
}

int bgCleanGate(BgMode mode) {
  int pid = 0;
  if (!bgOtherRunning(mode)) return -1;
  bgReadPid(bgPidPath(mode), &pid);
  fprintf(stderr,
          "satria: background build berjalan (PID: %d) — 'clean' diblokir\n"
          "        tunggu selesai (pantau dengan 'satria watch') atau\n"
          "        hentikan proses itu bila memang perlu\n",
          pid);
  return 2;
}

/* ==================== spawn (§2.1–§2.4) ==================== */

int bgSpawn(BgMode mode) {
  if (!g_argv[0]) {
    fprintf(stderr, "satria: background build gagal (argv tidak tersedia)\n");
    return 1;
  }
  mkdirs(".satria");
  /* Status lama dibuang di sini DAN di child — mencegah watcher membaca
     hasil build sebelumnya sebagai hasil run ini (§4.3). */
  fsRemoveFile(bgStatusPath());
  procEnvSet(BG_MARKER, "1");
  int pid = 0;
  bool ok = procSpawnDetached((char *const *)g_argv, bgLogPath(), &pid);
  procEnvSet(BG_MARKER, NULL);
  if (!ok) {
    fprintf(stderr, "satria: gagal menjalankan build di latar belakang\n");
    return 1;
  }
  bgWritePid(bgPidPath(mode), pid);
  return 0;
}

int bgMaybeSpawnSingle(const char *buildfile) {
  if (g_child) return -1;
  Config c = configDefaults();
  /* Read-only: parse/cached-read tanpa menulis .satria (cmdBuildEx yang
     akan menulis cache-nya sendiri). */
  if (!loadConfigEx(&c, buildfile, false) || !c.background) return -1;
  /* Child tidak punya TTY, jadi startup TIDAK dicetak di sini: barisnya
     hanya direkam (cold) untuk replay oleh `satria watch` — lihat startup.h. */
  bool cold = !fsFileExists(".satria/startup.state");
  startupBegin(false);
  startupPhase("loading configuration");
  startupPhaseEnd(true);
  bgStartupRecord(cold);
  return bgSpawn(BG_SINGLE);
}

void bgChildBegin(BgMode mode) {
  g_mode = mode;
  mkdirs(".satria");
  bgWritePid(bgPidPath(mode), procSelfPid());
  fsRemoveFile(bgStatusPath());
  fsRemoveFile(bgProgressPath());
  bgWriteProgress(bgProgressPath(), 0, "Starting");
}

void bgChildFinish(bool success) {
  if (!g_child) return;
  bgWriteProgress(bgProgressPath(), 100, success ? "Success" : "Failed");
  /* Status DULU, hapus PID SESUDAHNYA — "status ada" = penanda final. */
  FILE *f = fopen(bgStatusPath(), "w");
  if (f) {
    fputs(success ? "success" : "failed", f);
    fclose(f);
  }
  fsRemoveFile(bgPidPath(g_mode));
}

void bgProgress(int pct, const char *fmt, ...) {
  if (!g_child) return;
  char act[BG_ACT_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(act, sizeof(act), fmt, ap);
  va_end(ap);
  bgWriteProgress(bgProgressPath(), pct, act);
}

/* ==================== watcher (§3) ==================== */

/* Run masih berjalan? Status-final menang atas semua — lihat catatan urutan
   di bgChildFinish (PID bisa terdaur-ulang oleh OS setelah child keluar). */
static bool bgRunOngoing(BgMode mode) {
  if (fsFileExists(bgStatusPath())) return false;
  int pid = 0;
  if (!bgReadPid(bgPidPath(mode), &pid)) return false;
  return procAlive(pid);
}

/* Stream isi log dari `offset`; file ditimpa (size < offset) -> mulai dari 0.
   Return offset baru. */
static long long bgStreamFrom(const char *path, long long offset) {
  FILE *f = fopen(path, "rb");
  if (!f) return offset;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return offset;
  }
  long long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return offset;
  }
  if (size < offset) offset = 0;
  if (offset < size) {
    fseek(f, offset, SEEK_SET);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
      fwrite(buf, 1, n, stdout);
      offset += (long long)n;
    }
    fflush(stdout);
  }
  fclose(f);
  return offset;
}

/*
 * Cetak Build Summary (design §3B): blok "Summary :" TERAKHIR di log —
 * pola standar `> Summary :` yang dicetak satria di akhir pass build.
 * Tanpa blok summary (mis. pesan no-op): 20 baris terakhir log.
 */
static void bgPrintSummary(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return;
  }
  long long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return;
  }
  long long cap = size > 4 * 1024 * 1024 ? 4 * 1024 * 1024 : size;
  char *buf = malloc((size_t)cap + 1);
  if (!buf) {
    fclose(f);
    return;
  }
  fseek(f, size - cap, SEEK_SET);
  size_t got = fread(buf, 1, (size_t)cap, f);
  fclose(f);
  buf[got] = '\0';

  const char *out = NULL;
  char *found = NULL;
  for (char *p = buf; (p = strstr(p, "Summary :")) != NULL; p++) found = p;
  if (found) {
    out = found;
    while (out > buf && out[-1] != '\n') out--;
  } else if (got > 0) {
    out = buf;
    int lines = 0;
    for (size_t i = got; i-- > 0;) {
      if (buf[i] == '\n' && ++lines >= 20) {
        out = buf + i + 1;
        break;
      }
    }
  }
  if (out) {
    fputs(out, stdout);
    if (got > 0 && buf[got - 1] != '\n') fputc('\n', stdout);
    fflush(stdout);
  }
  free(buf);
}

/* Baca progress "pct|aktivitas"; true bila terbaca. */
static bool bgReadProgress(int *pct, char *act, size_t actN) {
  FILE *f = fopen(bgProgressPath(), "r");
  if (!f) return false;
  char line[BG_ACT_MAX + 32];
  bool ok = fgets(line, sizeof(line), f) != NULL;
  fclose(f);
  if (!ok) return false;
  char *bar = strchr(line, '|');
  if (!bar) return false;
  *bar = '\0';
  *pct = atoi(line);
  snprintf(act, actN, "%s", bar + 1);
  size_t len = strlen(act);
  while (len > 0 && (act[len - 1] == '\n' || act[len - 1] == '\r')) act[--len] = '\0';
  return true;
}

/* Replay blok startup yang direkam induk saat spawn cold (BG_STARTUP):
   satu-satunya tempat startup tampil untuk run background — induk tidak
   mencetaknya dan child tidak punya TTY. */
static void bgReplayStartup(void) {
  FILE *f = fopen(bgStartupPath(), "rb");
  if (!f) return;
  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  if (n == 0) return;
  fwrite(buf, 1, n, stdout);
  if (buf[n - 1] != '\n') fputc('\n', stdout);
  fflush(stdout);
}

/* Cetak status final + return rc keluar watcher (0 = success). */
static int bgPrintStatus(void) {
  char val[32] = {0};
  FILE *f = fopen(bgStatusPath(), "r");
  if (f) {
    if (!fgets(val, sizeof(val), f)) val[0] = '\0';
    fclose(f);
  }
  size_t len = strlen(val);
  while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r')) val[--len] = '\0';
  bool ok = strcmp(val, "success") == 0;
  printf("> status : %s\n", ok ? "success" : (val[0] ? val : "failed"));
  fflush(stdout);
  return ok ? 0 : 1;
}

int bgWatch(bool wsMode, bool full) {
  BgMode mode = wsMode ? BG_WS : BG_SINGLE;
  bool running = bgRunOngoing(mode);
  bool havePid = fsFileExists(bgPidPath(mode));

  if (!running) {
    if (havePid && !fsFileExists(bgStatusPath())) {
      /* Proses di PID itu sudah tidak ada & tak pernah menulis status. */
      fsRemoveFile(bgPidPath(mode));
      fprintf(stderr, "satria: build background berhenti tanpa status (crash? lihat %s)\n",
              bgLogPath());
      return 1;
    }
    if (!havePid && !fsFileExists(bgStatusPath())) {
      fprintf(stderr, "satria: tidak ada build background yang berjalan\n");
      return 1;
    }
    /* Selesai (PID hilang, status ada) ATAU tidak pernah ada: DITOLAK —
       watcher hanya melayani pekerjaan yang masih berjalan di background.
       Hasil akhir tetap tersimpan di background.log (ditimpa run berikutnya). */
    fprintf(stderr, "satria: tidak ada build background yang berjalan\n");
    return 1;
  }

  /* Berjalan (design §3B/§3C). Ctrl+C = default terminal: watcher mati,
     child TIDAK ikut (beda sesi, sudah di-detach) — detach alami §4.2. */
  if (termIsTTY()) {
    printf("[Tekan CTRL+C untuk detach dari watcher, build tetap berjalan di background]\n");
    fflush(stdout);
  }
  bgReplayStartup(); /* blok startup run ini (cold), sebelum baris progres */
  long long offset = 0;
  char last[BG_ACT_MAX + 32] = "";
  while (bgRunOngoing(mode)) {
    if (full) {
      offset = bgStreamFrom(bgLogPath(), offset);
    } else {
      int pct = 0;
      char act[BG_ACT_MAX];
      if (bgReadProgress(&pct, act, sizeof(act))) {
        char key[BG_ACT_MAX + 32];
        snprintf(key, sizeof(key), "%d|%s", pct, act);
        if (strcmp(key, last) != 0) {
          snprintf(last, sizeof(last), "%s", key);
          printf("> progress : %d%% (%s)\n", pct, act);
          fflush(stdout);
        }
      }
    }
    procSleepMs(BG_POLL_MS);
  }
  if (full)
    bgStreamFrom(bgLogPath(), offset);
  else
    bgPrintSummary(bgLogPath());
  return bgPrintStatus();
}

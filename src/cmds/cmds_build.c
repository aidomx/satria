/*
 * cmds_build — perintah build (cmdBuild/cmdCompdbGenerate/cmdBuildEx),
 * NaiveStatPlan, koleksi source, dan cek tabrakan object.
 * Dipisah dari commands.c (audit refactor: berkas kandidat >600 baris).
 */
#include "../commands.h"
#include "../cache/artifact.h"
#include "../cache/cache_store.h"
#include "../cache/router.h"
#include "../log/build_log.h"
#include "../log/build_report.h"
#include "../log/report_blob.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* trace audit cmdsCollectSources */
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#endif

#include "../cmdhelp.h"
#include "../background.h"
#include "../startup.h"
#include "cmds_internal.h"
#include "../compdb.h"
#include "../compile.h"
#include "../config.h"
#include "../deps.h"
#include "../embed.h"
#include "../pack/pack.h"
#include "../portability.h"
#include "../prof/prof.h"
#include "../uses/workspace.h"
#include "../util.h"
/* ==================== compdb -g (query-only) ==================== */

int cmdCompdbGenerate(const char *buildfilePath) {
  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) return 1;
  if (!resolveCompiler(&c)) return 1;

  if (!fsSetCwd(c.root)) {
    fprintf(stderr, "satria: cannot enter root '%s'\n", c.root);
    return 1;
  }

  /* Kemasan murni (output.binary = false) tidak punya compile step —
     tidak ada yang bisa direkam ke compile_commands.json. */
  if (!c.binary) {
    printf("%s CompDB  : skipped (output.binary = false)\n", repPrefix());
    return 0;
  }
  if (!compdbEnabled(&c)) {
    printf("%s CompDB  : disabled (output.compileCommands = false)\n", repPrefix());
    return 0;
  }

  List srcs = {0};
  listReserve(&srcs, 1024);
  for (int i = 0; i < c.sources.count; i++) {
    const char *entry = c.sources.items[i];
    size_t elen = strlen(entry);
    if (elen >= 2 &&
        (strcmp(entry + elen - 2, ".c") == 0 || strcmp(entry + elen - 2, ".cpp") == 0) &&
        fsFileExists(entry)) {
      listAdd(&srcs, entry);
      continue;
    }
    configWalkSources(&c, entry, &srcs, false); /* filter bahasa proyek */
  }
  listSort(&srcs); /* urutan entri json = urutan link (reprodusible) */

  if (srcs.count == 0) {
    fprintf(stderr,
            "satria: tidak ada source .c/.cpp di sources — tidak ada compdb untuk dibuat\n");
    return 1;
  }

  /* Cache hit / restore blob / render — mesin yang sama dengan jalur
     build (writeCompdb), tanpa decide/compile/link/fast-state. */
  writeCompdb(&c, &srcs);
  listFree(&srcs);
  return 0;
}

/* ==================== build ==================== */

/* Build penuh satu proyek — jalur satria satu-proyek & fase binary workspace. */
int cmdBuild(int jobs, const char *buildfilePath) {
  if (bgIsChild()) {
    /* Child background (design/background.md §2): tulis PID, jalankan
       pipeline normal, tutup run dengan status + hapus PID (§4.3). */
    bgChildBegin(BG_SINGLE);
    int rc = cmdBuildEx(jobs, buildfilePath, false);
    bgChildFinish(rc == 0);
    return rc;
  }
  int gate = bgBuildGate(BG_SINGLE); /* §4.1: tolak build ganda */
  if (gate >= 0) return gate;
  /* Keputusan spawn ada di cmdBuildEx SETELAH gerbang no-op (router /
     fast-state): run tanpa pekerjaan tidak boleh masuk jalur background. */
  return cmdBuildEx(jobs, buildfilePath, false);
}

/* ==================== [OPT] NaiveStatPlan (satu stat / file) ====================
 *
 * Audit bench compdb menunjukkan biaya dominan fase decide adalah stat
 * per file (mtime-ns) di lingkungan proot — bukan render compdb. Plan ini
 * mengumpulkan hasil stat SEKALI di awal (satu fsStampNsSize per file)
 * supaya fase lain tinggal memakai hasilnya tanpa men-stat ulang.
 *
 * Kontrol via env (default: satu stat / file — decide membaca plan):
 *   SATRIA_DECIDE_FROM_PLAN=0  decide men-stat src+objek sendiri (perilaku lama).
 *   SATRIA_STATPLAN_OBS=0      jangan stamp objek saat gather; decide men-stat
 *                            objek sendiri (perilaku lama).
 * Kedua env =0 bersama-sama memulihkan perilaku lama 100%.
 *
 * Guard jalur embedded/version header: emitEmbeddedHeader/
 * cmdsEmitVersionHeader berjalan SEBELUM decide dan bisa menghapus objek;
 * loop di cmdBuildEx meng-invalidasi slot plan.objM jadi -1 sehingga decide
 * melihat objek hilang dan tetap me-recompile (lihat cmdBuildEx).
 *
 *Konsumen aktif saat ini: decide. Konsumen masa depan:
 * keputusan link, PSA, restore compdb.
 */
/* StatPlan — tipe & API ada di cmds_internal.h (dipakai pass & finish). */

void statPlanInit(StatPlan *p) {
  memset(p, 0, sizeof(*p));
}

void statPlanFree(StatPlan *p) {
  free(p->path);
  free(p->srcM);
  free(p->objIdx);
  free(p->objPath);
  free(p->objM);
  statPlanInit(p);
}

/* true bila env diset dan bernilai "0" (opt-out eksplisit ke perilaku lama). */
bool envOff(const char *name) {
  const char *v = getenv(name);
  return v && strcmp(v, "0") == 0;
}

/* Stamp semua source sekali jalan; opsional (obsObj) stamp object unik.
   Dedup object pakai linear search: objCount kecil dan objectPathFor
   deterministik, jadi O(n^2) kecil ini masih jauh lebih murah daripada
   stat ganda pada FS lambat. */
/* Sumber yang SEHARUSNYA punya .d: proyek meminta -MMD/-MD lewat flags, bukan
   MSVC, dan berekstensi C/C++/Obj-C. Single-project tanpa flag itu tidak pernah
   menulis .d, jadi .d absen di sana adalah keadaan normal, bukan rusak. */
static bool statFlagsWantDotD(const Config *c) {
  for (int i = 0; i < c->flags.count; i++) {
    const char *f = c->flags.items[i];
    if (strcmp(f, "MMD") == 0 || strcmp(f, "-MMD") == 0 ||
        strcmp(f, "MD") == 0 || strcmp(f, "-MD") == 0)
      return true;
  }
  return false;
}

static bool statSrcHasDotD(const Config *c, const char *src) {
  if (compilerIsMSVC(c) || !statFlagsWantDotD(c)) return false;
  static const char *srcExt[] = {".c", ".C", ".cc", ".cpp", ".cxx", ".m", ".mm"};
  const char *dot = strrchr(src, '.');
  if (!dot) return false;
  for (size_t i = 0; i < sizeof(srcExt) / sizeof(srcExt[0]); i++)
    if (strcmp(dot, srcExt[i]) == 0) return true;
  return false;
}

typedef struct {
  const char *path;
  int64_t m;
  long long sz;
  bool ok;
} StatTask;

/* Hasil yang menyeberang pipe: layout tetap dan bebas pointer. */
typedef struct {
  int64_t m;
  long long sz;
  int ok;
} StatStamp;

#ifndef _WIN32
/* Sama seperti pre-stat P-fast workspace: bagi tugas ke beberapa child,
   masing-masing menulis array StatStamp untuk irisannya ke pipe. Parent
   membaca jumlah byte yang pasti, jadi child yang selesai lebih dulu tidak
   bisa menyisipkan hasil ke irisan lain. Gagal pipe/fork -> false dan
   pemanggil mengisi seluruh tugas secara serial. */
static bool statTasksFork(StatTask *tasks, size_t n, StatStamp *stamps) {
  int workers = cpuCount();
  if (workers < 2) return false;
  if (workers > 8) workers = 8;
  if ((size_t)workers > n) workers = (int)n;
  int pfd[8][2], made = 0;
  pid_t pid[8];
  size_t start[8], count[8];
  for (int w = 0; w < workers; w++) {
    if (pipe(pfd[w]) != 0) break;
    size_t s = n * (size_t)w / (size_t)workers;
    size_t e = n * (size_t)(w + 1) / (size_t)workers;
    pid_t p = fork();
    if (p < 0) {
      close(pfd[w][0]);
      close(pfd[w][1]);
      break;
    }
    if (p == 0) {
      close(pfd[w][0]);
      size_t bytes = (e - s) * sizeof(StatStamp);
      StatStamp *local = bytes ? malloc(bytes) : NULL;
      if (local) {
        for (size_t i = s; i < e; i++) {
          StatTask *t = &tasks[i];
          local[i - s].m = -1;
          local[i - s].sz = 0;
          local[i - s].ok = fsStampNsSize(t->path, &local[i - s].m, &local[i - s].sz);
          if (!local[i - s].ok) local[i - s].m = -1;
        }
        size_t off = 0;
        while (off < bytes) {
          ssize_t wr = write(pfd[w][1], (char *)local + off, bytes - off);
          if (wr > 0) {
            off += (size_t)wr;
            continue;
          }
          if (wr < 0 && errno == EINTR) continue;
          break;
        }
        free(local);
      }
      _exit(0);
    }
    close(pfd[w][1]);
    pid[w] = p;
    start[w] = s;
    count[w] = e - s;
    made++;
  }
  bool ok = made == workers;
  for (int w = 0; w < made; w++) {
    size_t bytes = count[w] * sizeof(StatStamp);
    size_t base = start[w] * sizeof(StatStamp), off = 0;
    while (off < bytes) {
      ssize_t rd = read(pfd[w][0], (char *)stamps + base + off, bytes - off);
      if (rd > 0) {
        off += (size_t)rd;
        continue;
      }
      if (rd < 0 && errno == EINTR) continue;
      break;
    }
    close(pfd[w][0]);
    int st = 0;
    while (waitpid(pid[w], &st, 0) < 0 && errno == EINTR) {}
    if (off != bytes || !WIFEXITED(st) || WEXITSTATUS(st) != 0) ok = false;
  }
  return ok;
}
#endif

/* Isi mtime/size semua tugas. Ambang 256 tugas (~85 source) membuat proyek
   kecil tetap serial; mulai 100 source biaya fork sudah terbayar di PRoot.
   SATRIA_STATPLAN_PAR=0 memaksa serial untuk debugging/perbandingan. */
static void statTasksRun(StatTask *tasks, size_t n) {
  if (n == 0) return;
  StatStamp *stamps = malloc(n * sizeof(*stamps));
  bool parallel = false;
#ifndef _WIN32
  if (stamps && n >= 256 && !envOff("SATRIA_STATPLAN_PAR"))
    parallel = statTasksFork(tasks, n, stamps);
#endif
  if (!parallel) {
    for (size_t i = 0; i < n; i++) {
      StatTask *t = &tasks[i];
      t->m = -1;
      t->sz = 0;
      t->ok = fsStampNsSize(t->path, &t->m, &t->sz);
      if (!t->ok) t->m = -1;
    }
    free(stamps);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    StatTask *t = &tasks[i];
    t->m = stamps[i].m;
    t->sz = stamps[i].sz;
    t->ok = stamps[i].ok != 0;
  }
  free(stamps);
}

void cmdsStatBatchStamp(const char *const *paths, int n, int64_t *mtime, long long *size,
                        unsigned char *ok) {
  if (n <= 0) return;
  StatTask *tasks = malloc(sizeof(*tasks) * (size_t)n);
  if (!tasks) {
    for (int i = 0; i < n; i++) {
      mtime[i] = -1;
      size[i] = 0;
      ok[i] = fsStampNsSize(paths[i], &mtime[i], &size[i]) ? 1 : 0;
      if (!ok[i]) mtime[i] = -1;
    }
    return;
  }
  for (int i = 0; i < n; i++) tasks[i].path = paths[i];
  statTasksRun(tasks, (size_t)n);
  for (int i = 0; i < n; i++) {
    mtime[i] = tasks[i].ok ? tasks[i].m : -1;
    size[i] = tasks[i].sz;
    ok[i] = tasks[i].ok ? 1 : 0;
  }
  free(tasks);
}

static bool statHasSuffix(const char *s, const char *suf) {
  size_t n = strlen(s), m = strlen(suf);
  return n >= m && strcmp(s + n - m, suf) == 0;
}

static void statDirOf(const char *path, char *out, size_t n) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *bslash = strrchr(path, '\\');
  if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
  if (!slash) snprintf(out, n, ".");
  else if (slash == path) snprintf(out, n, "/");
  else snprintf(out, n, "%.*s", (int)(slash - path), path);
}

static bool statSortedHas(const List *sorted, const char *path) {
  int lo = 0, hi = sorted->count - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    int c = strcmp(sorted->items[mid], path);
    if (c == 0) return true;
    if (c < 0) lo = mid + 1;
    else hi = mid - 1;
  }
  return false;
}

void statPlanBuild(StatPlan *p, const Config *c, const List *srcs) {
  int n = srcs->count;
  p->count = n;
  p->obsObj = !envOff("SATRIA_STATPLAN_OBS");
  /* +1 menghindari malloc(0) saat n==0 */
  p->path = malloc(sizeof(char *) * (size_t)(n + 1));
  p->srcM = malloc(sizeof(int64_t) * (size_t)(n + 1));
  p->objIdx = malloc(sizeof(int) * (size_t)(n + 1));
  p->objPath = malloc(sizeof(char[SATRIA_MAX_PATH]) * (size_t)(n + 1));
  p->objM = malloc(sizeof(int64_t) * (size_t)(n + 1));
  if (!p->path || !p->srcM || !p->objIdx || !p->objPath || !p->objM) {
    statPlanFree(p);
    return;
  }
  p->objCount = 0;

  /* Satu array tugas untuk seluruh stat: source (n), object unik, dan .d
     (hanya bila direktori object tidak seragam). Urutan tugas tidak
     memengaruhi hasil; tiap konsumen dipetakan lewat srcTask/objTask. */
  size_t cap = (size_t)n * 3 + 1;
  StatTask *tasks = malloc(cap * sizeof(*tasks));
  int *srcTask = malloc(sizeof(int) * (size_t)(n + 1));
  int *objTask = malloc(sizeof(int) * (size_t)(n + 1));
  int *dotdTask = malloc(sizeof(int) * (size_t)(n + 1));
  bool *dotdReq = calloc((size_t)(n + 1), sizeof(bool));
  bool *dotdAdded = calloc((size_t)(n + 1), sizeof(bool));
  size_t taskN = 0;
  if (!tasks || !srcTask || !objTask || !dotdTask || !dotdReq || !dotdAdded) {
    free(tasks);
    free(srcTask);
    free(objTask);
    free(dotdTask);
    free(dotdReq);
    free(dotdAdded);
    /* Alokasi gagal: kosongkan plan agar decide men-stat sendiri. */
    statPlanFree(p);
    return;
  }
  for (int i = 0; i < n; i++) {
    p->path[i] = srcs->items[i];
    srcTask[i] = (int)taskN;
    tasks[taskN].path = srcs->items[i];
    taskN++;

    char objPath[SATRIA_MAX_PATH];
    if (!objectPathFor(c, srcs->items[i], objPath, sizeof(objPath))) {
      p->objIdx[i] = -1;
      continue;
    }
    int slot = -1;
    for (int j = 0; j < p->objCount; j++) {
      if (strcmp(p->objPath[j], objPath) == 0) {
        slot = j;
        break;
      }
    }
    if (slot < 0) {
      slot = p->objCount;
      snprintf(p->objPath[slot], SATRIA_MAX_PATH, "%s", objPath);
      objTask[slot] = -1;
      dotdTask[slot] = -1;
      if (p->obsObj) {
        objTask[slot] = (int)taskN;
        tasks[taskN].path = p->objPath[slot];
        taskN++;
      } else {
        p->objM[slot] = -99; /* belum distat — konsumen WAJIB stat sendiri */
      }
      p->objCount++;
    }
    p->objIdx[i] = slot;
    if (p->obsObj && statSrcHasDotD(c, srcs->items[i])) dotdReq[slot] = true;
  }

  /* .d hanya perlu ADA/TIDAK. Bila semua object satu direktori (umumnya
     build/), satu readdir menggantikan n stat .d. Direktori campuran tetap
     memakai stat per .d seperti sebelumnya. */
  List dotdSeen = {0};
  bool dotdScan = false;
  if (p->obsObj && p->objCount > 0) {
    char dir[SATRIA_MAX_PATH];
    statDirOf(p->objPath[0], dir, sizeof(dir));
    bool oneDir = true;
    for (int slot = 1; slot < p->objCount; slot++) {
      char other[SATRIA_MAX_PATH];
      statDirOf(p->objPath[slot], other, sizeof(other));
      if (strcmp(dir, other) != 0) {
        oneDir = false;
        break;
      }
    }
    if (oneDir) {
      List files = {0};
      fsListDir(dir, NULL, &files);
      for (int i = 0; i < files.count; i++)
        if (statHasSuffix(files.items[i], ".d")) listAdd(&dotdSeen, files.items[i]);
      listFree(&files);
      listSort(&dotdSeen);
      dotdScan = true;
    }
  }
  if (p->obsObj && !dotdScan) {
    for (int i = 0; i < n; i++) {
      int slot = p->objIdx[i];
      if (slot < 0 || !dotdReq[slot] || dotdAdded[slot]) continue;
      dotdAdded[slot] = true;
      char dp[SATRIA_MAX_PATH * 2];
      if (dotDPathFor(c, srcs->items[i], dp, sizeof(dp))) {
        dotdTask[slot] = (int)taskN;
        tasks[taskN].path = strdup(dp);
        if (!tasks[taskN].path) dotdTask[slot] = -1;
        else taskN++;
      }
    }
  }

  statTasksRun(tasks, taskN);

  /* Kembalikan hasil ke plan dan memo per-proses. Memo source dipertahankan
     seperti jalur serial: save-walk sesudah pass membaca path yang sama.
     Object juga dicatat, lalu dibuang saat compile menulis ulang file. */
  for (int i = 0; i < n; i++) {
    StatTask *t = &tasks[srcTask[i]];
    p->srcM[i] = t->ok ? t->m : -1;
    if (t->ok) cmdsStatMemoPut(t->path, t->m, t->sz);
  }
  for (int slot = 0; slot < p->objCount; slot++) {
    if (!p->obsObj) continue;
    int64_t om = -1;
    if (objTask[slot] >= 0 && tasks[objTask[slot]].ok) {
      om = tasks[objTask[slot]].m;
      if (dotdTask[slot] >= 0) {
        if (!tasks[dotdTask[slot]].ok)
          om = -1;
        else
          cmdsStatMemoPut(tasks[dotdTask[slot]].path, tasks[dotdTask[slot]].m,
                          tasks[dotdTask[slot]].sz);
      } else if (dotdScan && dotdReq[slot]) {
        /* Nama .d = object dengan huruf terakhir 'o' -> 'd' (dotDPathFor). */
        char dp[SATRIA_MAX_PATH];
        snprintf(dp, sizeof(dp), "%s", p->objPath[slot]);
        size_t len = strlen(dp);
        if (len < 2 || dp[len - 2] != '.' || dp[len - 1] != 'o') {
          om = -1;
        } else {
          dp[len - 1] = 'd'; /* object.o -> object.d, baru dicocokkan ke dotdSeen */
          if (!statSortedHas(&dotdSeen, dp)) om = -1;
        }
      }
      cmdsStatMemoPut(tasks[objTask[slot]].path, tasks[objTask[slot]].m,
                      tasks[objTask[slot]].sz);
    }
    p->objM[slot] = om;
  }

  for (int slot = 0; slot < p->objCount; slot++)
    if (dotdTask[slot] >= 0) free((void *)tasks[dotdTask[slot]].path);
  listFree(&dotdSeen);
  free(tasks);
  free(srcTask);
  free(objTask);
  free(dotdTask);
  free(dotdReq);
  free(dotdAdded);
}

int cmdBuildEx(int jobs, const char *buildfilePath, bool libOnly) {
  profMark("cmd-entry");
  /* Sisa state lama (audit §Daftar Implementasi): mode single menyapu file
     tulisan versi lama sekali per run; mode workspace menyapu lewat
     wsLegacyStateRemove di workspaceRun. */
  if (!wsModeActive()) cmdsLegacyStateSweep();
  /* Fast no-op per fase (miniws): snapshot fase binary disimpan sebagai
     entri `build.state` di .satria/caches/fingerprint.cache, fase library di
     .satria/caches/lib.cache — no-op `satria -w` tidak membayar loadConfig +
     fingerprint + decide penuh per proyek. */
  bool fastOk = wsModeActive() ? wsFastStateValid(buildfilePath, libOnly)
                               : cmdsFastStateValid(buildfilePath, libOnly);
  profMark("fast-gate");
  if (fastOk) return 0; /* no-op: detail sudah dicetak fast-state */
  /* `background = true` (design/background.md §2): spawn HANYA bila build
     ini benar-benar mengerjakan sesuatu — run no-op (router opts.cache +
     fast-state) sudah keluar di atas dengan laporan no-op di terminal, tanpa
     child dan tanpa output kosong. Child (bgIsChild) melewati jalur ini. */
  if (!wsModeActive()) {
    /* Startup (cold) TIDAK dicetak induk — hanya direkam untuk `satria watch`
       (bgMaybeSpawnSingle / bgStartupRecord). */
    int start = bgMaybeSpawnSingle(buildfilePath);
    if (start >= 0) return start;
  }
  /* Build mengubah file. Mode single: memo dikosongkan total (aman, perilaku lama).
     Mode workspace: output dibuang per entri saat state disimpan, dan memo total
     dikosongkan bila build gagal (lihat wsExecOne), agar memo bisa dipakai lintas fase. */
  if (!wsModeActive()) cmdsStatMemoReset();
  /* Cold startup is defined by the absence of the persistent config state,
     not by missing build outputs. Keep warm startup silent. */
  bool coldStartup = !wsModeActive() && !fsFileExists(".satria/startup.state");
  bool showStartup = coldStartup && termIsTTY();
  startupBegin(showStartup); /* "startup :" bila tampil; fase di bawah */

  /* Detail laporan run (design/startup.md §10): tee stdout sejak awal; disimpan
     ke .satria/report.blob bila build sukses, dibuang bila gagal. */
  reportBlobBegin();
  buildReportBegin(); /* laporan aktif: dirender setelah pass (design/build_report.md) */
  buildReportSetCores(jobs);

  Config c = configDefaults();
  startupPhase("loading configuration");
  if (!loadConfig(&c, buildfilePath)) {
    startupPhaseEnd(false);
    reportBlobFinish(false);
    buildReportRelease();
    return 1;
  }
  startupPhase("preparing execution"); /* fase load selesai: done dicetak di sini */
  if (!resolveCompiler(&c)) {
    startupPhaseEnd(false);
    reportBlobFinish(false);
    buildReportRelease();
    return 1;
  }

  /* Toolchain efektif untuk header Build Report — format sama dengan baris
     "Build with" lama (warningFlags diakhiri spasi oleh joinFlags). */
  {
    char *wf = warningFlags(&c);
    char tc[512];
    snprintf(tc, sizeof(tc), "%s %s-std=%s", c.cc, wf ? wf : "", c.std);
    buildReportSetToolchain(tc);
    free(wf);
  }

  profMark("startup+config");

  /* Mode sinyal: build paralel selalu butuh handler forward SIGINT karena
     tiap compiler ada di process group sendiri (Ctrl+C dari terminal hanya
     sampai ke satria). Build serial mengikuti Buildfile: foreground. */
  procSetForeground(jobs > 1 ? false : c.foreground);

  if (!fsDirExists(c.root)) {
    startupPhaseEnd(false);
    fprintf(stderr, "satria: root '%s' is not a directory\n", c.root);
    reportBlobFinish(false);
    buildReportRelease();
    return 1;
  }
  if (!fsSetCwd(c.root)) {
    startupPhaseEnd(false);
    fprintf(stderr, "satria: cannot enter root '%s'\n", c.root);
    reportBlobFinish(false);
    buildReportRelease();
    return 1;
  }
  startupPhaseEnd(true);

  /* Router-centric untuk single-project (eksperimen P4): executor menulis
     cache grup lewat stage yang digabung proses ini sesudah pass, sehingga
     deps/lib/embed/fingerprint/opts kembali punya satu penulis.

     TIDAK aktif secara default: build single-project tidak punya penulis
     paralel, dan stage yang tidak digabung saat build gagal menghapus
     kemajuan compile parsial (fingerprint object yang sudah OK ikut hilang ->
     build berikutnya mengompilasi ulang semuanya, bertentangan dengan
     kontrak resume build). Jalur lama yang menyimpan state per object tetap
     dipakai; eksperimen ini dibuka eksplisit lewat SATRIA_WS_STAGE_SINGLE=1. */
  bool txn = false;
  const char *stageSingle = getenv("SATRIA_WS_STAGE_SINGLE");
  if (!wsModeActive() && workspaceRouterPilotActive() && stageSingle && *stageSingle &&
      strcmp(stageSingle, "0") != 0) {
    txn = csStageBegin();
    if (txn) csStageSetProjectAddr(cmdsCacheProjAddr());
  }

  bool logging = !wsModeActive() && c.logEnabled;
  if (logging) buildLogBegin();
  /* Dua pass eksplisit: compile dulu, lalu link sebagai keputusan router
     (untuk single-project, proses ini adalah router dan executor). */
  CmdsPassTail *tail = NULL;
  int result = cmdsBuildPassCompile(jobs, c, buildfilePath, libOnly, &tail);
  if (result == 0 && tail) result = cmdsLinkPass(tail);
  /* Satu penulis: merge stage + terapkan intent hanya setelah executor
     selesai. Build gagal tidak menggabungkan apa pun (target lama utuh). */
  if (txn) {
    if (result == 0 &&
        (!csStageMergeAll() || !optsRouterApplyIntents()))
      result = 1;
    csStageEnd();
  }
  if (logging) buildLogEnd(result);
  buildReportFinish(result == 0); /* render Build Report (live stream compile dipertahankan) */
  /* sukses: jadikan report.blob dan salin ke state (pulih bila file hilang) */
  if (reportBlobFinish(result == 0) && result == 0) fastReportPut();
  if (result == 0) artifactCacheSave(); /* metadata artefak (design/startup.md §9) */
  buildReportRelease(); /* record dipakai utk kondensasi blob — lepas di sini */
  return result;
}

/* Pasangan (object, sumber) untuk deteksi tabrakan: diurutkan per object. */
typedef struct {
  char obj[SATRIA_MAX_PATH];
  const char *src;
} ObjEntry;

static int objEntryCmp(const void *a, const void *b) {
  return strcmp(((const ObjEntry *)a)->obj, ((const ObjEntry *)b)->obj);
}

bool cmdsCheckObjectCollisions(const Config *c, const List *srcs) {
  if (srcs->count < 2) return true;
  ObjEntry *e = calloc((size_t)srcs->count, sizeof(ObjEntry));
  if (!e) return true; /* kehabisan memori: tidak ada perbandingan; build lanjut */
  int m = 0;
  for (int i = 0; i < srcs->count; i++) {
    const char *src = srcs->items[i];
    if (!objectPathFor(c, src, e[m].obj, sizeof(e[m].obj))) continue;
    e[m].src = src;
    m++;
  }
  qsort(e, (size_t)m, sizeof(ObjEntry), objEntryCmp);
  bool ok = true;
  for (int i = 1; i < m; i++) {
    if (strcmp(e[i].obj, e[i - 1].obj) != 0) continue;
    fprintf(stderr,
            "satria: tabrakan object: '%s' dan '%s' sama-sama -> %s\n"
            "        ubah nama salah satu file atau pindahkan dari root sumber yang sama\n",
            e[i - 1].src, e[i].src, e[i].obj);
    ok = false;
  }
  free(e);
  return ok;
}

/*
 * cmdsCollectSources — koleksi source .c/.cpp dari Config: entri FILE
 * (mis. hasil `satria -xf` untuk build.ninja flat) masuk langsung; selain
 * itu dianggap folder dan discan rekursif (walk cache .satria/sources.cache bila
 * mode workspace aktif — lihat wsModeSet). Urutan hasil = urutan link:
 * .c dan .cpp dihasilkan dua walkDir terpisah per folder root, jadi di-sort
 * ulang jadi satu barisan stabil — baris link reprodusible antar mesin dan
 * urutan inisialisasi object statis C++ tidak melekat pada readdir.
 * Dipakai cmdsBuildPassCompile dan jalur CompDB workspace (wsCompdbRefresh).
 */
void cmdsCollectSources(const Config *c, List *out) {
  /* Trace audit (SATRIA_PROFILE): durasi per entri sources — col timestamp
     diselaraskan dengan [walk] (keduanya ms absolut dari portMonoMs,
     wrapper monotonic portabel: MSVC tanpa CLOCK_MONOTONIC). */
  bool trace = profOn();
  double tc0 = 0;
  if (trace) {
    stateSourcesCacheLoad(); /* muat cache di luar hitungan [collect] pertama */
    tc0 = portMonoMs();
  }
  listReserve(out, 1024);
  for (int i = 0; i < c->sources.count; i++) {
    const char *entry = c->sources.items[i];
    size_t elen = strlen(entry);
    if (elen >= 2 &&
        (strcmp(entry + elen - 2, ".c") == 0 || strcmp(entry + elen - 2, ".cpp") == 0) &&
        fsFileExists(entry)) {
      listAdd(out, entry);
      if (trace) {
        double tc1 = portMonoMs();
        fprintf(stderr, "[collect] t=%.0f file=%s\n", tc1, entry);
      }
      continue;
    }
    /* Filter bahasa proyek: `use project c` mengabaikan .cpp di tree. */
    configWalkSources(c, entry, out, true);
    if (trace) {
      double tc1 = portMonoMs();
      fprintf(stderr, "[collect] t=%.0f dir=%s %.2f ms\n", tc1, entry, tc1 - tc0);
      tc0 = portMonoMs();
    }
  }
  /* Source embed hasil codegen: berada di outBuildDir (bukan root sumber),
     jadi ditambahkan eksplisit — ikut fingerprint, decide, compdb, link,
     dan kemasan library seperti source biasa (design/embedded.md §Pipeline).
     Hanya proyek binary: proyek kemasan mengarsipkan, bukan mengcompile. */
  if (c->binary && c->embCount > 0) {
    char emb[SATRIA_MAX_PATH + 32];
    snprintf(emb, sizeof(emb), "%s/embedded.c", c->outBuildDir);
    bool dup = false;
    for (int i = 0; i < out->count; i++) {
      if (strcmp(out->items[i], emb) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup) listAdd(out, emb);
  }
  listSort(out);
}


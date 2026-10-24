/* ws_startup.c — Startup workspace: output fase, skeleton direktori, dan pemuatan model (cold/warm).
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"

static void wsStartupFinish(bool ok);
static void wsStartupPhase(const char *msg);
static void wsStartupPhaseEnd(bool ok);
static void wsStartupPrepare(const char *wsDir);

/* ---- output startup (design/startup.md §8) ----
   Setiap fase yang selesai DIPERTAHANKAN sebagai baris "startup: <fase> done"
   dengan newline — informasi startup tetap terlihat setelah cold build.
   Hanya ke stderr bila stdout interaktif (termIsTTY): output ter-capture
   (test/CI, pipe) tetap bersih; wsGuardQuiet (deteksi anggota) ikut membungkam. */
static const char *wsStartupCur = NULL;
static void wsStartupFinish(bool ok) {
  if (!wsStartupCur) return;
  if (!wsGuardQuiet && termIsTTY()) {
    fprintf(stderr, "> %s: %s\n", wsStartupCur, ok ? "done" : "failed");
    fflush(stderr);
  }
  wsStartupCur = NULL;
}
/* Mulai fase baru; fase sebelumnya otomatis selesai (done). Fase tidak
   dicetak saat mulai, hanya saat selesai, supaya pesan dari parser (mis.
   peringatan Buildfile) tidak menimpa baris progres. */
static void wsStartupPhase(const char *msg) {
  wsStartupFinish(true);
  wsStartupCur = msg;
}
/* Akhiri fase terakhir: ok -> "done", gagal -> "failed". */
static void wsStartupPhaseEnd(bool ok) { wsStartupFinish(ok); }
/* Skeleton direktori startup (design/startup.md): dibuat SEKALI pada clean
   cold. `.satria` memuat startup.state, jadi state yang valid otomatis berarti
   `.satria` ada — jalur build tidak perlu mkdir `.satria` lagi. Direktori
   output (bin/ build/ lib/ modules/) ikut disiapkan di sini; mkdirs idempoten. */
static void wsStartupPrepare(const char *wsDir) {
  static const char *const dirs[] = {".satria", "bin", "build", "lib", "modules"};
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    char p[WS_PATH_LEN];
    snprintf(p, sizeof(p), "%s/%s", wsDir, dirs[i]);
    mkdirs(p);
  }
}
/* Muat model workspace + validasi dasar yang sama untuk semua mode (build,
   release, profile workspace=): state startup bila valid, kalau tidak parse
   Buildfile.ws — lalu minimal satu proyek dan validasi pack merge.
   cacheWrite=false (jalur baca-saja: profile/enumerasi) membaca cache yang
   valid tapi TIDAK menulisnya dan tidak mencetak fase startup.
   false = pesan error sudah dicetak dan model sudah dibebaskan; pemanggil
   tidak boleh memakai model. Validasi release & topo-sort tetap di
   pemanggil: urutannya berbeda antar mode (workspaceRun memvalidasi topo
   sebelum override release; workspaceEnumerate sebaliknya) dan keduanya
   membutuhkan model yang sudah dimuat. */
bool wsModelLoad(const char *wsDir, WsModel *m, const char **wsNameOut,
                        bool cacheWrite) {
  const char *wsName = workspaceFileName();
  if (!wsName) {
    fprintf(stderr, "satria: %s not found\n", WORKSPACE_FILENAME);
    return false;
  }
  if (wsNameOut) *wsNameOut = wsName;
  char wsFile[WS_PATH_LEN];
  snprintf(wsFile, sizeof(wsFile), "%s/%s", wsDir, wsName);
  char cacheFile[WS_PATH_LEN];
  snprintf(cacheFile, sizeof(cacheFile), "%s/%s", wsDir, WS_STARTUP_FILE);

  /* startUp() (design/startup.md §2): load bila valid, parse bila tidak.
     Startup penuh (fase + skeleton direktori) HANYA pada clean cold — state
     belum ada (clean pertama atau `rm -rf .satria`). Kondisi lain diam:
     no-op tidak mencetak apa pun, dan Buildfile yang berubah cukup
     memperbarui startup.state tanpa menjalankan ulang startup. */
  bool coldStart = cacheWrite && !fsFileExists(cacheFile);
  if (coldStart) {
    if (!wsGuardQuiet && termIsTTY()) fprintf(stderr, "startup :\n");
    wsStartupPrepare(wsDir);
    wsStartupPhase("discovery");
  }
  bool restored = wsStartupLoad(m, wsFile, wsName, cacheFile);
  if (!restored) {
    if (coldStart) wsStartupPhase("resolving dependencies");
    if (!wsParse(m, wsFile)) {
      if (coldStart) wsStartupPhaseEnd(false);
      wsModelFree(m);
      return false;
    }
  }
  if (m->projectCount == 0) {
    if (coldStart) wsStartupPhaseEnd(false);
    fprintf(stderr, "satria: workspace: no projects declared\n");
    wsModelFree(m);
    return false;
  }
  if (!wsValidatePackMerges(m)) {
    if (coldStart) wsStartupPhaseEnd(false);
    wsModelFree(m);
    return false;
  }
  if (cacheWrite) {
    if (!restored) {
      if (coldStart) wsStartupPhase("indexing sources");
      wsStartupSave(m, wsFile, wsName, cacheFile);
    }
    if (coldStart) {
      wsStartupPhase("preparing execution");
      wsStartupPhaseEnd(true);
    }
  }
  return true;
}

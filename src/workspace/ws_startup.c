/* ws_startup.c — Startup workspace: output fase, skeleton direktori, dan pemuatan model (cold/warm).
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"
#include "../startup.h"

static void wsStartupPrepare(const char *wsDir);

/* Pratinjau `background = true` di Buildfile.ws — dipanggil SEBELUM
 * wsModelLoad. Fase startup dicetak SAAT model dimuat, sedangkan keputusan
 * spawn baru diketahui belakangan (setelah gerbang no-op); tanpa pratinjau
 * ini run background masih akan mencetak startup ke terminal. Semantik
 * mengikuti wsCommonBool: key root-level, nilai persis "true", komentar
 * dibuang, keputusan terakhir menang. */
bool wsBackgroundPeek(const char *wsDir) {
  const char *wsName = workspaceFileName();
  if (!wsName) return false;
  char wsFile[WS_PATH_LEN];
  snprintf(wsFile, sizeof(wsFile), "%s/%s", wsDir, wsName);
  FILE *fp = fopen(wsFile, "r");
  if (!fp) return false;
  bool on = false;
  char line[WS_LINE_LEN];
  while (fgets(line, sizeof(line), fp)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    char *slash = strstr(line, "//");
    if (slash) *slash = '\0';
    char key[64];
    char *value = NULL;
    if (!wsSplitKV(line, key, sizeof(key), &value)) continue;
    if (strcmp(key, "background") == 0) on = strcmp(value, "true") == 0;
  }
  fclose(fp);
  return on;
}

/* Output fase startup: lihat src/startup.h (dipakai bersama single-project). */
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
   membutuhkan model yang sudah dimuat.
   spawnPending = run ini menuju spawn background (lihat wsBackgroundPeek):
   fase startup TIDAK dicetak, hanya direkam untuk replay `satria watch`. */
bool wsModelLoad(const char *wsDir, WsModel *m, const char **wsNameOut,
                        bool cacheWrite, bool spawnPending) {
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
    /* Spawn: child tidak punya TTY dan induk langsung keluar — startup cukup
       direkam (bgStartupRecord di jalur spawn), tidak dicetak di terminal. */
    startupBegin(spawnPending ? false : startupVisible(wsGuardQuiet));
    wsStartupPrepare(wsDir);
    startupPhase("discovery");
  }
  bool restored = wsStartupLoad(m, wsFile, wsName, cacheFile);
  if (!restored) {
    if (coldStart) startupPhase("resolving dependencies");
    if (!wsParse(m, wsFile)) {
      if (coldStart) startupPhaseEnd(false);
      wsModelFree(m);
      return false;
    }
  }
  if (m->projectCount == 0) {
    if (coldStart) startupPhaseEnd(false);
    fprintf(stderr, "satria: workspace: no projects declared\n");
    wsModelFree(m);
    return false;
  }
  if (!wsValidatePackMerges(m)) {
    if (coldStart) startupPhaseEnd(false);
    wsModelFree(m);
    return false;
  }
  if (cacheWrite) {
    if (!restored) {
      if (coldStart) startupPhase("indexing sources");
      wsStartupSave(m, wsFile, wsName, cacheFile);
    }
    if (coldStart) {
      startupPhase("preparing execution");
      startupPhaseEnd(true);
    }
  }
  return true;
}

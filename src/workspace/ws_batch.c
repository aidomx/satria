/* ws_batch.c  Dispatcher batch workspace. Eksekusi per runner ada di
 * src/runners/ (serial: realtime, default; parallel: child background).
 * Lihat runners/runners.h dan komentar modul di runners/parallel.c. */
#include "ws_internal.h"
#include "../runners/runners.h"
#include "../cache/router.h"


/* Jalankan batch anak build: runner dipilih oleh runnerWantParallel. */
int wsRunBatch(const WsModel *m, const WsWork *batch, int n, int jobs, const char *cmd,
               const char *wsDir, bool libOnly, bool compileOnly) {
  if (n <= 0) return 0;
  /* Fase 2 router-centric: executor (anak fork maupun serial) men-stage
     tulisan cache grup. Induk/runner menggabungkan per grup; serial melakukan
     merge setelah tiap proyek agar proyek berikutnya membaca state terbaru. */
  /* Stage hanya perlu saat ada penulis paralel. Runner serial menulis cache
     langsung seperti jalur non-plan; merge per proyek di runner serial mahal
     (rewrite fingerprint/lib cache) dan pada 10k source menambah 5-6 detik. */
  bool parallel = runnerWantParallel(n, jobs);
  bool txn = wsPlanActive() && parallel && csStageBegin();
  int rc = parallel
               ? runnerParallel(m, batch, n, jobs, cmd, wsDir, libOnly, compileOnly)
               : runnerSerial(m, batch, n, jobs, cmd, wsDir, libOnly, compileOnly);
  if (txn) {
    if (!csStageMergeAll() && rc == 0) rc = 1;
    if (!optsRouterApplyIntents() && rc == 0) rc = 1;
    csStageEnd();
  }
  return rc;
}

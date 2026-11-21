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
  bool txn = wsPlanActive() && csStageBegin();
  int rc = runnerWantParallel(n, jobs)
               ? runnerParallel(m, batch, n, jobs, cmd, wsDir, libOnly, compileOnly)
               : runnerSerial(m, batch, n, jobs, cmd, wsDir, libOnly, compileOnly);
  if (txn) {
    if (!csStageMergeAll() && rc == 0) rc = 1;
    if (!optsRouterApplyIntents() && rc == 0) rc = 1;
    csStageEnd();
  }
  return rc;
}

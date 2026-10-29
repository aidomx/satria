/* ws_batch.c  Dispatcher batch workspace. Eksekusi per runner ada di
 * src/runners/ (serial: realtime, default; parallel: child background).
 * Lihat runners/runners.h dan komentar modul di runners/parallel.c. */
#include "ws_internal.h"
#include "../runners/runners.h"

/* Jalankan batch anak build: runner dipilih oleh runnerWantParallel. */
int wsRunBatch(const WsModel *m, const WsWork *batch, int n, int jobs, const char *cmd,
               const char *wsDir, bool libOnly) {
  if (n <= 0) return 0;
  if (runnerWantParallel(n, jobs))
    return runnerParallel(m, batch, n, jobs, cmd, wsDir, libOnly);
  return runnerSerial(m, batch, n, jobs, cmd, wsDir, libOnly);
}

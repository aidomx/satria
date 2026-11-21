#pragma once
/* runners.h  Runner batch workspace. Satu fungsi per strategi; pemilihan
 * di runnerWantParallel (runners.c). Dipanggil oleh wsRunBatch (ws_batch.c).
 *
 *   runnerSerial   : urut worklist, realtime (default, foreground).
 *   runnerParallel : fork dep-ready (POSIX); di Windows memanggil serial.
 */
#include <stdbool.h>
#include "../workspace/ws_internal.h"

int runnerSerial(const WsModel *m, const WsWork *batch, int n, int jobs,
                 const char *cmd, const char *wsDir, bool libOnly, bool compileOnly);

int runnerParallel(const WsModel *m, const WsWork *batch, int n, int jobs,
                   const char *cmd, const char *wsDir, bool libOnly, bool compileOnly);

/* true bila batch ini sebaiknya dijalankan paralel (kebijakan di runners.c). */
bool runnerWantParallel(int n, int jobs);

/* true bila output proyek boleh dicetak begitu proyek selesai (bukan urut
   worklist). Hanya aktif saat paralel dipaksa lewat SATRIA_WS_PAR, supaya
   foreground yang dipaksa paralel tidak diam sampai batch selesai. */
bool runnerStreamOutput(void);

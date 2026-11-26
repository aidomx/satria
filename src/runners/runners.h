#pragma once
/* runners.h  Runner batch workspace. Satu fungsi per strategi; pemilihan
 * di runnerWantParallel (runners.c). Dipanggil oleh wsRunBatch (ws_batch.c).
 *
 *   runnerSerial   : urut worklist, realtime (SATRIA_WS_PAR=0).
 *   runnerParallel : fork dep-ready (POSIX, default n>1 & jobs>1); di Windows
 *                    memanggil serial. Output urut worklist untuk berkas
 *                    reguler; stdout pipe/soket = live (lihat
 *                    runnerStreamOutput).
 */
#include <stdbool.h>
#include "../workspace/ws_internal.h"

int runnerSerial(const WsModel *m, const WsWork *batch, int n, int jobs,
                 const char *cmd, const char *wsDir, bool libOnly, bool compileOnly);

int runnerParallel(const WsModel *m, const WsWork *batch, int n, int jobs,
                   const char *cmd, const char *wsDir, bool libOnly, bool compileOnly);

/* true bila batch ini sebaiknya dijalankan paralel (kebijakan di runners.c). */
bool runnerWantParallel(int n, int jobs);

/* true bila output proyek harus dicetak SAAT BERJALAN (bukan ditahan di file
   slot lalu dicetak urut worklist): anak fork menulis langsung ke stdout,
   line-buffered. Aktif bila stdout pipe/soket (konsumen membaca sambil build
   berjalan, mis. `satria | satria -w`) atau SATRIA_WS_PAR=1. Redirect ke
   berkas reguler tetap urut worklist agar log unit test/CI deterministik. */
bool runnerStreamOutput(void);

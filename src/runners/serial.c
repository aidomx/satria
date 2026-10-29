/* runners/serial.c  Runner serial batch workspace: realtime, urut worklist.
 * Lihat runners.h. Dipakai foreground dan sebagai fallback runnerParallel. */
#include "runners.h"

#include "../prof/prof.h"

/* Serial: penanda fase per anak, henti saat gagal — persis perilaku lama. */
int runnerSerial(const WsModel *m, const WsWork *batch, int n, int jobs,
                            const char *cmd, const char *wsDir, bool libOnly) {
  (void)wsDir; /* proyek dieksekusi in-process dari root workspace */
  const char *pre = libOnly ? "ws-libpass-pre" : "ws-binpass-pre";
  const char *post = libOnly ? "ws-libpass-post" : "ws-binpass-post";
  /* FASE library: per tingkat dependensi (identik batch-per-level lama);
     FASE binary: satu run datar atas worklist (topo). */
  int maxLevel = 0;
  if (libOnly)
    for (int i = 0; i < n; i++)
      if (batch[i].level > maxLevel) maxLevel = batch[i].level;
  for (int lv = 0; lv <= maxLevel; lv++) {
    for (int i = 0; i < n; i++) {
      if (libOnly && batch[i].level != lv) continue;
      profMark(pre);
      int rc = wsRunProject(m, batch[i].name, cmd, jobs, libOnly);
      profMark(post);
      if (rc != 0) return rc;
    }
  }
  return 0;
}

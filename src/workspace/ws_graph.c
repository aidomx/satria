/* ws_graph.c — Graf dependensi: dependensi pack lintas proyek dan topological sort depends_on.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"

static void wsCollectPackDeps(WsModel *m);
static bool wsVisit(WsModel *m, int idx, unsigned char *color, List *order);

/* ==================== dependensi pack lintas proyek ==================== */
/* Pack build selektif (-w <proyek>): entri pack.files yang menunjuk
   artefak proyek lain (src berprefix ../<nama>/... ) bermakna "hasil
   build proyek lain". Tanpa derekaman ini, build selektif tidak
   membangun proyek pemilik artefak itu, padahal entri berpath menuntut
   file jadi. Hasil: depends[] proyek pemilik pack.files bertambah
   (dedup — entri yang sudah ada tidak ditulis ulang). */
static void wsCollectPackDeps(WsModel *m) {
  for (int i = 0; i < m->projectCount; i++) {
    for (int j = 0; j < m->projects[i].lines.count; j++) {
      const char *ln = m->projects[i].lines.items[j];
      if (strncmp(ln, "pack.files = ", 13) != 0) continue;
      const char *v = ln + 13;
      char buf[WS_LINE_LEN * 2];
      if (strlen(v) >= sizeof(buf)) continue;
      copyStr(buf, sizeof(buf), v);
      for (char *save = buf;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        /* src:dst — src bagian kiri; hanya src berprefix ../ yang lintas
           proyek (mis. arsip hasil pack proyek lain). */
        char *colon = strchr(save, ':');
        if (colon) *colon = '\0';
        if (strncmp(save, "../", 3) == 0) {
          char *seg = save + 3;
          char *slash = strchr(seg, '/');
          if (slash && slash != seg) {
            *slash = '\0';
            int dep = wsFindProject(m, seg);
            if (dep >= 0 && dep != i) {
              bool exists = false;
              for (int k = 0; k < m->depends[i].count; k++)
                if (strcmp(m->depends[i].items[k], seg) == 0) exists = true;
              if (!exists) listAdd(&m->depends[i], seg);
            }
          }
        }
        if (!comma) break;
        save = comma + 1;
      }
    }
  }
}
/* ==================== topo sort depends_on ==================== */
/* DFS berwarna (0 putih, 1 abu, 2 hitam): siklus terdeteksi pasti, dan
   urutan post-order menjamin dependency dibangun duluan. */
static bool wsVisit(WsModel *m, int idx, unsigned char *color, List *order) {
  if (color[idx] == 2) return true;
  if (color[idx] == 1) {
    fprintf(stderr, "satria: workspace: dependency cycle involving '%s'\n", m->projects[idx].name);
    return false;
  }
  color[idx] = 1;
  for (int i = 0; i < m->depends[idx].count; i++) {
    int dep = wsFindProject(m, m->depends[idx].items[i]);
    if (dep < 0) {
      fprintf(stderr, "satria: workspace: '%s' depends on unknown project '%s'\n",
              m->projects[idx].name, m->depends[idx].items[i]);
      return false;
    }
    if (!wsVisit(m, dep, color, order)) return false;
  }
  color[idx] = 2;
  listAdd(order, m->projects[idx].name);
  return true;
}
bool wsTopoSort(WsModel *m) {
  /* Kumpulkan depends_on dari baris per proyek yang tersimpan
     ("depends_on = ..." setelah prefix projects.<nama>. di-strip). */
  for (int i = 0; i < m->projectCount; i++) {
    for (int j = 0; j < m->projects[i].lines.count; j++) {
      const char *ln = m->projects[i].lines.items[j];
      if (strncmp(ln, "depends_on ", 11) != 0) continue;
      const char *v = strchr(ln, '=');
      if (!v) continue;
      v = trim((char *)v + 1);
      for (char *save = (char *)v;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item) listAdd(&m->depends[i], item);
        if (!comma) break;
        save = comma + 1;
      }
    }
  }

  /* Proyek lain yang dirujuk lewat pack.files berpath ../<nama>/...
     (hasil pack proyek lain) juga build dependency. wsCollectPackDeps
     harus berjalan SEBELUM topo sort, karena wsVisit membaca depends[]. */
  wsCollectPackDeps(m);

  /* Kumpulkan juga dependensi library lintas-proyek (library.<os> =
     <nama proyek lain>). Sengaja TIDAK masuk DFS topo: siklus library
     sah (dua fase build), hanya dipakai untuk memperluas `-w <nama>`. */
  for (int i = 0; i < m->projectCount; i++) {
    for (int j = 0; j < m->projects[i].lines.count; j++) {
      const char *ln = m->projects[i].lines.items[j];
      if (strncmp(ln, "library.", 8) != 0 || strncmp(ln, "library.cdeps", 13) == 0)
        continue; /* cdeps bukan library.<os> yang memperluas closure build */
      const char *v = strchr(ln, '=');
      if (!v) continue;
      v = trim((char *)v + 1);
      for (char *save = (char *)v;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item && strcmp(item, m->projects[i].name) != 0 && wsFindProject(m, item) >= 0)
          listAdd(&m->libDeps[i], item);
        if (!comma) break;
        save = comma + 1;
      }
    }
  }

  /* cdeps sengaja TIDAK memperluas build selektif. cdeps menyatakan
     header/library yang dikonsumsi saat compile/link, bukan instruksi untuk
     membangun ulang proyek pemasok. `-w <target>` mengikuti depends_on dan
     library.<os> eksplisit; proyek cdeps lain yang sudah ada tetap dapat
     dipakai oleh konfigurasi target tanpa ikut dibangun otomatis. */

  unsigned char color[WS_MAX_PROJECTS] = {0};
  List order = {0};
  for (int i = 0; i < m->projectCount; i++)
    if (!wsVisit(m, i, color, &order)) {
      listFree(&order);
      return false;
    }
  m->order = order;
  return true;
}

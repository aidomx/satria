/* ws_worklist.c — Worklist build: proyek needed dalam urutan topo, closure selektif, dan perluasan library.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"


/* ==================== eksekusi ==================== */
/* Tandai closure dependency untuk build selektif. Topological order sudah
   menjamin dependency muncul lebih dulu; yang perlu dilakukan di sini hanya
   memperluas target `-w <nama>` menjadi dependency + target. */
bool wsMarkNeeded(const WsModel *m, int idx, unsigned char *needed,
                         unsigned char *visiting) {
  if (idx < 0 || idx >= m->projectCount) return false;
  if (needed[idx]) return true;
  if (visiting[idx]) {
    fprintf(stderr, "satria: workspace: dependency cycle involving '%s'\n", m->projects[idx].name);
    return false;
  }
  visiting[idx] = 1;
  for (int i = 0; i < m->depends[idx].count; i++) {
    int dep = wsFindProject(m, m->depends[idx].items[i]);
    if (dep < 0) {
      fprintf(stderr, "satria: workspace: '%s' depends on unknown project '%s'\n",
              m->projects[idx].name, m->depends[idx].items[i]);
      visiting[idx] = 0;
      return false;
    }
    if (!wsMarkNeeded(m, dep, needed, visiting)) {
      visiting[idx] = 0;
      return false;
    }
  }
  visiting[idx] = 0;
  needed[idx] = 1;
  return true;
}
/* Perluas set proyek yang dibutuhkan dengan dependensi library (libDeps)
   DAN depends_on secara transitif (fixpoint, bukan topo — siklus library
   sah). Contoh: `-w rupa` menarik ruka (library rupa), lalu rupamod
   (depends_on ruka) agar arsip embed tersedia untuk fase library ruka. */
void wsExpandLibDeps(const WsModel *m, unsigned char *needed) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = 0; i < m->projectCount; i++) {
      if (!needed[i]) continue;
      for (int j = 0; j < m->libDeps[i].count; j++) {
        int dep = wsFindProject(m, m->libDeps[i].items[j]);
        if (dep >= 0 && !needed[dep]) {
          needed[dep] = 1;
          changed = true;
        }
      }
      for (int j = 0; j < m->depends[i].count; j++) {
        int dep = wsFindProject(m, m->depends[i].items[j]);
        if (dep >= 0 && !needed[dep]) {
          needed[dep] = 1;
          changed = true;
        }
      }
    }
  }
}
/* ==================== Eksekusi anak build (workspaceRun) ====================
 *
 * Satu worklist per run: proyek needed dalam urutan topo, indeks model
 * dihitung SEKALI (loop kembar libpass/binpass tidak lagi memanggil
 * wsFindProject berulang). `level` = kedalaman depends_on; fase library
 * dijalankan per level — dua proyek pada level sama tidak saling butuh
 * hasil build (link .so fase library melewatkan artifact proyek lain),
 * jadi aman dikerjakan paralel. Fase binary TETAP pass terpisah seluruh
 * proyek: library lintas-proyek bisa saling siklik (rupa <-> ruka via
 * rupalib.cdeps/rukalib.cdeps di tests/rupaws) sehingga link hanya aman
 * setelah SEMUA library pass selesai.
 */

/* Isi work[] dengan proyek needed dalam urutan topo; kembalikan jumlah. */
int wsBuildWorklist(const WsModel *m, const unsigned char *needed, WsWork *work) {
  int n = 0;
  for (int i = 0; i < m->order.count; i++) {
    const char *name = m->order.items[i];
    int idx = wsFindProject(m, name);
    if (idx < 0 || !needed[idx]) continue;
    /* Topo menjamin dependensi sudah masuk work sebelumnya (i > dep). */
    int level = 0;
    for (int d = 0; d < m->depends[idx].count; d++) {
      int dep = wsFindProject(m, m->depends[idx].items[d]);
      if (dep < 0 || !needed[dep]) continue;
      for (int k = 0; k < n; k++)
        if (work[k].idx == dep && work[k].level >= level) level = work[k].level + 1;
    }
    work[n].idx = idx;
    work[n].name = name;
    work[n].level = level;
    n++;
  }
  return n;
}

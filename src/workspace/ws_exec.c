/* ws_exec.c — Eksekusi satu proyek dari root workspace (build/clean, archive-only).
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"
#include "../log/build_report.h" /* blok # Execution (design/build_report.md) */
#include "../prof/prof.h" /* profMark: audit overhead */
#include "../background.h" /* bgProgress: progres watcher (design/background.md) */


/* DELETE source: orphan compiler outputs kini dipangkas di dalam pass
 * (cmds_pass.c: wsPruneOrphanSources) memakai daftar sumber SEGAR hasil
 * koleksi — tanpa membuka satu pun file .d (audit 30/10: versi lama di sini
 * membaca baris pertama tiap .d = 10.003 fopen+read per build di workspace
 * 10k objek, dan lagipula memakai daftar sumber yang belum ter-scan).
 * Prune tetap HANYA fase library proyek non-arsip — syarat identik dengan
 * pemanggilan lama di bawah. */

/* true bila proyek kemasan murni (output.binary = false di baris proyek):
   fase build-nya arsip/pack, bukan link binary. Semua pemindai baris
   `output.binary = false` di file ini memakai helper ini agar format
   hidup di satu tempat. */
bool wsProjectArchiveOnly(const WsProject *p) {
  for (int i = 0; i < p->lines.count; i++)
    if (strcmp(p->lines.items[i], "output.binary = false") == 0) return true;
  return false;
}
/*
 * Jalankan SATU proyek dari root workspace — pengganti wsExecOne (yang
 * dulu chdir ke root proyek lalu memanggil cmdBuildEx; keduanya kini
 * dilarang design/workspace_v1.md):
 *   cmd "clean" -> Config in-memory (wsBuildConfig) + cmdsCleanRun;
 *   cmd "build" -> label pass -> fast-state -> wsBuildConfig ->
 *                  resolveCompiler -> cmdsBuildPassRun (library/binary).
 * State per proyek ditandai cmdsStateSetWsProject (.satria/workspace/<n>.*).
 */
int wsRunProject(const WsModel *m, const char *name, const char *cmd, int jobs,
                        bool libOnly) {
  int idx = wsFindProject(m, name);
  if (idx < 0) return 1;
  const WsProject *p = &m->projects[idx];
  if (!p->root[0]) return 1;

  cmdsStateSetWsProject(name);

  if (strcmp(cmd, "clean") == 0) {
    Config c;
    bool touched = false;
    int rc = wsBuildConfig(m, idx, &c) ? cmdsCleanRunEx(&c, &touched) : 1;
    if (touched) wsCleanTouched = true;
    cmdsStateSetWsProject("");
    return rc;
  }

  /* Label fase: proyek kemasan (output.binary = false) fase 1-nya ARSIP.
     Format lama `| Project :` / `| Fase :` diganti blok `# Execution` +
     `> Nama` (design/build_report.md): fase library mencetak `> Nama` +
     compile line; fase binary meng-upsert (tanpa cetak ulang). */
  const char *passLabel = "binary";
  if (libOnly) passLabel = wsProjectArchiveOnly(p) ? "archive" : "library";
  bgProgress(1, "Project: %s (%s)", name, passLabel); /* watcher §3B */

  /* Reuse the phase configuration for the build: Config disusun sekali di sini
   * dan dipakai sampai pass berjalan. Prune .d orphan TIDAK lagi di sini —
   * dipindah ke bawah, setelah gerbang fast-state (lihat catatan di sana).
   * Previously a cache miss constructed the same Config twice. */
  Config c = {0};
  bool configReady = false;
  profMark("cfg-pre"); /* profil: sebelum perakitan Config */
  if (!wsProjectArchiveOnly(p)) {
    if (!wsBuildConfig(m, idx, &c)) {
      cmdsStateSetWsProject("");
      return 1;
    }
    configReady = true;
    profMark("cfg-build"); /* profil: perakitan Config murni (tanpa prune) */
  }
  profMark("ws-config"); /* profil: perakitan Config (prune pindah setelah gate) */

  /* Fast no-op per fase — padanan bagian atas cmdBuildEx. Label META
     stabil per proyek (file sintesis sudah tidak ada; label lama berupa
     path .Buildfile gagal validasi -> rebuild penuh sekali saat migrasi). */
  char bfLabel[WS_NAME_LEN + 32];
  snprintf(bfLabel, sizeof(bfLabel), "Buildfile.ws:%s", name);
  if (wsFastStateValid(bfLabel, libOnly)) {
    wsLogFastOk[idx] = true;
    /* Artefak cached proyek fast-hit: catat ke # Execution summary (design §2)
       hanya bila path output bisa diresolusi dari config. Tanpa blok # Execution
       (tidak ada aktivitas compile). */
    if (buildReportActive() && configReady) {
      if (libOnly) {
        if (c.libRequested) {
          char lp[SATRIA_MAX_PATH * 2];
          if (c.libStatic) {
            cmdsLibStaticPath(&c, lp, sizeof(lp));
            buildReportAddLibrary(lp, REP_CACHED, fsFileSize(lp));
          }
          if (c.libShared) {
            cmdsLibSharedPath(&c, lp, sizeof(lp));
            buildReportAddLibrary(lp, REP_CACHED, fsFileSize(lp));
          }
        }
      } else if (c.binary) {
        char target[SATRIA_MAX_PATH * 2];
        snprintf(target, sizeof(target), "%s/%s", c.outBinaryDir, c.outBinaryName);
        buildReportAddBinary(target, REP_CACHED, fsFileSize(target));
      }
    }
    cmdsStateSetWsProject("");
    return 0;
  }
  profMark("ws-faststate"); /* profil: validasi fast-state per fase */

  if ((!configReady && !wsBuildConfig(m, idx, &c)) || !resolveCompiler(&c)) {
    cmdsStateSetWsProject("");
    return 1;
  }
  profMark("ws-resolvecc"); /* profil: resolveCompiler (probe compiler) */

  /* Prune .d orphan tidak lagi di sini: pindah ke cmdsBuildPassRun sesudah
     koleksi sumber (lihat catatan di atas file ini). */
  procSetForeground(jobs > 1 ? false : c.foreground);

  /* pack.* diabaikan (v1): proyek kemasan tanpa arsip & tanpa binary =
     tidak ada target build — lewati, jangan gagalkan seluruh run. */
  if (!c.binary && c.embCount == 0) {
    printf("%s Skipped : tanpa target build (pack.* diabaikan)\n", repPrefix());
    cmdsStateSetWsProject("");
    return 0;
  }

  /* Blok # Execution: cetak `> Nama` utk proyek ini (baru -> cetak; fase
     binary -> upsert tanpa cetak ulang). Setelah seluruh jalur awal kembali
     (fast-state / skip) agar proyek tanpa aktivitas compile tidak memblok.
     Proyek ARSIP-ONLY dilewati: tercatat pada bagian # Archive (step 2),
     bukan sebagai blok proyek di # Execution (design §5/§7). */
  if (buildReportActive() && !wsProjectArchiveOnly(p)) buildReportProject(name);

  int rc = cmdsBuildPassRun(jobs, c, bfLabel, libOnly);
  if (rc != 0) cmdsStatMemoReset(); /* build gagal: output mungkin berubah sebagian */
  cmdsStateSetWsProject("");
  return rc;
}

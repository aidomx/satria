/* ws_compdb.c — CompDB workspace: compile_commands.json gabungan dan pembaruan state.cache.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"
#include "../cache/state_blob.h"


/*
 * wsCompdbRefresh — CompDB root merged SELURUH workspace + state.cache:
 *   1. Susun Config + daftar source tiap proyek (urutan topo). Proyek yang
 *      fast-hit kali ini DAN cocok log (field 2+3) memuat source dari
 *      snapshot <n>.src tanpa re-scan — keputusan (a).
 *   2. Render/gabung ke SATU compile_commands.json di root; skip-write bila
 *      fingerprint gabungan == field 4 log — keputusan (b); backstop cache/
 *      blob di writeCompdbMulti.
 *   3. Tulis ulang state.cache.
 * Tidak menyentuh build state apa pun — aman untuk `-g compdb` query-only.
 * Return false bila gagal (pesan sudah dicetak).
 */
bool wsCompdbRefresh(const WsModel *m) {
  int n = m->projectCount;
  Config *cfgs = (Config *)calloc((size_t)n, sizeof(Config));
  List *srcs = (List *)calloc((size_t)n, sizeof(List));
  uint64_t *cfgH = (uint64_t *)calloc((size_t)n, sizeof(uint64_t));
  uint64_t *srcH = (uint64_t *)calloc((size_t)n, sizeof(uint64_t));
  const Config **cps = (const Config **)calloc((size_t)n, sizeof(Config *));
  List **sps = (List **)calloc((size_t)n, sizeof(List *));
  if (!cfgs || !srcs || !cfgH || !srcH || !cps || !sps) {
    free(cfgs);
    free(srcs);
    free(cfgH);
    free(srcH);
    free(cps);
    free(sps);
    return false;
  }

  bool ok = true;
  int cnt = 0;
  for (int i = 0; i < n && ok; i++) {
    cfgH[i] = wsLogCfgHash(m, i);
    if (!wsBuildConfig(m, i, &cfgs[i]) || !resolveCompiler(&cfgs[i])) {
      fprintf(stderr, "satria: workspace: gagal menyusun config '%s' (compdb)\n",
              m->projects[i].name);
      ok = false;
      break;
    }
    /* CompDB hanya mewakili proyek yang mengompilasi source; proyek
       kemasan (output.binary = false) & compdb=false tidak berkontribusi. */
    if (!cfgs[i].binary || !compdbEnabled(&cfgs[i]) || cfgs[i].sources.count == 0) continue;

    /* Keputusan (a): fast-hit + config cocok + snapshot tak berubah sejak
       log ditulis -> muat daftar dari snapshot, tanpa walk. */
    uint64_t logCfg = 0, logSrc = 0, logCdb = 0;
    bool reused = false;
    if (wsLogFastOk[i] && wsLogReadEntry(m->projects[i].name, &logCfg, &logSrc, &logCdb) &&
        logCfg == cfgH[i] && logSrc != 0 && logSrc == stateBlobHash(m->projects[i].name, "src"))
      reused = wsLogLoadSrcs(m->projects[i].name, &srcs[i]);
    if (!reused) cmdsCollectSources(&cfgs[i], &srcs[i]);
    srcH[i] = stateBlobHash(m->projects[i].name, "src"); /* field 3: snapshot src (0 bila belum ada) */

    if (srcs[i].count > 0) {
      cps[cnt] = &cfgs[i];
      sps[cnt] = &srcs[i];
      cnt++;
    }
  }

  uint64_t fp = 0;
  if (ok && cnt > 0) {
    fp = compdbMultiFingerprint(cps, sps, cnt);
    /* Keputusan (b): hash CompDB tercatat identik + file ada -> tidak
       ditulis ulang (tanpa render, tanpa sentuh disk). */
    if (fp && fsFileExists("compile_commands.json") && wsLogCompdbFp() == fp)
      printf("%s CompDB  : compile_commands.json unchanged (workspace)\n", repPrefix());
    else
      writeCompdbMulti(cps, sps, cnt);
  }

  if (ok) wsLogWrite(m, cfgH, srcH, fp);

  free(cfgs);
  free(srcs);
  free(cfgH);
  free(srcH);
  free(cps);
  free(sps);
  return ok;
}

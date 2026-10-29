/*
 * cmds_pass — isi pass build satu proyek (koleksi, fingerprint, decide,
 * compile). Ekor pass (libOnly/link/state/summary) diserahkan ke
 * cmds_passfinish.c lewat CmdsPassTail.
 * Dipisah dari commands.c (audit refactor: berkas kandidat >600 baris).
 */
#include "../commands.h"
#include "../cache/artifact.h"
#include "../log/build_log.h"
#include "../log/report_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#endif

#include "../cmdhelp.h"
#include "../background.h"
#include "../startup.h"
#include "cmds_internal.h"
#include "../compdb.h"
#include "../compile.h"
#include "../config.h"
#include "../deps.h"
#include "../embed.h"
#include "../pack/pack.h"
#include "../portability.h"
#include "../prof/prof.h"
#include "../uses/workspace.h"
#include "../util.h"
/*
 * cmdsBuildPassRun — isi pass build satu proyek (fase library / fase
 * binary): koleksi source, fingerprint, decide, compile, embed, library,
 * link, state. TANPA loadConfig & TANPA chdir — pemanggil sudah
 * menyiapkan Config (path selaras cwd) dan cwd yang benar:
 *   - single project : cmdBuildEx (cwd = root proyek lewat fsSetCwd)
 *   - workspace      : wsCmdBuild (cwd = root workspace, tidak pernah
 *                      chdir; cmdBuildEx TIDAK dipanggil — design/
 *                      workspace_v1.md §Pemisahan Jalur Build)
 * Config by-value: body hanya membaca field-nya (list dibagi, tak dimutasi).
 */
/* Build gagal sebagian (tidak diinterupsi): tetap simpan kemajuan agar build
   berikutnya melanjutkan dari sini, bukan mengulang dari awal.
   - object yang sukses: dicatat di snapshot deps (tidak dikompilasi ulang);
   - object yang gagal: DIHAPUS, sehingga build berikutnya wajib
     mengompilasinya (decide menganggap object hilang = pending);
   - fingerprint compile disimpan, tetapi link & sources ditandai belum jadi
     (link = 0, sources = 0) agar binary tetap di-link ulang. */
static void buildSavePartial(DepCache *deps, Config *c, const List *pending, const bool *ok,
                             const CmdsBuildFingerprint *fp) {
  char obj[MAX_PATH * 2];
  for (int i = 0; i < pending->count; i++) {
    const char *src = pending->items[i];
    if (!objectPathFor(c, src, obj, sizeof(obj))) continue;
    if (ok[i])
      depsRecordUpdate(deps, src);
    else
      remove(obj);
  }
  depsSave(deps);
  CmdsBuildFingerprint partial = *fp;
  partial.link = 0;
  partial.sources = 0;
  cmdsFingerprintSave(&partial, false);
}

int cmdsBuildPassRun(int jobs, Config c, const char *buildfilePath, bool libOnly) {
  /* Proyek kemasan (output.binary = false): SEBELUM koleksi source —
     proyek kemasan sah tidak punya satu .c pun. Dua varian:
       pack.*    -> pengemasan artefak (tar.gz/deb/checksum);
       embedded  -> arsip aset untuk di-embed binary lain. */
  if (!c.binary) {
    if (c.pack.requested) return cmdsPackOnlyProject(&c);
    if (c.embCount == 0) {
      fprintf(stderr, "satria: nothing to build (output.binary = false)\n");
      return 1;
    }
    mkdirs(c.outBuildDir);
    bool anyRebuilt = false;
    if (!buildEmbeddedArchivesEx(&c, &anyRebuilt)) return 1;
    printf("\n%s Summary :\n", repPrefix());
    bool anyMissing = false;
    for (int i = 0; i < c.embCount; i++) {
      if (!c.emb[i].enable) continue;
      bool have = fsFileExists(c.emb[i].archivePath);
      printf("    Archive  : %s%s\n", c.emb[i].archivePath, have ? "" : " (missing)");
      if (!have) anyMissing = true; /* entri hilang = build gagal, bukan sukses sunyi*/
    }
    if (anyMissing) {
      fprintf(stderr, "satria: build failed: entri embedded hilang (lihat '(missing)' di atas)\n");
      return 1;
    }
    /* No-op: tidak ada arsip yang dibangun ulang (design/build_report.md). */
    if (!anyRebuilt) printf("    Skipped  : true\n");
    printf("    Status   : Success\n");
    return 0;
  }

  /* Fase parse & generate embedded (design/embedded.md): arsip tiap entri
     dibangun dulu, lalu embedded.c (array byte + tabel getEmbed) dan
     embedded.h diterbitkan SEBELUM kompilasi — embedded.c ikut daftar
     source lewat cmdsCollectSources (fingerprint, decide, compdb, link,
     kemasan library), persis seperti source biasa. */
  bool embHeaderChanged = false;
  if (c.embCount > 0) {
    mkdirs(c.outBuildDir);
    if (!buildEmbeddedArchivesEx(&c, NULL)) return 1;
    if (!emitEmbeddedSource(&c, NULL)) return 1;
    if (!emitEmbeddedHeader(&c, &embHeaderChanged)) return 1;
    for (int i = 0; i < c.embCount; i++) {
      const EmbeddedEntry *e = &c.emb[i];
      if (!e->enable) continue;
      if (e->usePrebuilt)
        printf("%s Embedded: %s (file: %s)\n", repPrefix(), e->name, e->archivePath);
      else if (fsFileExists(e->archivePath))
        printf("%s Embedded: %s (archive: %s)\n", repPrefix(), e->name, e->archivePath);
    }
    profMark("embed");
  }

  List srcs = {0};
  if (!wsModeActive()) cmdsSingleSourcesCacheBegin();
  cmdsCollectSources(&c, &srcs);
  if (!wsModeActive()) cmdsSingleSourcesCacheEnd();

  /* Dua sumber yang berbeda tidak boleh memetakan ke object yang sama: itu
     akan menimpa object diam-diam (design/workspace.md: nama output yang
     bentrok harus dideteksi). */
  if (!cmdsCheckObjectCollisions(&c, &srcs)) {
    listFree(&srcs);
    return 1;
  }

  /* One-pass stat plan: stamp source (selalu) + object (default; hilangkan
   * dengan SATRIA_STATPLAN_OBS=0) SEKALI di sini. Konsumen berikutnya memakai
   * hasilnya tanpa stat ulang. */
  StatPlan plan;
  statPlanInit(&plan);
  statPlanBuild(&plan, &c, &srcs);
  profMark("collect+plan");

  if (srcs.count == 0) {
    fprintf(stderr, "satria: tidak ada source .c/.cpp di sources — proyek standar memakai folder "
                    "src/ (atau set sources = ... untuk tata letak lain)\n");
    statPlanFree(&plan);
    return 1;
  }

  CmdsBuildFingerprint currentFp;
  cmdsFingerprintMake(&c, &srcs, &currentFp);
  CmdsBuildFingerprint previousFp = {0};
  bool haveBuildFp = cmdsFingerprintLoad(&previousFp);
  bool compileConfigChanged = !haveBuildFp || previousFp.compile != currentFp.compile;
  bool sourceSetChanged = !haveBuildFp || previousFp.sources != currentFp.sources;
  bool linkConfigChanged = !haveBuildFp || previousFp.link != currentFp.link;
  if (!haveBuildFp)
    printf("%s Fingerprint: state build tidak terbaca; semua object dikompilasi ulang\n", repPrefix());

  profMark("fingerprint");

  if (haveBuildFp && compileConfigChanged)
    printf("%s Fingerprint: compile configuration changed; rebuilding objects\n", repPrefix());
  if (haveBuildFp && sourceSetChanged)
    printf("%s Fingerprint: source set changed; refreshing link inputs\n", repPrefix());
  if (haveBuildFp && linkConfigChanged)
    printf("%s Fingerprint: link configuration changed; relinking\n", repPrefix());

  mkdirs(c.outBuildDir);
  mkdirs(c.outBinaryDir);

  char *inc = includeFlags(&c);
  char *wf = warningFlags(&c);

  /* compile_commands.json tidak perlu dihitung ulang pada setiap build.
     Daftar command hanya berubah jika file belum ada, konfigurasi compile
     berubah, atau set source berubah. Perubahan isi source/header tidak
     mengubah command compile. */
  profMark("pre-compdb");
  if (compdbEnabled(&c) && !wsModeActive()) {
    /* Mode workspace: CompDB ditulis terpusat oleh jalur workspace (satu
       compile_commands.json mewakili SELURUH workspace) — pass per proyek
       tidak menulis apa pun (design/workspace_v1.md §No-op dan CompDB). */
    bool compdbMissing = !fsFileExists("compile_commands.json");
    if (compdbMissing || !haveBuildFp || compileConfigChanged || sourceSetChanged)
      writeCompdb(&c, &srcs);
  }
  profMark("compdb");

  /* Terbitkan build/version.h sebelum kompilasi agar konsumen melihat
     versi yang benar; perubahan memaksa kompilasi ulang total. embedded.h
     sudah diterbitkan di fase parse & generate (embHeaderChanged dihitung
     di sana) — jalurnya tetap memaksa recompile penuh di blok di bawah. */
  char obj[MAX_PATH];
  bool verHeaderChanged = false;
  if (!cmdsEmitVersionHeader(&c, &verHeaderChanged)) {
    statPlanFree(&plan);
    return 1;
  }
  if (embHeaderChanged || verHeaderChanged) {
    if (verHeaderChanged)
      printf("%s Version : .satria-version changed, recompiling all sources\n", repPrefix());
    else
      printf("%s Embedded: config changed, recompiling all sources\n", repPrefix());
    for (int i = 0; i < srcs.count; i++) {
      if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
      /* Hanya hapus bila object memang ada menurut plan stat (sudah dihitung
         sekali di awal run): object yang tidak ada (mis. sesudah clean) tidak
         perlu syscall unlink yang pasti gagal. Tanpa plan valid, hapus seperti
         biasa. */
      int oi = (plan.objIdx && plan.obsObj) ? plan.objIdx[i] : -1;
      if (oi < 0 || plan.objM[oi] >= 0) fsRemoveFile(obj);
      /* Stamp plan objek yang baru saja dihapus: tanpa ini decide-from-plan
         akan melihat objek lama (masih ada & fresh) dan melewati kompilasi
         yang justru wajib diulang. */
      if (plan.objIdx && plan.objIdx[i] >= 0) plan.objM[plan.objIdx[i]] = -1;
    }
  }

  printf("%s Build with %s %s-std=%s\n", repPrefix(), c.cc, wf, c.std);

  int total = 0, compiled = 0, skipped = 0, failed = 0, interrupted = 0;

  /* Fase 1: klasifikasi up-to-date vs perlu-kompilasi. Pakai mtime
     nanodetik (bukan detik) agar perubahan dalam detik yang sama tetap
     terdeteksi — sama seperti keputusan link di bawah. */
  /*
   * Header tracking dua lapis:
   *   1. mtime — object usang bila header transitive lebih baru. Murah.
   *   2. konten — bila mtime mengatakan stale (mis. setelah `touch`), hash
   *      isi dibandingkan dengan snapshot build sukses terakhir
   *      (.satria/caches/deps.cache). Konten sama => tidak perlu kompilasi ulang.
   *
   * Edges header diambil dari file .d compiler (flag -MMD, GCC/Clang) —
   * akurat & lengkap; pemindai #include hanya fallback (build pertama,
   * MSVC, .d belum ada). Lihat deps/.
   */
  List pending = {0};
  /* Hash objek lama untuk tiap source yang perlu dikompilasi. Setelah compiler
     selesai, hasil byte dibandingkan: source/header boleh berubah tanpa
     memaksa relink bila object efektif tetap identik. */
  List pendingObjHashes = {0};
  DepCache *deps = depsNew(&c);
  /* Warna warning compiler: hanya saat output ke terminal dan build.log mati
     (relay build.log membaca stderr; ANSI tidak boleh masuk log). */
  compileSetDiagColor(diagColorWanted() && !c.logEnabled);
  int headerStale = 0, headerTouched = 0;
  /* mtime object terbesar & apakah ada object yang hilang — dikumpulkan di
     sini agar keputusan link (no-op) tidak men-stat ulang semua object. */
  int64_t maxObjM = -1;
  bool anyObjMissing = false;
  /* Default: decide membaca plan (satu stat / file). SATRIA_DECIDE_FROM_PLAN=0
     mengembalikan stat sendiri (perilaku lama). */
  bool decideFromPlan = !envOff("SATRIA_DECIDE_FROM_PLAN");
  for (int i = 0; i < srcs.count; i++) {
    const char *src = srcs.items[i];
    if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;
    /* Sumber mtime dari plan one-pass; SATRIA_DECIDE_FROM_PLAN=0 = stat sendiri.
       Objek: dari plan bila objek ikut distamp (obsObj), selain itu stat
       sendiri. Semantik -1 (hilang) dipertahankan. */
    int64_t srcM = (decideFromPlan && plan.srcM) ? plan.srcM[i] : cmdsMTimeDecide(src);
    int64_t objM;
    /* Object dari plan hanya saat objek ikut distamp: stamp bisa basi bila
       objek dihapus jalur embedded/version header di atas — loop itu sudah
       meng-invalidasi slot (objM = -1) sehingga decide memaksa recompile. */
    if (decideFromPlan && plan.objIdx && plan.objIdx[i] >= 0 && plan.obsObj)
      objM = plan.objM[plan.objIdx[i]];
    else
      objM = cmdsMTimeDecide(obj); /* -1 = object belum ada (sekali stat) */
    if (objM < 0)
      anyObjMissing = true;
    else if (objM > maxObjM)
      maxObjM = objM;
    if (!compileConfigChanged && objM >= 0 && srcM >= 0 && objM >= srcM) {
      if (depsNewestHeaderMTimeAt(deps, src, srcM) > objM) {
        /* header lebih baru — tapi mungkin hanya `touch`: cek konten */
        if (depsContentUpToDate(deps, src)) {
          headerTouched++;
          skipped++;
          continue;
        }
        headerStale++;
      } else {
        skipped++;
        continue;
      }
    }
    /* Sumber lebih baru dari object (mis. `touch` yang tidak sempat di-PATCH,
       lalu run berikutnya membangun proyek lain lebih dulu): buktikan lewat
       hash isi terhadap snapshot build sukses terakhir (.satria/caches/deps.cache).
       Konten sama -> tanpa kompilasi (design/startup.md §4 — sentuh tanpa
       perubahan isi tak memicu build; jalur cache on & incremental lama
       kini sepakat). Snapshot tak ada (sumber baru / build pertama) atau
       isi berbeda -> tetap masuk antrean kompilasi. */
    if (!compileConfigChanged && objM >= 0 && srcM > objM && depsContentUpToDate(deps, src)) {
      skipped++;
      continue;
    }
    listAdd(&pending, src);
    char oldHash[32];
    uint64_t h = objM >= 0 ? cmdsStateFileHash(obj) : 0;
    snprintf(oldHash, sizeof(oldHash), "%016llx", (unsigned long long)h);
    listAdd(&pendingObjHashes, oldHash);
  }
  profMark("decide");
  if (headerTouched > 0)
    printf("%s Headers : %d source(s) skipped (touched, content unchanged)\n", repPrefix(),
           headerTouched);
  if (headerStale > 0)
    printf("%s Headers : %d source(s) stale due to header change\n", repPrefix(), headerStale);
  total = pending.count;
  bool *pendingOk = calloc((size_t)(total > 0 ? total : 1), sizeof(bool));

  /* Fase 2: kompilasi hanya yang berubah — paralel (-jN) atau serial. */
  if (jobs != 1 && total > 0) {
    cmdsRunParallelJobs(&c, inc, wf, &pending, obj, sizeof(obj), jobs, &compiled, &failed,
                        &interrupted, false, pendingOk);
  } else {
    for (int i = 0; i < pending.count; i++) {
      const char *src = pending.items[i];
      if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;

      /* Serial (satu job): output compiler langsung ke terminal / relay build.log,
         tanpa capture. Tidak ada job lain, jadi tidak ada interleaving. */
      double start = nowSeconds();
      bool ok = compileLibraryOne(&c, inc, wf, src, obj, NULL);
      pendingOk[i] = ok;
      double dt = nowSeconds() - start;

      compiled++;
      if (procInterrupted()) {
        /* foreground=false: child mati karena forward SIGINT dari satria —
           hentikan build, jangan lanjut ke source berikutnya. */
        interrupted++;
        if (ok || c.progressErrorAlways)
          printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, "INT", dt, src);
        break;
      }
      if (!ok) failed++;
      if (ok || c.progressErrorAlways)
        printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, ok ? "OK" : "FAIL", dt, src);
      bgProgress(5 + (compiled * 80) / total, "Compiling: %s", src); /* §3B */
    }
  }
  profMark("compile");

  if (interrupted > 0) {
    /* Ctrl+C: kemajuan dipertahankan seperti build gagal (lihat buildSavePartial).
       Object yang sudah selesai dicatat; object yang belum selesai (termasuk yang
       terpotong saat dibunuh) dihapus agar build berikutnya mengompilasinya. */
    buildSavePartial(deps, &c, &pending, pendingOk, &currentFp);
    fprintf(stderr, "\nsatria: build interrupted; progres disimpan, lanjut dari sini\n");
    listFree(&pendingObjHashes);
    free(pendingOk);
    free(inc);
    free(wf);
    depsFree(deps);
    statPlanFree(&plan);
    return 130;
  }

  if (failed > 0) {
    /* Kemajuan parsial dipertahankan (lihat buildSavePartial): build berikutnya
       melanjutkan dari object yang sudah berhasil, tidak mengulang dari awal. */
    buildSavePartial(deps, &c, &pending, pendingOk, &currentFp);
    fprintf(stderr, "\nsatria: build failed with %d error(s); linking skipped\n", failed);
    listFree(&pendingObjHashes);
    free(pendingOk);
    free(inc);
    free(wf);
    depsFree(deps);
    statPlanFree(&plan);
    return 1;
  }
  free(pendingOk); /* jalur sukses: hasil per source tidak diperlukan lagi */

  bool objectsChanged = compiled > 0; /* fallback konservatif */
  if (compiled > 0 && pendingObjHashes.count == pending.count) {
    objectsChanged = false;
    for (int i = 0; i < pending.count; i++) {
      char objPath[MAX_PATH];
      if (!objectPathFor(&c, pending.items[i], objPath, sizeof(objPath))) {
        objectsChanged = true;
        break;
      }
      uint64_t before = (uint64_t)strtoull(pendingObjHashes.items[i], NULL, 16);
      uint64_t after = cmdsStateFileHash(objPath);
      if (!before || !after || before != after) {
        objectsChanged = true;
        break;
      }
    }
  }
  listFree(&pendingObjHashes);

  profMark("objhash");


  /* Serahkan ekor pass (libOnly/link/state/summary) ke cmds_passfinish.c:
     seluruh state di bawah ini berpindah lewat konteks (CmdsPassTail,
     tipe di cmds_internal.h); fungsi ini tidak menyentuhnya lagi. */
  CmdsPassTail t = {0};
  t.c = c;
  t.buildfilePath = buildfilePath;
  t.libOnly = libOnly;
  t.srcs = srcs;
  t.plan = plan;
  t.deps = deps;
  t.pending = pending;
  t.inc = inc;
  t.wf = wf;
  memcpy(t.obj, obj, sizeof(t.obj));
  t.currentFp = currentFp;
  t.compiled = compiled;
  t.skipped = skipped;
  t.objectsChanged = objectsChanged;
  t.sourceSetChanged = sourceSetChanged;
  t.linkConfigChanged = linkConfigChanged;
  t.maxObjM = maxObjM;
  t.anyObjMissing = anyObjMissing;
  return cmdsBuildPassFinish(&t);
}

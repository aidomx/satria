/*
 * cmds_build — perintah build (cmdBuild/cmdCompdbGenerate/cmdBuildEx),
 * NaiveStatPlan, koleksi source, dan cek tabrakan object.
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
/* ==================== compdb -g (query-only) ==================== */

int cmdCompdbGenerate(const char *buildfilePath) {
  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) return 1;
  if (!resolveCompiler(&c)) return 1;

  if (!fsSetCwd(c.root)) {
    fprintf(stderr, "satria: cannot enter root '%s'\n", c.root);
    return 1;
  }

  /* Kemasan murni (output.binary = false) tidak punya compile step —
     tidak ada yang bisa direkam ke compile_commands.json. */
  if (!c.binary) {
    printf("%s CompDB  : skipped (output.binary = false)\n", repPrefix());
    return 0;
  }
  if (!compdbEnabled(&c)) {
    printf("%s CompDB  : disabled (output.compileCommands = false)\n", repPrefix());
    return 0;
  }

  List srcs = {0};
  listReserve(&srcs, 1024);
  for (int i = 0; i < c.sources.count; i++) {
    const char *entry = c.sources.items[i];
    size_t elen = strlen(entry);
    if (elen >= 2 &&
        (strcmp(entry + elen - 2, ".c") == 0 || strcmp(entry + elen - 2, ".cpp") == 0) &&
        fsFileExists(entry)) {
      listAdd(&srcs, entry);
      continue;
    }
    walkDir(entry, ".c", &srcs);
    walkDir(entry, ".cpp", &srcs);
  }
  listSort(&srcs); /* urutan entri json = urutan link (reprodusible) */

  if (srcs.count == 0) {
    fprintf(stderr,
            "satria: tidak ada source .c/.cpp di sources — tidak ada compdb untuk dibuat\n");
    return 1;
  }

  /* Cache hit / restore blob / render — mesin yang sama dengan jalur
     build (writeCompdb), tanpa decide/compile/link/fast-state. */
  writeCompdb(&c, &srcs);
  listFree(&srcs);
  return 0;
}

/* ==================== build ==================== */

/* Build penuh satu proyek — jalur satria satu-proyek & fase binary workspace. */
int cmdBuild(int jobs, const char *buildfilePath) {
  if (bgIsChild()) {
    /* Child background (design/background.md §2): tulis PID, jalankan
       pipeline normal, tutup run dengan status + hapus PID (§4.3). */
    bgChildBegin(BG_SINGLE);
    int rc = cmdBuildEx(jobs, buildfilePath, false);
    bgChildFinish(rc == 0);
    return rc;
  }
  int gate = bgBuildGate(BG_SINGLE); /* §4.1: tolak build ganda */
  if (gate >= 0) return gate;
  /* Keputusan spawn ada di cmdBuildEx SETELAH gerbang no-op (router /
     fast-state): run tanpa pekerjaan tidak boleh masuk jalur background. */
  return cmdBuildEx(jobs, buildfilePath, false);
}

/* ==================== [OPT] NaiveStatPlan (satu stat / file) ====================
 *
 * Audit bench compdb menunjukkan biaya dominan fase decide adalah stat
 * per file (mtime-ns) di lingkungan proot — bukan render compdb. Plan ini
 * mengumpulkan hasil stat SEKALI di awal (satu fsStampNsSize per file)
 * supaya fase lain tinggal memakai hasilnya tanpa men-stat ulang.
 *
 * Kontrol via env (default: satu stat / file — decide membaca plan):
 *   SATRIA_DECIDE_FROM_PLAN=0  decide men-stat src+objek sendiri (perilaku lama).
 *   SATRIA_STATPLAN_OBS=0      jangan stamp objek saat gather; decide men-stat
 *                            objek sendiri (perilaku lama).
 * Kedua env =0 bersama-sama memulihkan perilaku lama 100%.
 *
 * Guard jalur embedded/version header: emitEmbeddedHeader/
 * cmdsEmitVersionHeader berjalan SEBELUM decide dan bisa menghapus objek;
 * loop di cmdBuildEx meng-invalidasi slot plan.objM jadi -1 sehingga decide
 * melihat objek hilang dan tetap me-recompile (lihat cmdBuildEx).
 *
 *Konsumen aktif saat ini: decide. Konsumen masa depan:
 * keputusan link, PSA, restore compdb.
 */
/* StatPlan — tipe & API ada di cmds_internal.h (dipakai pass & finish). */

void statPlanInit(StatPlan *p) {
  memset(p, 0, sizeof(*p));
}

void statPlanFree(StatPlan *p) {
  free(p->path);
  free(p->srcM);
  free(p->objIdx);
  free(p->objPath);
  free(p->objM);
  statPlanInit(p);
}

/* true bila env diset dan bernilai "0" (opt-out eksplisit ke perilaku lama). */
bool envOff(const char *name) {
  const char *v = getenv(name);
  return v && strcmp(v, "0") == 0;
}

/* Stamp semua source sekali jalan; opsional (obsObj) stamp object unik.
   Dedup object pakai linear search: objCount kecil dan objectPathFor
   deterministik, jadi O(n^2) kecil ini masih jauh lebih murah daripada
   stat ganda pada FS lambat. */
void statPlanBuild(StatPlan *p, const Config *c, const List *srcs) {
  int n = srcs->count;
  p->count = n;
  p->obsObj = !envOff("SATRIA_STATPLAN_OBS");
  /* +1 menghindari malloc(0) saat n==0 */
  p->path = malloc(sizeof(char *) * (size_t)(n + 1));
  p->srcM = malloc(sizeof(int64_t) * (size_t)(n + 1));
  p->objIdx = malloc(sizeof(int) * (size_t)(n + 1));
  p->objPath = malloc(sizeof(char[MAX_PATH]) * (size_t)(n + 1));
  p->objM = malloc(sizeof(int64_t) * (size_t)(n + 1));
  if (!p->path || !p->srcM || !p->objIdx || !p->objPath || !p->objM) {
    statPlanFree(p);
    return;
  }
  p->objCount = 0;
  for (int i = 0; i < n; i++) {
    const char *src = srcs->items[i];
    p->path[i] = src;
    long long sz = 0;
    int64_t m = -1;
    fsStampNsSize(src, &m, &sz); /* gagal -> -1, semantik fsMTimeNs */
    p->srcM[i] = m;

    char objPath[MAX_PATH];
    if (!objectPathFor(c, src, objPath, sizeof(objPath))) {
      p->objIdx[i] = -1;
      continue;
    }
    int slot = -1;
    for (int j = 0; j < p->objCount; j++) {
      if (strcmp(p->objPath[j], objPath) == 0) {
        slot = j;
        break;
      }
    }
    if (slot < 0) {
      slot = p->objCount;
      snprintf(p->objPath[slot], MAX_PATH, "%s", objPath);
      if (p->obsObj) {
        int64_t om = -1;
        fsStampNsSize(objPath, &om, &sz);
        p->objM[slot] = om;
      } else {
        p->objM[slot] = -99; /* belum distat — konsumen WAJIB stat sendiri */
      }
      p->objCount++;
    }
    p->objIdx[i] = slot;
  }
}

int cmdBuildEx(int jobs, const char *buildfilePath, bool libOnly) {
  /* Sisa state lama (audit §Daftar Implementasi): mode single menyapu file
     tulisan versi lama sekali per run; mode workspace menyapu lewat
     wsLegacyStateRemove di workspaceRun. */
  if (!wsModeActive()) cmdsLegacyStateSweep();
  /* Fast no-op per fase (miniws): snapshot fase binary disimpan sebagai
     entri `build.state` di .satria/caches/fingerprint.cache, fase library di
     .satria/caches/lib.cache — no-op `satria -w` tidak membayar loadConfig +
     fingerprint + decide penuh per proyek. */
  bool fastOk = wsModeActive() ? wsFastStateValid(buildfilePath, libOnly)
                               : cmdsFastStateValid(buildfilePath, libOnly);
  if (fastOk) return 0; /* no-op: detail sudah dicetak fast-state */
  /* `background = true` (design/background.md §2): spawn HANYA bila build
     ini benar-benar mengerjakan sesuatu — run no-op (router opts.cache +
     fast-state) sudah keluar di atas dengan laporan no-op di terminal, tanpa
     child dan tanpa output kosong. Child (bgIsChild) melewati jalur ini. */
  if (!wsModeActive()) {
    /* Startup (cold) TIDAK dicetak induk — hanya direkam untuk `satria watch`
       (bgMaybeSpawnSingle / bgStartupRecord). */
    int start = bgMaybeSpawnSingle(buildfilePath);
    if (start >= 0) return start;
  }
  /* Build mengubah file. Mode single: memo dikosongkan total (aman, perilaku lama).
     Mode workspace: output dibuang per entri saat state disimpan, dan memo total
     dikosongkan bila build gagal (lihat wsExecOne), agar memo bisa dipakai lintas fase. */
  if (!wsModeActive()) cmdsStatMemoReset();
  /* Cold startup is defined by the absence of the persistent config state,
     not by missing build outputs. Keep warm startup silent. */
  bool coldStartup = !wsModeActive() && !fsFileExists(".satria/startup.state");
  bool showStartup = coldStartup && termIsTTY();
  startupBegin(showStartup); /* "startup :" bila tampil; fase di bawah */

  /* Detail laporan run (design/startup.md §10): tee stdout sejak awal; disimpan
     ke .satria/report.blob bila build sukses, dibuang bila gagal. */
  reportBlobBegin();

  Config c = configDefaults();
  startupPhase("loading configuration");
  if (!loadConfig(&c, buildfilePath)) {
    startupPhaseEnd(false);
    reportBlobFinish(false);
    return 1;
  }
  startupPhase("preparing execution"); /* fase load selesai: done dicetak di sini */
  if (!resolveCompiler(&c)) {
    startupPhaseEnd(false);
    reportBlobFinish(false);
    return 1;
  }

  profMark("startup+config");

  /* Mode sinyal: build paralel selalu butuh handler forward SIGINT karena
     tiap compiler ada di process group sendiri (Ctrl+C dari terminal hanya
     sampai ke satria). Build serial mengikuti Buildfile: foreground. */
  procSetForeground(jobs > 1 ? false : c.foreground);

  if (!fsDirExists(c.root)) {
    startupPhaseEnd(false);
    fprintf(stderr, "satria: root '%s' is not a directory\n", c.root);
    reportBlobFinish(false);
    return 1;
  }
  if (!fsSetCwd(c.root)) {
    startupPhaseEnd(false);
    fprintf(stderr, "satria: cannot enter root '%s'\n", c.root);
    reportBlobFinish(false);
    return 1;
  }
  startupPhaseEnd(true);

  bool logging = !wsModeActive() && c.logEnabled;
  if (logging) buildLogBegin();
  int result = cmdsBuildPassRun(jobs, c, buildfilePath, libOnly);
  if (logging) buildLogEnd(result);
  reportBlobFinish(result == 0);        /* sukses: jadikan report.blob; gagal: tahan yang lama */
  if (result == 0) artifactCacheSave(); /* metadata artefak (design/startup.md §9) */
  return result;
}

/* Pasangan (object, sumber) untuk deteksi tabrakan: diurutkan per object. */
typedef struct {
  char obj[MAX_PATH];
  const char *src;
} ObjEntry;

static int objEntryCmp(const void *a, const void *b) {
  return strcmp(((const ObjEntry *)a)->obj, ((const ObjEntry *)b)->obj);
}

bool cmdsCheckObjectCollisions(const Config *c, const List *srcs) {
  if (srcs->count < 2) return true;
  ObjEntry *e = calloc((size_t)srcs->count, sizeof(ObjEntry));
  if (!e) return true; /* kehabisan memori: tidak ada perbandingan; build lanjut */
  int m = 0;
  for (int i = 0; i < srcs->count; i++) {
    const char *src = srcs->items[i];
    if (!objectPathFor(c, src, e[m].obj, sizeof(e[m].obj))) continue;
    e[m].src = src;
    m++;
  }
  qsort(e, (size_t)m, sizeof(ObjEntry), objEntryCmp);
  bool ok = true;
  for (int i = 1; i < m; i++) {
    if (strcmp(e[i].obj, e[i - 1].obj) != 0) continue;
    fprintf(stderr,
            "satria: tabrakan object: '%s' dan '%s' sama-sama -> %s\n"
            "        ubah nama salah satu file atau pindahkan dari root sumber yang sama\n",
            e[i - 1].src, e[i].src, e[i].obj);
    ok = false;
  }
  free(e);
  return ok;
}

/*
 * cmdsCollectSources — koleksi source .c/.cpp dari Config: entri FILE
 * (mis. hasil `satria -xf` untuk build.ninja flat) masuk langsung; selain
 * itu dianggap folder dan discan rekursif (walk cache .satria/sources.cache bila
 * mode workspace aktif — lihat wsModeSet). Urutan hasil = urutan link:
 * .c dan .cpp dihasilkan dua walkDir terpisah per folder root, jadi di-sort
 * ulang jadi satu barisan stabil — baris link reprodusible antar mesin dan
 * urutan inisialisasi object statis C++ tidak melekat pada readdir.
 * Dipakai cmdsBuildPassRun dan jalur CompDB workspace (wsCompdbRefresh).
 */
void cmdsCollectSources(const Config *c, List *out) {
  listReserve(out, 1024);
  for (int i = 0; i < c->sources.count; i++) {
    const char *entry = c->sources.items[i];
    size_t elen = strlen(entry);
    if (elen >= 2 &&
        (strcmp(entry + elen - 2, ".c") == 0 || strcmp(entry + elen - 2, ".cpp") == 0) &&
        fsFileExists(entry)) {
      listAdd(out, entry);
      continue;
    }
    stateWalkDirCached(entry, ".c", out);
    stateWalkDirCached(entry, ".cpp", out);
  }
  /* Source embed hasil codegen: berada di outBuildDir (bukan root sumber),
     jadi ditambahkan eksplisit — ikut fingerprint, decide, compdb, link,
     dan kemasan library seperti source biasa (design/embedded.md §Pipeline).
     Hanya proyek binary: proyek kemasan mengarsipkan, bukan mengcompile. */
  if (c->binary && c->embCount > 0) {
    char emb[MAX_PATH + 32];
    snprintf(emb, sizeof(emb), "%s/embedded.c", c->outBuildDir);
    bool dup = false;
    for (int i = 0; i < out->count; i++) {
      if (strcmp(out->items[i], emb) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup) listAdd(out, emb);
  }
  listSort(out);
}


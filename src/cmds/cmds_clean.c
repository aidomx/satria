/*
 * cmds_clean — command clean: hapus artefak build dan\n * sweep sisa state lama (design/startup.md §6).
 * Dipisah dari cmds_state.c (audit refactor: berkas kandidat >600 baris).
 * Definisi & semantik lihat cmds_internal.h.
 */
#include "cmds_internal.h"
#include "../log/report_blob.h"
#include "../cache/artifact.h"
#include "../cache/state_blob.h"
#include "../cache/router.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../background.h"
#include "../cache/cache_store.h"
#include "../commands.h"
#include "../compdb.h"
#include "../deps.h"
#include "../compile.h"
#include "../embed_ld_cache.h"
#include "../portability.h"
#include "../util.h"

/* ==================== clean ==================== */

int cmdClean(const char *buildfilePath) {
  /* design/background.md §4.4: clean diblokir selama build background
     berjalan — artefak tidak boleh dihapus di tengah build. */
  int gate = bgCleanGate(BG_SINGLE);
  if (gate >= 0) return gate;
  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) return 1;
  return cmdsCleanRun(&c);
}

/* Kosongkan ISI folder; folder itu sendiri tetap ada (design/startup.md §6).
   Hanya anak langsung yang dihapus: file lewat fsRemoveFile, subfolder lewat
   fsRemoveTree. Folder tidak ada -> tidak ada yang dilakukan. */
bool cmdsEmptyDir(const char *dir, int *removed) {
  if (!fsDirExists(dir)) return true;
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  bool ok = true;
  for (int i = 0; i < files.count; i++) {
    if (fsRemoveFile(files.items[i]))
      (*removed)++;
    else
      ok = false;
  }
  for (int i = 0; i < dirs.count; i++) {
    if (fsRemoveTree(dirs.items[i]))
      (*removed)++;
    else
      ok = false;
  }
  listFree(&dirs);
  listFree(&files);
  return ok;
}

/* ==================== Pembersihan state lama (audit §Daftar Implementasi) ==
   Jalur tulis lama yang sudah digantikan blob (caches/state.cache) dan store
   (fingerprint.cache) tidak pernah dibaca lagi — file sisa dari versi lama
   dibuang sekali per run dan saat clean agar tidak menumpuk:
     .satria/workspace.state               gerbang no-op ws lama
     .satria/workspace/<p>.<leaf>          state per-proyek lama; leaf lama di
                                           bawah — <p>.Buildfile di folder yang
                                           SAMA adalah CURRENT, jangan disentuh
     .satria/{build.state, build.lib.state, build.fingerprint}   single lama
     .satria/caches/paths.cache            sudah dimigrasi ke sources.cache
   Menghapus path yang tidak ada = tanpa efek; aman dipanggil tiap run. */
static const char *kWsLegacySuffixes[] = {".build.fingerprint", ".ws.state",
                                          ".ws.lib.state",      ".deps.cache",
                                          ".src",               ".hdr"};

void cmdsLegacyStateSweep(void) {
  if (fsDirExists(".satria")) {
    fsRemoveFile(".satria/build.state");
    fsRemoveFile(".satria/build.lib.state");
    fsRemoveFile(".satria/build.fingerprint");
    fsRemoveFile(".satria/caches/paths.cache");
    /* sisa format v0.2.3 dan sebelumnya: log workspace dan daftar sumber lama */
    fsRemoveFile(".satria/workspace_log");
    fsRemoveFile(".satria/ws.src");
  }
  if (!fsDirExists(".satria/workspace")) return;
  List dirs = {0}, files = {0};
  fsListDir(".satria/workspace", &dirs, &files);
  for (int i = 0; i < files.count; i++) {
    const char *p = files.items[i];
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    size_t bl = strlen(base);
    for (size_t k = 0; k < sizeof(kWsLegacySuffixes) / sizeof(kWsLegacySuffixes[0]); k++) {
      size_t ln = strlen(kWsLegacySuffixes[k]);
      if (bl > ln && strcmp(base + bl - ln, kWsLegacySuffixes[k]) == 0) {
        fsRemoveFile(p);
        break;
      }
    }
  }
  for (int i = 0; i < dirs.count; i++) { /* <p>.hdr/ lama (salinan header semantik) */
    const char *p = dirs.items[i];
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    size_t bl = strlen(base);
    if (bl > 4 && strcmp(base + bl - 4, ".hdr") == 0) fsRemoveTree(p);
  }
  /* folder .satria/workspace yang sudah kosong (sisa format lama) ikut dibuang */
  List restDirs = {0}, restFiles = {0};
  fsListDir(".satria/workspace", &restDirs, &restFiles);
  if (restDirs.count == 0 && restFiles.count == 0) fsRemoveTree(".satria/workspace");
  listFree(&restDirs);
  listFree(&restFiles);
  listFree(&dirs);
  listFree(&files);
}

/* Jalankan logika bersih TANPA loadConfig — dipakai jalur clean tunggal
   (cmdClean) dan mode workspace (Config disusun dari Buildfile.ws oleh
   wsBuildConfig; path state mengikuti cmdsStateSetWsProject). */
int cmdsCleanRunEx(const Config *c, bool *anyOut) {
  bool any = false;
  /* bin/, build/, lib/, modules/: yang dibersihkan ISINYA saja. Folder
     dibuat sekali oleh startup dan tetap ada, jadi build berikutnya tidak
     perlu membuatnya ulang. modules/ hanya berisi arsip embed hasil generate
     (dibuat ulang oleh build), bukan input. "." dan path kosong ditolak. */
  if (c->cleanBuildDir) {
    const char *emptied[] = {c->outBuildDir, c->outBinaryDir, c->outLibDir, "modules"};
    for (int i = 0; i < (int)(sizeof(emptied) / sizeof(emptied[0])); i++) {
      const char *d = emptied[i];
      if (!d[0] || strcmp(d, ".") == 0 || !safeRelative(d) || !fsDirExists(d)) continue;
      int removed = 0;
      bool ok = cmdsEmptyDir(d, &removed);
      if (removed || !ok) {
        printf("> Emptied   : %s (%d entri%s)\n", d, removed, ok ? "" : ", sebagian gagal");
        any = true;
      }
    }
  }
  if (c->cleanCompileCommands && fsFileExists("compile_commands.json")) {
    fsRemoveFile("compile_commands.json");
    printf("> Removed   : compile_commands.json\n");
    any = true;
  }
  /* deps.cache tidak dihapus: file dipertahankan dan isinya dikosongkan
     bersama file cache lain di bawah (design/startup.md §6). Snapshot hash
     dependensi kosong -> verifikasi dimulai dari nol pada build berikutnya. */
  /* Isi tiap file cache di .satria/caches dikosongkan (design/startup.md §6)
     — file tetap ada (csOpen menerima file kosong), sehingga jumlah file tidak
     berubah dan router opts.cache tetap menunjuk target yang sah. Router
     sendiri TIDAK disentuh: setelah clean, optsRouterValidate gagal (grup
     proyek hilang dari caches) -> run berikutnya tidak boleh no-op, lalu
     build sukses merekam ulang entri + router. */
  if (fsDirExists(WS_CACHES_DIR)) {
    List cdirs = {0}, cfiles = {0};
    fsListDir(WS_CACHES_DIR, &cdirs, &cfiles);
    int n = 0;
    for (int i = 0; i < cfiles.count; i++) {
      FILE *fp = fopen(cfiles.items[i], "w");
      if (fp) {
        fclose(fp);
        n++;
      }
    }
    listFree(&cdirs);
    listFree(&cfiles);
    if (n) {
      printf("> Emptied   : %s (%d file)\n", WS_CACHES_DIR, n);
      any = true;
    }
  }
  /* Mode workspace: state fast & snapshot source per proyek ikut dibuang
     (root-centric) — clean berarti build berikutnya mulai dari nol. */
  if (cmdsStateWsProject()) {
    static const char *wsLeaves[] = {"ws.state", "src", NULL};
    for (int i = 0; wsLeaves[i]; i++) {
      if (stateBlobExists(cmdsStateWsProject(), wsLeaves[i])) {
        stateBlobDelete(cmdsStateWsProject(), wsLeaves[i]);
        printf("> Removed   : caches/state.cache [%s.%s]\n", cmdsStateWsProject(), wsLeaves[i]);
        any = true;
      }
    }
  }
  /* ld.cache (verdict deteksi GNU ld) TIDAK dihapus: file dan isinya
     dipertahankan. Validasinya sudah memeriksa path, mtime ns, dan ukuran
     binary ld yang sedang di-resolve (embed_ld_cache.c), sehingga verdict
     basi atau dari mesin lain otomatis dibuang dan diprobe ulang. */
  /* Staging packaging (.satria/pack) adalah cache — artefak final di dist/
     TIDAK disentuh (bisa jadi dirujuk embedded.* proyek lain). */
  if (fsDirExists(".satria/pack")) {
    fsRemoveTree(".satria/pack");
    printf("> Removed   : .satria/pack\n");
    any = true;
  }
  /* Sisa state lama (audit): file tulisan versi lama yang sudah digantikan
     blob/store — dibuang juga saat clean (lihat cmdsLegacyStateSweep). */
  cmdsLegacyStateSweep();
  /* startup.state TIDAK dihapus clean: workspaceRun (mode ws) menyinkronkannya
     dengan Buildfile.ws sebelum clean berjalan (design/startup.md §6). */
  if (anyOut) *anyOut = any;
  return 0;
}

int cmdsCleanRun(const Config *c) {
  bool any = false;
  int rc = cmdsCleanRunEx(c, &any);
  if (!any) printf("> Nothing to clean (see Buildfile: clean)\n");
  return rc;
}


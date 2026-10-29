/* workspace.c — Entry point mode workspace: workspaceRun (orkestrasi build/clean), workspaceFileName/Exists, workspaceEnumerate.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"
#include "../background.h"    /* background build + watch (design/background.md) */
#include "../cmds/cmds_internal.h" /* cmdsLegacyStateSweep: pembersihan state lama */
#include "../cache/router.h" /* optsSourcesRouteSync: sinkron sources <-> opts */
#include "../log/report_blob.h" /* detail laporan no-op (design/startup.md §10) */
#include "../log/build_log.h"

bool workspaceFileExists(void);

/* Sisa format lama (sebelum cache menggantikannya): .satria/workspace.state
   plus file per-proyek lama & paths.cache — lihat cmdsLegacyStateSweep
   (design/startup.md: audit penghapusan state lama). */
#define WS_LEGACY_STATE ".satria/workspace.state"
static void wsLegacyStateRemove(void) {
  fsRemoveFile(WS_LEGACY_STATE);
  cmdsLegacyStateSweep();
}

/* Read a global boolean switch from the restored startup model, not Buildfile.ws.
   Default false; project-specific settings are intentionally excluded. */
static bool wsCommonBool(const WsModel *m, const char *want) {
  bool enabled = false;
  for (int i = 0; i < m->commonLines.count; ++i) {
    char line[WS_LINE_LEN];
    char key[64];
    char *value = NULL;
    snprintf(line, sizeof(line), "%s", m->commonLines.items[i]);
    if (wsSplitKV(line, key, sizeof(key), &value) && strcmp(key, want) == 0) {
      if (strcmp(value, "true") == 0) enabled = true;
      else if (strcmp(value, "false") == 0) enabled = false;
    }
  }
  return enabled;
}

static bool wsLogEnabled(const WsModel *m) { return wsCommonBool(m, "log"); }

/* `background = true` (design/background.md §1): build dijalankan child
   terpisah dari root workspace. */
static bool wsBackgroundEnabled(const WsModel *m) {
  return wsCommonBool(m, "background");
}

static bool wsBuildLogActive = false;

static bool wsPathUnderRoot(const char *path, const char *root) {
  if (!path || !root || !*root) return false;
  if (strcmp(root, ".") == 0) return true;
  size_t n = strlen(root);
  return strncmp(path, root, n) == 0 && path[n] == '/';
}

/* Check only cached source/header entries whose metadata changed. This is a
   warning-only preflight for selective builds: it must never expand the
   worklist or alter cache/build decisions. */
static bool wsProjectOwnsPath(const WsModel *m, int idx, const char *path) {
  const char *root = m->projects[idx].root;
  if (!wsPathUnderRoot(path, root)) return false;
  size_t rootLen = strcmp(root, ".") == 0 ? 0 : strlen(root);
  /* Nested project roots belong to the most-specific project, not an outer
     project whose root happens to contain them. */
  for (int i = 0; i < m->projectCount; i++) {
    if (i == idx || !wsPathUnderRoot(path, m->projects[i].root)) continue;
    size_t otherLen = strcmp(m->projects[i].root, ".") == 0
                          ? 0 : strlen(m->projects[i].root);
    if (otherLen > rootLen) return false;
  }
  return true;
}

static bool wsProjectHasChangedInputs(const WsModel *m, int idx,
                                      const WsCacheMap *map) {
  for (size_t i = 0; i < map->n; i++) {
    const WsCachePath *r = &map->recs[i];
    if (r->type != 'S' && r->type != 'H') continue;
    if (!wsProjectOwnsPath(m, idx, r->path)) continue;
    struct stat sb;
    if (stat(r->path, &sb) != 0) return true;
    if ((long long)sb.st_size == r->size && wsCacheStampNs(&sb) == r->time)
      continue;
    uint64_t hash = wsStartupHash(r->path);
    if (!hash || hash != r->content) return true;
  }
  return false;
}

static void wsWarnSelectiveDivergence(const WsModel *m,
                                      const unsigned char *needed,
                                      const char *target) {
  WsCacheMap map = {0};
  if (!wsCacheLoadMap(&map)) return; /* first build / no trustworthy baseline */
  bool selectedDirty = false, unselectedDirty = false;
  char selected[WS_MAX_PROJECTS * (WS_NAME_LEN + 2)];
  char unselected[WS_MAX_PROJECTS * (WS_NAME_LEN + 2)];
  selected[0] = unselected[0] = '\0';
  for (int i = 0; i < m->projectCount; i++) {
    if (!wsProjectHasChangedInputs(m, i, &map)) continue;
    char *dst = needed[i] ? selected : unselected;
    size_t used = strlen(dst), cap = WS_MAX_PROJECTS * (WS_NAME_LEN + 2);
    if (used && used + 2 < cap) strcat(dst, ", ");
    if (strlen(dst) + strlen(m->projects[i].name) + 1 < cap)
      strcat(dst, m->projects[i].name);
    if (needed[i]) selectedDirty = true;
    else unselectedDirty = true;
  }
  wsCacheMapFree(&map);
  if (selectedDirty && unselectedDirty) {
    fprintf(stderr,
            "satria: warning: selective target '%s' has changed inputs in selected project(s) [%s] and unselected project(s) [%s].\n"
            "satria: warning: only the requested target/dependency closure will be built; other outputs may remain stale or become incompatible. Run 'satria -w' for a full workspace build.\n",
            target, selected, unselected);
  }
}

/*
 * workspace — implementasi mode `use workspace` (Buildfile.ws; nama lama
 * Buildfile.workspace masih diterima bila file baru tidak ada).
 *
 * Workspace adalah LAYER TIPIS di atas jalur build satu project yang sudah
 * ada: file workspace di-parse untuk daftar proyek + setting per proyek +
 * depends_on; tiap proyek disintesis jadi satu Buildfile (disimpan STABIL
 * di .satria/workspace/<nama>.Buildfile agar cache config & fast path tidak
 * miss tiap run), lalu dieksekusi lewat cmdBuild/cmdClean dengan cwd = root
 * proyeknya. FastState, fingerprint, library, compdb — semuanya ikut
 * otomatis tanpa diubah.
 *
 * Rute key di Buildfile.ws:
 *   projects = a, b           -> daftar proyek (validasi eksplisit)
 *   projects.<nama>.*         -> setting per proyek (alias `projects.<n> as
 *                                <singkatan>` diperbolehkan; batas titik
 *                                resolver menjaga `rupa` tak menelan
 *                                `rupamod`)
 *   key lain (compiler, flags, std, dst) -> default bersama semua proyek;
 *                                barisnya ikut ditulis apa adanya ke
 *                                Buildfile tiap proyek.
 *
 * Root proyek default = <root workspace>/<nama>; override: <n>.root.
 * Path relatif terhadap lokasi file workspace (aturan satria yang sudah
 * ada) — berarti relatif root workspace. Urutan build dari depends_on
 * (topological sort), bukan urutan `projects =`.
 */
static int wsRunCore(const char *cmd, int jobs, const char *only, const char *releaseSel) {
  profMark("ws-entry"); /* profil: pisahkan startup vs model-load */
  char wsDir[MAX_PATH];
  if (!fsGetCwd(wsDir, sizeof(wsDir))) {
    fprintf(stderr, "satria: cannot determine current directory\n");
    return 1;
  }
  WsModel m;
  const char *wsName = NULL; /* diisi wsModelLoad: nama file ws (pesan) */
  /* clean mendahulukan pengecekan .satria. Tanpa .satria tidak ada state yang
     perlu disinkronkan: startup TIDAK dijalankan, cukup kosongkan isi root
     output (bin/ build/ lib/ modules/). Dengan .satria, startup.state
     disinkronkan (diam bila sudah cocok), lalu clean berjalan. */
  wsCleanTouched = false;
  if (strcmp(cmd, "clean") == 0 && workspaceFileName()) {
    char satDir[WS_PATH_LEN];
    snprintf(satDir, sizeof(satDir), "%s/.satria", wsDir);
    if (!fsDirExists(satDir)) {
      if (!wsCleanRootOutputs(wsDir)) printf("> Nothing to clean (see Buildfile: clean)\n");
      return 0;
    }
  }
  /* Pratinjau `background = true` (lihat wsBackgroundPeek): fase startup
     dicetak di dalam wsModelLoad, sedangkan keputusan spawn baru dibuat
     setelah gerbang no-op — jadi niat spawn harus sudah diketahui lebih dulu. */
  bool spawnPending = strcmp(cmd, "build") == 0 && !bgIsChild() && wsBackgroundPeek(wsDir);
  if (!wsModelLoad(wsDir, &m, &wsName, true, spawnPending)) return 1;
  profMark("ws-model"); /* profil */
  if (strcmp(cmd, "build") == 0 && wsLogEnabled(&m)) {
    buildLogBegin();
    wsBuildLogActive = true;
  }
  if (!wsTopoSort(&m)) {
    wsModelFree(&m);
    return 1;
  }
  memset(wsLogFastOk, 0, sizeof(wsLogFastOk));

  /* `-g compdb` query-only: hasilkan CompDB merged SELURUH workspace +
     perbarui workspace_log, TANPA build dan TANPA mengubah build state
     (design/workspace_v1.md §No-op dan CompDB). `-w <nama>` tidak
     membatasi: compdb selalu mewakili seluruh workspace. */
  if (strcmp(cmd, "compdb") == 0) {
    wsModeSet(true);
    cmdsStatMemoSetCwd();
    bool cok = wsCompdbRefresh(&m);
    wsModeSet(false);
    wsModelFree(&m);
    return cok ? 0 : 1;
  }

  /* Dua mode (design/release.md revisi 2):
     - releaseSel : `-- key=value[,...]` — OVERRIDE setting release untuk
                    run ini (name, version, format, target->format; dsb).
                    Konfigurasi dasar tetap dari section release.*.
     - only       : `satria -w <proyek>` — build proyek + closure-nya;
                    release tetap workspace penuh bila release.* ada.
     - keduanya kosong : build + release seluruh workspace. */
  bool releaseOnly = releaseSel && *releaseSel;

  /* Override CLI (release -- name=binyan dst.): tangani target sebagai
     sinonim format (sintaks revisi 1) supaya transisi Buildfile lama
     yang memakai CLI tetap jalan. */
  if (releaseOnly) {
    for (const char *p = releaseSel; *p;) {
      char pair[WS_LINE_LEN];
      const char *comma = strchr(p, ',');
      size_t len = comma ? (size_t)(comma - p) : strlen(p);
      if (len == 0 || len >= sizeof(pair)) {
        fprintf(stderr, "satria: workspace: override release tidak valid\n");
        wsModelFree(&m);
        return 1;
      }
      memcpy(pair, p, len);
      pair[len] = '\0';
      char *eq = strchr(pair, '=');
      if (!eq) {
        fprintf(stderr,
                "satria: workspace: override '%s' tidak valid (pakai key=value, mis. name=rupa)\n",
                trim(pair));
        wsModelFree(&m);
        return 1;
      }
      *eq = '\0';
      char *k = trim(pair);
      char *v = trim(eq + 1);
      const char *key = strcmp(k, "target") == 0 ? "format" : k;
      if (strcmp(key, "format") == 0 && *v && strcmp(v, "tar") != 0 && strcmp(v, "deb") != 0 &&
          strcmp(v, "none") != 0) {
        fprintf(stderr, "satria: workspace: format '%s' tidak dikenal (tar, deb)\n", v);
        wsModelFree(&m);
        return 1;
      }
      if (strcmp(k, "name") == 0 && !wsValidName(v)) {
        fprintf(stderr, "satria: workspace: nama release '%s' tidak valid\n", v);
        wsModelFree(&m);
        return 1;
      }
      char ln[WS_LINE_LEN];
      snprintf(ln, sizeof(ln), "%s = %s", key, v);
      if (!listAdd(&m.release.lines, ln) || (m.release.present = true, false)) {
        wsModelFree(&m);
        return 1;
      }
      if (!comma) break;
      p = comma + 1;
    }
  }

  if (!wsValidateRelease(&m)) {
    wsModelFree(&m);
    return 1;
  }

  /* Config per proyek disusun di memori tiap pass (wsBuildConfig) —
     tidak ada file sintesis (design/workspace_v1.md §Pemisahan Jalur). */
  profMark("workspace-parse");

  unsigned char needed[WS_MAX_PROJECTS] = {0};
  if (releaseOnly || (only && *only)) {
    if (only && *only) {
      int target = wsFindProject(&m, only);
      if (target < 0) {
        fprintf(stderr, "satria: workspace: unknown project '%s'\n", only);
        wsModelFree(&m);
        return 1;
      }
      unsigned char visiting[WS_MAX_PROJECTS] = {0};
      if (!wsMarkNeeded(&m, target, needed, visiting)) {
        wsModelFree(&m);
        return 1;
      }
    } else {
      for (int i = 0; i < m.projectCount; i++)
        needed[i] = 1;
    }
    /* Build selektif juga menarik proyek yang dirujuk lewat library
       (library.<os> = <nama proyek>) — transitif; siklus library sah dan
       tidak membuat loop ini macet (fixpoint sederhana). */
    wsExpandLibDeps(&m, needed);
    if (strcmp(cmd, "build") == 0 && only && *only)
      wsWarnSelectiveDivergence(&m, needed, only);
  } else {
    for (int i = 0; i < m.projectCount; i++)
      needed[i] = 1;
  }

  int rc = 0;
  profMark("ws-parse+synth");

  /* Gerbang workspace per-closure: hanya entri milik proyek needed yang
     di-stat — `-w <nama>` tidak membayar validasi seluruh workspace. */
  if (strcmp(cmd, "build") == 0) {
    /* .satria di direktori anggota = state lama: diabaikan + peringatan
       (design/workspace_v1.md: state hanya di root workspace). */
    for (int i = 0; i < m.projectCount; i++) {
      char mp[WS_PATH_LEN];
      snprintf(mp, sizeof(mp), "%s/%s/.satria", wsDir, m.projects[i].root);
      if (fsDirExists(mp))
        fprintf(stderr, "satria: workspace: %s/.satria diabaikan — state workspace di root (%s)\n",
                m.projects[i].root, WS_CACHE_FP);
    }
  }

  wsLegacyStateRemove(); /* sisa workspace.state lama dibuang di setiap run, termasuk no-op */

  /* Cache GET no-op: semua entri fingerprint.cache + artifact.cache cocok ->
     tidak ada yang dibangun. Release TIDAK lagi menutup gerbang: artefak dan
     input release divalidasi lewat .satria/artifact.cache (design/startup.md
     §9); perubahan membatalkan no-op dan memicu ulang fase release. Status
     (SATRIA_STATUS) mengikuti hasil validasi yang sama. */
  if (strcmp(cmd, "build") == 0 && wsCacheEnabled() && !(only && *only) &&
      !(releaseSel && *releaseSel)) {
    bool hit = wsCacheNoopValid(&m);
    const char *rs = getenv("SATRIA_STATUS");
    if (rs && *rs && strcmp(rs, "0") != 0)
      fprintf(stderr, "satria status: mode=workspace gate=%s\n", hit ? "valid" : "cold");
    if (hit) {
      /* Detail laporan (§10): putar laporan tersimpan — blok STATUS di
         dalamnya adalah pesan no-op (seragam kedua mode). Tanpa laporan:
         pesan no-op dicetak mandiri. */
      if (reportBlobHas())
        reportBlobShow();
      else
        printf("STATUS\n  > Nothing to build\n  > Cache valid: %s\n", WS_CACHE_FP);
      wsModelFree(&m);
      return 0;
    }
  }
  /* `background = true` (design/background.md §2): spawn HANYA setelah gerbang
     no-op router di atas — run yang tidak ada yang dikerjakan tetap dilayani
     langsung (STATUS "Nothing to build"), tanpa child. Induk meluncurkan child
     memakai argv asli (sama persis: -w/-j/-- selektor) lalu keluar rc 0;
     child (bgIsChild) melewati cabang ini. */
  if (strcmp(cmd, "build") == 0 && !bgIsChild() && wsBackgroundEnabled(&m)) {
    /* Induk tidak menjalankan build: tutup relay build.log yang dibuka di
       atas (kembalikan stderr) sebelum keluar — child membuka relaynya
       sendiri. */
    if (wsBuildLogActive) {
      buildLogEnd(0);
      wsBuildLogActive = false;
    }
    /* Rekaman startup (cold) disimpan di sini — satu-satunya tempat startup
       run background tampil adalah replay `satria watch`. */
    bgStartupRecord(true);
    int brc = bgSpawn(BG_WS);
    wsModelFree(&m);
    return brc;
  }
  /* Run build penuh: tangkap laporan (tee) -> report.blob bila sukses.
     Jalur cache mode ws untuk baris STATUS di dalamnya. */
  if (strcmp(cmd, "build") == 0) {
    reportBlobCachePath(WS_CACHE_FP);
    reportBlobBegin();
    profMark("ws-reportbegin"); /* profil */
  }

  /* Worklist sekali: proyek needed urut topo + level dependensi. */
  WsWork work[WS_MAX_PROJECTS];
  int wcount = wsBuildWorklist(&m, needed, work);
  profMark("ws-worklist"); /* profil */

  if (strcmp(cmd, "build") == 0) {
    /* Mode ws aktif sekali per run: cache sumber (.satria/sources.cache) dipakai
       lintas proyek — satu traversal per direktori (design §Pipeline). */
    wsModeSet(true);
    cmdsStatMemoSetCwd(); /* memo stat ber-key cwd root workspace */
    /* CompDB harus tersedia meskipun kompilasi/link gagal. */
    profMark("ws-compdb-pre"); /* profil */
    if (!wsCompdbRefresh(&m)) {
      reportBlobFinish(false);
      wsModeSet(false);
      wsModelFree(&m);
      return 1;
    }
    profMark("ws-compdb"); /* profil */
    /* FASE 1 — library pass: tiap proyek mengkompilasi source-nya dan
       mengemas lib<name>.a/.so TANPA link binary. SELURUH worklist
       diserahkan ke penjadwal siap-dep (wsRunBatch): anak diluncurkan
       segera setelah depends_on-nya selesai — rupamod -> bade langsung,
       core berjalan menindih tanpa menunggu maupun ditunggu. Urutan topo
       dijaga penjadwal (archive rupamod jadi sebelum fase library bade
       meng-embed); jalur serial (-j1 / SATRIA_WS_PAR=0) tetap bertingkat
       dependensi persis perilaku lama. Alasan & ukuran gain (gap
       hedit/libhdr bench/ws.sh) ada di komentar modul ws_batch.c. */
    rc = wsRunBatch(&m, work, wcount, jobs, cmd, wsDir, true);
    /* FASE 2 — binary pass: link binary setelah SEMUA library tersedia
       (cdeps bisa siklik — lihat catatan wsBuildWorklist). Proyek
       kemasan (output.binary = false) tuntas di fase library — Lewati
       SENYAP: laporan hanya menampilkan proyek yang dikerjakan fase ini
       (design/build_report.md). */
    if (rc == 0) {
      WsWork bin[WS_MAX_PROJECTS];
      int bn = 0;
      for (int i = 0; i < wcount; i++)
        if (!wsProjectArchiveOnly(&m.projects[work[i].idx])) bin[bn++] = work[i];
      rc = wsRunBatch(&m, bin, bn, jobs, cmd, wsDir, false);
    }
  } else {
    /* clean: satu pass serial (binary pass), tanpa fase library. */
    for (int i = 0; rc == 0 && i < wcount; i++)
      rc = wsRunProject(&m, work[i].name, cmd, jobs, false);
  }
  if (strcmp(cmd, "build") == 0) wsModeSet(false);

  /* FASE 3 — release pass: pack SATU paket workspace pada root workspace,
     output ke dist/ (design/release.md revisi 2). Bila tidak ada section
     release.* sama sekali (dan tidak ada override CLI), langkah ini
     dilewati — workspace tanpa release berperilaku seperti dulu. */
  /* Build selektif (`-w <nama>`) tidak membuat paket: release hanya jalan untuk
     build penuh atau bila diminta eksplisit (`release`). */
  bool selectiveBuild = only && *only && !releaseOnly;
  if (rc == 0 && strcmp(cmd, "build") == 0 && m.release.present && !selectiveBuild) {
    profMark("release-pre");
    rc = wsRelease(&m, jobs, wsDir);
    profMark("release-post");
  }

  if (profOn())
    profReport(); /* delta antar fase workspace (build no-op:
                                 seluruh fase berada antara dua penanda) */

  /* State workspace = cache (fingerprint.cache + opts.cache). Build sukses ->
     simpan cache; clean -> cache dibiarkan, validasi berikutnya yang menilai. */
  if (strcmp(cmd, "build") == 0 && rc == 0) {
    profMark("ws-save-pre"); /* profil */
    if (!selectiveBuild && wsCacheEnabled()) wsCacheSave(&m, wsName);
    profMark("ws-cachesave"); /* profil */
    /* Sinkron dua arah sources <-> opts: run sukses bisa menulis ulang
       sources.cache (re-walk srcSave di fase mana pun, merge P wsCacheSave)
       SETELAH route proyek tercatat — segarkan semua route sekarang, supaya
       gate berikutnya membandingkan stempel terkini. Run gagal sengaja tidak
       disinkronkan: route basi menolak no-op sampai build berhasil. */
    optsSourcesRouteSync();
    wsLegacyStateRemove(); /* workspace.state tak lagi ditulis: buang sisa lama */
    profMark("ws-routesync"); /* profil */
  } else if (strcmp(cmd, "clean") == 0) {
    wsLegacyStateRemove();
    bool touched = wsCleanRootOutputs(wsDir) || wsCleanTouched;
    if (!touched) printf("> Nothing to clean (see Buildfile: clean)\n");
    wsCleanTouched = false;
  }

  /* Tutup tangkapan laporan: sukses -> report.blob; gagal -> tahan lama.
     (Tanpa-opsi bila Begin tak dipanggil — cmd selain build.) */
  if (strcmp(cmd, "build") == 0) reportBlobFinish(rc == 0);
  profMark("ws-report"); /* profil */

  wsModelFree(&m);
  return rc;
}

/* Entri mode workspace: jalankan inti, lalu tulis state yang masih pending ke
   caches/state.cache sekali (atomik). Kegagalan tulis = cache dilewati, bukan
   kegagalan build. */
int workspaceRun(const char *cmd, int jobs, const char *only, const char *releaseSel) {
  wsBuildLogActive = false;
  /* Gerbang background (design/background.md §4): tolak build ganda
     (§4.1) dan blok clean (§4.4) selama build background berjalan. Child
     sendiri dilewati — dialah pemilik PID di berkas pid. */
  if (!bgIsChild()) {
    int gate = strcmp(cmd, "build") == 0    ? bgBuildGate(BG_WS)
               : strcmp(cmd, "clean") == 0  ? bgCleanGate(BG_WS)
                                            : -1;
    if (gate >= 0) return gate;
  } else {
    bgChildBegin(BG_WS);
  }
  int rc = wsRunCore(cmd, jobs, only, releaseSel);
  if (bgIsChild()) bgChildFinish(rc == 0); /* §4.3: status lalu hapus PID */
  if (!stateBlobFlush())
    fprintf(stderr, "satria: peringatan: state workspace tidak tertulis (cache dilewati)\n");
  if (wsBuildLogActive) {
    buildLogEnd(rc);
    wsBuildLogActive = false;
  }
  profMark("ws-blobflush"); /* profil */
  if (profOn()) profReport(); /* profil: tutup blok post-phase */
  return rc;
}
const char *workspaceFileName(void) {
  if (fsFileExists(WORKSPACE_FILENAME)) return WORKSPACE_FILENAME;
  if (fsFileExists(WORKSPACE_FILENAME_LEGACY)) return WORKSPACE_FILENAME_LEGACY;
  return NULL;
}
bool workspaceFileExists(void) {
  return workspaceFileName() != NULL;
}

/* Status kasar mode workspace untuk SATRIA_STATUS (bootstrap.c): cache
   fingerprint ada. Bukan keputusan build — lihat wsCacheNoopValid. */
bool wsStateFileExists(void) {
  return fsFileExists(WS_CACHE_FP);
}
/*
 * workspaceEnumerate — parse & validasi model workspace untuk enumerasi
 * (dipakai `satria profile workspace=`): semua validasi model yang sama
 * dengan workspaceRun, TANPA sintesis/build apa pun. nama proyek hasil
 * topo sort diisi ke projectNames[]. Buffer internal model statis —
 * pointer hasil valid sampai panggilan berikutnya.
 */
int workspaceEnumerate(const char *wsDir, int *count, const char **projectNames, int maxProjects) {
  static WsModel mM;
  WsModel *m = &mM;
  *count = 0;

  if (!wsModelLoad(wsDir, m, NULL, false, false)) return 1;
  if (!wsValidateRelease(m)) {
    wsModelFree(m);
    return 1;
  }
  if (!wsTopoSort(m)) {
    wsModelFree(m);
    return 1;
  }

  /* Salin nama ke buffer statis agar valid setelah wsModelFree. */
  static WsProject namesStorage[WS_MAX_PROJECTS];
  (void)namesStorage;
  static char nameBufs[WS_MAX_PROJECTS][WS_NAME_LEN];
  for (int i = 0; i < m->order.count && *count < maxProjects; i++) {
    /* Indeks dibaca SEKALI: `a[(*count)++] = b[*count]` tidak berurutan
       (UB) dan bisa menunjuk slot berikutnya yang belum terisi. */
    int idx = (*count)++;
    copyStr(nameBufs[idx], WS_NAME_LEN, m->order.items[i]);
    projectNames[idx] = nameBufs[idx];
  }
  wsModelFree(m);
  return 0;
}

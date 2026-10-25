#ifndef SATRIA_V0_1_0_CMDS_INTERNAL_H
#define SATRIA_V0_1_0_CMDS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../config.h" /* Config, List */
#include "../portability.h" /* ProcHandle — tangkapan output compiler */

/* Ukuran buffer path untuk state workspace. WS_SCAN = WS_PATH_LEN - 1 sebagai teks
   lebar sscanf: lebar baca selalu lebih kecil dari buffer tujuan (-Wfortify-source). */
#define WS_PATH_LEN 3072
#define WS_SCAN "3071"

/*
 * cmds_internal — helper bersama antar-file modul cmds/.
 *
 * commands.c menyimpan command publik (cmdBuild/cmdInit/showHelp; cmdClean
 * ada di cmds_state.c). File di sini menyimpan fase-fase build yang
 * dipisah dari commands.c: fingerprint + fast no-op snapshot (cmds_state.c)
 * dan fase library/kompilasi paralel/emit header versi (cmds_lib.c).
 * Semua dulunya static di commands.c — di-prefix cmds* agar unik.
 */

/* ==================== cmds_lib.c ==================== */

/* Nama file library sesuai toolchain: lib<name>.a / <name>.lib (statis),
   lib<name>.so / .dylib / <name>.dll (dinamis). */
void cmdsLibStaticPath(const Config *c, char *out, size_t n);
void cmdsLibSharedPath(const Config *c, char *out, size_t n);

/* True bila SATU varian library (statis/shared) sudah ada dan lebih baru
   daripada seluruh object inputnya — varian itu boleh dilewati. */
bool cmdsLibVariantUpToDate(const Config *c, bool isStatic, const List *srcs);

/* Fase library: kemas statis (ar / lib) dan/atau link shared dari object
   source + embedded. true bila semua varian yang diminta sukses.
   linkPathLibs=false (fase library workspace) melewatkan entri library
   ber-path proyek lain pada link shared (lihat cmds_lib.c). */
bool cmdsBuildLibraryEx(const Config *c, const List *srcs, bool linkPathLibs);

/* Build penuh (fase normal binary): library lintas-proyek disertakan. */
bool cmdsBuildLibrary(const Config *c, const List *srcs);

/* Kompilasi paralel (-jN). Return false bila ada yang gagal/interrupt.
   quiet=true menekan baris status per job (dipakai `satria profile`, yang
   outputnya harus bersih agar laporan tidak tercampur log kompilasi). */
bool cmdsRunParallelJobs(const Config *c, const char *inc, const char *wf, const List *srcs,
                         char *objPath, size_t objCap, int jobs, int *outCompiled,
                         int *outFailed, int *outInterrupted, bool quiet);

/*
 * Cetak output compiler yang ditangkap (procCaptureTake) ke stdout — UTUH
 * setelah baris progress, sehingga urutan terjaga dan blok tidak bisa
 * tercampur job lain — lalu salin ke build.log bila relay log aktif.
 */
void cmdsEmitCapture(ProcHandle *h);

/* Terbitkan build/version.h dari .satria-version (root project). */
bool cmdsEmitVersionHeader(const Config *c, bool *changed);

/* ============ fase pack (proyek binary + pack.*) ============ */

/*
 * Jalankan packRun untuk proyek binary dengan pack.*: dipanggil di KEDUA
 * jalur sukses cmdBuild, setelah fingerprint disimpan. Return false bila
 * kemasan gagal (state tidak direkam — build berikutnya mengulang penuh).
 * *fastStateSaved diset true bila state sudah disimpan di sini; proyek
 * tanpa pack membiarkannya false dan pemanggil menyimpan seperti biasa.
 */
bool cmdsPackAfterBinary(const Config *c, bool *fastStateSaved, const List *srcs,
                         const char *target, const char *buildfilePath);

/* Proyek kemasan murni (output.binary = false + pack.*): embedded dulu
   (arsip untuk proyek lain), lalu packaging, lalu summary ringkas. */
int cmdsPackOnlyProject(const Config *c);

/* ==================== commands.c (pass build) ==================== */

/*
 * Isi pass build satu proyek — lihat definisi di commands.c. Koleksi
 * source, fingerprint, decide, compile, embed, library, link, state —
 * TANPA loadConfig dan TANPA chdir (Config & cwd disiapkan pemanggil:
 * cmdBuildEx untuk single, wsCmdBuild untuk workspace). Config by-value.
 */
int cmdsBuildPassRun(int jobs, Config c, const char *buildfilePath, bool libOnly);

/* Koleksi source .c/.cpp dari Config — entri file langsung, folder discan
   rekursif (walk cache .satria/sources.cache bila mode workspace aktif); hasil
   terurut (urutan link reprodusibel). */
void cmdsCollectSources(const Config *c, List *out);

/* Cek tabrakan path object antar-sumber (dua sumber -> object yang sama).
   Mencetak pasangan yang bentrok ke stderr; false = ada tabrakan. */
bool cmdsCheckObjectCollisions(const Config *c, const List *srcs);

/* Jalankan logika bersih dari Config yang sudah tersusun — TANPA loadConfig
   dan TANPA chdir (cmdClean memakainya setelah loadConfig; wsRunProject
   memakainya dengan Config hasil wsBuildConfig). Path state mengikuti
   cmdsStateSetWsProject. Return 0 = sukses. */
int cmdsCleanRun(const Config *c);
/* Sama dengan cmdsCleanRun tanpa pesan "Nothing to clean": *anyOut = ada entri terhapus. */
int cmdsCleanRunEx(const Config *c, bool *anyOut);
/* Kosongkan isi folder (folder tetap ada); true bila semua entri terhapus. */
bool cmdsEmptyDir(const char *dir, int *removed);

/* ==================== cmds_state.c ==================== */

/*
 * Lokasi file state (state_path): mode workspace membangun SEMUA proyek
 * dari satu cwd (root workspace), jadi file state per proyek diprefiks
 * nama di .satria/workspace/ (design/workspace_v1.md §State & Cache):
 *   .satria/workspace/<proyek>.build.fingerprint | .ws.state | .ws.lib.state
 *   .satria/workspace/<proyek>.deps.cache | .src
 * Mode single tetap .satria/<leaf> relatif cwd (root proyek). Dipanggil
 * wsCmdBuild sebelum tiap pass proyek; "" = kembali ke jalur lama.
 */
void cmdsStateSetWsProject(const char *name); /* "" / NULL = clear */
const char *cmdsStateWsProject(void);         /* NULL bila mode single */
void cmdsStatePath(char *out, size_t n, const char *leaf);
/* mkdirs induk path state (.satria atau .satria/workspace) sebelum tulis. */
void cmdsStateMkdirFor(const char *path);

typedef struct {
  uint64_t compile;
  uint64_t link;
  uint64_t sources;
} CmdsBuildFingerprint;

/* Hash konfigurasi compile/link + set source (FNV-1a). */
void cmdsFingerprintMake(const Config *c, const List *srcs, CmdsBuildFingerprint *fp);
bool cmdsFingerprintLoad(CmdsBuildFingerprint *fp);
/* Simpan ke .satria/build.fingerprint. libOnly = fase library workspace:
   hanya hash compile yang dikommit; hash sources/link dipertahankan dari
   catatan sebelumnya agar pass binary run yang sama masih mendeteksi
   source set / link config berubah (source dihapus -> link diulang). */
void cmdsFingerprintSave(const CmdsBuildFingerprint *fp, bool libOnly);

/* Buang fingerprint tersimpan (dipanggil saat build gagal/terinterupsi
   setelah konfigurasi berubah). */
void cmdsFingerprintInvalidate(void);

/* Fast no-op: true bila state build terakhir masih persis sama — summary
   diulang dari cache dan build dilewati sepenuhnya. libOnly memilih cache
   fase library (.satria/caches/lib.cache); fase binary memakai
   .satria/build.state. */
bool cmdsFastStateValid(const char *buildfilePath, bool libOnly);

/* ==== .satria/opts.cache (router; design/startup.md §3/§4) ====
   Indeks file caches (deps/lib/embed) yang memuat entri tiap proyek. No-op
   membaca router LEBIH DULU (optsRouterValidate): cache yang dirutekan
   hilang/kosong -> validasi dibatalkan tanpa memuat build.state.
   optsRouterRecord dipanggil setelah state tersimpan (SET sukses) dan
   mengganti penuh set entri proyek. Clean mempertahankan opts.cache;
   pengosongan isi caches membuat router gagal sehingga run berikutnya
   rebuild dan merekam ulang router. */
bool optsRouterRecord(const Config *c, bool libOnly);
bool optsProjMetaStamp(uint64_t proj, uint64_t content); /* metadata grup P (§4) */
/* Buang sisa state lama (file tulisan versi lama yang sudah digantikan
   blob/store) — sekali per run + saat clean (design/startup.md audit). */
void cmdsLegacyStateSweep(void);
bool optsRouterValidate(void);

/* ==== caches/lib.cache (fast-state fase library; design/startup.md §3/§4) ====
   File terpusat `.satria/caches/lib.cache` (cache_store) menggantikan
   .satria/build.lib.state (single) dan .satria/workspace/<proyek>.ws.lib.state:
   satu entri meta (payload token k=v; lib=<path> diulang untuk daftar
   library workspace) + satu entri per input (payload "<kind> [<content16hex>]
   <path>" — F membawa content16hex sebagai bukti sentuh (design §4),
   kompatibel mundur tanpanya; kind F file / D direktori / M hilang / L
   verdict ld). Fase binary masih memakai format lama — migrasinya lewat
   fingerprint.cache (opt-in). */
#define WS_CACHES_DIR ".satria/caches"
#define LIB_CACHE_FILE ".satria/caches/lib.cache"

typedef struct {
  char kind; /* 'F' file, 'D' direktori, 'M' hilang, 'L' verdict ld */
  char path[WS_PATH_LEN];
  long long mtime; /* ns; diabaikan untuk M dan L */
  long long size;  /* hanya F */
  unsigned long long content; /* FNV-1a isi (hanya F); 0 = tanpa bukti (entri lama) */
} LibStampRec;

typedef struct {
  char cwd[WS_PATH_LEN];
  char buildfile[WS_PATH_LEN];
  long long bfm, bfs;
  char target[WS_PATH_LEN * 2]; /* "" = tanpa target ("-" di file) */
  long long tm, ts;
  int skipped, compiled;
  bool libRequested, libStaticUp, libSharedUp;
  char libStaticPath[WS_PATH_LEN * 2];
  char libSharedPath[WS_PATH_LEN * 2];
  long long libStaticSize, libSharedSize;
  char **libs; /* opsional (workspace): daftar path library */
  int nlibs;
} LibCacheMeta;

/* Alamat proyek untuk cache terpusat: nama proyek (workspace — cmdsState
   sudah menyetelnya per pass) atau cwd (single / sandbox profile). Dipakai
   caches/lib.cache dan caches/deps.cache. */
uint64_t cmdsCacheProjAddr(void);
/* Simpan meta+stamp fase library (timpa entri proyek ini, atomik). */
bool libCacheSave(const LibCacheMeta *m, const LibStampRec *stamps, int n);
/* Muat entri proyek ini; false bila tidak ada/rusak. Pemanggil wajib
   libCacheMetaFree(m) dan free(stamps). */
bool libCacheLoad(LibCacheMeta *m, LibStampRec **stamps, int *n);
void libCacheMetaFree(LibCacheMeta *m);

/* Jalur fast-state workspace (cmds_wsstate.c). Aktif hanya saat workspace
   memanggil cmdBuildEx; single project tidak pernah mengaktifkannya. */
struct DepCache; /* forward: didefinisikan di deps.h */
void wsModeSet(bool on);
bool wsFastStateValid(const char *buildfilePath, bool libOnly);
bool cmdsStatCached(const char *path, int64_t *mtimeNs, long long *size);
void cmdsStatMemoSetCwd(void);
void cmdsStatMemoForget(const char *path);
int64_t cmdsMTimeDecide(const char *path);
/* Parser & validasi satu baris format state F/D/X (format bersama ws.state dan
   workspace.state lama — lihat header cmds_wsstate.c):
     F <mtime_ns> <size> [<content16hex>] <path> | D <mtime_ns> 0 <path> | X 0 0 <path>
   Baris F baru membawa content16hex (FNV-1a isi, bukti sentuh design §4);
   baris lama tanpa kolom itu tetap sah (content = 0). path dan content
   harus menampung WS_PATH_LEN karakter (lebar WS_SCAN). */
bool cmdsParseStateEntry(const char *line, char *kind, long long *mt, long long *sz,
                         unsigned long long *content, char *path);
/* true bila baris F/D/X masih cocok stamp disk saat ini (F: mtime+size,
   D: mtime, X: path tidak ada); false bila parse gagal atau stamp beda.
   F dengan content: stamp beda tapi hash isi sama (sentuh) tetap dianggap
   segar tanpa rebuild — jalur incremental lama menyusul perilaku cache on. */
bool cmdsStateEntryFresh(const char *line);
/* FNV-1a 64 atas isi file (padanan fastStateHash untuk cmds_wsstate.c);
   0 = tak terbaca. */
uint64_t cmdsStateFileHash(const char *path);
void cmdsStatMemoForgetTree(const char *dir);
void cmdsStatMemoForgetOutputs(const char *buildDir, const List *items);
void cmdsSingleSourcesCacheBegin(void);
void cmdsSingleSourcesCacheEnd(void);
void stateWalkDirCached(const char *dir, const char *ext, List *out);
void stateWalkDirStamps(const char *dir, const char *ext, List *out);
void wsAddSourceDirs(List *items);
bool wsModeActive(void);
void wsFastStateSave(struct DepCache *dc, const Config *c, const List *srcs, const char *target,
                     const char *buildfilePath, bool libOnly);
/* Kosongkan memo stat validasi fast-state; dipanggil sebelum build mengubah file. */
void cmdsStatMemoReset(void);

/* Rekam state build sukses (mtime+size seluruh input) untuk fast no-op.
   target = "" (fase library tanpa library) direkam sebagai "-" — validasi
   murni dari stamp input + object. */
void cmdsFastStateSave(const Config *c, const List *srcs, const char *target,
                       const char *buildfilePath, bool libOnly);

#endif /* SATRIA_V0_1_0_CMDS_INTERNAL_H */

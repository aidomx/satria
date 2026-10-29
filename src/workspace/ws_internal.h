/* ws_internal.h — deklarasi internal modul workspace (src/workspace/).
 * Berisi tipe model, makro bersama, dan prototipe lintas-file.
 * Tiap file .c di folder ini meng-include header ini; API publik tetap di
 * src/uses/workspace.h.
 */
#ifndef SATRIA_WORKSPACE_INTERNAL_H
#define SATRIA_WORKSPACE_INTERNAL_H

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../uses/workspace.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "../cmds/cmds_internal.h"
#include "../commands.h"
#include "../compdb.h"
#include "../config.h"
#include "../compile.h"
#include "../embed.h"
#include "../pack/pack.h"
#include "../pack/pack_internal.h"
#include "../portability.h"
#include "../prof/prof.h"
#include "../uses/alias.h"
#include "../cache/cache_store.h"
#include "../cache/state_blob.h"

#ifndef _WIN32
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ---- makro & tipe bersama ---- */

#define WS_NAME_LEN 64
#define WS_LINE_LEN 1024


/* State startup persisten (design/startup.md §3): konfigurasi hasil
   penerjemahan Buildfile.ws. DIPERTAHANKAN oleh clean (§6); divalidasi ulang
   lewat hash file workspace setiap run. */
#define WS_STARTUP_FILE ".satria/startup.state"

typedef struct {
  char name[WS_NAME_LEN];
  char root[MAX_PATH];
  List lines; /* baris setting per proyek TANPA prefix projects.<nama>. */
} WsProject;

/*
 * ReleaseConfig — konfigurasi release TUNGGAL workspace (design/
 * release.md revisi 2). Section release.* di Buildfile.ws; key-nya sama
 * dengan pack.* karena fase release menjalankan pack engine yang sama
 * pada root workspace. Isi paket bebas: file/folder proyek mana pun.
 */
typedef struct {
  bool present; /* section release.* disebut di Buildfile.ws */
  List lines;   /* baris "release.<key> = <value>" apa adanya */
} WsReleaseCfg;

typedef struct {
  WsProject projects[WS_MAX_PROJECTS];
  int projectCount;
  WsReleaseCfg release;
  List commonLines; /* key selain projects/projects.<nama> — default bersama */
  List order;       /* urutan build hasil topo sort (nama) */
  List depends[WS_MAX_PROJECTS];
  /* Proyek lain yang dirujuk lewat library.<os> = <nama proyek> (mis.
     rupalib.linux = ruka). TIDAK masuk topo sort — siklus library antar
     proyek (rupa <-> ruka) sah; dua fase build yang menyelesaikannya.
     Dipakai untuk memperluas build selektif `-w <nama>`. */
  List libDeps[WS_MAX_PROJECTS];
  /* Stage fase release: klarifikasi run CLI di sini bila ada. */
  char releaseStage[MAX_PATH];
} WsModel;

#define WS_STARTUP_MAGIC "SATRIA-STARTUP-1"

typedef struct {
  int idx;          /* indeks proyek di WsModel */
  const char *name; /* nama proyek (m->order.items[i], valid selama run) */
  int level;        /* 0 = tanpa dependensi needed; batch = level sama */
} WsWork;


#define WS_CACHE_DIR ".satria/caches"
#define WS_CACHE_FP ".satria/caches/fingerprint.cache"
/* Grup global fingerprint CompDB di state.cache (bukan grup proyek —
   distamp di wsCacheSave agar tak ada baris P polos yang tersisa). */
#define WS_COMPDB_GLOBAL "__satria_compdb__"
#define WS_CACHE_SOURCES ".satria/sources.cache"

typedef struct {
  uint64_t key;
  char type; /* S sumber, D direktori, O output, W Buildfile, H header */
  /* Metadata §4 (optimasi no-op): hash isi, stempel ns, ukuran. Ditulis
     bersama peta P di sources.cache sehingga jalur cepat bisa memvalidasi
     tanpa membuka fingerprint.cache. type D: content = 0, size = 0. */
  uint64_t content;
  long long time;
  long long size;
  int hasMeta; /* 1 = baris memuat <content> <time> <size> (format baru) */
  char path[WS_PATH_LEN];
} WsCachePath;

typedef struct {
  WsCachePath *recs;
  size_t n, cap;
  int meta; /* 1 = SEMUA entri ber-metadata -> fast path P sah (design/startup.md §4) */
} WsCacheMap;

#define WS_ROUTER_FILE ".satria/opts.cache"
#define WS_ROUTER_NAME "fingerprint"
#define WS_ROUTER_KEY "workspace"

#define WS_MEMBER_MAX_UP 16
/* ---- prototipe lintas-modul ---- */
int workspaceEnumerate(const char *wsDir, int *count, const char **projectNames, int maxProjects);
const char *workspaceFileName(void);
int workspaceRun(const char *cmd, int jobs, const char *only, const char *releaseSel);
void wsAddCommon(WsModel *m, const char *key, const char *value);
void wsApplyCdeps(const WsModel *m, const WsProject *p, Config *c);
bool wsBuildConfig(const WsModel *m, int idx, Config *c);
int wsBuildWorklist(const WsModel *m, const unsigned char *needed, WsWork *work);
bool wsCacheEnabled(void);
bool wsCacheLoadMap(WsCacheMap *m);
const WsCachePath *wsCacheMapFind(const WsCacheMap *m, uint64_t key);
void wsCacheMapFree(WsCacheMap *m);
/* Gate no-op workspace: router -> artefak -> P-fast (metadata peta path)
   -> stamp grup per proyek -> pemeriksaan penuh fingerprint. */
bool wsCacheNoopValid(const WsModel *m);
void wsCacheSave(const WsModel *m, const char *wsName);
long long wsCacheStampNs(const struct stat *sb);
bool wsCleanRootOutputs(const char *wsDir);
bool wsCompdbRefresh(const WsModel *m);
WsProject *wsEnsureProject(WsModel *m, const char *name);
void wsExpandLibDeps(const WsModel *m, unsigned char *needed);
int wsFindProject(const WsModel *m, const char *name);
bool wsIsEngineSection(const char *seg);
bool wsIsRelPath(const char *item);
bool wsLibraryValue(const WsModel *m, const WsProject *owner, const char *value, char *out, size_t n);
bool wsListHas(const List *l, const char *s);
uint64_t wsLogCfgHash(const WsModel *m, int idx);
/* Stamp proyek utk metadata grup P (§4): hash konfigurasi + daftar sumber
   kanonik. Dipakai identik oleh wsCacheSave dan gate no-op. */
uint64_t wsProjStamp(const WsModel *m, int idx);
uint64_t wsLogCompdbFp(void);
uint64_t wsLogFileHash(const char *path);
bool wsLogLoadSrcs(const char *name, List *out);
bool wsLogReadEntry(const char *name, uint64_t *cfg, uint64_t *src, uint64_t *cdb);
void wsLogWrite(const WsModel *m, const uint64_t *cfgH, const uint64_t *srcH, uint64_t cdb);
bool wsMapHeaders(const char *root, const char *value, bool global, const char *proj, bool split, char *outPub, char *outInt, size_t n);
bool wsMapList(const char *root, const char *value, bool headersForm, char *out, size_t n);
bool wsMarkNeeded(const WsModel *m, int idx, unsigned char *needed, unsigned char *visiting);
void wsModelFree(WsModel *m);
bool wsBackgroundPeek(const char *wsDir);
bool wsModelLoad(const char *wsDir, WsModel *m, const char **wsNameOut, bool cacheWrite,
                  bool spawnPending);
bool wsParse(WsModel *m, const char *path);
bool wsProjectArchiveOnly(const WsProject *p);
int wsRelease(WsModel *m, int jobs, const char *wsDir);
void wsRouterDrop(void);
bool wsRouterRecord(void);
bool wsRouterValidate(void);
int wsRunBatch(const WsModel *m, const WsWork *batch, int n, int jobs, const char *cmd, const char *wsDir, bool libOnly);
int wsRunProject(const WsModel *m, const char *name, const char *cmd, int jobs, bool libOnly);
bool wsSplitKV(char *line, char *key, size_t kn, char **val);
uint64_t wsStartupHash(const char *path);
bool wsStartupLoad(WsModel *m, const char *wsFile, const char *wsName, const char *cacheFile);
void wsStartupSave(const WsModel *m, const char *wsFile, const char *wsName, const char *cacheFile);
bool wsTopoSort(WsModel *m);
bool wsValidName(const char *s);
bool wsValidatePackMerges(WsModel *m);
bool wsValidateRelease(WsModel *m);

extern bool wsCleanTouched;
extern bool wsGuardQuiet;
extern bool wsLogFastOk[WS_MAX_PROJECTS];

#endif /* SATRIA_WORKSPACE_INTERNAL_H */

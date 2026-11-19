#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../util.h"

/* Gabung dua potongan path tanpa snprintf: jalur benchmark dibangun
   bertingkat (workdir -> tool -> file), dan snprintf bertingkat memicu
   -Wformat-truncation palsu walau ukuran sebenarnya aman. */
static inline void benchPathCat(char *out, size_t n, const char *a, const char *b) {
  size_t la = a ? strlen(a) : 0;
  if (la >= n) la = n ? n - 1 : 0;
  if (n) memmove(out, a ? a : "", la);
  size_t rem = n > la ? n - la - 1 : 0;
  size_t lb = b ? strlen(b) : 0;
  if (lb > rem) lb = rem;
  if (lb) memmove(out + la, b, lb);
  if (n) out[la + lb] = '\0';
}

/* ==================== konfigurasi benchmark ==================== */

#define BENCH_MAX_TOOLS 16
#define BENCH_MAX_SCEN 16
#define BENCH_LABEL_LEN 80
#define BENCH_PATH_LEN (SATRIA_MAX_PATH * 2)
#define BENCH_SCEN_ALL "cold,noop,leaf,bade,mod,touch,hedit,sel,dep,libtouch,libhdr,rel"

typedef enum {
  BENCH_SATRIA = 0,
  BENCH_CMAKE,
  BENCH_NINJA,
  BENCH_MAKE,
  BENCH_BEAR,
  BENCH_MESON,
  BENCH_XMAKE,
  BENCH_TUP,
  BENCH_FLOOR, /* lantai gcc langsung: gcc -c 1 file + link, tanpa build tool */
} BenchKind;

typedef struct {
  char label[BENCH_LABEL_LEN];
  BenchKind kind;
  char bin[BENCH_PATH_LEN]; /* binary satria untuk kandidat satria */
  char ws[BENCH_PATH_LEN];  /* workspace kandidat */
  char version[160];
  bool primeOk;
  bool primeFail;
} BenchTool;

typedef struct {
  int obj, arc, lib, bin, cc, lnk;
} BenchDelta;

/* Satu pengukuran (baris results.csv + view.csv). */
typedef struct {
  char label[BENCH_LABEL_LEN];
  char scen[32];
  long ms;
  int rc;
  int obj, arc, lib, bin, cc, lnk;
  int expObj, expArc, expLib, expBin, expCc; /* -1 = tidak di-hard-check */
  bool miss;
  bool fail;
} BenchRec;

typedef struct {
  /* opsi -f */
  int source, header, module, runs, jobs;
  bool warmup, trace, strace, artifacts, keep, rolling;
  bool compare;
  bool workspace; /* --workspace: graf 3 proyek; default: satu proyek */
  char scenarios[256];
  char toolsSpec[256];
  bool toolsExplicit;
  char compilers[128];
  char satriaArgs[512];
  char binsSpec[BENCH_PATH_LEN];

  char workdir[BENCH_PATH_LEN];
  char self[BENCH_PATH_LEN];
  char errlog[BENCH_PATH_LEN];
  char csv[BENCH_PATH_LEN];
  char viewcsv[BENCH_PATH_LEN];
  char worklog[BENCH_PATH_LEN];
  char cclog[BENCH_PATH_LEN];
  char syscsv[BENCH_PATH_LEN];
  char wrapdir[BENCH_PATH_LEN];
  char primedir[BENCH_PATH_LEN];

  BenchTool tools[BENCH_MAX_TOOLS];
  int ntools;

  /* Lantai gcc (bukan tool): hanya untuk edit/editrel. */
  BenchTool floor;
  bool haveFloor;

  BenchRec *recs;
  int nrec, caprec;

  int mismatchRuns;
  int notAcc;
  bool traceReady;      /* backend trace tersedia dan siap dipakai */
  bool satriaTrace;     /* default: `satria trace` (LD_PRELOAD), bukan strace */
  char traceSkip[160];  /* alasan trace dilewati */
} Bench;

/* ==================== bench_ws.c: workspace & validasi ==================== */

void benchGenWorkspace(Bench *b, BenchTool *t);
void benchMutate(const Bench *b, const char *scen, int run, const char *label, const char *ws);

/* ==================== bench_single.c: proyek tunggal ==================== */

void benchGenProject(Bench *b, BenchTool *t);
void benchMutateSingle(const Bench *b, const char *scen, int run, const char *label,
                       const char *ws);
void benchGenNinjaGraph(const Bench *b, const char *ws);
bool benchBuildfileCompdb(const char *ws, bool enabled);

bool benchCopyTree(const char *src, const char *dst);
bool benchPathExists(const char *path);

typedef struct {
  char path[BENCH_PATH_LEN];
  long long size;
  int64_t mtimeNs;
  int64_t ctimeNs;
} BenchSnapEnt;

typedef struct {
  BenchSnapEnt *v;
  int n, cap;
} BenchSnap;

bool benchSnapTake(const char *root, BenchSnap *out);
void benchSnapFree(BenchSnap *s);
void benchDelta(const BenchSnap *before, const BenchSnap *after, BenchDelta *d);

/* ==================== bench_exec.c: proses, shim, strace ==================== */

bool benchRunArgv(const char *cwd, char *const argv[], const char *errlog, int *rc, double *ms);
bool benchRunArgvOut(const char *cwd, char *const argv[], const char *errlog,
                     const char *outPath, int *rc, double *ms);
bool benchRuntimeOk(const Bench *b, const BenchTool *t);

/* Mode perintah tool. */
enum {
  BENCH_CMD_FULL = 0,
  BENCH_CMD_SEL,
  BENCH_CMD_DEP,
  BENCH_CMD_REL,
  BENCH_CMD_CONFIGURE,
  BENCH_CMD_FLOOR,
  BENCH_CMD_COMPDB,
};

typedef struct {
  char store[12][BENCH_PATH_LEN];
  char *argv[14];
} BenchArgv;

void benchToolCommand(const Bench *b, const BenchTool *t, int mode, const char *ver, BenchArgv *out);
bool benchFloorBuild(const Bench *b, const char *cwd, int *rc, double *ms);
int benchTraceTool(const Bench *b, const BenchTool *t, const char *reportPath, bool follow,
                   int mode);

/* Mode generator embed (CMake carray): satria bench --embed-source <in> <out.c> <sym> */
int benchEmbedSource(const char *const argv[], int argc);

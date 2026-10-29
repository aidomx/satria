#ifndef SATRIA_PROFILE_H
#define SATRIA_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h" /* MAX_PATH — field ProfCtx */

/*
 * profile — `satria profile <context> [key=value ...] [--report <file>]`
 * (design/profile.md).
 *
 * Antarmuka diagnostik: mengukur biaya nyata operasi satria pada direktori
 * yang sudah ada, TANPA Buildfile dan TANPA menyentuh state build normal
 * (non-mutating; keluaran operasi ditulis ke direktori sementara yang
 * terisolasi dan dihapus setelah pengukuran).
 *
 * Context (semua context design "Prioritas Awal" diimplementasikan):
 *   archive=<dir> [with=tar|tar,gz|tar,xz|tar,bz2] — discover/collect/tar/kompresi
 *   embed=<dir|file>                — discover/collect/(tar)/ld
 *   library=<dir> [with=static|shared] — discover/collect/ar(/shared)
 *   binary=<dir> [jobs=N]           — config/gather/compile/link (terisolasi)
 *   project=<dir> [jobs=N]          — config/fingerprint/decide/compile/link
 *                                     (fase incremental; sandbox deps)
 *   workspace=<dir>                 — parse/dependency/library (baca
 *                                     model Buildfile.ws; fase build
 *                                     tidak bermakna diisolasi -> tak
 *                                     dikarang)
 * Opsi global: --report <file>, --repeat N (1..100; tiap run sandbox
 * fresh, fase hasil bolak-balik diakumulasi).
 *
 * with= = pemilih backend/operasi yang bermakna per context (design: opsi
 * tidak harus valid global). Untuk archive ini alat pembanding kompresi
 * (gzip vs xz vs bz2) — biaya & ukuran hasil terukur per fase.
 *
 * Output: stdout TTY = ringkasan singkat ("apa yang mahal?"); non-TTY
 * (pipe/redirect, mis. `satria profile archive=. > profile.md`) = laporan
 * rinci. `--report <file>` menulis laporan rinci ke file, stdout tetap
 * ringkas (pembuatan file tidak ikut terukur).
 */
int cmdProfile(const char *const *args, int nargs);

/* ==================== tipe bersama modul profile ====================
   Dipakai profile.c (entry/output), prof_steps.c (archive/embed/library),
   dan prof_sandbox.c (binary/project/workspace). */

/* Backend kompresi archive: pembanding biaya kompresi (design: profile
   adalah alat ukur untuk memutuskan optimasi — gz vs xz vs bz2). */
typedef enum { COMP_NONE = 0, COMP_GZIP, COMP_XZ, COMP_BZ2 } CompBackend;

typedef struct {
  const char *type;          /* "archive" | "embed" | "library" | "binary" |
                             "project" | "workspace" */
  const char *target;        /* value context apa adanya (untuk header) */
  char dir[MAX_PATH * 2];    /* target absolut (dir, atau file utk embed) */
  bool dirIsFile;            /* embed=<file> arsip jadi */
  bool doTar;                /* archive: selalu tar dulu; false = tak valid */
  CompBackend comp;          /* archive: COMP_NONE = with=tar */
  bool libStatic, libShared; /* library: backend ar / gcc -shared */
  int jobs;                  /* binary/project: paralel (default jumlah core) */
  int repeat;                /* --repeat N: ulangi sesi (default 1) */
  char tmp[MAX_PATH * 2];    /* direktori kerja sementara */
  const char *compiler;      /* utk laporan rinci; NULL = n/a */
  const char *reportFile;    /* --report <file> (opsi global) */
  /* metrik input */
  int files;
  int dirs;
  uint64_t bytes;
  /* hasil workspace (utk context workspace=) */
  int wsProjects;
  int wsLibCount;
} ProfCtx;

/* ============ API internal antar-file modul (semula static) ============ */

uint64_t profSizeOf(const char *path);          /* 0 bila tak terbaca */
void profFmtComma(char *out, size_t n, uint64_t v);
const char *profCompName(CompBackend b);        /* "gzip" | "xz" | ... */
const char *profCompTool(CompBackend b);        /* "gzip" CLI */
bool profExpandTilde(const char *in, char *out, size_t n);
bool profAbsPath(const char *in, char *out, size_t n);
bool profTmpDirMake(ProfCtx *p);                /* TMPDIR lalu /tmp */
void profPlatformString(char *out, size_t n);

/* Langkah ukur per context (prof_steps.c / prof_sandbox.c). */
int profRunArchive(ProfCtx *p);
int profRunEmbed(ProfCtx *p);
int profRunLibrary(ProfCtx *p);
int profRunWorkspace(ProfCtx *p);
int profRunProject(ProfCtx *p);
int profRunBinary(ProfCtx *p);

#endif /* SATRIA_PROFILE_H */

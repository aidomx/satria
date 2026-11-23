#pragma once
/* Runtime DSL unit test (design/unit_test_dsl.md, M4b) — padanan C dari
   tests/unit/lib/dsl.sh. eval.c menangani ekspresi/selektor, exec.c
   mengeksekusi event parser dan menulis verdict PASS/FAIL/PRECONDITION
   dengan format yang sama seperti eksekutor bash. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../portability.h"
#include "../util.h"

#define DSL_VAL 8192

typedef struct {
  char *name;
  char *value;
} DslVar;

/* Hasil run berlabel: file keluaran + rc. `bg` menyimpan PID proses latar. */
typedef struct {
  char *label;
  char *out;
  int rc;
  bool hasRc;
  int pid;
} DslRun;

/* Rekaman time.mark. */
typedef struct {
  char *name;
  char *path;
  int64_t ns;
  bool valid;
} DslMark;

typedef struct {
  char work[SATRIA_MAX_PATH];
  char repo[SATRIA_MAX_PATH];
  char discovery[SATRIA_MAX_PATH];
  char satria[SATRIA_MAX_PATH + 32]; /* binary yang dijalankan run.* */
  int jobs;
  const char *fixture; /* nama fixture, "" bila tidak ada */
  bool snap;           /* kelas [snap]: work.clean ditolak */
  DslVar *vars;
  int nvars, capvars;
  DslRun *runs;
  int nruns, capruns;
  DslMark *marks;
  int nmarks, capmarks;
  /* 0 = ok, 1 = PRECONDITION, 2 = kesalahan fatal */
  int errKind;
  char errMsg[512];
} DslCtx;

void dslCtxInit(DslCtx *c, const char *work, const char *repo, const char *discovery,
                int jobs);
void dslCtxFree(DslCtx *c);

const char *dslVarGet(DslCtx *c, const char *name);
void dslVarSet(DslCtx *c, const char *name, const char *value);
void dslVarSetN(DslCtx *c, const char *name, const char *value, size_t n);

/* Ekspresi L3 → nilai di `out`. Return 0 ok, 1 precondition, 2 fatal. */
int dslEval(DslCtx *c, const char *expr, char *out, size_t outn);

/* Interpolasi {var} lalu selektor project()/repo()/work()/ws(). */
int dslResolve(DslCtx *c, const char *raw, char *out, size_t outn);
void dslInterp(DslCtx *c, const char *s, char *out, size_t outn);

void dslUnesc(const char *s, char *out, size_t outn);
void dslUnq(char *s); /* buang kutip ganda pembungkus, in place */
bool dslTruthy(const char *s);

/* ERE: cari pola di mana pun dalam satu baris (grep -E per baris). */
bool dslRegexSearch(const char *pattern, const char *text);
/* Cocokkan pola terhadap tiap baris teks; true bila ada yang cocok. */
bool dslRegexText(const char *pattern, const char *text, size_t len);
/* Jumlah baris teks yang cocok pola (grep -cE). */
int dslRegexCount(const char *pattern, const char *text, size_t len);

const char *dslWPath(DslCtx *c, const char *p, char *buf, size_t n);
char *dslReadFile(const char *path, size_t *lenOut);

/* Catat kesalahan (printf-style); selalu return 1/2 sesuai kind. */
int dslErr(DslCtx *c, int kind, const char *fmt, ...);

/* M4b CLI: `satria test <file.test>`. */
int testExecMain(int argc, const char *const *argv);

/* Utilitas bersama runner (M4c): salin fixture tanpa artefak build, dan
   konversi compile_commands.json → sources.tsv (proyek TAB sumber TAB objek). */
bool testCopyFixtureTree(const char *fixdir, const char *dst);
bool testCompdbToTsv(const char *jsonPath, const char *tsvPath);

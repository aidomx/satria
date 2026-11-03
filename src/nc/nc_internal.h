#ifndef SATRIA_V0_1_0_NC_INTERNAL_H
#define SATRIA_V0_1_0_NC_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "../util.h" /* SATRIA_MAX_PATH */

/*
 * nc_internal — tipe model build.ninja & helper bersama modul nc/.
 *
 * Dipakai antar-file nc_util.c / nc_parse.c / nc_expand.c / nc_convert.c;
 * BUKAN API publik (publik tetap ninjaconv.h). Semua fungsi di sini dulunya
 * static di ninjaconv.c — dijadikan eksternal dengan prefix nc* yang sudah
 * unik sehingga tidak ada tabrakan nama.
 */

/* ==================== kumpulan string unik ==================== */

typedef struct {
  char **items;
  int count;
  int capacity;
} NcList;

bool ncAdd(NcList *l, const char *s);
void ncFree(NcList *l);

/* ==================== util dasar ==================== */

/* Strndup [a, b). Return malloc; pemanggil yang free. */
char *ncStrndup(const char *a, const char *b);

/* Token berikut (pemisah spasi/tab). NULL di akhir. Pemanggil yang free. */
char *ncNextTok(char **cursor);

/* strtok_r portabel — MSVC tidak punya (POSIX-only). */
char *ncTok(char *str, const char *delim, char **save);

/* Append tanpa fixed buffer: mencegah truncation dan underflow ukuran snprintf. */
bool ncAppendText(char **buf, size_t *len, size_t *cap, const char *text);

/* ==================== model file ninja ==================== */

#define NC_MAX_RULES 128

typedef struct {
  char *name;
  char *command; /* template; NULL bila rule tanpa command */
} NcRule;

typedef struct {
  char *out;     /* output pertama edge */
  char *rule;    /* nama rule */
  char *in;      /* input eksplisit pertama (sebelum '|') */
  char *ins;     /* SELURUH input eksplisit — untuk cek keanggotaan link */
  char *flags;   /* nilai FLAGS edge (malloc) atau NULL */
  char *incs;    /* nilai INCLUDES edge */
  char *defs;    /* nilai DEFINES edge */
  char *depfile; /* nilai DEP_FILE edge — argumen -MF dalam template */
} NcEdge;

typedef struct {
  NcRule rules[NC_MAX_RULES];
  int nrules;
  NcEdge *edges;
  int nedges, edgeCap;
  NcList varNames; /* variabel top-level (build.ninja & file include) */
  NcList varVals;
} NcFile;

const NcRule *ncRuleFind(const NcFile *f, const char *name);
void ncFileFree(NcFile *f);

/* Variabel top-level: nama sama -> nilai terakhir yang menang. */
void ncVarSet(NcFile *f, const char *key, const char *val);

/* Nilai variabel top-level, atau NULL. */
const char *ncVarGet(const NcFile *f, const char *name);

/* ==================== path helper ==================== */

/* Raw path (relatif thd dir ninja, atau absolut) -> path relatif cwd. */
void ncFixPath(const char *raw, const char *ninjaDir, char *out, size_t cap);

/* Sama, untuk token berprefix -I. *fixedIn: token sudah pernah difix
   (berasal dari INCLUDES yang di-pre-fix) — jangan join ninjaDir lagi. */
void ncFixIncludeTok(const char *tok, const char *ninjaDir, bool fixedIn, char *out, size_t cap);

/* ==================== parsing build.ninja ==================== */

/* Parse file ninja (dan `include`-nya, satu tingkat) ke dalam f. */
bool ncParseInto(NcFile *f, const char *path, const char *dirHint, int depth);

/* ==================== ekspansi template & klasifikasi command ==================== */

typedef struct {
  NcList flags;
  NcList includes;
  char std[SATRIA_MAX_PATH * 4];
  char src[SATRIA_MAX_PATH * 2];
  char obj[SATRIA_MAX_PATH * 2];
} NcCompile;

/* Ganti semua $var / ${var} pada template dengan nilai: parameter edge
   dulu, lalu variabel top-level, lalu nama khusus ninja (in/out/...).
   Var tak dikenal -> "". Hasil malloc. */
char *ncExpand(const NcFile *f, const char *tpl, const char *in, const char *out,
               const char *flags, const char *incs, const char *defs, const char *depfile);

/* Parse command hasil ekspansi; true bila edge compile valid (-c src, -o obj). */
bool ncParseCompile(char *cmd, NcCompile *cc, const char *ninjaDir);

#endif /* SATRIA_V0_1_0_NC_INTERNAL_H */

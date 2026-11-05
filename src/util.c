#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* ==================== Laporan build ==================== */

/*
 * repPrefix: prefix baris laporan selalu "*" (sama untuk single dan
 * workspace). Tidak lagi bergantung pada locale atau codepage konsol.
 */
const char *repPrefix(void) {
  return "*";
}

/* ==================== List & string helpers ==================== */

bool listAdd(List *l, const char *s) {
  if (!l || !s || !*s) return false;
  if (l->count == l->capacity) {
    if (l->capacity > 0x3fffffff) return false;
    int next = l->capacity ? l->capacity * 2 : 8;
    char **items = realloc(l->items, (size_t)next * sizeof(*items));
    if (!items) return false;
    l->items = items;
    l->capacity = next;
  }
  char *copy = strdup(s);
  if (!copy) return false;
  l->items[l->count++] = copy;
  return true;
}

void listFree(List *l) {
  if (!l) return;
  for (int i = 0; i < l->count; i++)
    free(l->items[i]);
  free(l->items);
  l->items = NULL;
  l->count = 0;
  l->capacity = 0;
}

/* Pembanding leksikografis byte demi byte (strcmp) untuk qsort List.
   Dipakai pemakai walkDir yang urutannya menentukan output (daftar link,
   daftar entri tar) supaya hasil tidak bergantung urutan readdir
   filesystem — link dan arsip jadi reprodusible antar mesin, dan urutan
   inisialisasi object statis C++ tidak lagi melekat pada readdir. */
static int cmpStr(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa, *sb);
}

/* Pesan kapasitas SEKALI di awal agar listAdd tidak realloc bolak-balik.
   Gagal alokasi dibiarkan diam-diam: listAdd tetap tumbuh doubling seperti
   biasa, reserve hanya optimasi. */
void listReserve(List *l, int want) {
  if (!l || want <= l->capacity) return;
  if (want > 0x3fffffff) want = 0x3fffffff;
  char **items = realloc(l->items, (size_t)want * sizeof(*items));
  if (!items) return;
  l->items = items;
  l->capacity = want;
}

void listSort(List *l) {
  if (!l || l->count < 2) return;
  qsort(l->items, (size_t)l->count, sizeof(l->items[0]), cmpStr);
}

void copyStr(char *dst, size_t n, const char *src) {
  snprintf(dst, n, "%s", src ? src : "");
}

char *trim(char *s) {
  while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
    s++;
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
    --end;
  *end = '\0';
  return s;
}

bool parseBool(const char *s, bool *out) {
  if (!s || !out) return false;
  if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 || strcmp(s, "on") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 || strcmp(s, "off") == 0) {
    *out = false;
    return true;
  }
  return false;
}

/* ==================== popen/pclose portabel ==================== */

#ifdef _WIN32
/* MSVC: tidak ada popen/pclose (POSIX); _popen/_pclose setara (cmd.exe). */
FILE *popenRB(const char *cmd) {
  return _popen(cmd, "r");
}
void pcloseRB(FILE *fp) {
  if (fp) _pclose(fp);
}
#else
FILE *popenRB(const char *cmd) {
  return popen(cmd, "r");
}
void pcloseRB(FILE *fp) {
  if (fp) pclose(fp);
}
#endif

/* ==================== Filesystem helpers ==================== */

void mkdirs(const char *path) {
  char tmp[SATRIA_MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      fsMakeDir(tmp);
      *p = '/';
    }
  }
  fsMakeDir(tmp);
}

void mkparent(const char *path) {
  char tmp[SATRIA_MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
  if (!slash) return;
  *slash = '\0';
  /* Object satu direktori berturut-turut (walkDir mengelompokkan per
     direktori): lewati mkdir berulang — tiap mkdirs() = satu syscall per
     komponen path, mahal di proot.
     Cache di-key juga dengan CWD: workspace/profile mengganti cwd antar
     proyek, jadi path relatif sama ("build/sub") hanya boleh dilewati
     bila direktori kerja TIDAK berubah — tanpa itu mkdir dilewati di
     proyek berikutnya dan object gagal ditulis ("can't create build/..."). */
  static char last[SATRIA_MAX_PATH];
  static char lastCwd[SATRIA_MAX_PATH];
  if (last[0] && strcmp(last, tmp) == 0) {
    char cwd[SATRIA_MAX_PATH];
    if (fsGetCwd(cwd, sizeof(cwd)) && strcmp(lastCwd, cwd) == 0) return;
  }
  mkdirs(tmp);
  if (fsDirExists(tmp)) {
    snprintf(last, sizeof(last), "%s", tmp);
    if (!fsGetCwd(lastCwd, sizeof(lastCwd))) lastCwd[0] = '\0'; /* gagal: selalu miss */
  }
}

/** Recursively collect paths under `dir` whose name ends with `ext`.
   With ext == "" every regular file matches; with ext == "/" only
   directories are collected (used by fsRemoveTree).
   Hasil TIDAK diurutkan: sortir di dalam rekursi berarti qsort O(n log n)
   diulang di tiap level (pohon dalam = pergeseran pointer berulang) padahal
   satu qsort pada data lengkap di pemanggil lebih murah. Pemanggil yang
   butuh urutan stabil antar mesin memanggil listSort() sendiri SETELAH
   walkDir selesai (lihat commands.c, embed.c, pack/). */
void walkDir(const char *dir, const char *ext, List *out) {
  List dirs = {0}, files = {0};
  /* Pre-alokasi kedua list per level: chunk 512 entry cukup untuk tipikal
     satu folder; realokasi lanjutan tetap ditangani listAdd (doubling). */
  listReserve(&dirs, 512);
  listReserve(&files, 512);
  fsListDir(dir, &dirs, &files);
  if (ext[0] == '/') {
    for (int i = 0; i < dirs.count; i++)
      listAdd(out, dirs.items[i]);
    for (int i = 0; i < dirs.count; i++)
      walkDir(dirs.items[i], ext, out);
    listFree(&dirs);
    listFree(&files);
    return;
  }
  for (int i = 0; i < files.count; i++) {
    const char *path = files.items[i];
    size_t len = strlen(path), elen = strlen(ext);
    if (len > elen && strcmp(path + len - elen, ext) == 0) listAdd(out, path);
  }
  for (int i = 0; i < dirs.count; i++)
    walkDir(dirs.items[i], ext, out);
  listFree(&dirs);
  listFree(&files);
}

bool newerThan(const char *a, const char *b) {
  return fsNewerThan(a, b);
}

bool safeRelative(const char *path) {
  if (!path || !*path) return false;
  if (path[0] == '/' || path[0] == '\\') return false;
  if (path[0] && path[1] == ':') return false; /* drive Windows: C:\... */
  if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0) return false;
  if (strstr(path, "..")) return false;
  return true;
}

/* ==================== Proses & waktu ==================== */

bool runCmd(const char *cmd) {
  return procRun(cmd);
}

double nowSeconds(void) {
  return monotonicSeconds();
}

/* ==================== reproducible archive helpers ==================== */

long long reproEpoch(void) {
  const char *e = getenv("SOURCE_DATE_EPOCH");
  if (e && *e) {
    char *end = NULL;
    long long v = strtoll(e, &end, 10);
    if (end && *end == '\0' && v >= 0) return v;
  }
  return 0;
}

const char *tarDetFlags(void) {
  static char flags[160];
  static int state = 0; /* 0 belum dicek, 1 GNU, 2 bukan GNU */
  if (state == 0) {
    state = 2;
    /* SUPPRESION stderr per platform: cmd.exe (_popen Windows) tidak
       kenal /dev/null — pakai `2>nul`; POSIX pakai `2>/dev/null`.
       pcloseRB: MSVC hanya punya _pclose (POSIX pclose, LNK2019 05/11). */
#ifdef _WIN32
    const char *supp = " 2>nul";
#else
    const char *supp = " 2>/dev/null";
#endif
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "tar --version%s", supp);
    FILE *fp = popenRB(cmd);
    if (fp) {
      char line[160];
      if (fgets(line, sizeof(line), fp) && strstr(line, "GNU tar")) state = 1;
      pcloseRB(fp);
    }
    if (state == 1)
      snprintf(flags, sizeof(flags),
               "--sort=name --mtime=@%lld --owner=0 --group=0 --numeric-owner ",
               reproEpoch());
    else
      flags[0] = '\0';
  }
  return flags;
}


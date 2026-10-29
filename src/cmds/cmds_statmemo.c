/*
 * cmds_statmemo — memo stat per-proses untuk validasi fast-state\n * plus invalidasi (forget) saat file berubah/dihapus.
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

/* ==================== memo stat per proses ====================
   Validasi fast-state dipanggil per fase (library lalu binary) dalam satu
   proses dan memeriksa daftar stamp yang nyaris identik. Hasil stat disimpan
   di sini supaya fase kedua tidak men-stat ulang path yang sama. Memo hanya
   sah selama tidak ada build di antaranya: cmdBuildEx memanggil
   cmdsStatMemoReset tepat sebelum build dimulai. */
typedef struct {
  char *path;
  bool exists;
  int64_t mtime;
  long long size;
} StatMemoEntry;
static StatMemoEntry *statMemo = NULL;
static size_t statMemoCap = 0, statMemoCount = 0;
/* cwd saat validasi. Path relatif (mis. "src") berarti direktori berbeda di
   tiap proyek, jadi kunci memo harus menyertakan cwd agar tidak tertukar. */
static char statMemoCwd[MAX_PATH * 2];

static uint64_t statMemoHash(const char *s) {
  uint64_t h = 14695981039346656037ULL;
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 1099511628211ULL;
  }
  return h;
}

static void statMemoRehash(size_t newCap) {
  StatMemoEntry *fresh = calloc(newCap, sizeof(StatMemoEntry));
  if (!fresh) return; /* gagal alokasi: lewati memo, pemanggil men-stat langsung */
  StatMemoEntry *old = statMemo;
  size_t oldCap = statMemoCap;
  statMemo = fresh;
  statMemoCap = newCap;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].path) continue;
    size_t j = statMemoHash(old[i].path) & (newCap - 1);
    while (statMemo[j].path) j = (j + 1) & (newCap - 1);
    statMemo[j] = old[i];
  }
  free(old);
}

/* Kunci memo = path absolut ternormalisasi secara leksikal (buang "." dan
   "..", tanpa menyentuh symlink). Dengan begitu "../library/a.h" dari proyek
   core dan dari proyek bade menjadi satu entri, sehingga tidak di-stat dua kali. */
static void memoKeyFor(const char *path, char *out, size_t n) {
  char joined[MAX_PATH * 4];
  if (path[0] == '/' || statMemoCwd[0] == '\0')
    snprintf(joined, sizeof(joined), "%s", path);
  else
    snprintf(joined, sizeof(joined), "%s/%s", statMemoCwd, path);

  char work[MAX_PATH * 4];
  snprintf(work, sizeof(work), "%s", joined);
  const char *seg[256];
  int ns = 0;
  char *savePtr = NULL;
  for (char *tok = strtok_r(work, "/", &savePtr); tok; tok = strtok_r(NULL, "/", &savePtr)) {
    if (strcmp(tok, ".") == 0 || tok[0] == '\0') continue;
    if (strcmp(tok, "..") == 0) {
      if (ns > 0) ns--;
      continue;
    }
    if (ns >= 256) {
      snprintf(out, n, "%s", joined); /* terlalu dalam: pakai path apa adanya */
      return;
    }
    seg[ns++] = tok;
  }
  size_t used = 0;
  out[used++] = '/';
  out[used] = '\0';
  for (int i = 0; i < ns; i++) {
    size_t len = strlen(seg[i]);
    if (used + len + 2 > n) {
      snprintf(out, n, "%s", joined);
      return;
    }
    memcpy(out + used, seg[i], len);
    used += len;
    out[used++] = '/';
    out[used] = '\0';
  }
  if (used > 1) out[used - 1] = '\0'; /* buang '/' terakhir */
}

bool memoStampNsSize(const char *path, int64_t *mtimeNs, long long *size) {
  char key[MAX_PATH * 4];
  memoKeyFor(path, key, sizeof(key));

  if (statMemoCap == 0 || (statMemoCount + 1) * 2 > statMemoCap)
    statMemoRehash(statMemoCap ? statMemoCap * 2 : 4096);
  if (statMemoCap == 0) return fsStampNsSize(path, mtimeNs, size);

  size_t j = statMemoHash(key) & (statMemoCap - 1);
  while (statMemo[j].path && strcmp(statMemo[j].path, key) != 0)
    j = (j + 1) & (statMemoCap - 1);
  StatMemoEntry *e = &statMemo[j];
  if (e->path) {
    if (!e->exists) return false;
    *mtimeNs = e->mtime;
    *size = e->size;
    return true;
  }
  bool ok = fsStampNsSize(path, mtimeNs, size);
  /* Hanya hasil positif yang disimpan: file yang belum ada bisa dibuat build,
     jadi negatif selalu di-stat ulang. */
  if (ok) {
    char *dup = strdup(key);
    if (dup) {
      e->path = dup;
      e->exists = true;
      e->mtime = *mtimeNs;
      e->size = *size;
      statMemoCount++;
    }
  }
  return ok;
}

/* Memo stat untuk validator workspace (cmds_wsstate.c): kunci dan semantik sama
   dengan validasi single project, sehingga fase library dan binary berbagi hasil. */
/* Kunci memo memakai cwd; pemanggil di luar cmdsFastStateValid (validator ws)
   wajib menyetelnya dulu, jika tidak entri proyek lain bisa terpakai. */
void cmdsStatMemoSetCwd(void) {
  if (!fsGetCwd(statMemoCwd, sizeof(statMemoCwd))) statMemoCwd[0] = '\0';
}

bool cmdsStatCached(const char *path, int64_t *mtimeNs, long long *size) {
  return memoStampNsSize(path, mtimeNs, size);
}

/* Parser baris format state F/D/X (format bersama ws.state dan workspace.state
   lama — lihat header cmds_wsstate.c). Dipakai validator di cmds_wsstate.c
   agar format hidup di SATU tempat. Baris F opsional membawa content16hex
   (bukti sentuh; lihat cmdsParseStateEntry). */
bool cmdsParseStateEntry(const char *line, char *kind, long long *mt, long long *sz,
                         unsigned long long *content, char *path) {
  char k = line[0];
  path[0] = '\0';
  *mt = 0;
  *sz = 0;
  *content = 0;
  if (k == 'X') {
    if (sscanf(line, "X %*d %*d %" WS_SCAN "s", path) != 1) return false;
  } else if (k == 'D') {
    if (sscanf(line, "D %lld %*d %" WS_SCAN "s", mt, path) != 2) return false;
  } else if (k == 'F' || k == 'O' || k == 'I') {
    /* Bentuk lama: F <mt> <sz> <path>. Bentuk baru (bukti sentuh, design §4):
       F <mt> <sz> <content16hex> <path> — path tidak pernah mengandung spasi
       (guard yang sama dengan seluruh format teks state), jadi jumlah token
       membedakan keduanya secara unik. */
    long long m2 = 0, s2 = 0;
    char a[WS_PATH_LEN], b[WS_PATH_LEN];
    int n = sscanf(line, "%*c %lld %lld %" WS_SCAN "s %" WS_SCAN "s", &m2, &s2, a, b);
    if (n == 4) {
      if (sscanf(a, "%16llx", content) != 1) return false;
      snprintf(path, WS_PATH_LEN, "%s", b);
    } else if (n == 3) {
      snprintf(path, WS_PATH_LEN, "%s", a);
    } else {
      return false;
    }
    *mt = m2;
    *sz = s2;
  } else {
    return false;
  }
  *kind = k;
  return true;
}

/* true bila baris F/D/X masih cocok stamp disk saat ini: F = mtime+size sama,
   D = mtime sama, X = path masih tidak ada. False bila parse gagal atau stamp
   beda. Memakai memo stat (cmdsStatCached) — pemanggil validator wajib sudah
   menyetel cwd memo (cmdsStatMemoSetCwd) bila lintas proyek. */
bool cmdsStateEntryFresh(const char *line) {
  char kind = 0;
  long long mt = 0, sz = 0;
  unsigned long long content = 0;
  char p[WS_PATH_LEN];
  int64_t cm = -1;
  long long cs = -1;
  if (!cmdsParseStateEntry(line, &kind, &mt, &sz, &content, p)) return false;
  bool exists = kind == 'I' ? fsStampNsSize(p, &cm, &cs) : cmdsStatCached(p, &cm, &cs);
  if (kind == 'X') return !exists;
  if (!exists) return false;
  if (kind == 'D') return cm == (int64_t)mt;
  if (cm == (int64_t)mt && cs == sz) return true;
  /* Output O tetap stamp-based; tidak hash semua object/output pada setiap
     penyimpanan state. Input library lintas-proyek I menyimpan hash konten:
     perubahan stamp/ukuran yang tidak mengubah byte bukan perubahan input
     linker dan tidak boleh memaksa relink. Format lama tanpa hash konservatif. */
  if (kind == 'O') return false;
  if (kind == 'I' && !content) return false;
  if (content && cs == sz) {
    uint64_t h = fastStateHash(p);
    if (h && h == (uint64_t)content) return true;
  }
  return false;
}

void cmdsStatMemoReset(void) {
  for (size_t i = 0; i < statMemoCap; i++) free(statMemo[i].path);
  free(statMemo);
  statMemo = NULL;
  statMemoCap = 0;
  statMemoCount = 0;
}

/* Buang satu entri memo (kunci = path ternormalisasi, sama seperti insert). */
void cmdsStatMemoForget(const char *path) {
  if (statMemoCap == 0 || !path) return;
  char key[MAX_PATH * 4];
  memoKeyFor(path, key, sizeof(key));
  /* Bangun ulang tabel tanpa kunci itu; open addressing tak punya tombstone. */
  StatMemoEntry *old = statMemo;
  size_t oldCap = statMemoCap;
  statMemo = calloc(oldCap, sizeof(StatMemoEntry));
  if (!statMemo) {
    statMemo = old;
    statMemoCap = oldCap;
    return;
  }
  size_t kept = 0;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].path) continue;
    if (strcmp(old[i].path, key) == 0) {
      free(old[i].path);
      continue;
    }
    size_t j = statMemoHash(old[i].path) & (oldCap - 1);
    while (statMemo[j].path) j = (j + 1) & (oldCap - 1);
    statMemo[j] = old[i];
    kept++;
  }
  free(old);
  statMemoCount = kept;
}

/* Buang semua entri di bawah folder `dir` (relatif cwd), termasuk folder itu. */
void cmdsStatMemoForgetTree(const char *dir) {
  if (statMemoCap == 0 || !dir) return;
  char prefix[MAX_PATH * 4];
  memoKeyFor(dir, prefix, sizeof(prefix));
  size_t pl = strlen(prefix);
  StatMemoEntry *old = statMemo;
  size_t oldCap = statMemoCap;
  statMemo = calloc(oldCap, sizeof(StatMemoEntry));
  if (!statMemo) {
    statMemo = old;
    statMemoCap = oldCap;
    return;
  }
  size_t kept = 0;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].path) continue;
    bool drop = strncmp(old[i].path, prefix, pl) == 0 &&
                (old[i].path[pl] == '\0' || old[i].path[pl] == '/');
    if (drop) {
      free(old[i].path);
      continue;
    }
    size_t j = statMemoHash(old[i].path) & (oldCap - 1);
    while (statMemo[j].path) j = (j + 1) & (oldCap - 1);
    statMemo[j] = old[i];
    kept++;
  }
  free(old);
  statMemoCount = kept;
}

/* Satu rehash untuk invalidasi seluruh output workspace. Jika alokasi gagal,
   reset memo agar tidak ada stamp output basi yang dipakai. */
static int memoKeyCompare(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void cmdsStatMemoForgetOutputs(const char *buildDir, const List *items) {
  if (!statMemoCap) return;
  char prefix[MAX_PATH * 4] = "";
  if (buildDir && *buildDir) memoKeyFor(buildDir, prefix, sizeof(prefix));
  size_t pl = strlen(prefix);
  size_t n = 0;
  for (int i = 0; items && i < items->count; i++)
    if (items->items[i][0] == 'O') n++;
  char **keys = n ? calloc(n, sizeof(*keys)) : NULL;
  if (n && !keys) { cmdsStatMemoReset(); return; }
  size_t used = 0;
  for (int i = 0; items && i < items->count; i++) {
    if (items->items[i][0] != 'O') continue;
    keys[used] = malloc(MAX_PATH * 4);
    if (!keys[used]) {
      for (size_t j = 0; j < used; j++) free(keys[j]);
      free(keys);
      cmdsStatMemoReset();
      return;
    }
    memoKeyFor(items->items[i] + 2, keys[used], MAX_PATH * 4);
    used++;
  }
  if (used > 1) qsort(keys, used, sizeof(*keys), memoKeyCompare);
  StatMemoEntry *fresh = calloc(statMemoCap, sizeof(*fresh));
  if (!fresh) {
    for (size_t j = 0; j < used; j++) free(keys[j]);
    free(keys);
    cmdsStatMemoReset();
    return;
  }
  size_t kept = 0;
  for (size_t i = 0; i < statMemoCap; i++) {
    if (!statMemo[i].path) continue;
    const char *path = statMemo[i].path;
    bool inTree = pl && strncmp(path, prefix, pl) == 0 &&
                  (path[pl] == '\0' || path[pl] == '/');
    bool inOutputs = used && bsearch(&path, keys, used, sizeof(*keys), memoKeyCompare);
    if (inTree || inOutputs) { free(statMemo[i].path); continue; }
    size_t j = statMemoHash(path) & (statMemoCap - 1);
    while (fresh[j].path) j = (j + 1) & (statMemoCap - 1);
    fresh[j] = statMemo[i];
    kept++;
  }
  free(statMemo);
  statMemo = fresh;
  statMemoCount = kept;
  for (size_t j = 0; j < used; j++) free(keys[j]);
  free(keys);
}

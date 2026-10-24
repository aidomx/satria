/*
 * embed_ld_cache — cache persisten hasil deteksi GNU ld (.satria/ld.cache).
 *
 * ldAvailable() (embed.c) hanya meng-cache hasilnya per proses, jadi
 * setiap run yang membangun proyek ber-`embedded` men-spawn sh + ld lagi
 * untuk `ld --version` (3–5 ms per run; lihat problem_note.txt P1).
 * Cache ini merekam verdict probe lintas-run: run berikutnya cukup
 * SATU stat ke binary ld hasil resolusi PATH (path + mtime ns + size)
 * untuk memvalidasi — tanpa spawn.
 *
 * Aturan penting: SATRIA_NO_LD tidak pernah dibaca di sini — env override
 * itu diputuskan embed.c SEBELUM menyentuh cache, sehingga fallback tanpa
 * ld (SATRIA_NO_LD=1, dipakai Uji 2 verify-fpic.sh) tetap selalu jalan.
 * Cache juga tidak dipakai di Windows (jalur embed MSVC memakai fallback
 * C-array; lihat embed.h).
 *
 * Format file: "SATRIALD1", version u32, mtime ns (2x u32 LE), size (2x u32
 * LE), key (u32 len + bytes), verdict u8. Helper semuanya menulis byte
 * demi byte dengan urutan eksplisit — aman lintas arsitektur/endianness.
 */
#include "embed_ld_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"
#include "util.h"

#define LD_CACHE_MAGIC "SATRIALD1"
#define LD_CACHE_VERSION 1u
#define LD_CACHE_FILE ".satria/ld.cache"

/* Cap key path di file — path absolut binary ld muat jauh di bawah ini. */
#define LD_CACHE_KEY_MAX 1024
/* Pemisah entri PATH (GNU make style: ':' on POSIX, ';' on Windows; cache
   sendiri hanya dipakai POSIX, tapi tetap benar di dua OS). */
#ifdef _WIN32
#define PATHSepChar ';'
#else
#define PATHSepChar ':'
#endif

const char *ldCacheFile(void) { return LD_CACHE_FILE; }

/* ---------- helper IO urutan byte eksplisit (aman lintas-arch) ---------- */

static bool cacheWriteU32(FILE *fp, uint32_t v) {
  unsigned char b[4] = {(unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
                        (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff)};
  return fwrite(b, 1, sizeof(b), fp) == sizeof(b);
}

static bool cacheReadU32(FILE *fp, uint32_t *v) {
  unsigned char b[4];
  if (fread(b, 1, sizeof(b), fp) != sizeof(b)) return false;
  *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return true;
}

static bool cacheWriteU8(FILE *fp, uint8_t v) { return fwrite(&v, 1, sizeof(v), fp) == sizeof(v); }

static bool cacheReadU8(FILE *fp, uint8_t *v) { return fread(v, 1, sizeof(*v), fp) == sizeof(*v); }

/* int64_t ditulis sebagai dua u32 lossless (kuintat lewat 2^32 dsb.). */
static bool cacheWriteI64(FILE *fp, int64_t v) {
  uint32_t lo = (uint32_t)((uint64_t)v & 0xffffffffULL);
  uint32_t hi = (uint32_t)((uint64_t)v >> 32);
  return cacheWriteU32(fp, lo) && cacheWriteU32(fp, hi);
}

static bool cacheReadI64(FILE *fp, int64_t *v) {
  uint32_t lo = 0, hi = 0;
  if (!cacheReadU32(fp, &lo) || !cacheReadU32(fp, &hi)) return false;
  *v = (int64_t)(((uint64_t)hi << 32) | (uint64_t)lo);
  return true;
}

static bool cacheWriteSize(FILE *fp, long long v) {
  return cacheWriteI64(fp, (int64_t)v);
}

static bool cacheReadSize(FILE *fp, long long *v) {
  int64_t tmp = 0;
  if (!cacheReadI64(fp, &tmp)) return false;
  *v = (long long)tmp;
  return true;
}

static bool cacheWriteKey(FILE *fp, const char *s) {
  size_t len = s ? strlen(s) : 0;
  if (len >= LD_CACHE_KEY_MAX) return false;
  uint32_t n = (uint32_t)len;
  return cacheWriteU32(fp, n) && (len == 0 || fwrite(s, 1, len, fp) == len);
}

static bool cacheReadKey(FILE *fp, char *dst, size_t cap) {
  uint32_t n = 0;
  if (!cacheReadU32(fp, &n) || n >= cap) return false;
  if (n && fread(dst, 1, n, fp) != n) return false;
  dst[n] = '\0';
  return true;
}

/*
 * Resolusi binary `exe` di PATH semantik probeAvailable (port/port_proc.c):
 * path absolut/relatif eksplisit apa adanya, selain itu scan PATH dan
 * ambil entri PERTAMA yang ada. Tidak men-spawn apa pun — fsFileExists
 * per entri (stat), jauh lebih murah dari probe `--version`.
 */
bool ldCacheResolve(const char *exe, char *out, size_t n) {
  if (!exe || !*exe) return false;
  /* Path eksplisit (absolut maupun relatif): apa adanya, hanya cek ada. */
  if (strchr(exe, '/') || strchr(exe, '\\')) {
    if (!fsFileExists(exe)) return false;
    snprintf(out, n, "%s", exe);
    return true;
  }

  const char *pathvar = getenv("PATH");
  if (!pathvar) pathvar = "/usr/bin:/bin";
  const char *p = pathvar;
  while (*p) {
    const char *colon = strchr(p, PATHSepChar);
    size_t len = colon ? (size_t)(colon - p) : strlen(p);
    if (len > 0 && len < MAX_PATH) {
      char dir[MAX_PATH];
      memcpy(dir, p, len);
      dir[len] = '\0';
      int w = snprintf(out, n, "%s/%s", dir, exe);
      if (w > 0 && (size_t)w < n && fsFileExists(out)) return true;
    }
    if (!colon) break;
    p = colon + 1;
  }
  return false;
}

static bool pathSame(const char *a, const char *b) {
  return a && b && strcmp(a, b) == 0;
}

bool cacheReadLD(const char *ldPath, bool *out) {
  *out = false;
  if (!ldPath || !*ldPath) return false;

  /* Satu stat untuk key: binary ld harus masih ada; mtime+size dipakai
     untuk membanding dengan yang direkam. */
  int64_t mtimeNs = 0;
  long long size = 0;
  if (!fsStampNsSize(ldPath, &mtimeNs, &size) || mtimeNs < 0) return false;

  FILE *fp = fopen(LD_CACHE_FILE, "rb");
  if (!fp) return false;

  char magic[sizeof(LD_CACHE_MAGIC) - 1];
  uint32_t version = 0;
  int64_t cachedMtimeNs = 0;
  long long cachedSize = 0;
  char key[LD_CACHE_KEY_MAX];
  uint8_t verdict = 0;

  bool ok = fread(magic, 1, sizeof(magic), fp) == sizeof(magic) &&
            memcmp(magic, LD_CACHE_MAGIC, sizeof(magic)) == 0 &&
            cacheReadU32(fp, &version) && version == LD_CACHE_VERSION &&
            cacheReadI64(fp, &cachedMtimeNs) && cacheReadSize(fp, &cachedSize) &&
            cacheReadKey(fp, key, sizeof(key)) && cacheReadU8(fp, &verdict) &&
            cachedMtimeNs == mtimeNs && cachedSize == size && pathSame(key, ldPath);
  fclose(fp);
  if (ok) *out = (verdict != 0);
  return ok;
}

/* Cache-level helpers consumed by fastState/ldAvailable.
 * cacheCheckLD() never probes: caller resolves ld, then cacheReadLD only.
 * cacheUpdateLD() only records a verdict already obtained by the caller.
 */
bool cacheCheckLD(const char *ldPath, bool *out) {
  return cacheReadLD(ldPath, out);
}

void cacheWriteLD(const char *ldPath, bool gnu) {
  if (!ldPath || !*ldPath) return;
  int64_t mtimeNs = 0;
  long long size = 0;
  if (!fsStampNsSize(ldPath, &mtimeNs, &size) || mtimeNs < 0) return;

  /* .satria harus ada sebelum file cache bisa ditulis (mkdir satu level,
     diam bila sudah ada). */
  mkdirs(".satria");
  char tmp[sizeof(LD_CACHE_FILE) + 8];
  snprintf(tmp, sizeof(tmp), "%s.tmp", LD_CACHE_FILE);
  FILE *fp = fopen(tmp, "wb");
  if (!fp) return;

  const char magic[] = LD_CACHE_MAGIC;
  bool ok = fwrite(magic, 1, sizeof(magic) - 1, fp) == sizeof(magic) - 1 &&
            cacheWriteU32(fp, LD_CACHE_VERSION) && cacheWriteI64(fp, mtimeNs) &&
            cacheWriteSize(fp, size) && cacheWriteKey(fp, ldPath) &&
            cacheWriteU8(fp, gnu ? 1u : 0u);
  ok = fclose(fp) == 0 && ok;
  if (!ok) {
    fsRemoveFile(tmp);
    return;
  }
  /* POSIX rename atomik; Windows MSVCRT rename menolak menimpa file yang
     sudah ada — hapus dulu (pola yang sama dipakai cfgCacheSave). */
  if (!fsRenameReplace(tmp, LD_CACHE_FILE)) fsRemoveFile(tmp);
}

/* Update the persistent verdict for the currently resolved ld. */
void cacheUpdateLD(const char *ldPath, bool gnu) {
  cacheWriteLD(ldPath, gnu);
}

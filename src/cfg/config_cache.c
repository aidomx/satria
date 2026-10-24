/*
 * config_cache — Persistent Buildfile cache (dipindah apa adanya dari
 * config.c; lihat config_cache.h untuk API-nya).
 *
 * Cache menyimpan hasil parse Buildfile agar run berikutnya tidak membuka
 * file konfigurasi sama sekali (validasi hanya stat mtime+size). Format
 * biner: magic, version, mtime ns, size, lalu seluruh field Config.
 */
#include "config_cache.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../portability.h"
#include "../util.h"

#define CONFIG_CACHE_MAGIC "SATRIACFG1"
#define CONFIG_CACHE_VERSION 11u /* 10: defines + suffix flags/library/headers/std (design/flags.md) */
#define CONFIG_CACHE_DIR ".satria"
#define CONFIG_CACHE_FILE "startup.state"

static bool cacheWriteBytes(FILE *fp, const void *p, size_t n) {
  return fwrite(p, 1, n, fp) == n;
}

static bool cacheReadBytes(FILE *fp, void *p, size_t n) {
  return fread(p, 1, n, fp) == n;
}

static bool cacheWriteU32(FILE *fp, uint32_t v) {
  return cacheWriteBytes(fp, &v, sizeof(v));
}
static bool cacheReadU32(FILE *fp, uint32_t *v) {
  return cacheReadBytes(fp, v, sizeof(*v));
}
static bool cacheWriteU8(FILE *fp, uint8_t v) {
  return cacheWriteBytes(fp, &v, sizeof(v));
}
static bool cacheReadU8(FILE *fp, uint8_t *v) {
  return cacheReadBytes(fp, v, sizeof(*v));
}

static bool cacheWriteString(FILE *fp, const char *s) {
  uint32_t n = (uint32_t)(s ? strlen(s) : 0);
  return cacheWriteU32(fp, n) && (!n || cacheWriteBytes(fp, s, n));
}

static bool cacheReadString(FILE *fp, char *dst, size_t cap) {
  uint32_t n = 0;
  if (!cacheReadU32(fp, &n) || n >= cap) return false;
  if (n && !cacheReadBytes(fp, dst, n)) return false;
  dst[n] = '\0';
  return true;
}

static bool cacheWriteList(FILE *fp, const List *l) {
  if (!cacheWriteU32(fp, (uint32_t)l->count)) return false;
  for (int i = 0; i < l->count; i++)
    if (!cacheWriteString(fp, l->items[i])) return false;
  return true;
}

static bool cacheReadList(FILE *fp, List *l) {
  uint32_t count = 0;
  if (!cacheReadU32(fp, &count) || count > INT_MAX) return false;
  for (uint32_t i = 0; i < count; i++) {
    char item[MAX_PATH * 2];
    if (!cacheReadString(fp, item, sizeof(item))) return false;
    if (!listAdd(l, item)) return false;
  }
  return true;
}

static bool cacheWriteEmbedded(FILE *fp, const EmbeddedEntry *e) {
  uint8_t b = e->enable ? 1 : 0;
  uint8_t tar = e->tar ? 1 : 0;
  uint8_t pre = e->usePrebuilt ? 1 : 0;
  return cacheWriteString(fp, e->name) && cacheWriteU8(fp, b) && cacheWriteString(fp, e->src) &&
         cacheWriteString(fp, e->extract) && cacheWriteString(fp, e->pattern) &&
         cacheWriteString(fp, e->archiveDir) && cacheWriteString(fp, e->archiveName) &&
         cacheWriteU8(fp, tar) && cacheWriteString(fp, e->ext) &&
         cacheWriteString(fp, e->archivePath) && cacheWriteString(fp, e->objectPath) &&
         cacheWriteU8(fp, pre) && cacheWriteString(fp, e->prebuiltPath) &&
         cacheWriteString(fp, e->variable) && cacheWriteList(fp, &e->excludes);
}

static bool cacheReadEmbedded(FILE *fp, EmbeddedEntry *e) {
  uint8_t b = 0, tar = 0, pre = 0;
  memset(e, 0, sizeof(*e));
  if (!cacheReadString(fp, e->name, sizeof(e->name)) || !cacheReadU8(fp, &b) ||
      !cacheReadString(fp, e->src, sizeof(e->src)) ||
      !cacheReadString(fp, e->extract, sizeof(e->extract)) ||
      !cacheReadString(fp, e->pattern, sizeof(e->pattern)) ||
      !cacheReadString(fp, e->archiveDir, sizeof(e->archiveDir)) ||
      !cacheReadString(fp, e->archiveName, sizeof(e->archiveName)) || !cacheReadU8(fp, &tar) ||
      !cacheReadString(fp, e->ext, sizeof(e->ext)) ||
      !cacheReadString(fp, e->archivePath, sizeof(e->archivePath)) ||
      !cacheReadString(fp, e->objectPath, sizeof(e->objectPath)) || !cacheReadU8(fp, &pre) ||
      !cacheReadString(fp, e->prebuiltPath, sizeof(e->prebuiltPath)) ||
      !cacheReadString(fp, e->variable, sizeof(e->variable)) || !cacheReadList(fp, &e->excludes))
    return false;
  e->enable = b != 0;
  e->tar = tar != 0;
  e->usePrebuilt = pre != 0;
  return true;
}

/* Suffix spesifik (design/flags.md): 4 properti array + std per target. */
static bool cacheWriteSfx(FILE *fp, const Config *c) {
  for (int i = 0; i < SFX_COUNT; i++)
    if (!cacheWriteList(fp, &c->sfxFlags[i]) || !cacheWriteList(fp, &c->sfxLibrary[i]) ||
        !cacheWriteList(fp, &c->sfxHeaders[i]) || !cacheWriteList(fp, &c->sfxDefines[i]) ||
        !cacheWriteString(fp, c->sfxStd[i]))
      return false;
  return true;
}

static bool cacheReadSfx(FILE *fp, Config *c) {
  for (int i = 0; i < SFX_COUNT; i++)
    if (!cacheReadList(fp, &c->sfxFlags[i]) || !cacheReadList(fp, &c->sfxLibrary[i]) ||
        !cacheReadList(fp, &c->sfxHeaders[i]) || !cacheReadList(fp, &c->sfxDefines[i]) ||
        !cacheReadString(fp, c->sfxStd[i], sizeof(c->sfxStd[i])))
      return false;
  return true;
}

static bool cacheWriteConfig(FILE *fp, const Config *c) {
  uint8_t cleanBuild = c->cleanBuildDir ? 1 : 0;
  uint8_t cleanCompdb = c->cleanCompileCommands ? 1 : 0;
  uint8_t progress = c->progressBar ? 1 : 0;
  uint8_t progressError = c->progressErrorAlways ? 1 : 0;
  uint8_t binary = c->binary ? 1 : 0;
  uint8_t foreground = c->foreground ? 1 : 0;
  uint8_t logEnabled = c->logEnabled ? 1 : 0;

  if (!cacheWriteString(fp, c->root) || !cacheWriteList(fp, &c->sources) ||
      !cacheWriteList(fp, &c->flags) || !cacheWriteList(fp, &c->headerInternal) ||
      !cacheWriteList(fp, &c->headerPublic) || !cacheWriteList(fp, &c->libraries) ||
      !cacheWriteList(fp, &c->compilers) || !cacheWriteList(fp, &c->defines) ||
      !cacheWriteString(fp, c->std) || !cacheWriteString(fp, c->cc) ||
      !cacheWriteString(fp, c->target) || !cacheWriteU8(fp, cleanBuild) ||
      !cacheWriteU8(fp, cleanCompdb) || !cacheWriteU8(fp, progress) ||
      !cacheWriteU8(fp, progressError) || !cacheWriteU8(fp, foreground) ||
      !cacheWriteU8(fp, logEnabled) ||
      !cacheWriteU8(fp, binary) || !cacheWriteString(fp, c->outBinaryName) ||
      !cacheWriteString(fp, c->outBinaryDir) || !cacheWriteString(fp, c->outBuildDir) ||
      !cacheWriteString(fp, c->outCompileCommands) || !cacheWriteString(fp, c->outLibName) ||
      !cacheWriteString(fp, c->outLibDir) || !cacheWriteU8(fp, c->libRequested ? 1 : 0) ||
      !cacheWriteU8(fp, c->libStatic ? 1 : 0) || !cacheWriteU8(fp, c->libShared ? 1 : 0) ||
      !cacheWriteList(fp, &c->excludes) || !cacheWriteString(fp, c->pack.name) ||
      !cacheWriteString(fp, c->pack.version) || !cacheWriteList(fp, &c->pack.files) ||
      !cacheWriteString(fp, c->pack.output) || !cacheWriteString(fp, c->pack.compress) ||
      !cacheWriteString(fp, c->pack.checksum) || !cacheWriteString(fp, c->pack.format) ||
      !cacheWriteString(fp, c->pack.debMaintainer) ||
      !cacheWriteString(fp, c->pack.debDescription) ||
      !cacheWriteString(fp, c->pack.debInstallPrefix) ||
      !cacheWriteString(fp, c->pack.debArchitecture) ||
      !cacheWriteU8(fp, c->pack.requested ? 1 : 0) || !cacheWriteU8(fp, c->langCpp ? 1 : 0) ||
      !cacheWriteSfx(fp, c) || !cacheWriteU32(fp, (uint32_t)c->embCount))
    return false;

  for (int i = 0; i < c->embCount; i++)
    if (!cacheWriteEmbedded(fp, &c->emb[i])) return false;
  return true;
}

static bool cacheReadConfig(FILE *fp, Config *c) {
  uint8_t cleanBuild = 0, cleanCompdb = 0, progress = 0, progressError = 0, foreground = 0;
  uint8_t binary = 1, logEnabled = 0;
  uint8_t libRequested = 0, libStatic = 0, libShared = 0, packRequested = 0, langCpp = 0;
  uint32_t embCount = 0;

  if (!cacheReadString(fp, c->root, sizeof(c->root)) || !cacheReadList(fp, &c->sources) ||
      !cacheReadList(fp, &c->flags) || !cacheReadList(fp, &c->headerInternal) ||
      !cacheReadList(fp, &c->headerPublic) ||      !cacheReadList(fp, &c->libraries) ||
      !cacheReadList(fp, &c->compilers) || !cacheReadList(fp, &c->defines) ||
      !cacheReadString(fp, c->std, sizeof(c->std)) || !cacheReadString(fp, c->cc, sizeof(c->cc)) ||
      !cacheReadString(fp, c->target, sizeof(c->target)) || !cacheReadU8(fp, &cleanBuild) ||
      !cacheReadU8(fp, &cleanCompdb) || !cacheReadU8(fp, &progress) ||
      !cacheReadU8(fp, &progressError) || !cacheReadU8(fp, &foreground) ||
      !cacheReadU8(fp, &logEnabled) ||
      !cacheReadU8(fp, &binary) ||
      !cacheReadString(fp, c->outBinaryName, sizeof(c->outBinaryName)) ||
      !cacheReadString(fp, c->outBinaryDir, sizeof(c->outBinaryDir)) ||
      !cacheReadString(fp, c->outBuildDir, sizeof(c->outBuildDir)) ||
      !cacheReadString(fp, c->outCompileCommands, sizeof(c->outCompileCommands)) ||
      !cacheReadString(fp, c->outLibName, sizeof(c->outLibName)) ||
      !cacheReadString(fp, c->outLibDir, sizeof(c->outLibDir)) || !cacheReadU8(fp, &libRequested) ||
      !cacheReadU8(fp, &libStatic) || !cacheReadU8(fp, &libShared) ||
      !cacheReadList(fp, &c->excludes) ||
      !cacheReadString(fp, c->pack.name, sizeof(c->pack.name)) ||
      !cacheReadString(fp, c->pack.version, sizeof(c->pack.version)) ||
      !cacheReadList(fp, &c->pack.files) ||
      !cacheReadString(fp, c->pack.output, sizeof(c->pack.output)) ||
      !cacheReadString(fp, c->pack.compress, sizeof(c->pack.compress)) ||
      !cacheReadString(fp, c->pack.checksum, sizeof(c->pack.checksum)) ||
      !cacheReadString(fp, c->pack.format, sizeof(c->pack.format)) ||
      !cacheReadString(fp, c->pack.debMaintainer, sizeof(c->pack.debMaintainer)) ||
      !cacheReadString(fp, c->pack.debDescription, sizeof(c->pack.debDescription)) ||
      !cacheReadString(fp, c->pack.debInstallPrefix, sizeof(c->pack.debInstallPrefix)) ||
      !cacheReadString(fp, c->pack.debArchitecture, sizeof(c->pack.debArchitecture)) ||
      !cacheReadU8(fp, &packRequested) || !cacheReadU8(fp, &langCpp) || !cacheReadSfx(fp, c) ||
      !cacheReadU32(fp, &embCount) || embCount > MAX_EMBEDDED)
    return false;
  c->pack.requested = packRequested != 0;
  c->langCpp = langCpp != 0;

  c->cleanBuildDir = cleanBuild != 0;
  c->cleanCompileCommands = cleanCompdb != 0;
  c->progressBar = progress != 0;
  c->progressErrorAlways = progressError != 0;
  c->foreground = foreground != 0;
  c->logEnabled = logEnabled != 0;
  c->binary = binary != 0;
  c->libRequested = libRequested != 0;
  c->libStatic = libStatic != 0;
  c->libShared = libShared != 0;
  c->embCount = (int)embCount;
  for (int i = 0; i < c->embCount; i++)
    if (!cacheReadEmbedded(fp, &c->emb[i])) return false;
  return true;
}

void cfgCachePathFor(const char *path, char *out, size_t n) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(path, '\\');
  if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
  const char *base = slash ? slash + 1 : path;

  /* Nama cache per-file konfigurasi: "Buildfile" memakai nama lama
     (kompatibel dengan cache yang sudah ada), selain itu <basename>.cache
     agar `satria -f lain` tidak berbagi cache dengan default. */
  const char *cacheName = strcmp(base, "Buildfile") == 0 ? CONFIG_CACHE_FILE : NULL;
  char nameBuf[64];
  if (!cacheName) {
    snprintf(nameBuf, sizeof(nameBuf), "%s.cache", base);
    cacheName = nameBuf;
  }

  if (slash) {
    size_t dirLen = (size_t)(slash - path);
    if (dirLen == 0)
      snprintf(out, n, "/%s/%s", CONFIG_CACHE_DIR, cacheName);
    else
      snprintf(out, n, "%.*s/%s/%s", (int)dirLen, path, CONFIG_CACHE_DIR, cacheName);
  } else {
    snprintf(out, n, "%s/%s", CONFIG_CACHE_DIR, cacheName);
  }
}

bool cfgCacheLoad(Config *c, const char *path, const char *cachePath) {
  FILE *fp = fopen(cachePath, "rb");
  if (!fp) return false;

  char magic[sizeof(CONFIG_CACHE_MAGIC) - 1];
  uint32_t version = 0;
  int64_t cachedMTimeNs = 0;
  int64_t cachedSize = 0;
  int64_t currentMTimeNs = fsMTimeNs(path);
  long long currentSize = fsFileSize(path);

  bool ok = cacheReadBytes(fp, magic, sizeof(magic)) &&
            memcmp(magic, CONFIG_CACHE_MAGIC, sizeof(magic)) == 0 && cacheReadU32(fp, &version) &&
            version == CONFIG_CACHE_VERSION &&
            cacheReadBytes(fp, &cachedMTimeNs, sizeof(cachedMTimeNs)) &&
            cacheReadBytes(fp, &cachedSize, sizeof(cachedSize)) && currentMTimeNs >= 0 &&
            currentSize >= 0 && cachedMTimeNs == currentMTimeNs &&
            cachedSize == (int64_t)currentSize && cacheReadConfig(fp, c);
  fclose(fp);
  return ok;
}

void cfgCacheSave(const Config *c, const char *path, const char *cachePath) {
  int64_t mtimeNs = fsMTimeNs(path);
  long long size = fsFileSize(path);
  if (mtimeNs < 0 || size < 0) return;

  char parent[MAX_PATH];
  snprintf(parent, sizeof(parent), "%s", cachePath);
  char *slash = strrchr(parent, '/');
#ifdef _WIN32
  char *backslash = strrchr(parent, '\\');
  if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
  if (slash) {
    *slash = '\0';
    mkdirs(parent);
  }

  char tmp[MAX_PATH + 32];
  snprintf(tmp, sizeof(tmp), "%s.tmp", cachePath);
  FILE *fp = fopen(tmp, "wb");
  if (!fp) return;

  const char magic[] = CONFIG_CACHE_MAGIC;
  bool ok = cacheWriteBytes(fp, magic, sizeof(magic) - 1) &&
            cacheWriteU32(fp, CONFIG_CACHE_VERSION) &&
            cacheWriteBytes(fp, &mtimeNs, sizeof(int64_t)) &&
            cacheWriteBytes(fp, &size, sizeof(int64_t)) && cacheWriteConfig(fp, c);
  if (fclose(fp) != 0) ok = false;
  if (ok) {
    /* Penggantian atomik (fsRenameReplace: MoveFileEx REPLACE_EXISTING di
       Windows — MSVCRT rename menolak mengganti file yang ada). */
    if (!fsRenameReplace(tmp, cachePath)) fsRemoveFile(tmp);
  } else {
    fsRemoveFile(tmp);
  }
}

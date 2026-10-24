#ifndef SATRIA_V0_1_0_CONFIG_CACHE_H
#define SATRIA_V0_1_0_CONFIG_CACHE_H

#include <stddef.h>

#include "../config.h"

/*
 * config_cache — Persistent Buildfile cache.
 *
 * Dipindah apa adanya dari config.c: hasil parse Buildfile diserialisasi
 * ke .satria/startup.state (per-nama file konfigurasi) dan divalidasi
 * lewat mtime+size. Format biner dibaca/ditulis lewat helper cache*()
 * internal di file .c-nya.
 *
 *   cfgCachePathFor : hitung path cache untuk file konfigurasi `path`.
 *   cfgCacheLoad    : true bila cache valid (magic+version+mtime+size cocok)
 *                     dan `c` terisi penuh.
 *   cfgCacheSave    : tulis cache secara atomik (tmp lalu rename).
 */

void cfgCachePathFor(const char *path, char *out, size_t n);
bool cfgCacheLoad(Config *c, const char *path, const char *cachePath);
void cfgCacheSave(const Config *c, const char *path, const char *cachePath);

#endif /* SATRIA_V0_1_0_CONFIG_CACHE_H */

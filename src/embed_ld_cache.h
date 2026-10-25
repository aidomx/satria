#ifndef SATRIA_V0_1_0_EMBED_LD_CACHE_H
#define SATRIA_V0_1_0_EMBED_LD_CACHE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * embed_ld_cache — cache persisten hasil deteksi GNU ld (.satria/ld.cache).
 *
 * Jalur build tidak lagi memakai ld (codegen embedded.c — design/
 * embedded.md); modul ini dipakai untuk validasi stamp 'L' legacy di
 * fast-state dan probe `satria profile embed=`. Verdict probe disimpan
 * lintas-run: run berikutnya cukup SATU stat ke binary ld hasil resolusi
 * PATH (path + mtime ns + size asli) untuk memvalidasi — tanpa spawn
 * `ld --version` (3–5 ms).
 */

/* Resolusi binary `exe` (mis. "ld") di PATH, semantik setara probeAvailable.
   Path tersimpan di `out` (kapasitas n) sebagai key cache; false bila
   tidak ditemukan. Hanya ada di POSIX — cache tidak dipakai di Windows.
*/
bool ldCacheResolve(const char *exe, char *out, size_t n);

/* Baca verdict tersimpan; hanya return true bila binary di `ldPath` masih
   persis yang diperiksa (path + mtime ns + size cocok — satu stat).
   false = cache tidak ada/rusak/key beda → pemanggil menjalankan probe
   fisik sendiri lalu menyimpannya via cacheWriteLD. */
bool cacheReadLD(const char *ldPath, bool *out);

/* Fast cache check: validate .satria/ld.cache for an already-resolved ld; never probes. */
bool cacheCheckLD(const char *ldPath, bool *out);

/* Update cache for an already-resolved ld using an already-probed verdict. */
void cacheUpdateLD(const char *ldPath, bool gnu);

/* Simpan verdict probe (gnu=true berarti GNU ld) terkait binary ldPath.
   Diam bila stat path gagal; tulis atomik (file .tmp lalu rename). */
void cacheWriteLD(const char *ldPath, bool gnu);

/* Nama file cache (".satria/ld.cache") — dipakai cmdClean saat menghapus
   state build. */
const char *ldCacheFile(void);

#endif /* SATRIA_V0_1_0_EMBED_LD_CACHE_H */

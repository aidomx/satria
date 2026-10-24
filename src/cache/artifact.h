/* artifact.cache — .satria/artifact.cache (design/startup.md §9): metadata
   artefak TANPA kode C (paket release, arsip proyek kemasan) dan input release
   (sisi src `release.files`) yang tidak tercakup fingerprint.

   Mengapa perlu: tanpa metadata tertulis, no-op tidak bisa memvalidasi
   artefak release — inilah alasan gerbang no-op ditutup saat release aktif.
   Alur: run sukses mendaftarkan path (artifactCacheAdd) lalu flush
   (artifactCacheSave); no-op memvalidasi semua entri (artifactCacheValid)
   — file hilang/berubah -> batal no-op dan artefak dibangun ulang. */
#ifndef SATRIA_ARTIFACT_H
#define SATRIA_ARTIFACT_H
#include <stdbool.h>

#define ARTIFACT_CACHE_FILE ".satria/artifact.cache"

/* Daftarkan path artefak/input (file ATAU folder) selama run; entri folder
   dibangun rekursif (D per direktori + F per file) saat flush. Path yang tidak
   ada saat flush dilewati. */
void artifactCacheAdd(const char *path);

/* Tulis ulang artifact.cache dari daftar run ini (hapus-lalu-tulis, segar).
   Daftar kosong: file lama dipertahankan; file belum ada -> dibuat (kosong)
   supaya cold build selalu menghasilkan .satria/artifact.cache. */
bool artifactCacheSave(void);

/* Validasi seluruh entri: stat (mtime ns + ukuran), fallback hash isi bila
   stamp beda (sentuh -> PATCH time/size, tanpa batal). false bila file
   hilang/rusak/entri berubah/hilang. */
bool artifactCacheValid(void);
#endif

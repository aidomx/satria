/* state_blob — state per-proyek workspace di caches/state.cache (stage 2).

   Menggantikan file per proyek `.satria/workspace/<proyek>.<leaf>`
   (build.fingerprint, src, ws.state, hdr/<hash>.txt). Setiap (proyek, leaf)
   disimpan sebagai SATU record cache_store:
     PUT <hash proyek> <hash leaf> 0 0 <size> V<isi di-escape>
   payload "V<isi>" = nilai; payload "D" = dihapus (marker pending).

   Penulisan dikumpulkan di memori (pending) dan di-flush sekali per run
   (stateBlobFlush). Anak fork (batch paralel) menulis ke stage sendiri
   (stateBlobExport) dan induk menggabungkannya (stateBlobImport). Jalur
   single-project TIDAK memakai modul ini. */
#ifndef SATRIA_STATE_BLOB_H
#define SATRIA_STATE_BLOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATE_CACHE_FILE ".satria/caches/state.cache"

/* Ambil isi (proyek, leaf). false bila tak ada. *buf milik pemanggil (free). */
bool stateBlobGet(const char *proj, const char *leaf, char **buf, size_t *len);
/* Simpan isi (pending; tertulis saat stateBlobFlush). */
bool stateBlobPut(const char *proj, const char *leaf, const char *buf, size_t len);
bool stateBlobDelete(const char *proj, const char *leaf);
bool stateBlobExists(const char *proj, const char *leaf);
/* FNV-1a 64 atas isi; 0 bila tak ada atau kosong. */
uint64_t stateBlobHash(const char *proj, const char *leaf);

/* Stamp metadata grup P (§4) di state.cache: P <proj> <content> <time> <size>
   — format & makna identik dengan baris P di cache grup lain dan kedua mode
   build (design/startup.md §4). Dibuka dari file (bukan pending): aman
   dipanggil kapan pun — flush tertunda memuat ulang file yang sudah ber-meta
   sehingga meta ikut bertahan. File belum ada / grup tanpa entri: no-op. */
bool stateBlobProjMeta(const char *proj, uint64_t content, long long time);

/* Tulis seluruh pending ke state.cache (atomik). Tanpa pending: no-op. */
bool stateBlobFlush(void);
/* Anak fork: buang pending & base warisan (sebelum menulis apa pun). */
void stateBlobForkChild(void);
/* Anak fork: tulis pending ke file stage lalu lepas. */
bool stateBlobExport(const char *stagePath);
/* Induk: gabungkan file stage ke pending, lalu hapus file stage. */
bool stateBlobImport(const char *stagePath);

/* Pembaca baris mirip fgets atas buffer memori (untuk parser teks state). */
char *stateBufGets(const char *buf, size_t len, size_t *pos, char *line, int n);

#endif

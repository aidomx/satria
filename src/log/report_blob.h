/* report_blob — .satria/report.blob: ringkasan laporan build (design/startup.md §10).
   Setelah build sukses, keluaran run TIDAK disimpan mentah: dikondensasi ke
   format terstruktur dua blok — STATUS (pesan no-op) + LAST BUILD SUMMARY
   (total time, arsip/perpustakaan/binary/release agregat, status). Baris
   Releases hanya ada bila fase release dijalankan pada build tersimpan. Pada
   no-op file itu diputar ulang sehingga pesan no-op seragam di kedua mode.
   Output proses build TIDAK berubah: stdout di-tee (terminal tetap menerima
   salinan yang sama). */
#ifndef SATRIA_REPORT_BLOB_H
#define SATRIA_REPORT_BLOB_H
#include <stddef.h>
#include <stdbool.h>
/* Jalur cache untuk baris "> Cache valid: ..." di blok STATUS. Default
   .satria/caches/state.cache (single); workspace menyetel WS_CACHE_FP. */
void reportBlobCachePath(const char *p);
/* Mulai menangkap stdout (tee). Panggil SETELAH gerbang no-op: run no-op tidak
   menyentuh file. Tanpa-opsi bila sudah aktif / di Windows (tidak ada tee). */
void reportBlobBegin(void);
/* Selesai menangkap. save=true -> tmp dijadikan report.blob (hanya bila ada
   isi); save=false -> tmp dibuang, laporan lama dipertahankan. Tanpa-opsi
   bila tidak aktif. */
bool reportBlobFinish(bool save);
/* Tampilkan isi report.blob tersimpan ke stdout (bila file ada). */
void reportBlobShow(void);
/* true bila report.blob tersimpan dan berisi (ringkasan siap ditampilkan). */
bool reportBlobHas(void);
#endif

/* Salin isi report.blob (free *buf oleh pemanggil). false bila tak ada. */
bool reportBlobRead(char **buf, size_t *len);
/* Pulihkan report.blob dari salinan state (atomik). */
bool reportBlobRestore(const char *buf, size_t len);


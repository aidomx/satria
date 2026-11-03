#pragma once
#include <stddef.h>
/* Diagnostic stream recorder. No cache or workspace-state dependency. */
int buildLogShow(void);
void buildLogBegin(void);
void buildLogEnd(int status);
/* Relay stderr aktif? (log = true pada Buildfile). Dipakai report untuk
   memutuskan baris WARN: dengan log aktif output mentah compiler pindah ke
   build.log, report menampilkan WARN (design/build_report.md §5). Di Windows
   relay POSIX tidak tersedia — selalu 0. */
int buildLogActive(void);
/*
 * Salinan output compiler yang ditangkap (procCaptureTake) masuk build.log
 * — dipanggil compile loop SETELAH baris progress. Relay tidak melihat
 * output ini (sudah diarahkan ke file tangkapan), jadi tanpa API ini log
 * kehilangan warning/error compiler. Bila ada SATU baris pun diagnostik di
 * buffer, SELURUH buffer ditulis (konteks source/note ikut — `satria log`
 * memperlihatkan diagnostic lengkap saat log = true memindahkan output
 * mentah dari report). Tanpa diagnostik: tidak ada tulisan, file tidak
 * dibuat (design/startup.md §3). No-op bila relay tidak aktif (log = false).
 */
void buildLogCapture(const char *buf, size_t n);

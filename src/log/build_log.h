#pragma once
#include <stddef.h>
/* Diagnostic stream recorder. No cache or workspace-state dependency. */
int buildLogShow(void);
void buildLogBegin(void);
void buildLogEnd(int status);
/*
 * Salinan output compiler yang ditangkap (procCaptureTake) masuk build.log
 * dengan filter diagnostic yang sama dengan relay stderr — dipanggil compile
 * loop SETELAH baris progress. Relay tidak melihat output ini (sudah
 * diarahkan ke file tangkapan), jadi tanpa API ini log kehilangan
 * warning/error compiler. No-op bila relay tidak aktif (log = false):
 * build.log tidak pernah dibuat oleh jalur ini.
 */
void buildLogCapture(const char *buf, size_t n);

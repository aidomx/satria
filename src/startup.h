#pragma once
/* startup.h  Output fase startup BERSAMA untuk single-project dan workspace
 * (design/startup.md 8). Satu tempat untuk kontrol visibilitas: fase hanya
 * tampil bila visible (stdout/stderr TTY dan tidak quiet), dan fase tercetak
 * SAAT SELESAI (pesan dari parser tidak menimpa baris progres).
 *
 * Pemakaian:  startupBegin(visible);  startupPhase("nama");  ...
 *             startupPhaseEnd(ok);    -- akhiri fase terakhir.
 *
 * Rekaman: setiap baris yang dihasilkan juga disimpan di memori sampai
 * startupBegin berikutnya (startupRecordHas/startupRecordSave). Dipakai
 * background.c: pada spawn (background = true) induk TIDAK mencetak startup
 * ke terminal — cukup direkam, lalu `satria watch` me-replay-nya. */
#include <stdbool.h>

/* Visibilitas: TTY dan tidak dalam mode quiet (deteksi anggota workspace). */
bool startupVisible(bool quiet);

/* Mulai satu run startup: bila visible, cetak "startup :". Wajib dipanggil
 * sebelum startupPhase/startupPhaseEnd. */
void startupBegin(bool visible);

/* Mulai fase baru; fase sebelumnya otomatis selesai (done). */
void startupPhase(const char *msg);

/* Akhiri fase terakhir: ok -> "done", gagal -> "failed". */
void startupPhaseEnd(bool ok);

/* Ada baris startup terekam sejak startupBegin terakhir (fase memang hanya
   dijalankan pada cold start; hangus saat startupBegin berikutnya). */
bool startupRecordHas(void);

/* Tulis rekaman baris startup ke `path` (ditimpa) — dipakai background.c
   menyimpan .satria/background.startup untuk replay `satria watch`. */
void startupRecordSave(const char *path);

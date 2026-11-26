/* runners.c  Kebijakan pemilihan runner batch workspace (lihat runners.h). */
#include <stdlib.h>
#include <string.h>

#include "runners.h"
#include "../log/report_blob.h" /* stdout ASLI sebelum relay tee */

#ifndef _WIN32
#include <unistd.h>
#endif

bool runnerStreamOutput(void) {
#ifdef _WIN32
  return false;
#else
  const char *env = getenv("SATRIA_WS_PAR");
  if (env && *env) return strcmp(env, "0") != 0;
  /* Pipa/soket: konsumen membaca sambil build berjalan (`satria | satria -w`,
     `| tee log`, wrapper CI). Anak menulis LANGSUNG ke stdout pada mode ini —
     file slot menahan seluruh progres proyek sampai proyek itu selesai,
     sehingga pada cold build proyek besar (rupaws: 176 objek) terminal diam
     puluhan detik lalu seluruh blok muncul sekaligus. Redirect ke berkas
     REGULER tetap memakai slot + urutan worklist: log unit test/CI harus
     deterministik.
     Jenis stdout dibaca dari fd ASLI: selama build report_blob memasang pipe
     tee di STDOUT_FILENO, jadi fstat(STDOUT_FILENO) selalu "pipe". */
  return reportBlobStdoutIsPipe();
#endif
}

bool runnerWantParallel(int n, int jobs) {
#ifdef _WIN32
  (void)n; (void)jobs;
  return false; /* fork tidak tersedia: serial */
#else
  if (n <= 1 || jobs <= 1) return false;
  const char *env = getenv("SATRIA_WS_PAR");
  if (env && *env) return strcmp(env, "0") != 0; /* 1 = paksa, 0 = serial */
  /* Default PARALEL untuk batch >1 proyek, KECUALI stdout TTY. Runner serial
     menjalankan link per proyek berurutan (ar/so fase library, lalu binary),
     sementara satu panggilan `cc`/`ld` berbiaya tetap ~0,25-0,37 dtk di mesin
     bersyscall lambat dan tidak bisa dibagi ke dalam proyek. Penjadwal
     siap-dep menindih link proyek independen tanpa mengubah urutan CETAK
     (flush urut worklist untuk berkas reguler; pipe live, lihat
     runnerStreamOutput). Ukuran bench:
     hedit 2,9 -> 1,8 dtk, libhdr 2,3 -> 1,6 dtk (setara make -jN).
     Di terminal (TTY) output proyek tetap realtime: serial, karena anak fork
     menulis ke slot yang baru dicetak saat proyeknya selesai.
     TTY dibaca dari stdout ASLI (reportBlobStdoutIsTty): selama build
     report_blob sudah mengganti STDOUT_FILENO dengan pipe tee, sehingga
     isatty(STDOUT_FILENO) selalu false dan terminal keliru memilih paralel —
     progres lalu tertahan per proyek sampai slotnya dicetak. */
#ifndef _WIN32
  return !reportBlobStdoutIsTty();
#else
  return true; /* tak tercapai: _WIN32 sudah return false di atas */
#endif
#endif
}

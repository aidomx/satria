#pragma once
/* satria trace: jalankan satria dengan tracer openat (LD_PRELOAD) lalu rangkum
   log akses file (setara `sort | uniq -c | sort -rn | head`). Hanya POSIX:
   tracer memakai dlsym(RTLD_NEXT). Windows menolak dengan pesan jelas.

   satria trace [--out FILE] [--top N] [--so FILE] [--] <args satria>
   Opsi trace harus di depan. Token pertama yang bukan opsi trace mulai argumen
   untuk satria yang dijalankan (mis. `satria trace -w core`). */
int cmdTrace(const char *const *args, int nargs, const char *argv0);

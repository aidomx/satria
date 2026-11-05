#include <rupa.h>
#include <stdio.h>

/* Nilai tetap, bukan bade_value()+1. Pemanggilan lintas ke bade_value()
   membuat simbol belum terselesaikan di shared library core: di Linux lolos
   (diselesaikan saat runtime), di Windows DLL gagal link. Relasi cdeps
   dua arah tetap dideklarasikan di Buildfile.ws; yang diuji tes graf, bukan
   simbol. Lihat todos/4_11_2026_optimizer.md (siklus cdeps shared Windows). */
int core_value(void) {
  return 8;
}

void thisCore() {
  printf("this core\n");
}

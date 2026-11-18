#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "satria.h"
#include "bench/bench.h"

/*
 * src/main.c — dispatcher & loader.
 *
 * File ini TIDAK berisi operasi satria itu sendiri. Tugasnya hanya:
 * mendelegasikan seluruh argumen ke satriaRun(), yang diimplementasikan oleh src/satria.c
 */

int main(int argc, const char *argv[]) {
  /* Mode shim benchmark: binary ini dieksekusi lewat symlink .wrap/gcc atau
     .wrap/ld (RB_BENCH_SHIM di-set `satria bench`). */
  if (getenv("RB_BENCH_SHIM") && argv[0]) {
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (strcmp(base, "gcc") == 0 || strcmp(base, "ld") == 0)
      return benchShimMain(argc, (char **)argv);
  }

  if (argc > 1 && (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0)) {
    printf("satria %s\n", satriaVersion());
    return 0;
  }

  /* Semua command lain (default build, init, clean, help, ...)
     didelegasikan ke implementasi versi aktif. */
  return satriaRun(argc, argv);
}

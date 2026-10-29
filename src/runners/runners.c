/* runners.c  Kebijakan pemilihan runner batch workspace (lihat runners.h). */
#include <stdlib.h>
#include <string.h>

#include "runners.h"
#include "../background.h"

bool runnerWantParallel(int n, int jobs) {
#ifdef _WIN32
  (void)n; (void)jobs;
  return false; /* fork tidak tersedia: serial */
#else
  if (n <= 1 || jobs <= 1) return false;
  const char *env = getenv("SATRIA_WS_PAR");
  if (env && *env) return strcmp(env, "0") != 0; /* 1 = paksa, 0 = serial */
  return bgIsChild();  /* default: paralel hanya di child background */
#endif
}

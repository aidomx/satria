#include "prof.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/*
 * Implementasi prof — lihat prof.h.
 * Buffer kecil statis; label adalah string literal pemanggil (tidak
 * disalin). Melebihi kapasitas: hitungan tetap naik agar laporan tahu
 * bahwa ada yang terpotong.
 */

#define PROF_MAX 96 /* sub-fase audit (walk-*, tail-*, gate-*, cfg-*, fs-*) menambah penanda per blok */
/*#define _POSIX_C_SOURCE 199309L*/

typedef struct {
  const char *label;
  double t;
} ProfMark;

static bool g_on = false;
static ProfMark g_marks[PROF_MAX];
static int g_n = 0;

static double profNow(void) {
#ifdef _WIN32
  static LARGE_INTEGER freq;
  static bool initialized = false;
  LARGE_INTEGER counter;

  if (!initialized) {
    if (!QueryPerformanceFrequency(&freq)) return 0.0;
    initialized = true;
  }

  if (!QueryPerformanceCounter(&counter)) return 0.0;
  return (double)counter.QuadPart / (double)freq.QuadPart;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

void profInit(void) {
  const char *e = getenv("SATRIA_PROFILE");
  g_on = e && *e && strcmp(e, "0") != 0;
  g_n = 0;
}

bool profOn(void) {
  return g_on;
}

void profMark(const char *label) {
  if (!g_on || !label) return;
  if (g_n < PROF_MAX) {
    g_marks[g_n].label = label;
    g_marks[g_n].t = profNow();
  }
  g_n++;
}

void profReport(void) {
  int n = g_n < PROF_MAX ? g_n : PROF_MAX;
  if (g_on && n >= 2) {
    double t0 = g_marks[0].t;
    double prev = t0;
    fprintf(stderr, "\n> Profile   : %s\n", g_marks[0].label);
    for (int i = 1; i < n; i++) {
      fprintf(stderr, "  %-16s %8.1f ms\n", g_marks[i].label, (g_marks[i].t - prev) * 1000.0);
      prev = g_marks[i].t;
    }
    /* sisa waktu dari penanda terakhir sampai laporan (mis. fase pack +
       pencetakan Summary) — melengkapi total. */
    fprintf(stderr, "  %-16s %8.1f ms\n", "(sampai laporan)", (profNow() - prev) * 1000.0);
    fprintf(stderr, "  %-16s %8.1f ms\n", "total", (profNow() - t0) * 1000.0);
    if (g_n > PROF_MAX) fprintf(stderr, "  (%d penanda terpotong)\n", g_n - PROF_MAX);
  }
  g_n = 0; /* consume: blok berikutnya mulai dari nol */
}

/* ==================== sesi profile ==================== */

/*
 * Implementasi sesi (lihat prof.h). Tidak memakai g_on: command profile
 * selalu mengukur, terlepas dari SATRIA_PROFILE. PROF_MAX fase cukup untuk
 * context saat ini (<= 5 fase per context); fase melebihi kapasitas
 * dibuang senyap — jumlah context kecil dan tertib.
 */
static ProfPhase g_phases[PROF_MAX];
static int g_np = 0;
static double g_phaseT0 = 0.0;
static const char *g_phaseLabel = NULL;

void profSessionReset(void) {
  g_np = 0;
  g_phaseLabel = NULL;
}

void profPhaseBegin(const char *label) {
  if (!label) return;
  g_phaseLabel = label;
  g_phaseT0 = profNow();
}

void profPhaseEnd(uint64_t inBytes, uint64_t outBytes) {
  if (!g_phaseLabel) return;
  if (g_np < PROF_MAX) {
    g_phases[g_np].label = g_phaseLabel;
    g_phases[g_np].ms = (profNow() - g_phaseT0) * 1000.0;
    g_phases[g_np].inBytes = inBytes;
    g_phases[g_np].outBytes = outBytes;
    g_np++;
  }
  g_phaseLabel = NULL;
}

const ProfPhase *profPhases(int *count) {
  if (count) *count = g_np < PROF_MAX ? g_np : PROF_MAX;
  return g_phases;
}

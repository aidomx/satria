/* startup.c  Output fase startup bersama (lihat startup.h). Dipakai
 * commands.c (single-project), background.c (spawn single) dan
 * workspace/ws_startup.c (workspace). */
#include <stdio.h>
#include <string.h>

#include "startup.h"
#include "portability.h"

static bool g_visible = false;     /* run startup ini terlihat di terminal */
static const char *g_phase = NULL; /* fase yang sedang berjalan (belum selesai) */

/* Rekaman baris startup (lihat startup.h): selalu diisi, terminal atau tidak,
   supaya `satria watch` bisa me-replay blok startup run background. */
#define REC_MAX 1024
static char g_rec[REC_MAX];
static size_t g_recLen = 0;

static void recAdd(const char *s) {
  size_t n = strlen(s);
  if (n >= REC_MAX - g_recLen) return; /* buffer penuh: sisanya dibuang */
  memcpy(g_rec + g_recLen, s, n);
  g_recLen += n;
  g_rec[g_recLen] = '\0';
}

bool startupVisible(bool quiet) { return !quiet && termIsTTY(); }

void startupBegin(bool visible) {
  g_visible = visible;
  g_phase = NULL;
  g_recLen = 0;
  g_rec[0] = '\0';
  recAdd("startup :\n");
  if (g_visible) {
    fprintf(stderr, "startup :\n");
    fflush(stderr);
  }
}

static void startupFinish(bool ok) {
  if (!g_phase) return;
  char line[256];
  snprintf(line, sizeof(line), "> %s: %s\n", g_phase, ok ? "done" : "failed");
  recAdd(line);
  if (g_visible) {
    fputs(line, stderr);
    fflush(stderr);
  }
  g_phase = NULL;
}

void startupPhase(const char *msg) {
  startupFinish(true);
  g_phase = msg;
}

void startupPhaseEnd(bool ok) { startupFinish(ok); }

bool startupRecordHas(void) { return g_recLen > 0; }

void startupRecordSave(const char *path) {
  if (!path || !path[0]) return;
  FILE *f = fopen(path, "w");
  if (!f) return;
  fwrite(g_rec, 1, g_recLen, f);
  fclose(f);
}

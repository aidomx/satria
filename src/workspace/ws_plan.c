/* ws_plan.c — Router plan (eksperimen P4, design/router_centric.md).

   Peran: induk workspace adalah ROUTER. Sebelum executor (wsRunBatch)
   dijalankan, router menetapkan rencana eksplisit: proyek mana yang boleh
   dieksekusi pada fase ini (library atau binary). Executor hanya boleh
   menjalankan rencana itu; setiap eksekusi menulis penanda di .satria/ agar
   router dapat MEMVERIFIKASI pasca-eksekusi bahwa yang berjalan persis
   seperti rencana.

   Pilot ini read-only: keputusan build, cache, dan penulisan state tidak
   berubah. Aktif hanya bila SATRIA_ROUTER_PLAN diset (bukan "0"). Tanpa env
   itu seluruh fungsi menjadi no-op (satu getenv ter-cache per proses).
*/
#include "ws_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char name[WS_NAME_LEN];
  bool libOnly;
} WsPlanEntry;

static WsPlanEntry g_plan[WS_MAX_PROJECTS];
static int g_planN = 0;
static int g_planEnv = -1; /* -1 belum dibaca, 0 off, 1 on */

static bool wsPlanOn(void) {
  if (g_planEnv < 0) {
    const char *v = getenv("SATRIA_ROUTER_PLAN");
    g_planEnv = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
  }
  return g_planEnv == 1;
}

bool wsPlanActive(void) { return wsPlanOn(); }

static void wsPlanMarker(int i, char *out, size_t cap) {
  snprintf(out, cap, ".satria/ws-plan-%d", i);
}

void wsPlanBegin(const WsModel *m, const WsWork *work, int n, bool libOnly) {
  g_planN = 0;
  if (!wsPlanOn()) return;
  (void)m;
  if (n > WS_MAX_PROJECTS) n = WS_MAX_PROJECTS;
  for (int i = 0; i < n; i++) {
    snprintf(g_plan[i].name, sizeof(g_plan[i].name), "%s", work[i].name);
    g_plan[i].libOnly = libOnly;
  }
  g_planN = n;
}

/* Dipanggil executor tepat sebelum bekerja. true = sesuai rencana (penanda
   ditulis); false = di luar rencana. Nilai kembali bersifat informasional:
   pemanggil tidak boleh mengubah keputusan build karenanya. */
bool wsPlanAdmit(const char *name, bool libOnly) {
  if (!wsPlanOn() || g_planN <= 0) return true;
  int idx = -1;
  for (int i = 0; i < g_planN; i++)
    if (strcmp(g_plan[i].name, name) == 0) {
      idx = i;
      break;
    }
  const char *phase = libOnly ? "library" : "binary";
  if (idx < 0) {
    fprintf(stderr, "satria: router-plan: %s di luar rencana fase %s\n", name, phase);
    return false;
  }
  if (g_plan[idx].libOnly != libOnly) {
    fprintf(stderr, "satria: router-plan: %s dieksekusi fase %s, rencana fase %s\n",
            name, phase, g_plan[idx].libOnly ? "library" : "binary");
    return false;
  }
  char path[64];
  wsPlanMarker(idx, path, sizeof(path));
  FILE *f = fopen(path, "w");
  if (f) {
    fputc(libOnly ? 'L' : 'B', f);
    fclose(f);
  }
  return true;
}

/* Dipanggil router sesudah batch: cocokkan penanda executor dengan rencana.
   rc != 0 memaafkan proyek yang tidak sempat diluncurkan (build berhenti),
   tetapi penanda fase yang salah tetap dilaporkan. */
void wsPlanEnd(int rc) {
  if (!wsPlanOn() || g_planN <= 0) return;
  int ok = 0, bad = 0;
  for (int i = 0; i < g_planN; i++) {
    char path[64];
    wsPlanMarker(i, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
      if (rc == 0) {
        fprintf(stderr, "satria: router-plan: %s tidak dieksekusi (rencana %s)\n",
                g_plan[i].name, g_plan[i].libOnly ? "library" : "binary");
        bad++;
      }
      continue;
    }
    int c = fgetc(f);
    fclose(f);
    remove(path);
    char want = g_plan[i].libOnly ? 'L' : 'B';
    if (c != want) {
      fprintf(stderr, "satria: router-plan: penanda %s tidak cocok (0x%02x != %c)\n",
              g_plan[i].name, c, want);
      bad++;
    } else {
      ok++;
    }
  }
  const char *phase = g_plan[0].libOnly ? "library" : "binary";
  fprintf(stderr, "satria: router-plan: fase %s: %d rencana, %d terverifikasi, %d menyimpang\n",
          phase, g_planN, ok, bad);
  g_planN = 0;
}

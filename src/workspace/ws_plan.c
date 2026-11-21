/* ws_plan.c — Rencana scheduler workspace (eksperimen P4, design/router_centric.md).

   Catatan istilah: ROUTER adalah lapisan cache `src/cache/opt*.c`
   (opts.cache), bukan file ini. File ini adalah rencana eksekusi milik
   scheduler (induk workspace): proyek mana yang boleh dieksekusi pada fase
   ini (library atau binary). Executor hanya menjalankan rencana itu; setiap
   eksekusi menulis penanda di .satria/ agar scheduler dapat MEMVERIFIKASI
   pasca-eksekusi bahwa yang berjalan persis seperti rencana. Fase 1 P4 akan
   mengisi rencana ini dengan keputusan router (route/group/fast-state).

   Pilot ini read-only: keputusan build, cache, dan penulisan state tidak
   berubah. Aktif hanya bila SATRIA_WS_PLAN diset (bukan "0"). Tanpa env
   itu seluruh fungsi menjadi no-op (satu getenv ter-cache per proses).
*/
#include "ws_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char name[WS_NAME_LEN];
  bool libOnly;
  bool routeOk; /* keputusan router: route + grup cache proyek */
  bool fastOk;  /* verdict fast-state yang dihitung scheduler */
} WsPlanEntry;

static WsPlanEntry g_plan[WS_MAX_PROJECTS];
static int g_planN = 0;
static int g_planEnv = -1; /* -1 belum dibaca, 0 off, 1 on */
/* Proyek yang benar-benar membangun membuat verdict fast-state proyek lain
   berpotensi basi. Hit berikutnya jatuh ke validasi executor (perilaku lama). */
static bool g_planDirty = false;

static bool wsPlanOn(void) {
  if (g_planEnv < 0) {
    const char *v = getenv("SATRIA_WS_PLAN");
    g_planEnv = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
  }
  return g_planEnv == 1;
}

bool wsPlanActive(void) { return wsPlanOn(); }

static void wsPlanMarker(int i, char *out, size_t cap) {
  snprintf(out, cap, ".satria/ws-plan-%d", i);
}

/* Fase 1 P4: scheduler menghitung route + verdict fast-state SEBELUM batch.
   Verdict dipakai executor sehingga ia tidak memanggil router/validasi lagi.
   Bila ada proyek yang benar-benar membangun, hit berikutnya tidak dipercaya
   (g_planDirty) — executor memvalidasi ulang seperti jalur lama. */
void wsPlanBegin(const WsModel *m, const WsWork *work, int n, bool libOnly) {
  g_planN = 0;
  g_planDirty = false;
  if (!wsPlanOn()) return;
  if (n > WS_MAX_PROJECTS) n = WS_MAX_PROJECTS;
  /* Buang penanda basi dari run yang berhenti paksa: verifikasi tidak boleh
     memakai penanda eksekusi lama. */
  for (int i = 0; i < WS_MAX_PROJECTS; i++) {
    char path[64];
    wsPlanMarker(i, path, sizeof(path));
    remove(path);
  }
  for (int i = 0; i < n; i++) {
    snprintf(g_plan[i].name, sizeof(g_plan[i].name), "%s", work[i].name);
    g_plan[i].libOnly = libOnly;
    g_plan[i].routeOk = false;
    g_plan[i].fastOk = false;
    cmdsStateSetWsProject(work[i].name);
    /* Label META identik dengan wsRunProject supaya verdict menilai state
       fase yang sama (bahasa efektif termasuk dalam label). */
    WsLang langEff = wsProjectLang(m, work[i].idx);
    const char *langTag = langEff == WS_LANG_C ? "c" : langEff == WS_LANG_CPP ? "cpp" : "hybrid";
    char bfLabel[WS_NAME_LEN + 48];
    snprintf(bfLabel, sizeof(bfLabel), "%s:%s:%s", WORKSPACE_FILENAME, work[i].name, langTag);
    g_plan[i].fastOk = wsFastStateValidOut(bfLabel, libOnly, &g_plan[i].routeOk);
    cmdsStateSetWsProject("");
  }
  g_planN = n;
}

static WsPlanEntry *wsPlanFind(const char *name, bool libOnly) {
  if (!wsPlanOn() || !name) return NULL;
  for (int i = 0; i < g_planN; i++)
    if (g_plan[i].libOnly == libOnly && strcmp(g_plan[i].name, name) == 0) return &g_plan[i];
  return NULL;
}

/* 1 = hit terverifikasi scheduler, 0 = miss terverifikasi, -1 = tidak ada /
   verdict basi -> executor memakai jalur validasi lama. */
int wsPlanFastConsume(const char *name, bool libOnly) {
  WsPlanEntry *e = wsPlanFind(name, libOnly);
  if (!e) return -1;
  if (e->fastOk && g_planDirty) return -1;
  return e->fastOk ? 1 : 0;
}

void wsPlanNoteBuilt(const char *name) {
  if (wsPlanFind(name, false) || wsPlanFind(name, true)) g_planDirty = true;
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
    fprintf(stderr, "satria: ws-plan: %s di luar rencana fase %s\n", name, phase);
    return false;
  }
  if (g_plan[idx].libOnly != libOnly) {
    fprintf(stderr, "satria: ws-plan: %s dieksekusi fase %s, rencana fase %s\n",
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
        fprintf(stderr, "satria: ws-plan: %s tidak dieksekusi (rencana %s)\n",
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
      fprintf(stderr, "satria: ws-plan: penanda %s tidak cocok (0x%02x != %c)\n",
              g_plan[i].name, c, want);
      bad++;
    } else {
      ok++;
    }
  }
  const char *phase = g_plan[0].libOnly ? "library" : "binary";
  int fast = 0, route = 0;
  for (int i = 0; i < g_planN; i++) {
    if (g_plan[i].fastOk) fast++;
    if (g_plan[i].routeOk) route++;
  }
  fprintf(stderr,
          "satria: ws-plan: fase %s: %d rencana, %d terverifikasi, %d menyimpang "
          "(route %d/%d, fast %d/%d)\n",
          phase, g_planN, ok, bad, route, g_planN, fast, g_planN);
  g_planN = 0;
  g_planDirty = false;
}

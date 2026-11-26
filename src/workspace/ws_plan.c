/* ws_plan.c — Rencana scheduler workspace (eksperimen P4, design/router_centric.md).

   Catatan istilah: ROUTER adalah lapisan cache `src/cache/opt*.c`
   (opts.cache), bukan file ini. File ini adalah rencana eksekusi milik
   scheduler (induk workspace): proyek mana yang boleh dieksekusi pada fase
   ini (library atau binary). Executor hanya menjalankan rencana itu; setiap
   eksekusi menulis penanda di .satria/ agar scheduler dapat MEMVERIFIKASI
   pasca-eksekusi bahwa yang berjalan persis seperti rencana. Fase 1 P4 akan
   mengisi rencana ini dengan keputusan router (route/group/fast-state).

   Rencana aktif secara default sejak P4 selesai diuji; `SATRIA_PLAN=0`
   (atau `off`/`false`) mematikan seluruh jalur baru sebagai fallback. Env
   yang sama dipakai jalur single-project; `SATRIA_WS_PLAN` masih diterima
   sebagai alias lama.
   Keputusan build, cache, dan penulisan state tetap identik dengan jalur
   lama (invarian §4 design/router_centric.md).

   Verdict route/fast-state hanya dihitung di depan batch bila runner akan
   paralel (default sejak link lintas proyek ditindih). Anak fork yang
   diluncurkan setelah ada proyek selesai membuang verdict warisan
   (wsPlanInvalidate): proyek itu bisa menulis input bersama. Runner serial
   (SATRIA_WS_PAR=0) tidak precompute dan memvalidasi per proyek di executor.
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
/* Verdict fast-state/route tersedia dari precompute? Runner serial tidak
   precompute — executor memakai jalur validasi lama (consume = -1). */
static bool g_planHaveVerdict = false;
/* Proyek yang benar-benar membangun membuat verdict fast-state proyek lain
   berpotensi basi. Hit berikutnya jatuh ke validasi executor (perilaku lama). */
static bool g_planDirty = false;

static bool wsPlanOn(void) {
  if (g_planEnv < 0) {
    const char *v = getenv("SATRIA_PLAN");
    if (!v || !*v) v = getenv("SATRIA_WS_PLAN"); /* alias lama */
    /* Default aktif; nilai 0/off/false adalah satu-satunya jalan keluar. */
    bool off = v && (strcmp(v, "0") == 0 || strcmp(v, "off") == 0 ||
                     strcmp(v, "false") == 0);
    g_planEnv = off ? 0 : 1;
  }
  return g_planEnv == 1;
}

bool wsPlanActive(void) { return wsPlanOn(); }

typedef struct {
  unsigned char fast;
  unsigned char route;
} WsPlanWire;

static void wsPlanComputeOne(const WsModel *m, const WsWork *work, int i,
                             bool libOnly, WsPlanWire *out) {
  cmdsStateSetWsProject(work[i].name);
  /* Label META identik dengan wsRunProject supaya verdict menilai state
     fase yang sama (bahasa efektif termasuk dalam label). */
  WsLang langEff = wsProjectLang(m, work[i].idx);
  const char *langTag = langEff == WS_LANG_C ? "c" : langEff == WS_LANG_CPP ? "cpp" : "hybrid";
  char bfLabel[WS_NAME_LEN + 48];
  snprintf(bfLabel, sizeof(bfLabel), "%s:%s:%s", WORKSPACE_FILENAME, work[i].name, langTag);
  bool route = false;
  out->fast = wsFastStateValidOut(bfLabel, libOnly, &route) ? 1 : 0;
  out->route = route ? 1 : 0;
  cmdsStateSetWsProject("");
}

#ifndef _WIN32
/* Verdict tiap proyek independen (hanya membaca state). Validasi serial
   membuat scheduler menunggu N proyek sebelum batch; fork di sini mengisi
   hasil yang sama secara paralel. Gagal fork/pipe -> pemanggil menghitung
   ulang serial (perilaku lama). */
static bool wsPlanFork(const WsModel *m, const WsWork *work, int n, bool libOnly,
                       WsPlanWire *out) {
  int workers = cpuCount();
  if (workers < 2) return false;
  if (workers > 8) workers = 8;
  if (workers > n) workers = n;
  int pfd[8][2], made = 0;
  pid_t pid[8];
  size_t start[8], count[8];
  fflush(NULL);
  for (int w = 0; w < workers; w++) {
    if (pipe(pfd[w]) != 0) break;
    size_t s = (size_t)n * (size_t)w / (size_t)workers;
    size_t e = (size_t)n * (size_t)(w + 1) / (size_t)workers;
    pid_t p = fork();
    if (p < 0) {
      close(pfd[w][0]);
      close(pfd[w][1]);
      break;
    }
    if (p == 0) {
      close(pfd[w][0]);
      size_t cnt = e - s, bytes = cnt * sizeof(*out);
      WsPlanWire *local = bytes ? malloc(bytes) : NULL;
      if (local) {
        for (size_t i = 0; i < cnt; i++)
          wsPlanComputeOne(m, work, (int)(s + i), libOnly, &local[i]);
        size_t off = 0;
        while (off < bytes) {
          ssize_t wr = write(pfd[w][1], (char *)local + off, bytes - off);
          if (wr > 0) {
            off += (size_t)wr;
            continue;
          }
          if (wr < 0 && errno == EINTR) continue;
          break;
        }
        free(local);
      }
      _exit(0);
    }
    close(pfd[w][1]);
    pid[w] = p;
    start[w] = s;
    count[w] = e - s;
    made++;
  }
  bool ok = made == workers;
  for (int w = 0; w < made; w++) {
    size_t bytes = count[w] * sizeof(*out);
    size_t base = start[w] * sizeof(*out), off = 0;
    while (off < bytes) {
      ssize_t rd = read(pfd[w][0], (char *)out + base + off, bytes - off);
      if (rd > 0) {
        off += (size_t)rd;
        continue;
      }
      if (rd < 0 && errno == EINTR) continue;
      break;
    }
    close(pfd[w][0]);
    int st = 0;
    while (waitpid(pid[w], &st, 0) < 0 && errno == EINTR) {}
    if (off != bytes || !WIFEXITED(st) || WEXITSTATUS(st) != 0) ok = false;
  }
  return ok;
}
#endif

static void wsPlanMarker(int i, char *out, size_t cap) {
  snprintf(out, cap, ".satria/ws-plan-%d", i);
}

/* Fase 1 P4: scheduler menghitung route + verdict fast-state SEBELUM batch
   saat runner paralel (precompute=true). Verdict dipakai executor sehingga ia
   tidak memanggil router/validasi lagi. Bila ada proyek yang benar-benar
   membangun, hit berikutnya tidak dipercaya (g_planDirty) — executor
   memvalidasi ulang seperti jalur lama.

   precompute=false (runner serial): rencana tetap dibentuk untuk verifikasi
   penanda, tetapi verdict tidak dihitung di depan. Serial memvalidasi tepat
   sebelum proyeknya dieksekusi, jadi precompute hanya menduplikasi kerja
   yang langsung basi begitu proyek dependensi membangun. */
void wsPlanBegin(const WsModel *m, const WsWork *work, int n, bool libOnly,
                 bool precompute) {
  g_planN = 0;
  g_planDirty = false;
  g_planHaveVerdict = false;
  if (!wsPlanOn()) return;
  if (n > WS_MAX_PROJECTS) n = WS_MAX_PROJECTS;
  /* Buang penanda basi dari run yang berhenti paksa: verifikasi tidak boleh
     memakai penanda eksekusi lama. Hanya n indeks yang akan dipakai. */
  for (int i = 0; i < n; i++) {
    char path[64];
    wsPlanMarker(i, path, sizeof(path));
    remove(path);
  }
  for (int i = 0; i < n; i++) {
    snprintf(g_plan[i].name, sizeof(g_plan[i].name), "%s", work[i].name);
    g_plan[i].libOnly = libOnly;
    g_plan[i].routeOk = false;
    g_plan[i].fastOk = false;
  }
  if (precompute) {
    WsPlanWire *wire = n > 0 ? calloc((size_t)n, sizeof(*wire)) : NULL;
    bool parallel = false;
    if (wire) {
#ifndef _WIN32
      if (n >= 2 && !envOff("SATRIA_WSPLAN_PAR"))
        parallel = wsPlanFork(m, work, n, libOnly, wire);
#endif
      if (!parallel)
        for (int i = 0; i < n; i++) wsPlanComputeOne(m, work, i, libOnly, &wire[i]);
      for (int i = 0; i < n; i++) {
        g_plan[i].fastOk = wire[i].fast != 0;
        g_plan[i].routeOk = wire[i].route != 0;
      }
      g_planHaveVerdict = true;
    }
    /* Alokasi gagal: tetap beri rencana, verdict jatuh ke fallback executor. */
    free(wire);
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
  if (!g_planHaveVerdict) return -1;
  WsPlanEntry *e = wsPlanFind(name, libOnly);
  if (!e) return -1;
  if (e->fastOk && g_planDirty) return -1;
  /* Miss dari rencana dipakai apa adanya: executor tidak mengulang validasi
     state yang sudah dibaca scheduler. Bila miss ternyata basi karena input
     dari proyek sebelumnya berubah, hasilnya rebuild aman (bukan no-op palsu). */
  return e->fastOk ? 1 : 0;
}

void wsPlanNoteBuilt(const char *name) {
  if (wsPlanFind(name, false) || wsPlanFind(name, true)) g_planDirty = true;
}

/* Anak fork mewarisi verdict precompute INDUK. Bila sudah ada proyek yang
   selesai sebelum anak ini diluncurkan, proyek itu bisa saja menulis input
   bersama (tar embed rupamod, arsip/library) sehingga hit warisan bisa basi —
   anak berikutnya wajib memvalidasi sendiri. Anak gelombang pertama tetap
   memakai verdict precompute. */
void wsPlanInvalidate(void) { g_planDirty = true; }

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
  /* Sukses senyap: plan aktif tidak menambah keluaran. Penyimpangan per
     proyek tetap dilaporkan di atas; ringkasan fase hanya akan mengulanginya. */
  for (int i = 0; i < g_planN; i++) {
    char path[64];
    wsPlanMarker(i, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
      if (rc == 0)
        fprintf(stderr, "satria: ws-plan: %s tidak dieksekusi (rencana %s)\n",
                g_plan[i].name, g_plan[i].libOnly ? "library" : "binary");
      continue;
    }
    int c = fgetc(f);
    fclose(f);
    remove(path);
    char want = g_plan[i].libOnly ? 'L' : 'B';
    if (c != want)
      fprintf(stderr, "satria: ws-plan: penanda %s tidak cocok (0x%02x != %c)\n",
              g_plan[i].name, c, want);
  }
  g_planN = 0;
  g_planDirty = false;
}

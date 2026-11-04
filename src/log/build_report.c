/* build_report.c — lihat build_report.h (design/build_report.md).
   Model record + renderer laporan final. Murni observasi: tidak mengubah
   keputusan build. Detail compile dicetak live di bawah blok proyek; bagian
   final (ringkasan, artefak, summary) dirender pada finish(). Kegagalan
   alokasi tidak menggagalkan build — record yang tak muat dibuang diam-diam.

   Urutan heading LAZY (design §3): header statis dirender lebih dulu;
   # Archive dibuka saat record arsip pertama dirender; # Execution dibuka
   sebelum konten proyek pertama. Pada workspace arsip proyek lain baru
   diketahui SETELAH header, jadi heading tidak boleh ikut di header.

   Mode STAGING (child fork runner paralel): record artefak direkam TANPA
   render (child mengekspor lewat buildReportExport, induk mengimpor saat
   flush worklist lalu merender) — urutan keluaran serial & paralel identik. */
#include "build_report.h"
#include "../startup.h" /* startupShown: pemisah startup -> Build Report */
#include "../util.h" /* List, listAdd, listFree, copyStr, nowSeconds, SATRIA_MAX_PATH */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Model ---- */

typedef struct {
  char path[SATRIA_MAX_PATH];
  RepArtStatus st;
  long long size; /* < 0 = tanpa ukuran (mis. checksum) */
  bool printed;   /* baris sudah keluar (hindari dobel lintas render) */
} RepArt;

typedef struct {
  char name[128];
  List detail; /* baris terformat: note / compile / WARN */
  int compiled, skipped, warnings;
  bool hasCounts;
  bool execNoted; /* ada catatan (*) dicetak live di # Execution */
} RepProj;

static struct {
  bool active;
  bool staging;       /* child fork: record saja, tanpa render heading/arsip */
  bool headerRendered; /* header statis (+ # Archive bila ada) sudah dicetak */
  bool archOpened;    /* heading # Archive sudah dicetak */
  bool execOpened;    /* heading # Execution sudah dicetak */
  double t0;
  char toolchain[512];
  RepCompdb compdb;
  char compdbNote[256];
  bool hasCompdbNote;
  char fingerprint[256];
  bool hasFingerprint;
  char headerNote[320]; /* baris "! ..." pada header (opsional) */
  bool hasHeaderNote;

  RepArt *arch;
  int nArch, capArch;
  RepProj *proj;
  int nProj, capProj;
  int cur; /* proyek aktif (-1 bila tidak ada) */
  RepArt *lib;
  int nLib, capLib;
  RepArt *bin;
  int nBin, capBin;
  RepArt *rel;
  int nRel, capRel;
} R;

/* Tumbuh array bertipe; false bila alokasi gagal (pemanggil melewati record). */
static bool repGrow(void **ptr, int *cap, int want, size_t elem) {
  int nc = *cap ? *cap * 2 : 8;
  while (nc < want) nc *= 2;
  void *np = realloc(*ptr, (size_t)nc * elem);
  if (!np) return false;
  *ptr = np;
  *cap = nc;
  return true;
}

static void addArt(RepArt **arr, int *n, int *cap, const char *path,
                   RepArtStatus st, long long size) {
  if (!path || !*path) return;
  /* Upsert per path (design/build_report.md §7): fase library + fase binary
     workspace bisa mencatat artefak yang sama. Status lebih signifikan menang
     (BUILT/LINKED > CACHED) agar hasil build nyata tidak tertimpa oleh cek
     up-to-date pass berikutnya. */
  for (int i = 0; i < *n; i++) {
    if (strcmp((*arr)[i].path, path) == 0) {
      RepArt *e = &(*arr)[i];
      if (st != REP_CACHED && e->st == REP_CACHED) {
        e->st = st;
        e->size = size;
      }
      return;
    }
  }
  if (*n >= *cap && !repGrow((void **)arr, cap, *n + 1, sizeof(RepArt))) return;
  RepArt *a = &(*arr)[(*n)++];
  copyStr(a->path, sizeof(a->path), path);
  a->st = st;
  a->size = size;
  a->printed = false;
}

/* ---- Format helper ---- */

/* Ukuran konsisten: B / %.1fKB / %.1fMB (selaras kode lama %.1fKB). */
static void fmtSize(long long b, char *out, size_t n) {
  if (b < 1024)
    snprintf(out, n, "%lldB", b);
  else if (b < 1024 * 1024)
    snprintf(out, n, "%.1fKB", (double)b / 1024.0);
  else
    snprintf(out, n, "%.1fMB", (double)b / (1024.0 * 1024.0));
}

/* Durasi: <60s -> "%.2fs"; else "XmYs". */
static void fmtDur(double s, char *out, size_t n) {
  if (s < 60.0)
    snprintf(out, n, "%.2fs", s);
  else {
    int m = (int)(s / 60.0);
    snprintf(out, n, "%dm%.0fs", m, s - m * 60.0);
  }
}

static const char *artStatus(RepArtStatus s) {
  switch (s) {
    case REP_BUILT: return "built";
    case REP_LINKED: return "linked";
    case REP_CACHED: default: return "cached";
  }
}

static const char *compdbText(void) {
  if (R.hasCompdbNote && R.compdbNote[0]) return R.compdbNote;
  switch (R.compdb) {
    case REP_COMPDB_REFRESHED: return "compile_commands.json refreshed";
    case REP_COMPDB_UNCHANGED: return "compile_commands.json unchanged";
    case REP_COMPDB_RESTORED: return "compile_commands.json restored";
    case REP_COMPDB_DISABLED: return "disabled (output.compileCommands = false)";
    case REP_COMPDB_SKIPPED: return "skipped (command list unchanged)";
    case REP_COMPDB_FAILED: return "failed";
    default: return "-";
  }
}

/* ---- API lifecycle ---- */

void buildReportBegin(void) {
  if (R.active) return;
  memset(&R, 0, sizeof(R));
  R.active = true;
  R.cur = -1;
  R.t0 = nowSeconds();
}

bool buildReportActive(void) { return R.active; }

void buildReportStaging(bool on) {
  if (R.active) R.staging = on;
}

void buildReportSetToolchain(const char *cmd) {
  if (!R.active) return;
  if (cmd && *cmd) copyStr(R.toolchain, sizeof(R.toolchain), cmd);
}

void buildReportSetCompdb(RepCompdb st, const char *note) {
  if (!R.active) return;
  R.compdb = st;
  if (note && *note) {
    copyStr(R.compdbNote, sizeof(R.compdbNote), note);
    R.hasCompdbNote = true;
  }
}

void buildReportSetFingerprint(const char *note) {
  if (!R.active) return;
  if (note && *note) {
    copyStr(R.fingerprint, sizeof(R.fingerprint), note);
    R.hasFingerprint = true;
  }
}

void buildReportSetHeaderNote(const char *text) {
  if (!R.active) return;
  if (text && *text) {
    copyStr(R.headerNote, sizeof(R.headerNote), text);
    R.hasHeaderNote = true;
  }
}

void buildReportAddArchive(const char *path, RepArtStatus st, long long size) {
  if (R.active) addArt(&R.arch, &R.nArch, &R.capArch, path, st, size);
}
void buildReportAddLibrary(const char *path, RepArtStatus st, long long size) {
  if (R.active) addArt(&R.lib, &R.nLib, &R.capLib, path, st, size);
}
void buildReportAddBinary(const char *path, RepArtStatus st, long long size) {
  if (R.active) addArt(&R.bin, &R.nBin, &R.capBin, path, st, size);
}
void buildReportAddRelease(const char *path, RepArtStatus st, long long size) {
  if (R.active) addArt(&R.rel, &R.nRel, &R.capRel, path, st, size);
}

static RepProj *curProj(void) {
  return (R.active && R.cur >= 0 && R.cur < R.nProj) ? &R.proj[R.cur] : NULL;
}

void buildReportProject(const char *name) {
  /* name kosong = proyek tunggal tanpa header "> Name" (contoh single-project). */
  if (!R.active || !name) return;
  /* Arsip proyek sebelumnya dirender SEBELUM blok ini (posisi identik dengan
     runner paralel yang mengimpor lalu merender sebelum dump slot), lalu
     heading # Execution dibuka lazy. Keduanya no-op saat staging (child). */
  buildReportRenderArchives();
  buildReportBeginExecution();
  for (int i = 0; i < R.nProj; i++) {
    if (strcmp(R.proj[i].name, name) == 0) {
      R.cur = i; /* upsert: proyek sudah ada (fase binary) — TANPA cetak ulang
         "> Name" agar fase library+binary jadi SATU blok (design §3). */
      return;
    }
  }
  if (R.nProj >= R.capProj &&
      !repGrow((void **)&R.proj, &R.capProj, R.nProj + 1, sizeof(RepProj)))
    return;
  RepProj *p = &R.proj[R.nProj++];
  memset(p, 0, sizeof(*p));
  copyStr(p->name, sizeof(p->name), name);
  R.cur = R.nProj - 1;
  /* Blok # Execution: cetak "> Name" HANYA untuk proyek baru (fase library
     pass). Fase binary pass memakai upsert di atas — tidak mencetak ulang.
     Baris kosong sebelum blok berikutnya (bukan pertama) memisahkan dari
     hitungan blok sebelumnya (design §3).
     STAGING (child fork paralel): heading TIDAK dicetak child — catatan proyek
     tidak melintasi fork, sehingga child fase binary akan mencetak ulang
     "> Name" dari memori segarnya. Heading menjadi tanggung jawab induk saat
     flush worklist (wsSlotDump), posisi identik dengan runner serial. */
  if (!R.staging && p->name[0]) {
    if (R.nProj > 1) printf("\n");
    printf("> %s\n", p->name);
  }
}

static void addDetail(const char *line) {
  RepProj *p = curProj();
  if (p) listAdd(&p->detail, line); /* gagal alokasi: diam, build jalan */
}

void buildReportNote(const char *fmt, ...) {
  if (!R.active) return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  /* Catatan dicetak LIVE di bawah # Execution (sebelum compile line) sesuai
     urutan desain (build_report.md §3) — bukan direkam untuk render akhir. */
  buildReportBeginExecution();
  printf("* %s\n", buf);
  RepProj *p = curProj();
  if (p) p->execNoted = true;
}

void buildReportCompileLine(int idx, int total, const char *tag, double secs,
                            const char *src) {
  char line[560];
  snprintf(line, sizeof(line), "[%d/%d] %s %.2fs %s", idx, total, tag, secs, src);
  addDetail(line);
}

void buildReportWarning(const char *diag) {
  if (!diag || !*diag) return;
  char line[560];
  snprintf(line, sizeof(line), "WARN %s", diag);
  addDetail(line);
}

void buildReportProjectCounts(int compiled, int skipped, int warnings) {
  RepProj *p = curProj();
  if (!p) return;
  /* Fase library workspace sudah mencatat hitungan (design §5): fase binary
     TIDAK menimpa — angkanya "fast-tail", bukan compile pass kedua. */
  if (p->hasCounts) return;
  buildReportBeginExecution();
  /* Cetak LIVE di bawah blok proyek (design §3): hitungan menyusul compile
     line per proyek, bukan digabung di akhir laporan. */
  if (p->execNoted || compiled > 0) printf("\n");
  if (warnings > 0)
    printf("* %d compiled, %d skipped, %d warning%s\n", compiled, skipped,
           warnings, warnings == 1 ? "" : "s");
  else
    printf("* %d compiled, %d skipped\n", compiled, skipped);
  p->compiled = compiled;
  p->skipped = skipped;
  p->warnings = warnings;
  p->hasCounts = true;
}

/* ---- Heading lazy & render artefak ---- */

/* Render baris artefak arsip yang belum tampil. Heading # Archive dibuka
   sekali — hanya bila # Execution belum terbuka (arsip selalu didahulukan
   pada urutan desain); arsip yang baru diketahui sesudah # Execution terbuka
   dicetak apa adanya di posisinya (record tetap benar, tanpa heading ganda). */
void buildReportRenderArchives(void) {
  if (!R.active || R.staging || !R.headerRendered) return;
  for (int i = 0; i < R.nArch; i++) {
    RepArt *a = &R.arch[i];
    if (a->printed) continue;
    if (!R.archOpened && !R.execOpened) {
      printf("\n# Archive\n");
      R.archOpened = true;
    }
    if (a->size < 0)
      printf("* %s (%s)\n", a->path, artStatus(a->st));
    else {
      char sz[32];
      fmtSize(a->size, sz, sizeof(sz));
      printf("* %s (%s, %s)\n", a->path, sz, artStatus(a->st));
    }
    a->printed = true;
  }
  fflush(stdout);
}

/* Buka bagian # Execution (sekali). Staging: induk yang mencetak heading
   sebelum dump slot — child tidak boleh menuliskannya ke slot. */
void buildReportBeginExecution(void) {
  if (!R.active || R.staging || R.execOpened) return;
  if (!R.headerRendered) buildReportRenderHeader();
  if (!R.headerRendered) return;
  printf("\n# Execution\n");
  R.execOpened = true;
  fflush(stdout);
}

/* ---- Export / import record artefak (fragment antar fork) ---- */

static void repWriteRows(FILE *f, char kind, const RepArt *arr, int n) {
  for (int i = 0; i < n; i++)
    fprintf(f, "%c\t%d\t%lld\t%s\n", kind, (int)arr[i].st, arr[i].size, arr[i].path);
}

bool buildReportExport(const char *path) {
  if (!R.active || !path || !*path) return false;
  FILE *f = fopen(path, "w");
  if (!f) return false;
  repWriteRows(f, 'A', R.arch, R.nArch);
  repWriteRows(f, 'L', R.lib, R.nLib);
  repWriteRows(f, 'B', R.bin, R.nBin);
  repWriteRows(f, 'R', R.rel, R.nRel);
  fclose(f);
  return true;
}

void buildReportImport(const char *path) {
  if (!R.active || !path || !*path) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[SATRIA_MAX_PATH + 64];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    char *f1 = strchr(line, '\t'); /* kind \t st \t size \t path */
    if (!f1) continue;
    *f1 = '\0';
    char *f2 = strchr(f1 + 1, '\t');
    if (!f2) continue;
    *f2 = '\0';
    char *f3 = strchr(f2 + 1, '\t');
    if (!f3) continue;
    *f3 = '\0';
    if (line[0] == '\0' || line[1] != '\0') continue;
    int st = atoi(f1 + 1);
    long long sz = atoll(f2 + 1);
    const char *p = f3 + 1;
    if (!*p) continue;
    if (st < 0 || st > (int)REP_LINKED) st = REP_CACHED;
    switch (line[0]) {
      case 'A': addArt(&R.arch, &R.nArch, &R.capArch, p, (RepArtStatus)st, sz); break;
      case 'L': addArt(&R.lib, &R.nLib, &R.capLib, p, (RepArtStatus)st, sz); break;
      case 'B': addArt(&R.bin, &R.nBin, &R.capBin, p, (RepArtStatus)st, sz); break;
      case 'R': addArt(&R.rel, &R.nRel, &R.capRel, p, (RepArtStatus)st, sz); break;
      default: break;
    }
  }
  fclose(f);
  remove(path);
}

/* ---- Render ---- */

static const char *baseOf(const char *p) {
  const char *s = strrchr(p, '/');
#ifdef _WIN32
  const char *b = strrchr(p, '\\');
  if (b && (!s || b > s)) s = b;
#endif
  return s ? s + 1 : p;
}

static void renderArtLine(const char *path, RepArtStatus st, long long size) {
  char sz[32];
  if (size < 0)
    printf("* %s (%s)\n", path, artStatus(st));
  else {
    fmtSize(size, sz, sizeof(sz));
    printf("* %s (%s, %s)\n", path, sz, artStatus(st));
  }
}

/* Grup ringkasan: basename, lebar disamakan dalam satu kelompok. */
static void renderGroup(const char *group, const RepArt *arr, int n) {
  int width = 0;
  for (int i = 0; i < n; i++) {
    int bl = (int)strlen(baseOf(arr[i].path));
    if (bl > width) width = bl;
  }
  printf("> %s\n", group);
  for (int i = 0; i < n; i++) {
    const char *base = baseOf(arr[i].path);
    char sz[32];
    if (arr[i].size < 0)
      printf("* %-*s (%s)\n", width, base, artStatus(arr[i].st));
    else {
      fmtSize(arr[i].size, sz, sizeof(sz));
      printf("* %-*s (%s, %s)\n", width, base, sz, artStatus(arr[i].st));
    }
  }
}

/* Render HEADER statis SEBELUM compile (design §3: "cetak header saat
   begin"). # Execution TIDAK ikut dicetak di sini — heading dibuka lazy
   oleh buildReportBeginExecution saat konten proyek pertama muncul, karena
   pada workspace record arsip proyek lain baru masuk setelah header. */
void buildReportRenderHeader(void) {
  if (!R.active || R.headerRendered) return;
  /* Pemisah: blok startup yang tampil diberi baris kosong sebelum Build Report. */
  if (startupShown()) printf("\n");
  printf("Build Report\n");
  printf("------------\n");
  printf("%-12s: %s\n", "Toolchain", R.toolchain[0] ? R.toolchain : "-");
  printf("%-12s: %s\n", "CompDB", compdbText());
  if (R.hasFingerprint) printf("%-12s: %s\n", "Fingerprint", R.fingerprint);
  if (R.hasHeaderNote) printf("! %s\n", R.headerNote);
  R.headerRendered = true;
  /* # Archive bila record sudah diketahui (single-project: arsip dibangun
     sebelum header — urutan persis contoh desain). */
  buildReportRenderArchives();
  fflush(stdout);
}

/* Render bagian TAIL setelah compile: sisa # Archive (bila ada record
   telat), # Execution summary, # Release, # Summary. Header sudah dicetak
   buildReportRenderHeader — JANGAN dicetak ulang (design §3). Bila header
   belum dirender (gagal sebelum compile), lewati render total agar tidak
   ada laporan separuh tanpa header. */
void buildReportFinish(bool success) {
  if (!R.active) return;
  double secs = nowSeconds() - R.t0;
  char dur[32];
  fmtDur(secs, dur, sizeof(dur));

  if (!R.headerRendered) {
    return; /* gagal sebelum header: tanpa laporan separuh */
  }

  /* Arsip yang belum tercetak (mis. proyek terakhir tanpa blok proyek). */
  buildReportRenderArchives();

  /* Hitungan per proyek sudah dicetak LIVE oleh buildReportProjectCounts
     (di bawah tiap blok > Nama). Bagian ini lanjut ke # Execution summary. */

  /* # Execution summary — kelompok lib/ lalu bin/ (yang kosong dilewati). */
  if (R.nLib || R.nBin) {
    printf("\n# Execution summary\n");
    bool first = true;
    if (R.nLib) {
      renderGroup("lib/", R.lib, R.nLib);
      first = false;
    }
    if (R.nBin) {
      if (!first) printf("\n");
      renderGroup("bin/", R.bin, R.nBin);
    }
  }

  /* # Release — hanya bila fase release dijalankan. */
  if (R.nRel) {
    printf("\n# Release\n");
    for (int i = 0; i < R.nRel; i++)
      renderArtLine(R.rel[i].path, R.rel[i].st, R.rel[i].size);
  }

  /* # Summary — selalu terakhir. */
  printf("\n# Summary\n");
  printf("%-11s: %s\n", "Total time", dur);
  printf("%-11s: %s\n", "Status", success ? "Success" : "Failed");
  fflush(stdout);
  /* Record TIDAK dibuang di sini: dipakai buildReportAgg untuk blob
     no-op (reportBlobFinish). Lepaskan lewat buildReportRelease(). */
}

/* Agregat blob LAST BUILD SUMMARY dari record EKSPLISIT (design §9) —
   bukan parsing teks: arsip/library yang dibuat ulang pada run ini,
   binary linked vs up-to-date, release created vs up-to-date. */
bool buildReportAgg(int *nArch, int *nLibStatic, int *nLibShared, int *nBin,
                    int *nBinUp, int *nRel, int *nRelUp) {
  if (!R.active) return false;
  int ar = 0, ls = 0, lsh = 0, bn = 0, bu = 0, rl = 0, rlu = 0;
  for (int i = 0; i < R.nArch; i++)
    if (R.arch[i].st == REP_BUILT) ar++;
  for (int i = 0; i < R.nLib; i++) {
    if (R.lib[i].st != REP_BUILT) continue;
    const char *p = R.lib[i].path;
    size_t n = strlen(p);
    if (n >= 2 && strcmp(p + n - 2, ".a") == 0)
      ls++;
    else if (n >= 3 && strcmp(p + n - 3, ".so") == 0)
      lsh++;
    else
      ls++; /* ekstensi lain (.lib/.dylib/.dll) dihitung statis */
  }
  for (int i = 0; i < R.nBin; i++) {
    if (R.bin[i].st == REP_LINKED)
      bn++;
    else if (R.bin[i].st == REP_CACHED)
      bu++;
  }
  for (int i = 0; i < R.nRel; i++) {
    if (R.rel[i].size < 0) continue; /* sidecar checksum: paket jangan dihitung ganda */
    if (R.rel[i].st == REP_BUILT)
      rl++;
    else if (R.rel[i].st == REP_CACHED)
      rlu++;
  }
  if (nArch) *nArch = ar;
  if (nLibStatic) *nLibStatic = ls;
  if (nLibShared) *nLibShared = lsh;
  if (nBin) *nBin = bn;
  if (nBinUp) *nBinUp = bu;
  if (nRel) *nRel = rl;
  if (nRelUp) *nRelUp = rlu;
  return true;
}

void buildReportRelease(void) {
  if (!R.active) return;
  for (int i = 0; i < R.nProj; i++) listFree(&R.proj[i].detail);
  free(R.arch);
  free(R.proj);
  free(R.lib);
  free(R.bin);
  free(R.rel);
  memset(&R, 0, sizeof(R));
  R.cur = -1;
}
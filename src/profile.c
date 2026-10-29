#include "profile.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmds/cmds_internal.h"
#include "compile.h"
#include "config.h"
#include "deps.h"
#include "embed.h"
#include "portability.h"
#include "prof/prof.h"
#include "satria.h"
#include "uses/workspace.h"
#include "util.h"

/*
 * profile — antarmuka diagnostik `satria profile <context> [opsi]`.
 * Lihat profile.h dan design/profile.md.
 *
 * Aturan yang dijaga di sini:
 *  - Profil apa yang ada: context langsung ke direktori; hanya context
 *    binary yang membaca Buildfile (sumber cc/flags) dan TANPA menulis
 *    cache (loadConfigEx cacheWrite=false).
 *  - Non-mutating: tidak menulis .satria (state build), Buildfile, atau
 *    artefak build normal. Semua keluaran operasi masuk direktori sementara
 *    terisolasi (TMPDIR lalu /tmp) dan dihapus di SEMUA jalur keluar.
 *  - Memakai ulang mesin timing prof (profPhaseBegin/End) — bukan
 *    subsistem timing kedua — dan mesin build yang ada (kompilasi paralel
 *    cmdsRunParallelJobs, pola tar/embed/library dari cmds_lib.c).
 *  - Output profil dicetak SETELAH pengukuran selesai sehingga pencetakan
 *    tidak ikut terukur.
 */

/* CompBackend & ProfCtx: pindah ke profile.h (dipakai prof_steps.c dan
   prof_sandbox.c setelah file ini dipecah). */

/* ==================== output ==================== */

/* Header ringkasan: sumber kalimat profil, mis. "archive=rupamod with=tar,gz". */
static void ctxHeader(const ProfCtx *p, char *out, size_t n) {
  snprintf(out, n, "%s=%s", p->type, p->target);
  if (strcmp(p->type, "archive") == 0) {
    size_t len = strlen(out);
    if (p->comp == COMP_NONE)
      snprintf(out + len, n - len, " with=tar");
    else
      snprintf(out + len, n - len, " with=tar,%s", profCompName(p->comp));
  } else if (strcmp(p->type, "library") == 0) {
    size_t len = strlen(out);
    if (p->libStatic && p->libShared)
      snprintf(out + len, n - len, " with=static,shared");
    else if (p->libShared)
      snprintf(out + len, n - len, " with=shared");
    else
      snprintf(out + len, n - len, " with=static");
  }
}

/* Antarmuka terminal: ringkas, menjawab "apa yang mahal?". */
static void printSummary(const ProfCtx *p) {
  char hdr[MAX_PATH * 2 + 64];
  ctxHeader(p, hdr, sizeof(hdr));
  printf("Profile : %s\n\n", hdr);

  int n;
  const ProfPhase *ph = profPhases(&n);
  double total = 0;
  for (int i = 0; i < n; i++) {
    printf("%-12s %9.1f ms\n", ph[i].label, ph[i].ms);
    total += ph[i].ms;
  }
  printf("%-12s %9.1f ms\n", "total", total);
}

/* Laporan rinci (design/profile.md): context/environment/input/phases/summary.
   Dikirim ke FILE* (stdout non-TTY, atau file --report). Field bertambah
   mengikuti context, tetap manusiawi. */
static void printReport(const ProfCtx *p, FILE *out) {
  char nb[32];
  fprintf(out, "satria profile report\n===================\n\n");

  fprintf(out, "context\n\n---\n\n");
  fprintf(out, "type       : %s\n", p->type);
  fprintf(out, "target     : %s\n", p->target);
  fprintf(out, "directory  : %s\n", p->dir);
  char hdr[MAX_PATH * 2 + 64];
  ctxHeader(p, hdr, sizeof(hdr));
  const char *withEq = strstr(hdr, " with=");
  fprintf(out, "backend    : %s\n", withEq ? withEq + 6 : "default");

  fprintf(out, "\nenvironment\n\n---\n\n");
  fprintf(out, "satria       : %s\n", satriaVersion());
  char plat[128];
  profPlatformString(plat, sizeof(plat));
  fprintf(out, "platform   : %s\n", plat);
  fprintf(out, "compiler   : %s\n",
          p->compiler ? p->compiler : "n/a (context tidak memuat Buildfile)");
  char cwd[MAX_PATH];
  if (fsGetCwd(cwd, sizeof(cwd))) fprintf(out, "cwd        : %s\n", cwd);

  fprintf(out, "\ninput\n\n---\n\n");
  profFmtComma(nb, sizeof(nb), (uint64_t)p->files);
  fprintf(out, "files      : %s\n", nb);
  profFmtComma(nb, sizeof(nb), (uint64_t)p->dirs);
  fprintf(out, "directories: %s\n", nb);
  profFmtComma(nb, sizeof(nb), p->bytes);
  fprintf(out, "bytes      : %s\n", nb);
  if (strcmp(p->type, "workspace") == 0) {
    profFmtComma(nb, sizeof(nb), (uint64_t)p->wsProjects);
    fprintf(out, "projects   : %s\n", nb);
  }

  fprintf(out, "\nphases\n\n---\n\n");
  int n;
  const ProfPhase *ph = profPhases(&n);
  double total = 0, best = 0;
  const char *dom = NULL;
  for (int i = 0; i < n; i++) {
    fprintf(out, "%s\ntime     : %.1f ms\n", ph[i].label, ph[i].ms);
    if (ph[i].inBytes) {
      profFmtComma(nb, sizeof(nb), ph[i].inBytes);
      fprintf(out, "input    : %s bytes\n", nb);
    }
    if (ph[i].outBytes) {
      profFmtComma(nb, sizeof(nb), ph[i].outBytes);
      fprintf(out, "output   : %s bytes\n", nb);
    }
    fprintf(out, "\n");
    total += ph[i].ms;
    if (ph[i].ms > best) {
      best = ph[i].ms;
      dom = ph[i].label;
    }
  }

  fprintf(out, "summary\n\n---\n\n");
  fprintf(out, "total      : %.1f ms\n", total);
  fprintf(out, "dominant   : %s\n", dom ? dom : "-");
}

/* ==================== parser & entry ==================== */

static bool knownContext(const char *name) {
  return strcmp(name, "project") == 0 || strcmp(name, "workspace") == 0 ||
         strcmp(name, "archive") == 0 || strcmp(name, "library") == 0 ||
         strcmp(name, "embed") == 0 || strcmp(name, "binary") == 0;
}

/* Parse nilai with= archive: tar | none | gz/gzip | xz | bz2. */
static int parseArchiveWith(ProfCtx *p, const char *val) {
  char vbuf[64];
  copyStr(vbuf, sizeof(vbuf), val);
  p->doTar = false;
  p->comp = COMP_NONE;
  char *savePtr = NULL;
  char *tok = strtok_r(vbuf, ",", &savePtr);
  for (; tok; tok = strtok_r(NULL, ",", &savePtr)) {
    tok = trim(tok);
    if (strcmp(tok, "tar") == 0 || strcmp(tok, "none") == 0) {
      p->doTar = true;
    } else if (strcmp(tok, "gz") == 0 || strcmp(tok, "gzip") == 0) {
      p->comp = COMP_GZIP;
    } else if (strcmp(tok, "xz") == 0) {
      p->comp = COMP_XZ;
    } else if (strcmp(tok, "bz2") == 0 || strcmp(tok, "bzip2") == 0) {
      p->comp = COMP_BZ2;
    } else {
      fprintf(stderr,
              "satria: profile: nilai 'with' tidak dikenal: '%s' "
              "(pakai: tar | tar,gz | tar,xz | tar,bz2)\n",
              tok);
      return 2;
    }
  }
  if (!p->doTar && p->comp != COMP_NONE) {
    fprintf(stderr, "satria: profile: 'with=gz/xz/bz2' tidak bermakna tanpa tar\n");
    return 2;
  }
  return 0;
}

/* Parse nilai with= library: static | shared (pola pembanding backend). */
static int parseLibraryWith(ProfCtx *p, const char *val) {
  char vbuf[64];
  copyStr(vbuf, sizeof(vbuf), val);
  p->libStatic = false;
  p->libShared = false;
  char *savePtr = NULL;
  char *tok = strtok_r(vbuf, ",", &savePtr);
  for (; tok; tok = strtok_r(NULL, ",", &savePtr)) {
    tok = trim(tok);
    if (strcmp(tok, "static") == 0) {
      p->libStatic = true;
    } else if (strcmp(tok, "shared") == 0) {
      p->libShared = true;
    } else {
      fprintf(stderr, "satria: profile: nilai 'with' tidak dikenal: '%s' (pakai: static | shared)\n",
              tok);
      return 2;
    }
  }
  if (!p->libStatic && !p->libShared) {
    fprintf(stderr, "satria: profile: 'with' kosong (pakai: static | shared)\n");
    return 2;
  }
  return 0;
}

/*
 * cmdProfile — parser context + opsi, lalu jalankan context handler.
 * Context: name=value pertama; sisanya opsi milik context (with=, jobs=)
 * atau opsi global (--report <file>, --repeat N). Kombinasi invalid
 * dilaporkan jelas — pola pesan design:
 * "profile option 'with' is not valid for context 'x'".
 */
int cmdProfile(const char *const *args, int nargs) {
  if (nargs < 1 || !args[0] || !args[0][0]) {
    fprintf(stderr, "satria: profile memerlukan context (mis. 'satria profile archive=<dir>')\n");
    return 2;
  }

  const char *eq = strchr(args[0], '=');
  if (!eq || eq == args[0]) {
    fprintf(stderr, "satria: profile context harus berbentuk name=value (mis. archive=rupamod)\n");
    return 2;
  }
  char name[16];
  size_t nl = (size_t)(eq - args[0]);
  if (nl >= sizeof(name)) nl = sizeof(name) - 1;
  memcpy(name, args[0], nl);
  name[nl] = '\0';
  const char *value = eq + 1;
  if (!*value) {
    fprintf(stderr, "satria: profile context '%s' memerlukan direktori\n", name);
    return 2;
  }
  if (!knownContext(name)) {
    fprintf(stderr,
            "satria: unknown profile context '%s' (yang dikenal: project, workspace, "
            "archive, library, embed, binary)\n",
            name);
    return 2;
  }

  ProfCtx p;
  memset(&p, 0, sizeof(p));
  p.type = name;
  p.doTar = true; /* default archive: tar,gz */
  p.comp = COMP_GZIP;
  p.libStatic = true;  /* default library: static (ar) */
  p.jobs = cpuCount(); /* default binary/project: paralel sebanyak core */
  p.repeat = 1;        /* tanpa --repeat: satu run */

  /* Expand tilde SEKARANG: value menunjuk ke args[0] di tengah token
     (archive=~/rupa). profAbsPath hanya menerima path utuh, jadi hasil expand
     disalin ke buffer p sendiri (targetBuf) dan p.target menunjuk ke sana.
     Tanpa ini `~/...` gagal "direktori tidak ada" karena shell tidak
     meng-expand tilde di tengah token. */
  {
    static char targetBuf[MAX_PATH * 3];
    if (profExpandTilde(value, targetBuf, sizeof(targetBuf)))
      p.target = targetBuf;
    else
      p.target = value;
  }

  for (int i = 1; i < nargs; i++) {
    const char *a = args[i];

    /* Opsi global --: --report <file> (design "Opsi Masa Depan"). */
    if (strncmp(a, "--", 2) == 0) {
      if (strcmp(a, "--report") == 0) {
        if (i + 1 >= nargs || !args[i + 1][0]) {
          fprintf(stderr, "satria: --report requires a file argument\n");
          return 2;
        }
        p.reportFile = args[++i];
        continue;
      }
      if (strcmp(a, "--repeat") == 0) {
        if (i + 1 >= nargs || !args[i + 1][0]) {
          fprintf(stderr, "satria: --repeat requires a count (mis. --repeat 3)\n");
          return 2;
        }
        char *end = NULL;
        long n = strtol(args[++i], &end, 10);
        if (!args[i][0] || *end || n < 1 || n > 100) {
          fprintf(stderr, "satria: profile: --repeat tidak valid: '%s' (1..100)\n", args[i]);
          return 2;
        }
        p.repeat = (int)n;
        continue;
      }
      fprintf(stderr, "satria: profile: opsi tidak dikenal: '%s'\n", a);
      return 2;
    }

    const char *e = strchr(a, '=');
    if (!e || e == a) {
      fprintf(stderr, "satria: profile option harus berbentuk key=value: '%s'\n", a);
      return 2;
    }
    char key[16];
    size_t kl = (size_t)(e - a);
    if (kl >= sizeof(key)) kl = sizeof(key) - 1;
    memcpy(key, a, kl);
    key[kl] = '\0';
    const char *val = e + 1;

    if (strcmp(key, "with") == 0 && strcmp(p.type, "archive") == 0) {
      int rc = parseArchiveWith(&p, val);
      if (rc != 0) return rc;
    } else if (strcmp(key, "with") == 0 && strcmp(p.type, "library") == 0) {
      int rc = parseLibraryWith(&p, val);
      if (rc != 0) return rc;
    } else if (strcmp(key, "jobs") == 0 &&
               (strcmp(p.type, "binary") == 0 || strcmp(p.type, "project") == 0)) {
      char *end = NULL;
      long n = strtol(val, &end, 10);
      if (!*val || *end || n < 1) {
        fprintf(stderr, "satria: profile: jobs tidak valid: '%s'\n", val);
        return 2;
      }
      p.jobs = (int)n;
    } else {
      fprintf(stderr, "satria: profile option '%s' is not valid for context '%s'\n", key, p.type);
      return 2;
    }
  }

  /* Target: cukup ada; tidak perlu Buildfile (prinsip utama design). */
  if (!profAbsPath(p.target, p.dir, sizeof(p.dir))) {
    fprintf(stderr, "satria: profile: cannot resolve directory '%s'\n", p.target);
    return 2;
  }
  if (strcmp(p.type, "embed") == 0 && fsFileExists(p.dir)) {
    p.dirIsFile = true; /* embed arsip jadi: tanpa fase tar */
  } else if (!fsDirExists(p.dir)) {
    fprintf(stderr, "satria: profile %s=%s: direktori tidak ada\n", name, p.target);
    return 2;
  }

  /* Alat eksternal diperiksa SEBELUM pengukuran agar kegagalan tool tidak
     tercatat sebagai fase. */
  if ((strcmp(p.type, "archive") == 0 || strcmp(p.type, "embed") == 0) && !probeAvailable("tar")) {
    fprintf(stderr, "satria: profile: 'tar' tidak ditemukan di PATH\n");
    return 1;
  }
  if (strcmp(p.type, "archive") == 0 && p.comp != COMP_NONE) {
    const char *tool = profCompTool(p.comp);
    if (!probeAvailable(tool)) {
      fprintf(stderr, "satria: profile: '%s' tidak ditemukan di PATH\n", tool);
      return 1;
    }
  }
  if (strcmp(p.type, "embed") == 0 && !embedProbeGnuLd()) {
    fprintf(stderr, "satria: profile embed=: GNU ld tidak tersedia "
                    "(jalur fallback C-array tidak diprofilkan)\n");
    return 1;
  }

  if (!profTmpDirMake(&p)) {
    fprintf(stderr, "satria: profile: cannot create temp directory\n");
    return 1;
  }

  /* Sesuai design --repeat: pengulangan TIDAK boleh mengubah perilaku
     cache/state — sandbox tmp dipertahankan di antara run (deps cache
     object tetap hidup), jadi run kedua dst. mengukur jalur WARM: decide
     makin mahal / compile mendekati 0. Ini workload yang jujur: cache
     boleh hidup, cukup dibatasi dalam sandbox (state build normal tetap
     tak tersentuh) — bukan mengarang cold murni tiap run.
  */
  profSessionReset();
  int rc = 0;
  for (int run = 0; run < p.repeat && rc == 0; run++) {
    if (strcmp(p.type, "archive") == 0)
      rc = profRunArchive(&p);
    else if (strcmp(p.type, "embed") == 0)
      rc = profRunEmbed(&p);
    else if (strcmp(p.type, "library") == 0)
      rc = profRunLibrary(&p);
    else if (strcmp(p.type, "workspace") == 0)
      rc = profRunWorkspace(&p);
    else if (strcmp(p.type, "project") == 0)
      rc = profRunProject(&p);
    else
      rc = profRunBinary(&p);
  }

  /* Bersih-bersih di SEMUA jalur keluar — data sementara tak boleh
     tertinggal (design: non-mutating). */
  fsRemoveTree(p.tmp);

  if (rc != 0) return rc;

  /* Laporan SETELAH pengukuran & cleanup — tidak ikut terukur.
     --report <file>: file menerima laporan rinci, stdout tetap ringkas.
     Tanpa --report: TTY = ringkasan; pipe/redirect = laporan rinci. */
  if (p.reportFile) {
    FILE *rf = fopen(p.reportFile, "w");
    if (!rf) {
      fprintf(stderr, "satria: profile: cannot write %s\n", p.reportFile);
      return 1;
    }
    printReport(&p, rf);
    fclose(rf);
    printSummary(&p);
    printf("Report   : %s\n", p.reportFile);
    return 0;
  }
  if (termIsTTY())
    printSummary(&p);
  else
    printReport(&p, stdout);
  return 0;
}

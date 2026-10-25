#include "commands.h"
#include "cache/artifact.h"
#include "log/build_log.h"
#include "log/report_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmdhelp.h"
#include "cmds/cmds_internal.h"
#include "compdb.h"
#include "compile.h"
#include "config.h"
#include "deps.h"
#include "embed.h"
#include "pack/pack.h"
#include "portability.h"
#include "prof/prof.h"
#include "uses/workspace.h"
#include "util.h"

/*
 * commands — command publik satria (build/init/help; clean ada di
 * cmds/cmds_state.c). Fase build yang berdiri sendiri dipisah modular:
 *   - cmds/cmds_lib.c   : fase library, kompilasi paralel, header versi.
 *   - cmds/cmds_state.c : fingerprint build, fast no-op snapshot, clean.
 */

/* Workspace menjalankan library pass lalu binary pass dalam satu proses.
   Simpan verdict byte-object dari pass pertama agar pass binary tidak memakai
   mtime object yang baru disentuh sebagai alasan relink bila byte object sama. */
#define WS_PHASE_OBJECT_MAX 128
#define WS_PHASE_PROJECT_LEN 128
typedef struct {
  char project[WS_PHASE_PROJECT_LEN];
  bool used;
  bool changed;
} WsPhaseObjectChange;
static WsPhaseObjectChange wsPhaseObjectChanges[WS_PHASE_OBJECT_MAX];

/* Varian bernama (commands.h): dipakai juga oleh jalur fork fase library
   (ws_batch.c) yang menstaging verdict anak ke tabel induk — nama proyek
   tidak bisa diambil dari cmdsStateWsProject() di induk (sudah direset). */
void cmdsWsPhaseVerdictSet(const char *project, bool changed) {
  if (!wsModeActive() || !project || !*project) return;
  int freeSlot = -1;
  for (int i = 0; i < WS_PHASE_OBJECT_MAX; i++) {
    if (wsPhaseObjectChanges[i].used && strcmp(wsPhaseObjectChanges[i].project, project) == 0) {
      wsPhaseObjectChanges[i].changed = changed;
      return;
    }
    if (!wsPhaseObjectChanges[i].used && freeSlot < 0) freeSlot = i;
  }
  if (freeSlot >= 0) {
    snprintf(wsPhaseObjectChanges[freeSlot].project,
             sizeof(wsPhaseObjectChanges[freeSlot].project), "%s", project);
    wsPhaseObjectChanges[freeSlot].used = true;
    wsPhaseObjectChanges[freeSlot].changed = changed;
  }
}

bool cmdsWsPhaseVerdictGet(const char *project, bool *changed) {
  if (!wsModeActive() || !project || !*project) return false;
  for (int i = 0; i < WS_PHASE_OBJECT_MAX; i++) {
    if (wsPhaseObjectChanges[i].used && strcmp(wsPhaseObjectChanges[i].project, project) == 0) {
      if (changed) *changed = wsPhaseObjectChanges[i].changed;
      return true;
    }
  }
  return false;
}

static void wsPhaseObjectChangeSet(bool changed) {
  cmdsWsPhaseVerdictSet(cmdsStateWsProject(), changed);
}

static bool wsPhaseObjectChangeGet(bool *changed) {
  return cmdsWsPhaseVerdictGet(cmdsStateWsProject(), changed);
}

/* ==================== help (dari src/cmd.txt — sumber tunggal) ==================== */

void showHelp(void) {
  cmdHelpMain();
}

/* ==================== init ==================== */

int cmdInit(const char *buildfilePath) {
  const char *path = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  if (fsFileExists(path)) {
    printf("> %s already exists, nothing to do\n", path);
    return 0;
  }

  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "satria: cannot create %s\n", path);
    return 1;
  }

  /* Template minimal: proyek standar (src/ + include/ opsional) cukup
     `use project` — sisanya konvensi satria (sources=src, headers=include
     bila ada, binary = nama folder). Proyek non-standar bebas menambah
     field eksplisit; lihat docs/guide/buildfile.md. */
  fputs("use project\n", fp);
  fclose(fp);

  printf("> Created   : %s\n", path);
  return 0;
}

/* ==================== init -w (template workspace) ==================== */

int cmdInitWorkspace(const char *buildfilePath) {
  const char *path = buildfilePath && *buildfilePath ? buildfilePath : WORKSPACE_FILENAME;
  if (fsFileExists(path)) {
    printf("> %s already exists, nothing to do\n", path);
    return 0;
  }

  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "satria: cannot create %s\n", path);
    return 1;
  }

  /* Template minimal workspace: cukup daftar proyek. Konvensi per proyek
     sama dengan proyek standar (folder <nama>/ berisi src/). */
  fputs("use workspace\n"
        "\n"
        "# Proyek standar: folder <nama>/ berisi src/ (include/ opsional).\n"
        "projects = app\n",
        fp);
  fclose(fp);

  printf("> Created   : %s\n", path);
  return 0;
}

/* ==================== init -p (interaktif) ==================== */

/* Satu baris jawaban; false bila stdin berakhir (EOF -> pembatalan).
   Prompt lengkap (termasuk simbol `|`) disiapkan askPrompt. */
static bool askLine(const char *prompt, char *buf, size_t n) {
  printf("%s", prompt);
  fflush(stdout);
  if (!fgets(buf, (int)n, stdin)) return false;
  size_t l = strlen(buf);
  while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
    buf[--l] = '\0';
  return true;
}

/* Nama proyek/folder: huruf, angka, '_' '-' (nama binary juga aman). */
static bool projectNameValid(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n > 63) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

/* Nama acak satria-xxxxxx: /dev/urandom (POSIX) atau rand() (Windows/fallback). */
static void randomProjectName(char *out, size_t n) {
  unsigned char rnd[4] = {0};
  FILE *f = fopen("/dev/urandom", "rb");
  if (f) {
    if (fread(rnd, 1, sizeof(rnd), f) != sizeof(rnd)) rnd[0] = 0;
    fclose(f);
  } else {
    static bool seeded = false;
    if (!seeded) {
      srand((unsigned)time(NULL));
      seeded = true;
    }
    for (size_t i = 0; i < sizeof(rnd); i++)
      rnd[i] = (unsigned char)(rand() & 0xFF);
  }
  unsigned v = ((unsigned)rnd[0] << 24) | ((unsigned)rnd[1] << 16) | ((unsigned)rnd[2] << 8) |
               (unsigned)rnd[3];
  snprintf(out, n, "satria-%06x", v & 0xFFFFFFu);
}

/* Warna prompt hanya saat stdout terminal; NO_COLOR (standar no-color.org),
   SATRIA_NO_COLOR, dan TERM=dumb mematikan. Di pipe/CI otomatis polos. */
static bool termColorWanted(void) {
  if (!termIsTTY()) return false;
  const char *no = getenv("NO_COLOR");
  if (no && *no) return false;
  no = getenv("SATRIA_NO_COLOR");
  if (no && *no) return false;
  const char *term = getenv("TERM");
  if (term && strcmp(term, "dumb") == 0) return false;
  return true;
}

/* Prompt dengan simbol `▌` berwarna cyan bila warna aktif; polos selainnya. */
static bool askPrompt(const char *text, char *buf, size_t n) {
  char prompt[768];
  if (termColorWanted())
    snprintf(prompt, sizeof(prompt), "\033[36m▌\033[0m %s", text);
  else
    snprintf(prompt, sizeof(prompt), "▌ %s", text);
  return askLine(prompt, buf, n);
}

/* Pisah list koma, trim tiap item, gabung ulang dengan ", "; false bila
   hasil melebihi kapasitas. Item kosong dilewati; kosong total => dst[0]. */
static bool normalizeList(char *dst, size_t n, const char *src) {
  size_t w = 0;
  bool any = false;
  dst[0] = '\0';
  char buf[512];
  copyStr(buf, sizeof(buf), src);
  for (char *save = buf;;) {
    char *comma = strchr(save, ',');
    if (comma) *comma = '\0';
    char *item = trim(save);
    if (*item) {
      size_t l = strlen(item);
      if (w + l + 3 >= n) return false;
      if (any) {
        dst[w++] = ',';
        dst[w++] = ' ';
      }
      memcpy(dst + w, item, l + 1);
      w += l;
      any = true;
    }
    if (!comma) break;
    save = comma + 1;
  }
  return true;
}

static void upperName(char *dst, size_t n, const char *src) {
  size_t i = 0;
  for (; src[i] && i + 1 < n; i++) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c == '-') c = '_'; /* guard macro tidak boleh berisi '-' */
    dst[i] = c;
  }
  dst[i] = '\0';
}

static bool writeNew(const char *path, const char *content) {
  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "satria: cannot create %s\n", path);
    return false;
  }
  fputs(content, fp);
  fclose(fp);
  printf("> Created   : %s\n", path);
  return true;
}

int cmdInteractiveInit(void) {
  char line[512];
  char name[64];
  char stdv[64] = {0}, compv[192] = {0}, flagsv[512] = {0};
  bool wantInclude = false, wantWs = false, langCpp = false;

  printf("> satria init interaktif - enter memakai default\n\n");

  if (!askPrompt("Project name? [enter for random name] ", line, sizeof(line))) goto eof;
  copyStr(name, sizeof(name), trim(line));
  if (!name[0]) randomProjectName(name, sizeof(name));
  if (!projectNameValid(name)) {
    fprintf(stderr,
            "satria: nama proyek '%s' tidak valid - pakai huruf, angka, '_' atau '-' (maks 63)\n",
            name);
    return 2;
  }

  /* Bahasa: default c; cpp men-scaffold src/main.cpp. Konvensi default
     cpp (std c++17, linker C++) ada di configFinalize — proyek standar
     tak perlu menulis std/compiler eksplisit. */
  if (!askPrompt("language? [c, cpp] [enter = c] ", line, sizeof(line))) goto eof;
  {
    char *v = trim(line);
    langCpp = (v[0] | 32) == 'c' && (v[1] | 32) == 'p' && (v[2] | 32) == 'p' && v[3] == '\0';
  }

  /* Pertanyaan lanjutan: jawaban selain default ditulis eksplisit ke
     Buildfile; sisanya tetap mengandalkan konvensi (lihat configFinalize). */
  if (!askPrompt(langCpp ? "std? [enter = default c++17] " : "std? [enter = default gnu11] ", line,
                 sizeof(line)))
    goto eof;
  if (!normalizeList(stdv, sizeof(stdv), line)) {
    fprintf(stderr, "satria: nilai std terlalu panjang\n");
    return 2;
  }
  if (!askPrompt("compiler? [enter = auto-detect] ", line, sizeof(line))) goto eof;
  if (!normalizeList(compv, sizeof(compv), line)) {
    fprintf(stderr, "satria: nilai compiler terlalu panjang\n");
    return 2;
  }
  if (!askPrompt("flags? [enter = default] ", line, sizeof(line))) goto eof;
  if (!normalizeList(flagsv, sizeof(flagsv), line)) {
    fprintf(stderr, "satria: nilai flags terlalu panjang\n");
    return 2;
  }
  if (!askPrompt("Create include/? [y/N] ", line, sizeof(line))) goto eof;
  {
    char *v = trim(line);
    wantInclude = (v[0] == 'y' || v[0] == 'Y');
  }
  if (!askPrompt("Uses workspace? [y/N] ", line, sizeof(line))) goto eof;
  {
    char *v = trim(line);
    wantWs = (v[0] == 'y' || v[0] == 'Y');
  }

  /* Daftar proyek workspace: proyek utama + anggota tambahan. */
  char names[16][64];
  int nn = 0;
  char depA[64] = {0}, depB[64] = {0};
  copyStr(names[nn++], sizeof(names[0]), name);
  if (wantWs) {
    if (!askPrompt("Workspace projects? [enter = app,tool] ", line, sizeof(line))) goto eof;
    char memlist[512];
    if (!normalizeList(memlist, sizeof(memlist), line)) {
      fprintf(stderr, "satria: daftar proyek terlalu panjang\n");
      return 2;
    }
    if (!memlist[0]) copyStr(memlist, sizeof(memlist), "app,tool");
    char buf[512];
    copyStr(buf, sizeof(buf), memlist);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item) {
        if (!projectNameValid(item)) {
          fprintf(stderr,
                  "satria: nama proyek '%s' tidak valid - pakai huruf, angka, '_' atau '-'\n",
                  item);
          return 2;
        }
        if (nn >= 16) {
          fprintf(stderr, "satria: init -p mendukung maksimal 16 proyek workspace\n");
          return 2;
        }
        if (strcmp(item, name) != 0) /* proyek utama sudah di daftar */
          copyStr(names[nn++], sizeof(names[0]), item);
      }
      if (!comma) break;
      save = comma + 1;
    }

    if (!askPrompt("depends_on? (format: <proyek> depends <proyek>) [enter = none] ", line,
                   sizeof(line)))
      goto eof;
    {
      char *v = trim(line);
      if (v[0]) {
        if (sscanf(v, "%63s depends %63s", depA, depB) != 2) {
          fprintf(stderr,
                  "satria: format depends_on '%s' tidak dipahami - contoh: tool depends app\n", v);
          return 2;
        }
        bool aOk = false, bOk = false;
        for (int i = 0; i < nn; i++) {
          if (strcmp(names[i], depA) == 0) aOk = true;
          if (strcmp(names[i], depB) == 0) bOk = true;
        }
        if (!aOk || !bOk) {
          fprintf(stderr,
                  "satria: depends_on '%s depends %s': proyek tidak ada di daftar - "
                  "tambahkan dulu di Workspace projects\n",
                  depA, depB);
          return 2;
        }
      }
    }
  }

  /* Pra-cek SEMUA target file sebelum menulis apa pun: satu konflik pun
     membatalkan scaffold - tidak ada yang ditimpa, tidak ada parsial. */
  {
    const char *srcName = langCpp ? "main.cpp" : "main.c";
    char p[MAX_PATH * 2];
    char conflictPath[MAX_PATH * 2] = "";
    snprintf(p, sizeof(p), "%s/src/%s", name, srcName);
    if (fsFileExists(p)) copyStr(conflictPath, sizeof(conflictPath), p);
    if (!conflictPath[0] && wantInclude) {
      snprintf(p, sizeof(p), "%s/include/%s.h", name, name);
      if (fsFileExists(p)) copyStr(conflictPath, sizeof(conflictPath), p);
    }
    for (int i = 1; i < nn && !conflictPath[0]; i++) {
      snprintf(p, sizeof(p), "%s/src/%s", names[i], srcName);
      if (fsFileExists(p)) copyStr(conflictPath, sizeof(conflictPath), p);
    }
    if (!conflictPath[0]) {
      if (wantWs) {
        if (fsFileExists(WORKSPACE_FILENAME))
          copyStr(conflictPath, sizeof(conflictPath), WORKSPACE_FILENAME);
      } else {
        snprintf(p, sizeof(p), "%s/Buildfile", name);
        if (fsFileExists(p)) copyStr(conflictPath, sizeof(conflictPath), p);
      }
    }
    if (conflictPath[0]) {
      fprintf(stderr, "satria: %s sudah ada - init dihentikan (tidak ada yang ditimpa)\n",
              conflictPath);
      return 1;
    }
  }

  /* Bahasa cpp: std eksplisit ke Buildfile hasil scaffold — konvensi
     deteksi .cpp di configFinalize tetap melayani proyek manual. */
  if (langCpp && !stdv[0]) copyStr(stdv, sizeof(stdv), "c++17");

  /* Isi file: Buildfile hanya berisi tambahan non-default di atas
     `use project` (sisanya konvensi); main.c siap build & jalan. */
  char body[2048], tmp[640];
  body[0] = '\0';
  strcat(body, "use project\n");
  if (stdv[0]) {
    snprintf(tmp, sizeof(tmp), "\nstd = %s\n", stdv);
    strcat(body, tmp);
  }
  if (compv[0]) {
    snprintf(tmp, sizeof(tmp), "\ncompiler = %s\n", compv);
    strcat(body, tmp);
  }
  if (flagsv[0]) {
    snprintf(tmp, sizeof(tmp), "\nflags = %s\n", flagsv);
    strcat(body, tmp);
  }

  char mainc[1024];
  if (langCpp) {
    if (wantInclude)
      snprintf(
          mainc, sizeof(mainc),
          "#include <iostream>\n\n#include \"%s.h\"\n\nvoid hello() {\n"
          "  std::cout << \"hello from %s\\n\";\n}\n\nint main() {\n  hello();\n  return 0;\n}\n",
          name, name);
    else
      snprintf(mainc, sizeof(mainc),
               "#include <iostream>\n\nint main() {\n  std::cout << \"hello from %s\\n\";\n"
               "  return 0;\n}\n",
               name);
  } else if (wantInclude)
    snprintf(mainc, sizeof(mainc),
             "#include <stdio.h>\n\n#include \"%s.h\"\n\nvoid hello(void) {\n"
             "  printf(\"hello from %s\\n\");\n}\n\nint main(void) {\n  hello();\n  return 0;\n}\n",
             name, name);
  else
    snprintf(mainc, sizeof(mainc),
             "#include <stdio.h>\n\nint main(void) {\n  printf(\"hello from %s\\n\");\n"
             "  return 0;\n}\n",
             name);

  char hdr[512];
  if (wantInclude) {
    char guard[80];
    upperName(guard, sizeof(guard), name);
    snprintf(hdr, sizeof(hdr), "#ifndef %s_H\n#define %s_H\n\n%s\n\n#endif\n", guard, guard,
             langCpp ? "void hello();" : "void hello(void);");
  }

  char ws[4096];
  if (wantWs) {
    ws[0] = '\0';
    strcat(ws, "use workspace\n\nprojects = ");
    for (int i = 0; i < nn; i++) {
      strcat(ws, names[i]);
      if (i + 1 < nn) strcat(ws, ", ");
    }
    strcat(ws, "\n");
    if (depA[0]) {
      snprintf(tmp, sizeof(tmp), "%s.depends_on = %s\n", depA, depB);
      strcat(ws, tmp);
    }
  }

  /* Scaffold. */
  printf("\n");
  {
    char psrc[MAX_PATH * 2], pb[MAX_PATH * 2], phdr[MAX_PATH * 2];
    mkdirs(name);
    snprintf(psrc, sizeof(psrc), "%s/src", name);
    mkdirs(psrc);
    if (wantInclude) {
      snprintf(phdr, sizeof(phdr), "%s/include", name);
      mkdirs(phdr);
    }

    if (!wantWs) {
      snprintf(pb, sizeof(pb), "%s/Buildfile", name);
      if (!writeNew(pb, body)) return 1;
    }
    snprintf(psrc, sizeof(psrc), "%s/src/%s", name, langCpp ? "main.cpp" : "main.c");
    if (!writeNew(psrc, mainc)) return 1;
    if (wantInclude) {
      snprintf(phdr, sizeof(phdr), "%s/include/%s.h", name, name);
      if (!writeNew(phdr, hdr)) return 1;
    }

    /* Anggota workspace: folder <nama>/ + src/main.c (tanpa Buildfile —
       proyek workspace memakai Buildfile hasil sintesis satria). */
    for (int i = 1; i < nn; i++) {
      char msrc[MAX_PATH * 2], mdir[MAX_PATH * 2], mbody[512];
      mkdirs(names[i]);
      snprintf(mdir, sizeof(mdir), "%s/src", names[i]);
      mkdirs(mdir);
      snprintf(msrc, sizeof(msrc), "%s/src/%s", names[i], langCpp ? "main.cpp" : "main.c");
      snprintf(mbody, sizeof(mbody),
               langCpp
                   ? "#include <iostream>\n\nint main() {\n  std::cout << \"hello from %s\\n\";\n"
                     "  return 0;\n}\n"
                   : "#include <stdio.h>\n\nint main(void) {\n  printf(\"hello from %s\\n\");\n"
                     "  return 0;\n}\n",
               names[i]);
      if (!writeNew(msrc, mbody)) return 1;
    }

    if (wantWs) {
      if (!writeNew(WORKSPACE_FILENAME, ws)) return 1;
      printf("\n> Next      : satria -w\n");
    } else {
      printf("\n> Next      : cd %s && satria\n", name);
    }
  }
  return 0;

eof:
  fprintf(stderr, "\nsatria: init dibatalkan (EOF)\n");
  return 1;
}

/* ==================== compdb -g (query-only) ==================== */

int cmdCompdbGenerate(const char *buildfilePath) {
  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) return 1;
  if (!resolveCompiler(&c)) return 1;

  if (!fsSetCwd(c.root)) {
    fprintf(stderr, "satria: cannot enter root '%s'\n", c.root);
    return 1;
  }

  /* Kemasan murni (output.binary = false) tidak punya compile step —
     tidak ada yang bisa direkam ke compile_commands.json. */
  if (!c.binary) {
    printf("%s CompDB  : skipped (output.binary = false)\n", repPrefix());
    return 0;
  }
  if (!compdbEnabled(&c)) {
    printf("%s CompDB  : disabled (output.compileCommands = false)\n", repPrefix());
    return 0;
  }

  List srcs = {0};
  listReserve(&srcs, 1024);
  for (int i = 0; i < c.sources.count; i++) {
    const char *entry = c.sources.items[i];
    size_t elen = strlen(entry);
    if (elen >= 2 &&
        (strcmp(entry + elen - 2, ".c") == 0 || strcmp(entry + elen - 2, ".cpp") == 0) &&
        fsFileExists(entry)) {
      listAdd(&srcs, entry);
      continue;
    }
    walkDir(entry, ".c", &srcs);
    walkDir(entry, ".cpp", &srcs);
  }
  listSort(&srcs); /* urutan entri json = urutan link (reprodusible) */

  if (srcs.count == 0) {
    fprintf(stderr,
            "satria: tidak ada source .c/.cpp di sources — tidak ada compdb untuk dibuat\n");
    return 1;
  }

  /* Cache hit / restore blob / render — mesin yang sama dengan jalur
     build (writeCompdb), tanpa decide/compile/link/fast-state. */
  writeCompdb(&c, &srcs);
  listFree(&srcs);
  return 0;
}

/* ==================== build ==================== */

/* Build penuh satu proyek — jalur satria satu-proyek & fase binary workspace. */
int cmdBuild(int jobs, const char *buildfilePath) {
  return cmdBuildEx(jobs, buildfilePath, false);
}

/* ==================== [OPT] NaiveStatPlan (satu stat / file) ====================
 *
 * Audit bench compdb menunjukkan biaya dominan fase decide adalah stat
 * per file (mtime-ns) di lingkungan proot — bukan render compdb. Plan ini
 * mengumpulkan hasil stat SEKALI di awal (satu fsStampNsSize per file)
 * supaya fase lain tinggal memakai hasilnya tanpa men-stat ulang.
 *
 * Kontrol via env (default: satu stat / file — decide membaca plan):
 *   SATRIA_DECIDE_FROM_PLAN=0  decide men-stat src+objek sendiri (perilaku lama).
 *   SATRIA_STATPLAN_OBS=0      jangan stamp objek saat gather; decide men-stat
 *                            objek sendiri (perilaku lama).
 * Kedua env =0 bersama-sama memulihkan perilaku lama 100%.
 *
 * Guard jalur embedded/version header: emitEmbeddedHeader/
 * cmdsEmitVersionHeader berjalan SEBELUM decide dan bisa menghapus objek;
 * loop di cmdBuildEx meng-invalidasi slot plan.objM jadi -1 sehingga decide
 * melihat objek hilang dan tetap me-recompile (lihat cmdBuildEx).
 *
 *Konsumen aktif saat ini: decide. Konsumen masa depan:
 * keputusan link, PSA, restore compdb.
 */
typedef struct {
  int count;                 /* == srcs.count */
  const char **path;         /* path source (alias ke list srcs) */
  int64_t *srcM;             /* mtime ns source (<0 = hilang) */
  int *objIdx;               /* -1 = path object gagal; else slot objPath */
  int objCount;              /* jumlah object unik yang distamp */
  char (*objPath)[MAX_PATH]; /* path object unik */
  int64_t *objM;             /* mtime ns object (<0 = hilang) */
  bool obsObj;               /* D1: objek ikut distamp saat gather */
} StatPlan;

static void statPlanInit(StatPlan *p) {
  memset(p, 0, sizeof(*p));
}

static void statPlanFree(StatPlan *p) {
  free(p->path);
  free(p->srcM);
  free(p->objIdx);
  free(p->objPath);
  free(p->objM);
  statPlanInit(p);
}

/* true bila env diset dan bernilai "0" (opt-out eksplisit ke perilaku lama). */
static bool envOff(const char *name) {
  const char *v = getenv(name);
  return v && strcmp(v, "0") == 0;
}

/* Stamp semua source sekali jalan; opsional (obsObj) stamp object unik.
   Dedup object pakai linear search: objCount kecil dan objectPathFor
   deterministik, jadi O(n^2) kecil ini masih jauh lebih murah daripada
   stat ganda pada FS lambat. */
static void statPlanBuild(StatPlan *p, const Config *c, const List *srcs) {
  int n = srcs->count;
  p->count = n;
  p->obsObj = !envOff("SATRIA_STATPLAN_OBS");
  /* +1 menghindari malloc(0) saat n==0 */
  p->path = malloc(sizeof(char *) * (size_t)(n + 1));
  p->srcM = malloc(sizeof(int64_t) * (size_t)(n + 1));
  p->objIdx = malloc(sizeof(int) * (size_t)(n + 1));
  p->objPath = malloc(sizeof(char[MAX_PATH]) * (size_t)(n + 1));
  p->objM = malloc(sizeof(int64_t) * (size_t)(n + 1));
  if (!p->path || !p->srcM || !p->objIdx || !p->objPath || !p->objM) {
    statPlanFree(p);
    return;
  }
  p->objCount = 0;
  for (int i = 0; i < n; i++) {
    const char *src = srcs->items[i];
    p->path[i] = src;
    long long sz = 0;
    int64_t m = -1;
    fsStampNsSize(src, &m, &sz); /* gagal -> -1, semantik fsMTimeNs */
    p->srcM[i] = m;

    char objPath[MAX_PATH];
    if (!objectPathFor(c, src, objPath, sizeof(objPath))) {
      p->objIdx[i] = -1;
      continue;
    }
    int slot = -1;
    for (int j = 0; j < p->objCount; j++) {
      if (strcmp(p->objPath[j], objPath) == 0) {
        slot = j;
        break;
      }
    }
    if (slot < 0) {
      slot = p->objCount;
      snprintf(p->objPath[slot], MAX_PATH, "%s", objPath);
      if (p->obsObj) {
        int64_t om = -1;
        fsStampNsSize(objPath, &om, &sz);
        p->objM[slot] = om;
      } else {
        p->objM[slot] = -99; /* belum distat — konsumen WAJIB stat sendiri */
      }
      p->objCount++;
    }
    p->objIdx[i] = slot;
  }
}

int cmdBuildEx(int jobs, const char *buildfilePath, bool libOnly) {
  /* Sisa state lama (audit §Daftar Implementasi): mode single menyapu file
     tulisan versi lama sekali per run; mode workspace menyapu lewat
     wsLegacyStateRemove di workspaceRun. */
  if (!wsModeActive()) cmdsLegacyStateSweep();
  /* Fast no-op per fase (miniws): fase binary memakai build.state, fase
     library workspace memakai build.lib.state — no-op `satria -w` tidak
     membayar loadConfig + fingerprint + decide penuh per proyek. */
  bool fastOk = wsModeActive() ? wsFastStateValid(buildfilePath, libOnly)
                               : cmdsFastStateValid(buildfilePath, libOnly);
  if (fastOk) return 0; /* no-op: detail sudah dicetak fast-state */
  /* Build mengubah file. Mode single: memo dikosongkan total (aman, perilaku lama).
     Mode workspace: output dibuang per entri saat state disimpan, dan memo total
     dikosongkan bila build gagal (lihat wsExecOne), agar memo bisa dipakai lintas fase. */
  if (!wsModeActive()) cmdsStatMemoReset();
  /* Cold startup is defined by the absence of the persistent config state,
     not by missing build outputs. Keep warm startup silent. */
  bool coldStartup = !wsModeActive() && !fsFileExists(".satria/startup.state");
  bool showStartup = coldStartup && termIsTTY();
  if (showStartup) {
    fprintf(stderr, "startup :\n");
    fflush(stderr);
  }

  /* Detail laporan run (design/startup.md §10): tee stdout sejak awal; disimpan
     ke .satria/report.blob bila build sukses, dibuang bila gagal. */
  reportBlobBegin();

  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) {
    if (showStartup) fprintf(stderr, "> loading configuration: failed\n");
    reportBlobFinish(false);
    return 1;
  }
  if (showStartup) fprintf(stderr, "> loading configuration: done\n");
  if (!resolveCompiler(&c)) {
    if (showStartup) fprintf(stderr, "> preparing execution: failed\n");
    reportBlobFinish(false);
    return 1;
  }

  profMark("startup+config");

  /* Mode sinyal: build paralel selalu butuh handler forward SIGINT karena
     tiap compiler ada di process group sendiri (Ctrl+C dari terminal hanya
     sampai ke satria). Build serial mengikuti Buildfile: foreground. */
  procSetForeground(jobs > 1 ? false : c.foreground);

  if (!fsDirExists(c.root)) {
    if (showStartup) fprintf(stderr, "> preparing execution: failed\n");
    fprintf(stderr, "satria: root '%s' is not a directory\n", c.root);
    reportBlobFinish(false);
    return 1;
  }
  if (!fsSetCwd(c.root)) {
    if (showStartup) fprintf(stderr, "> preparing execution: failed\n");
    fprintf(stderr, "satria: cannot enter root '%s'\n", c.root);
    reportBlobFinish(false);
    return 1;
  }
  if (showStartup) {
    fprintf(stderr, "> preparing execution: done\n");
    fflush(stderr);
  }

  bool logging = !wsModeActive() && c.logEnabled;
  if (logging) buildLogBegin();
  int result = cmdsBuildPassRun(jobs, c, buildfilePath, libOnly);
  if (logging) buildLogEnd(result);
  reportBlobFinish(result == 0);        /* sukses: jadikan report.blob; gagal: tahan yang lama */
  if (result == 0) artifactCacheSave(); /* metadata artefak (design/startup.md §9) */
  return result;
}

/* Pasangan (object, sumber) untuk deteksi tabrakan: diurutkan per object. */
typedef struct {
  char obj[MAX_PATH];
  const char *src;
} ObjEntry;

static int objEntryCmp(const void *a, const void *b) {
  return strcmp(((const ObjEntry *)a)->obj, ((const ObjEntry *)b)->obj);
}

bool cmdsCheckObjectCollisions(const Config *c, const List *srcs) {
  if (srcs->count < 2) return true;
  ObjEntry *e = calloc((size_t)srcs->count, sizeof(ObjEntry));
  if (!e) return true; /* kehabisan memori: tidak ada perbandingan; build lanjut */
  int m = 0;
  for (int i = 0; i < srcs->count; i++) {
    const char *src = srcs->items[i];
    if (!objectPathFor(c, src, e[m].obj, sizeof(e[m].obj))) continue;
    e[m].src = src;
    m++;
  }
  qsort(e, (size_t)m, sizeof(ObjEntry), objEntryCmp);
  bool ok = true;
  for (int i = 1; i < m; i++) {
    if (strcmp(e[i].obj, e[i - 1].obj) != 0) continue;
    fprintf(stderr,
            "satria: tabrakan object: '%s' dan '%s' sama-sama -> %s\n"
            "        ubah nama salah satu file atau pindahkan dari root sumber yang sama\n",
            e[i - 1].src, e[i].src, e[i].obj);
    ok = false;
  }
  free(e);
  return ok;
}

/*
 * cmdsCollectSources — koleksi source .c/.cpp dari Config: entri FILE
 * (mis. hasil `satria -xf` untuk build.ninja flat) masuk langsung; selain
 * itu dianggap folder dan discan rekursif (walk cache .satria/sources.cache bila
 * mode workspace aktif — lihat wsModeSet). Urutan hasil = urutan link:
 * .c dan .cpp dihasilkan dua walkDir terpisah per folder root, jadi di-sort
 * ulang jadi satu barisan stabil — baris link reprodusible antar mesin dan
 * urutan inisialisasi object statis C++ tidak melekat pada readdir.
 * Dipakai cmdsBuildPassRun dan jalur CompDB workspace (wsCompdbRefresh).
 */
void cmdsCollectSources(const Config *c, List *out) {
  listReserve(out, 1024);
  for (int i = 0; i < c->sources.count; i++) {
    const char *entry = c->sources.items[i];
    size_t elen = strlen(entry);
    if (elen >= 2 &&
        (strcmp(entry + elen - 2, ".c") == 0 || strcmp(entry + elen - 2, ".cpp") == 0) &&
        fsFileExists(entry)) {
      listAdd(out, entry);
      continue;
    }
    stateWalkDirCached(entry, ".c", out);
    stateWalkDirCached(entry, ".cpp", out);
  }
  /* Source embed hasil codegen: berada di outBuildDir (bukan root sumber),
     jadi ditambahkan eksplisit — ikut fingerprint, decide, compdb, link,
     dan kemasan library seperti source biasa (design/embedded.md §Pipeline).
     Hanya proyek binary: proyek kemasan mengarsipkan, bukan mengcompile. */
  if (c->binary && c->embCount > 0) {
    char emb[MAX_PATH + 32];
    snprintf(emb, sizeof(emb), "%s/embedded.c", c->outBuildDir);
    bool dup = false;
    for (int i = 0; i < out->count; i++) {
      if (strcmp(out->items[i], emb) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup) listAdd(out, emb);
  }
  listSort(out);
}

/*
 * cmdsBuildPassRun — isi pass build satu proyek (fase library / fase
 * binary): koleksi source, fingerprint, decide, compile, embed, library,
 * link, state. TANPA loadConfig & TANPA chdir — pemanggil sudah
 * menyiapkan Config (path selaras cwd) dan cwd yang benar:
 *   - single project : cmdBuildEx (cwd = root proyek lewat fsSetCwd)
 *   - workspace      : wsCmdBuild (cwd = root workspace, tidak pernah
 *                      chdir; cmdBuildEx TIDAK dipanggil — design/
 *                      workspace_v1.md §Pemisahan Jalur Build)
 * Config by-value: body hanya membaca field-nya (list dibagi, tak dimutasi).
 */
int cmdsBuildPassRun(int jobs, Config c, const char *buildfilePath, bool libOnly) {
  /* Proyek kemasan (output.binary = false): SEBELUM koleksi source —
     proyek kemasan sah tidak punya satu .c pun. Dua varian:
       pack.*    -> pengemasan artefak (tar.gz/deb/checksum);
       embedded  -> arsip aset untuk di-embed binary lain. */
  if (!c.binary) {
    if (c.pack.requested) return cmdsPackOnlyProject(&c);
    if (c.embCount == 0) {
      fprintf(stderr, "satria: nothing to build (output.binary = false)\n");
      return 1;
    }
    mkdirs(c.outBuildDir);
    bool anyRebuilt = false;
    if (!buildEmbeddedArchivesEx(&c, &anyRebuilt)) return 1;
    printf("\n%s Summary :\n", repPrefix());
    bool anyMissing = false;
    for (int i = 0; i < c.embCount; i++) {
      if (!c.emb[i].enable) continue;
      bool have = fsFileExists(c.emb[i].archivePath);
      printf("    Archive  : %s%s\n", c.emb[i].archivePath, have ? "" : " (missing)");
      if (!have) anyMissing = true; /* entri hilang = build gagal, bukan sukses sunyi*/
    }
    if (anyMissing) {
      fprintf(stderr, "satria: build failed: entri embedded hilang (lihat '(missing)' di atas)\n");
      return 1;
    }
    /* No-op: tidak ada arsip yang dibangun ulang (design/build_report.md). */
    if (!anyRebuilt) printf("    Skipped  : true\n");
    printf("    Status   : Success\n");
    return 0;
  }

  /* Fase parse & generate embedded (design/embedded.md): arsip tiap entri
     dibangun dulu, lalu embedded.c (array byte + tabel getEmbed) dan
     embedded.h diterbitkan SEBELUM kompilasi — embedded.c ikut daftar
     source lewat cmdsCollectSources (fingerprint, decide, compdb, link,
     kemasan library), persis seperti source biasa. */
  bool embHeaderChanged = false;
  if (c.embCount > 0) {
    mkdirs(c.outBuildDir);
    if (!buildEmbeddedArchivesEx(&c, NULL)) return 1;
    if (!emitEmbeddedSource(&c, NULL)) return 1;
    if (!emitEmbeddedHeader(&c, &embHeaderChanged)) return 1;
    for (int i = 0; i < c.embCount; i++) {
      const EmbeddedEntry *e = &c.emb[i];
      if (!e->enable) continue;
      if (e->usePrebuilt)
        printf("%s Embedded: %s (file: %s)\n", repPrefix(), e->name, e->archivePath);
      else if (fsFileExists(e->archivePath))
        printf("%s Embedded: %s (archive: %s)\n", repPrefix(), e->name, e->archivePath);
    }
    profMark("embed");
  }

  List srcs = {0};
  if (!wsModeActive()) cmdsSingleSourcesCacheBegin();
  cmdsCollectSources(&c, &srcs);
  if (!wsModeActive()) cmdsSingleSourcesCacheEnd();

  /* Dua sumber yang berbeda tidak boleh memetakan ke object yang sama: itu
     akan menimpa object diam-diam (design/workspace.md: nama output yang
     bentrok harus dideteksi). */
  if (!cmdsCheckObjectCollisions(&c, &srcs)) {
    listFree(&srcs);
    return 1;
  }

  /* One-pass stat plan: stamp source (selalu) + object (default; hilangkan
   * dengan SATRIA_STATPLAN_OBS=0) SEKALI di sini. Konsumen berikutnya memakai
   * hasilnya tanpa stat ulang. */
  StatPlan plan;
  statPlanInit(&plan);
  statPlanBuild(&plan, &c, &srcs);
  profMark("collect+plan");

  if (srcs.count == 0) {
    fprintf(stderr, "satria: tidak ada source .c/.cpp di sources — proyek standar memakai folder "
                    "src/ (atau set sources = ... untuk tata letak lain)\n");
    statPlanFree(&plan);
    return 1;
  }

  CmdsBuildFingerprint currentFp;
  cmdsFingerprintMake(&c, &srcs, &currentFp);
  CmdsBuildFingerprint previousFp = {0};
  bool haveBuildFp = cmdsFingerprintLoad(&previousFp);
  bool compileConfigChanged = !haveBuildFp || previousFp.compile != currentFp.compile;
  bool sourceSetChanged = !haveBuildFp || previousFp.sources != currentFp.sources;
  bool linkConfigChanged = !haveBuildFp || previousFp.link != currentFp.link;

  profMark("fingerprint");

  if (haveBuildFp && compileConfigChanged)
    printf("%s Fingerprint: compile configuration changed; rebuilding objects\n", repPrefix());
  if (haveBuildFp && sourceSetChanged)
    printf("%s Fingerprint: source set changed; refreshing link inputs\n", repPrefix());
  if (haveBuildFp && linkConfigChanged)
    printf("%s Fingerprint: link configuration changed; relinking\n", repPrefix());

  mkdirs(c.outBuildDir);
  mkdirs(c.outBinaryDir);

  char *inc = includeFlags(&c);
  char *wf = warningFlags(&c);

  /* compile_commands.json tidak perlu dihitung ulang pada setiap build.
     Daftar command hanya berubah jika file belum ada, konfigurasi compile
     berubah, atau set source berubah. Perubahan isi source/header tidak
     mengubah command compile. */
  profMark("pre-compdb");
  if (compdbEnabled(&c) && !wsModeActive()) {
    /* Mode workspace: CompDB ditulis terpusat oleh jalur workspace (satu
       compile_commands.json mewakili SELURUH workspace) — pass per proyek
       tidak menulis apa pun (design/workspace_v1.md §No-op dan CompDB). */
    bool compdbMissing = !fsFileExists("compile_commands.json");
    if (compdbMissing || !haveBuildFp || compileConfigChanged || sourceSetChanged)
      writeCompdb(&c, &srcs);
  }
  profMark("compdb");

  /* Terbitkan build/version.h sebelum kompilasi agar konsumen melihat
     versi yang benar; perubahan memaksa kompilasi ulang total. embedded.h
     sudah diterbitkan di fase parse & generate (embHeaderChanged dihitung
     di sana) — jalurnya tetap memaksa recompile penuh di blok di bawah. */
  char obj[MAX_PATH];
  bool verHeaderChanged = false;
  if (!cmdsEmitVersionHeader(&c, &verHeaderChanged)) {
    statPlanFree(&plan);
    return 1;
  }
  if (embHeaderChanged || verHeaderChanged) {
    if (verHeaderChanged)
      printf("%s Version : .satria-version changed, recompiling all sources\n", repPrefix());
    else
      printf("%s Embedded: config changed, recompiling all sources\n", repPrefix());
    for (int i = 0; i < srcs.count; i++) {
      if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
      /* Hanya hapus bila object memang ada menurut plan stat (sudah dihitung
         sekali di awal run): object yang tidak ada (mis. sesudah clean) tidak
         perlu syscall unlink yang pasti gagal. Tanpa plan valid, hapus seperti
         biasa. */
      int oi = (plan.objIdx && plan.obsObj) ? plan.objIdx[i] : -1;
      if (oi < 0 || plan.objM[oi] >= 0) fsRemoveFile(obj);
      /* Stamp plan objek yang baru saja dihapus: tanpa ini decide-from-plan
         akan melihat objek lama (masih ada & fresh) dan melewati kompilasi
         yang justru wajib diulang. */
      if (plan.objIdx && plan.objIdx[i] >= 0) plan.objM[plan.objIdx[i]] = -1;
    }
  }

  printf("%s Build with %s %s-std=%s\n", repPrefix(), c.cc, wf, c.std);

  int total = 0, compiled = 0, skipped = 0, failed = 0, interrupted = 0;

  /* Fase 1: klasifikasi up-to-date vs perlu-kompilasi. Pakai mtime
     nanodetik (bukan detik) agar perubahan dalam detik yang sama tetap
     terdeteksi — sama seperti keputusan link di bawah. */
  /*
   * Header tracking dua lapis:
   *   1. mtime — object usang bila header transitive lebih baru. Murah.
   *   2. konten — bila mtime mengatakan stale (mis. setelah `touch`), hash
   *      isi dibandingkan dengan snapshot build sukses terakhir
   *      (.satria/deps.cache). Konten sama => tidak perlu kompilasi ulang.
   *
   * Edges header diambil dari file .d compiler (flag -MMD, GCC/Clang) —
   * akurat & lengkap; pemindai #include hanya fallback (build pertama,
   * MSVC, .d belum ada). Lihat deps/.
   */
  List pending = {0};
  /* Hash objek lama untuk tiap source yang perlu dikompilasi. Setelah compiler
     selesai, hasil byte dibandingkan: source/header boleh berubah tanpa
     memaksa relink bila object efektif tetap identik. */
  List pendingObjHashes = {0};
  DepCache *deps = depsNew(&c);
  int headerStale = 0, headerTouched = 0;
  /* mtime object terbesar & apakah ada object yang hilang — dikumpulkan di
     sini agar keputusan link (no-op) tidak men-stat ulang semua object. */
  int64_t maxObjM = -1;
  bool anyObjMissing = false;
  /* Default: decide membaca plan (satu stat / file). SATRIA_DECIDE_FROM_PLAN=0
     mengembalikan stat sendiri (perilaku lama). */
  bool decideFromPlan = !envOff("SATRIA_DECIDE_FROM_PLAN");
  for (int i = 0; i < srcs.count; i++) {
    const char *src = srcs.items[i];
    if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;
    /* Sumber mtime dari plan one-pass; SATRIA_DECIDE_FROM_PLAN=0 = stat sendiri.
       Objek: dari plan bila objek ikut distamp (obsObj), selain itu stat
       sendiri. Semantik -1 (hilang) dipertahankan. */
    int64_t srcM = (decideFromPlan && plan.srcM) ? plan.srcM[i] : cmdsMTimeDecide(src);
    int64_t objM;
    /* Object dari plan hanya saat objek ikut distamp: stamp bisa basi bila
       objek dihapus jalur embedded/version header di atas — loop itu sudah
       meng-invalidasi slot (objM = -1) sehingga decide memaksa recompile. */
    if (decideFromPlan && plan.objIdx && plan.objIdx[i] >= 0 && plan.obsObj)
      objM = plan.objM[plan.objIdx[i]];
    else
      objM = cmdsMTimeDecide(obj); /* -1 = object belum ada (sekali stat) */
    if (objM < 0)
      anyObjMissing = true;
    else if (objM > maxObjM)
      maxObjM = objM;
    if (!compileConfigChanged && objM >= 0 && srcM >= 0 && objM >= srcM) {
      if (depsNewestHeaderMTimeAt(deps, src, srcM) > objM) {
        /* header lebih baru — tapi mungkin hanya `touch`: cek konten */
        if (depsContentUpToDate(deps, src)) {
          headerTouched++;
          skipped++;
          continue;
        }
        headerStale++;
      } else {
        skipped++;
        continue;
      }
    }
    /* Sumber lebih baru dari object (mis. `touch` yang tidak sempat di-PATCH,
       lalu run berikutnya membangun proyek lain lebih dulu): buktikan lewat
       hash isi terhadap snapshot build sukses terakhir (.satria/deps.cache).
       Konten sama -> tanpa kompilasi (design/startup.md §4 — sentuh tanpa
       perubahan isi tak memicu build; jalur cache on & incremental lama
       kini sepakat). Snapshot tak ada (sumber baru / build pertama) atau
       isi berbeda -> tetap masuk antrean kompilasi. */
    if (!compileConfigChanged && objM >= 0 && srcM > objM && depsContentUpToDate(deps, src)) {
      skipped++;
      continue;
    }
    listAdd(&pending, src);
    char oldHash[32];
    uint64_t h = objM >= 0 ? cmdsStateFileHash(obj) : 0;
    snprintf(oldHash, sizeof(oldHash), "%016llx", (unsigned long long)h);
    listAdd(&pendingObjHashes, oldHash);
  }
  profMark("decide");
  if (headerTouched > 0)
    printf("%s Headers : %d source(s) skipped (touched, content unchanged)\n", repPrefix(),
           headerTouched);
  if (headerStale > 0)
    printf("%s Headers : %d source(s) stale due to header change\n", repPrefix(), headerStale);
  total = pending.count;

  /* Fase 2: kompilasi hanya yang berubah — paralel (-jN) atau serial. */
  if (jobs != 1 && total > 0) {
    cmdsRunParallelJobs(&c, inc, wf, &pending, obj, sizeof(obj), jobs, &compiled, &failed,
                        &interrupted, false);
  } else {
    for (int i = 0; i < pending.count; i++) {
      const char *src = pending.items[i];
      if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;

      ProcHandle cap;
      double start = nowSeconds();
      bool ok = compileLibraryOne(&c, inc, wf, src, obj, &cap);
      double dt = nowSeconds() - start;

      compiled++;
      if (procInterrupted()) {
        /* foreground=false: child mati karena forward SIGINT dari satria —
           hentikan build, jangan lanjut ke source berikutnya. */
        interrupted++;
        if (ok || c.progressErrorAlways)
          printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, "INT", dt, src);
        cmdsEmitCapture(&cap);
        break;
      }
      if (!ok) failed++;
      if (ok || c.progressErrorAlways)
        printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, ok ? "OK" : "FAIL", dt, src);
      /* Output compiler dicetak UTUH setelah baris progress — stream yang
         sama menjamin urutan & anti-interleaving (lihat cmdsEmitCapture). */
      cmdsEmitCapture(&cap);
    }
  }
  profMark("compile");

  if (interrupted > 0) {
    /* Jangan percaya fingerprint lama setelah sebagian object mungkin telah
       ditulis dengan konfigurasi baru sebelum interupsi. */
    if (compileConfigChanged) cmdsFingerprintInvalidate();
    fprintf(stderr, "\nsatria: build interrupted; stopping\n");
    listFree(&pendingObjHashes);
    free(inc);
    free(wf);
    statPlanFree(&plan);
    return 130;
  }

  if (failed > 0) {
    /* Sebagian object bisa berhasil dibuat sebelum object lain gagal. Hapus
       state agar build berikutnya tidak melewatkan rebuild konfigurasi penuh. */
    if (compileConfigChanged) cmdsFingerprintInvalidate();
    fprintf(stderr, "\nsatria: build failed with %d error(s); linking skipped\n", failed);
    listFree(&pendingObjHashes);
    free(inc);
    free(wf);
    statPlanFree(&plan);
    return 1;
  }

  bool objectsChanged = compiled > 0; /* fallback konservatif */
  if (compiled > 0 && pendingObjHashes.count == pending.count) {
    objectsChanged = false;
    for (int i = 0; i < pending.count; i++) {
      char objPath[MAX_PATH];
      if (!objectPathFor(&c, pending.items[i], objPath, sizeof(objPath))) {
        objectsChanged = true;
        break;
      }
      uint64_t before = (uint64_t)strtoull(pendingObjHashes.items[i], NULL, 16);
      uint64_t after = cmdsStateFileHash(objPath);
      if (!before || !after || before != after) {
        objectsChanged = true;
        break;
      }
    }
  }
  listFree(&pendingObjHashes);

  profMark("objhash");

  /* Fase library workspace (pass 1 dari cmdBuildEx, lihat commands.h):
     kemas lib<name>.a/.so dari object milik proyek ini lalu berhenti —
     link binary ditunda ke fase normal (pass 2) agar proyek yang saling
     memakai library lintas-proyek bisa link setelah SEMUA library ada.
     Library ber-path proyek lain (mis. ../ruka/lib/libruka.a) tidak
     disertakan pada link .so: .a proyek lain belum tentu ada di titik ini
     dan shared library memang tidak perlu inline-kan kode proyek lain. */
  if (libOnly) {
    if (!cmdsBuildLibraryEx(&c, &srcs, false)) {
      free(inc);
      free(wf);
      depsFree(deps);
      return 1;
    }
    profMark("libs");
    if (depsSnapshotIncomplete(deps)) {
      for (int i = 0; i < srcs.count; i++)
        depsRecordUpdate(deps, srcs.items[i]);
    } else if (compiled > 0) {
      for (int i = 0; i < pending.count; i++)
        depsRecordUpdate(deps, pending.items[i]);
    }
    depsSave(deps);
    if (wsModeActive()) wsFastStateSave(deps, &c, &srcs, "", buildfilePath, true);
    depsFree(deps);
    wsPhaseObjectChangeSet(objectsChanged);
    /* libOnly=true: commit hash compile saja — sources/link diserahkan ke
       pass binary supaya source set/link config berubah tetap terdeteksi
       di pass yang sama (lihat cmdsFingerprintSave). */
    cmdsFingerprintSave(&currentFp, libOnly);
    /* State fase library (miniws): no-op pass 1 berikutnya dilewati total.
       target "" = proyek tanpa library — direkam sebagai "-". */
    if (!wsModeActive()) cmdsFastStateSave(&c, &srcs, "", buildfilePath, true);
    printf("\n%s Summary :\n", repPrefix());
    if (c.libRequested) {
      char libp[MAX_PATH * 2];
      if (c.libStatic) {
        cmdsLibStaticPath(&c, libp, sizeof(libp));
        printf("    Library  : %s\n", libp);
      }
      if (c.libShared) {
        cmdsLibSharedPath(&c, libp, sizeof(libp));
        printf("    Library  : %s\n", libp);
      }
    }
    printf("    Compiled : %d\n", compiled);
    printf("    Skipped  : %d\n", skipped);
    printf("    Status   : Success\n");
    free(inc);
    free(wf);
    profReport();
    return 0;
  }

  /* link: hanya jalankan linker bila target belum ada atau salah satu
     input object lebih baru daripada target. Gunakan nanosecond mtime agar
     keputusan incremental tidak kehilangan perubahan yang terjadi dalam
     detik yang sama. */
  char target[MAX_PATH * 2];
  snprintf(target, sizeof(target), "%s/%s", c.outBinaryDir, c.outBinaryName);
#ifdef _WIN32
  /* Windows dapat menambahkan .exe otomatis saat link. */
  if (!fsFileExists(target)) {
    char withExe[MAX_PATH * 2];
    snprintf(withExe, sizeof(withExe), "%s.exe", target);
    if (fsFileExists(withExe)) snprintf(target, sizeof(target), "%s", withExe);
  }
#endif

  bool phaseObjectsChanged = false;
  bool phaseObjectVerdict = compiled == 0 && wsPhaseObjectChangeGet(&phaseObjectsChanged);
  bool linkNeeded = !fsFileExists(target) || linkConfigChanged || sourceSetChanged || objectsChanged ||
                    (phaseObjectVerdict && phaseObjectsChanged);
  if (!linkNeeded) {
    int64_t targetMtime = fsMTimeNs(target);
    if (targetMtime < 0) {
      linkNeeded = true;
    } else {
      if ((compiled > 0 && !objectsChanged) || (phaseObjectVerdict && !phaseObjectsChanged)) {
        /* Object pass ini atau library pass sebelumnya boleh saja menulis
           ulang object, tetapi byte hasilnya identik. Mtime baru bukan input
           linker baru, jadi jangan relink hanya karena timestamp. */
      } else if (compiled == 0 && !anyObjMissing) {
        /* Tidak ada object yang ditulis ulang sejak fase 1: mtime yang sudah
           dikumpulkan masih berlaku — nol stat tambahan pada build no-op. */
        if (maxObjM > targetMtime) linkNeeded = true;
      } else {
        for (int i = 0; i < srcs.count && !linkNeeded; i++) {
          if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
          int64_t objectMtime = fsMTimeNs(obj); /* sesudah compile: langsung, bukan memo */
          if (objectMtime < 0 || objectMtime > targetMtime) linkNeeded = true;
        }
      }

      /* Library lintas-proyek (cdeps -> `library = <dep>/lib/x.a`) adalah input
         link nyata: perubahan isi .a proyek lain harus me-relink binary ini,
         meski object milik proyek sendiri tidak berubah. Tanpa ini bin/bade
         menyimpan salinan basi kode core — audit ws-benchmark 2026-10-15. */
      for (int i = 0; i < c.libraries.count && !linkNeeded; i++) {
        const char *lib = c.libraries.items[i];
        if (!strchr(lib, '/') && !strchr(lib, '\\')) continue; /* bentuk -l: bukan path */
        int64_t libM = fsMTimeNs(lib);
        if (libM < 0 || libM > targetMtime) linkNeeded = true;
      }
    }
  }

  if (!linkNeeded) {
    printf("%s Linking : %s (up-to-date)\n", repPrefix(), target);
    /* Library bisa jadi masih perlu dibangun (baru diaktifkan di
       Buildfile / terhapus manual) meski binary sudah up-to-date. */
    if (c.libRequested && !cmdsBuildLibraryEx(&c, &srcs, true)) {
      free(inc);
      free(wf);
      return 1;
    }
    free(inc);
    free(wf);
    profReport();
    /* Build sukses: rekam snapshot hash source + dependensi agar build
       berikutnya bisa membedakan `touch` (isi sama) dari perubahan konten.
       No-op murni (tidak ada kompilasi, tidak ada yang berubah sejak rekam
       terakhir) melewatinya — DFS + ribuan snapshotSet murni CPU sia-sia
       pada project besar. depsSave tetap dipanggil: verifikasi konten
       (kasus touch) memperbarui mtime snapshot, dan depsSave sendiri murah
       bila tidak ada yang berubah. */
    if (depsSnapshotIncomplete(deps)) {
      for (int i = 0; i < srcs.count; i++)
        depsRecordUpdate(deps, srcs.items[i]);
    } else if (compiled > 0) {
      for (int i = 0; i < pending.count; i++)
        depsRecordUpdate(deps, pending.items[i]);
    }
    depsSave(deps);
    if (wsModeActive()) wsFastStateSave(deps, &c, &srcs, target, buildfilePath, libOnly);
    depsFree(deps);
    cmdsFingerprintSave(&currentFp, libOnly);
    /* Packaging (pack.*) sebelum summary; fast state disimpan di dalamnya
       bila pack sukses — gagal kemasan tidak merekam state. */
    bool fastSaved = false;
    if (!cmdsPackAfterBinary(&c, &fastSaved, &srcs, target, buildfilePath)) {
      statPlanFree(&plan);
      return 1;
    }
    printf("\n%s Summary :\n", repPrefix());
    long long sizeBytes = fsFileSize(target);
    double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;
    printf("    Target   : %s\n", target);
    printf("    Size     : %.1fKB\n", sizeKb);
    printf("    Compiled : %d\n", compiled);
    printf("    Skipped  : %d\n", skipped);
    printf("    Status   : Success\n");
    if (!fastSaved && !wsModeActive()) cmdsFastStateSave(&c, &srcs, target, buildfilePath, false);
    return 0;
  }

  size_t n = strlen(c.cc) + strlen(c.outBinaryDir) + strlen(c.outBinaryName) + 256;
  for (int i = 0; i < srcs.count; i++)
    if (objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) n += strlen(obj) + 2;
  for (int i = 0; i < c.libraries.count; i++)
    n += strlen(c.libraries.items[i]) + 8;

  char *cmd = malloc(n);
  strcpy(cmd, c.cc);
  for (int i = 0; i < srcs.count; i++) {
    if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
    strcat(cmd, " ");
    strcat(cmd, obj);
  }
  if (compilerIsMSVC(&c)) {
    /* MSVC: object dikumpulkan dulu, opsi linker setelah token /link.
       /OUT menentukan target; link.exe untuk EXE tanpa /LD tidak menulis
       .lib/.exp sampingan, jadi tidak perlu /IMPLIB. (Nama variabel beda
       dari `target` luar — MSVC /W4 memperingatkan shadowing, C4456.) */
    char linkTarget[MAX_PATH * 2];
    snprintf(linkTarget, sizeof(linkTarget), "%s/%s", c.outBinaryDir, c.outBinaryName);
    /* link.exe TIDAK menambahkan .exe pada /OUT yang tanpa ekstensi (beda dari
       MinGW gcc yang menambahkannya sendiri). Tanpa ekstensi, binary self-host
       berakhir sebagai file terpisah dari target inkremental bin/<name>.exe —
       ringkasan membaca .exe basi dan run berikutnya menautkan ulang. */
    {
      size_t ltLen = strlen(linkTarget);
      if (ltLen < 4 || strcmp(linkTarget + ltLen - 4, ".exe") != 0)
        strcat(linkTarget, ".exe");
    }
    strcat(cmd, " /link /nologo /INCREMENTAL:NO /OUT:");
    strcat(cmd, linkTarget);
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      if (lib[0] == '-') lib++; /* -lssl -> ssl */
      if (lib[0] == 'l') lib++; /* "lssl" -> "ssl", seperti konvensi headers "I." */
      strcat(cmd, " ");
      strcat(cmd, lib);
      strcat(cmd, ".lib");
    }
  } else {
    strcat(cmd, " -o ");
    strcat(cmd, c.outBinaryDir);
    strcat(cmd, "/");
    strcat(cmd, c.outBinaryName);
    /* Driver C (compiler = gcc eksplisit) + source C++: runtime libstdc++
       harus di-link manual; driver C++ (g++/cl) sudah membawanya sendiri. */
    if (c.langCpp && !compilerIsCppDriver(&c)) strcat(cmd, " -lstdc++");
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      /* "ssl" -> -lssl, "lm" -> -lm (leading 'l' sudah termasuk, seperti "I."
         di headers); entri yang sudah diawali '-' diteruskan apa adanya */
      strcat(cmd, " ");
      if (strchr(lib, '/') || strchr(lib, '\\')) {
        /* Workspace project-library resolution may provide an explicit
           artifact path (../project/lib/libname.a). Do not turn it into
           -l../...; pass the path directly to the linker. */
        strcat(cmd, lib);
      } else {
        if (lib[0] == '-' || lib[0] == 'l')
          strcat(cmd, "-");
        else
          strcat(cmd, "-l");
        strcat(cmd, lib);
      }
    }
  }

  printf("%s Linking : %s/%s\n", repPrefix(), c.outBinaryDir, c.outBinaryName);
  bool linked = runCmd(cmd);
  profMark("link");
  free(cmd);

  free(inc);
  free(wf);

  if (!linked) {
    fprintf(stderr, "satria: link failed\n");
    statPlanFree(&plan);
    return 1;
  }

  /* Library statis/shared dari object yang sama — object tidak dihapus. */
  if (!cmdsBuildLibraryEx(&c, &srcs, true)) {
    statPlanFree(&plan);
    return 1;
  }

  /* Build sukses: rekam snapshot hash (lihat jalur up-to-date di atas). */
  if (depsSnapshotIncomplete(deps)) {
    for (int i = 0; i < srcs.count; i++)
      depsRecordUpdate(deps, srcs.items[i]);
  } else if (compiled > 0) {
    for (int i = 0; i < pending.count; i++)
      depsRecordUpdate(deps, pending.items[i]);
  }
  depsSave(deps);
  if (wsModeActive()) wsFastStateSave(deps, &c, &srcs, target, buildfilePath, libOnly);
  depsFree(deps);
  cmdsFingerprintSave(&currentFp, libOnly);

  /* Packaging (pack.*) setelah link & state — lihat jalur up-to-date. */
  bool fastSaved = false;
  if (!cmdsPackAfterBinary(&c, &fastSaved, &srcs, target, buildfilePath)) {
    statPlanFree(&plan);
    return 1;
  }
  profReport();

  long long sizeBytes = fsFileSize(target);
  double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;

  printf("\n%s Summary :\n", repPrefix());
  printf("    Target   : %s\n", target);
  printf("    Size     : %.1fKB\n", sizeKb);
  printf("    Compiled : %d\n", compiled);
  printf("    Skipped  : %d\n", skipped);
  printf("    Status   : Success\n");
  if (!fastSaved && !wsModeActive()) cmdsFastStateSave(&c, &srcs, target, buildfilePath, false);
  statPlanFree(&plan);
  return 0;
}

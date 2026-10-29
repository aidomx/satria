#include "commands.h"
#include "cache/artifact.h"
#include "log/build_log.h"
#include "log/report_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#endif

#include "cmdhelp.h"
#include "background.h"
#include "startup.h"
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

void wsPhaseObjectChangeSet(bool changed) {
  cmdsWsPhaseVerdictSet(cmdsStateWsProject(), changed);
}

bool wsPhaseObjectChangeGet(bool *changed) {
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

/* Warna warning compiler mengikuti STDERR (tempat diagnostik dicetak). Stdout
   bisa dialihkan tee laporan (report.blob) sehingga tidak lagi TTY di tengah
   build; stderr tetap terminal. Aturan env sama dengan termColorWanted. */
static bool stderrIsTTY(void) {
#ifdef _WIN32
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(2) == 1;
#endif
}

bool diagColorWanted(void) {
  if (!stderrIsTTY()) return false;
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


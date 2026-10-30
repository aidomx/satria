/*
 * bootstrap.c — fase 1 (Parse) & fase 2 (Apply) dari design/bootstrap.md.
 *
 *   bootstrapParse  : argv -> Bootstrap. MURNI parsing: tidak ada chdir,
 *                     fopen, mmap, atau mutasi apapun (design aturan fase 1).
 *   bootstrapApply  : mutasi environment yang sudah diputuskan parser:
 *                     `-f` chdir ke direktorinya, `-xf/-xcf` konversi
 *                     build.ninja -> Buildfile, evaluasi state build.
 *   bootstrapCleanup: free strdup milik Bootstrap.
 *
 * Aturan kepemilikan memori (design aturan 1): string opsional menunjuk ke
 * argv milik caller — kecuali ws_resolve_sel hasil penggabungan token `--`
 * (strdup) dan conv_abs (buffer statis apply). argv hidup sepanjang main(),
 * jadi cleanup hanya perlu membebaskan strdup yang jelas.
 */

#include "bootstrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "config.h"
#include "makeconv.h"
#include "ninjaconv.h"
#include "portability.h"
#include "uses/workspace.h"

/* -xf TIDAK pernah menulis Buildfile: konversi masuk file sementara ini
   (dihapus setelah command selesai), sehingga Buildfile project tak mungkin
   tertimpa oleh hasil konversi. */
#define XF_TMP "Buildfile.xf.tmp"

/* Kapasitas gabungan pasangan selektor release setelah `--`. */
#define WS_SEL_MAX 1024

/* ---------- Opsi CLI: -jN / -j / --jobs=N ---------- */

static bool parseJobsArg(const char *inlineVal, bool hasInline, int *outJobs, const char **err) {
  const char *v = NULL;

  if (hasInline) { /* -j4 / --jobs=4 */
    v = inlineVal;
  } else { /* -j / --jobs (tanpa nilai) -> default jumlah core CPU */
    *outJobs = cpuCount();
    return true;
  }

  if (!v || !*v) { /* -j'' : nilai kosong -> default core */
    *outJobs = cpuCount();
    return true;
  }

  char *end = NULL;
  long n = strtol(v, &end, 10);
  if (end == v || *end != '\0' || n < 1) {
    *err = v;
    return false;
  }
  *outJobs = (int)n;
  return true;
}

/* ---------- Fase 1: Parse ---------- */

/* Kata yang berarti command (bukan nama proyek) setelah `-w`. */
static bool bootstrapIsCommandWord(const char *s) {
  static const char *const kCommands[] = {
    "build", "clean", "init", "help", "version", "release", "watch", "log", "trace",
  };
  for (size_t k = 0; k < sizeof(kCommands) / sizeof(kCommands[0]); k++)
    if (strcmp(s, kCommands[k]) == 0) return true;
  return false;
}

int bootstrapParse(int argc, const char *argv[], Bootstrap *bs) {
  memset(bs, 0, sizeof(*bs));
  bs->cmd = CMD_NONE;
  bs->buildfile = "Buildfile";
  bs->jobs = 0; /* 0 = otomatis: jumlah core CPU (ditetapkan di bawah) */
  bs->conv_keep = false;
  bs->ws_sel_argv = NULL; /* diisi saat `--` ditemukan */

  bool haveF = false;     /* true bila -f diberikan secara eksplisit */
  bool convSet = false;   /* terlihat -xf / -xcf */
  const char *cmd = NULL; /* token command asli, NULL bila tidak ada */
  bool brokeAtProfile = false;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *err = NULL;

    /* -w: mode workspace (Buildfile.ws). `satria -w` = semua proyek;
       `satria -w <nama>` = hanya proyek itu. OPSI, bukan command — argumen
       setelahnya TIDAK diperlakukan sebagai command (kecuali command yang
       dikenal, lihat di bawah). */
    if (strcmp(a, "-w") == 0) {
      bs->want_workspace = true;
      /* Kata berikutnya = nama proyek KECUALI command yang dikenal
         (satria -w clean harus tetap berarti clean, bukan proyek "clean"). */
      if (i + 1 < argc && argv[i + 1][0] && argv[i + 1][0] != '-' &&
          !bootstrapIsCommandWord(argv[i + 1]))
        bs->ws_only = argv[++i];
      continue;
    }

    if (strcmp(a, "-p") == 0) { /* satria init -p: mode interaktif */
      bs->interactive = true;
      continue;
    }

    /* -g compdb: hasilkan compile_commands.json tanpa build (query-only). */
    if (strcmp(a, "-g") == 0) {
      if (i + 1 >= argc || !argv[i + 1][0] || argv[i + 1][0] == '-') {
        fprintf(stderr, "satria: -g requires a target (mis. 'satria -g compdb')\n");
        return 2;
      }
      const char *gt = argv[++i];
      if (strcmp(gt, "compdb") != 0) {
        fprintf(stderr, "satria: unknown -g target '%s' (yang tersedia: compdb)\n", gt);
        return 2;
      }
      bs->generate_compdb = true;
      continue;
    }

    if (strcmp(a, "-xf") == 0 || strcmp(a, "-xcf") == 0) {
      bs->conv_keep = (a[2] == 'c');
      if (i + 1 >= argc || !argv[i + 1][0]) {
        fprintf(stderr, "satria: %s requires a build.ninja or 'make' argument\n", a);
        return 2;
      }
      bs->conv_file = argv[++i];
      /* Kata kunci "make": konversi Makefile (design/convert.md) —
         Makefile/makefile/GNUmakefile dicari di direktori kerja saat
         Apply (setelah chdir -f bila ada). */
      bs->conv_make = (strcmp(bs->conv_file, "make") == 0);
      convSet = true;
      continue;
    }

    if (strcmp(a, "-f") == 0) {
      if (i + 1 >= argc || !argv[i + 1][0]) {
        fprintf(stderr, "satria: -f requires a file argument\n");
        return 2;
      }
      bs->buildfile = argv[++i];
      haveF = true;
      continue;
    }
    if (strncmp(a, "-f", 2) == 0 && a[2]) { /* -fBuildfile.aliased */
      bs->buildfile = a + 2;
      haveF = true;
      continue;
    }

    if (strcmp(a, "-j") == 0 || strcmp(a, "--jobs") == 0) {
      if (!parseJobsArg(NULL, false, &bs->jobs, &err)) {
        fprintf(stderr, "satria: invalid job count '%s'\n", err ? err : a);
        return 2;
      }
      continue;
    }
    if (strncmp(a, "-j", 2) == 0 && a[2] != '\0') { /* -j4 */
      if (!parseJobsArg(a + 2, true, &bs->jobs, &err)) {
        fprintf(stderr, "satria: invalid job count '%s'\n", err ? err : a + 2);
        return 2;
      }
      continue;
    }
    if (strncmp(a, "--jobs=", 7) == 0) { /* --jobs=4 */
      if (!parseJobsArg(a + 7, true, &bs->jobs, &err)) {
        fprintf(stderr, "satria: invalid job count '%s'\n", err ? err : a + 7);
        return 2;
      }
      continue;
    }

    /* `-- key=value ...`: release via CLI. SEMUA token setelah `--`
       DISIMPAN sebagai array pointer (strv) + flag; penggabungan dan
       validasi dilakukan fase Apply (delegasi ke commands), agar fase
       Parse tetap bebas alokasi berat. Diterima sebelum/sesudah command. */
    if (strcmp(a, "--") == 0) {
      if (i + 1 >= argc || !argv[i + 1][0]) {
        fprintf(stderr, "satria: -- requires key=value (mis. name=rupa)\n");
        return 2;
      }
      int n = 0;
      while (i + 1 < argc && argv[i + 1][0]) {
        i++;
        n++;
      }
      /* salin daftar pointer agar ditutup NULL:
         argv[0..n-1] milik caller; array kecil malloc -> di-clean. */
      const char **sel = malloc((size_t)(n + 1) * sizeof(*sel));
      if (!sel) {
        fprintf(stderr, "satria: out of memory\n");
        return 2;
      }
      for (int k = 0; k < n; k++)
        sel[k] = argv[i - n + 1 + k];
      sel[n] = NULL;
      bs->ws_sel_argv = sel;
      continue;
    }

    /* `satria watch --full`: stream background.log alih-alih baris progress
       (design/background.md §3C). Opsi milik command watch; token lain yang
       tidak dikenal tetap diabaikan setelah command seperti sebelumnya. */
    if (strcmp(a, "--full") == 0) {
      bs->watch_full = true;
      continue;
    }

    if (!cmd) {
      cmd = a;
      /* `profile` diagnostik berdiri sendiri (design/profile.md): SEMUA
         argumen setelahnya adalah context/opsi milik profile — berhenti
         parse opsi global agar tidak salah makna (mis. with=tar bukan
         opsi -f/-w satria). */
      if (strcmp(a, "profile") == 0) {
        bs->cmd = CMD_PROFILE;
        bs->profile_argv = argv + i + 1;
        bs->profile_argc = argc - i - 1;
        brokeAtProfile = true;
        break;
      }
      /* `trace` juga berdiri sendiri: sisa argumen (opsi trace lalu args satria)
         diteruskan utuh ke cmdTrace, tidak diparse sebagai opsi global. */
      if (strcmp(a, "trace") == 0) {
        bs->cmd = CMD_TRACE;
        bs->trace_argv = argv + i + 1;
        bs->trace_argc = argc - i - 1;
        brokeAtProfile = true;
        break;
      }
    }
  }

  /* Peta command ke enum. Existing: version ditangani main.c lebih awal
     (tanpa chdir); release di workspace adalah "command" workspace, bukan
     enum terpisah — hanya berlaku dengan -w. */
  if (!brokeAtProfile) {
    if (!cmd || !*cmd) {
      bs->cmd = CMD_BUILD; /* `satria` polos = build */
    } else if (strcmp(cmd, "build") == 0) {
      bs->cmd = CMD_BUILD;
    } else if (strcmp(cmd, "init") == 0) {
      bs->cmd = CMD_INIT;
    } else if (strcmp(cmd, "clean") == 0) {
      bs->cmd = CMD_CLEAN;
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
      bs->cmd = CMD_HELP;
    } else if (strcmp(cmd, "release") == 0) {
      /* release di workspace = build + release; di mode satu-proyek, ini
         dianggap command asing — ditolak di satria.c agar jelas. */
      bs->cmd = CMD_RELEASE;
    } else if (strcmp(cmd, "log") == 0 || strcmp(cmd, "--log") == 0) {
      bs->cmd = CMD_LOG;
    } else if (strcmp(cmd, "watch") == 0) {
      bs->cmd = CMD_WATCH;
    } else if (strcmp(cmd, "version") == 0) {
      bs->cmd = CMD_VERSION;
    } else {
      fprintf(stderr, "satria: unknown command %s\n\n", cmd);
      showHelp();
      return 1;
    }
  }

  if (bs->jobs < 1) bs->jobs = cpuCount(); /* tanpa -j: paralel sebanyak core */
  bs->have_f = haveF;
  bs->cmd_token = cmd;
  (void)convSet;
  return 0;
}

/* ---------- Fase 2: Apply ---------- */

int bootstrapApply(Bootstrap *bs) {
  /* build.ninja relatif dievaluasi terhadap cwd PEMANGGIL — absolut-kan
     sebelum chdir -f di bawah. */
  static char convAbs[MAX_PATH * 2];
  bs->conv_abs = NULL;
  /* "make" bukan path — resolusi nama file dilakukan setelah chdir -f. */
  if (bs->conv_file && !bs->conv_make) {
    if (bs->conv_file[0] == '/') {
      snprintf(convAbs, sizeof(convAbs), "%s", bs->conv_file);
    } else {
      char cwd0[MAX_PATH];
      if (fsGetCwd(cwd0, sizeof(cwd0)))
        snprintf(convAbs, sizeof(convAbs), "%s/%s", cwd0, bs->conv_file);
      else
        snprintf(convAbs, sizeof(convAbs), "%s", bs->conv_file);
    }
    bs->conv_abs = convAbs;
  }

  /* -f menunjuk file di luar cwd (mis. `satria -f ../proj/Buildfile.example`):
     masuk ke direktori file itu dulu — sources/headers/output di Buildfile
     relatif terhadap lokasinya, bukan cwd pemanggil (gaya make -C /
     ninja -C). File polos tanpa direktori berarti cwd sudah benar. */
  {
    /* static: buildfile menunjuk ke dalam buffer ini setelah blok berakhir */
    static char dir[MAX_PATH];
    copyStr(dir, sizeof(dir), bs->buildfile);
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *bs2 = strrchr(dir, '\\');
    if (!slash || (bs2 && bs2 > slash)) slash = bs2;
#endif
    if (slash) {
      *slash = '\0';
      if (!fsSetCwd(dir[0] ? dir : "/")) {
        fprintf(stderr, "satria: cannot enter directory '%s'\n", dir[0] ? dir : "/");
        return 2;
      }
      bs->buildfile = slash + 1; /* lanjut sebagai nama file polos */
    }
  }

  /* Konversi build.ninja -> Buildfile (di direktori kerja saat ini).
     -xf  : selalu ke file SEMENTARA — Buildfile eksisting tidak disentuh.
     -xcf : menulis Buildfile, tapi konfirmasi dulu bila sudah ada. */
  if (bs->conv_file) {
    char err[256];
    const char *dst = bs->conv_keep ? "Buildfile" : XF_TMP;
    const char *src = bs->conv_abs;
    if (bs->conv_make) {
      /* design/convert.md Resolusi Argumen: "make" -> cari Makefile,
         makefile, atau GNUmakefile di direktori kerja saat ini. */
      static char mkAbs[MAX_PATH * 2];
      static const char *cands[] = {"Makefile", "makefile", "GNUmakefile", NULL};
      src = NULL;
      for (int k = 0; cands[k]; k++) {
        if (fsFileExists(cands[k])) {
          char cwd0[MAX_PATH];
          if (fsGetCwd(cwd0, sizeof(cwd0)))
            snprintf(mkAbs, sizeof(mkAbs), "%s/%s", cwd0, cands[k]);
          else
            snprintf(mkAbs, sizeof(mkAbs), "%s", cands[k]);
          src = mkAbs;
          break;
        }
      }
      if (!src) {
        fprintf(stderr,
                "satria: tidak menemukan Makefile/makefile/GNUmakefile di direktori saat ini\n");
        return 1;
      }
      bs->conv_abs = src; /* dipakai pesan "Removed" di satria.c */
    }
    if (bs->conv_keep && fsFileExists(dst)) {
      printf("> '%s' sudah ada. Timpa dengan hasil konversi %s? [y/N] ", dst, src);
      fflush(stdout);
      char ans[16] = {0};
      if (!fgets(ans, sizeof(ans), stdin)) ans[0] = '\0';
      if (ans[0] != 'y' && ans[0] != 'Y') {
        printf("> Dibatalkan : %s tidak diubah\n", dst);
        return 0; /* dibatalkan: parser & dispatch boleh lanjut, tapi walau
                     begitu jalur ini menandai "berhenti dengan sukses". */
      }
    }
    bool ok = bs->conv_make ? makeToBuildfile(src, dst, err, sizeof(err))
                            : ninjaToBuildfile(src, dst, err, sizeof(err));
    if (!ok) {
      fprintf(stderr, "satria: %s\n", err);
      return 1;
    }
    if (!bs->conv_keep)
      printf("> Temporary  : %s (dihapus setelah command; Buildfile tak disentuh)\n", dst);
    bs->buildfile = dst;
  }

  /* Evaluasi state build (design fase 2 + design/workspace.md): cold bila
     tidak ada state sebelumnya; noop/dirty ditentukan oleh mesin build
     sendiri saat dispatch (command lain tidak butuh implicit evaluate).
     Gerbang cold: mode workspace = keberadaan cache fingerprint
     (.satria/caches/fingerprint.cache, wsStateFileExists), mode single =
     .satria/startup.state. File lama (.satria/workspace.state dan
     .satria/build.state per-proyek) sudah tidak ditulis/dibaca. */
  bool wsMode = bs->want_workspace;
  /* Mode workspace: cold kasar dari keberadaan file gate — validasi stamp
     gate kini per-closure di workspaceRun (murah untuk `-w <nama>`).
     Status di sini hanya untuk SATRIA_STATUS/evaluasi; mesin build
     menentukan noop/dirty sendiri saat dispatch. */
  bs->status.cold = wsMode ? !wsStateFileExists() : !fsFileExists(".satria/startup.state");
  bs->status.has_state = !bs->status.cold;
  if (getenv("SATRIA_STATUS") && *getenv("SATRIA_STATUS") &&
      strcmp(getenv("SATRIA_STATUS"), "0") != 0)
    fprintf(stderr, "satria status: mode=%s cold=%d\n", wsMode ? "workspace" : "project",
            bs->status.cold ? 1 : 0);
  return 0;
}

/* Gabungkan selektor -- key=value ... menjadi satu string koma-join.
   NULL bila tak ada. Free via cleanup. */
char *bootstrapJoinSelector(const Bootstrap *bs) {
  if (!bs->ws_sel_argv) return NULL;
  size_t cap = 1;
  for (int i = 0; bs->ws_sel_argv[i]; i++) {
    size_t tl = strlen(bs->ws_sel_argv[i]);
    if (tl >= cap) cap = tl + 1;
  }
  char *sel = malloc(cap + WS_SEL_MAX);
  if (!sel) {
    fprintf(stderr, "satria: out of memory\n");
    return NULL;
  }
  size_t off = 0;
  for (int i = 0; bs->ws_sel_argv[i]; i++) {
    const char *tok = bs->ws_sel_argv[i];
    size_t tl = strlen(tok);
    if (off + tl + 2 >= cap + WS_SEL_MAX) {
      free(sel);
      fprintf(stderr, "satria: --: terlalu banyak pasangan key=value\n");
      return NULL;
    }
    if (off) sel[off++] = ',';
    memcpy(sel + off, tok, tl + 1);
    off += tl;
  }
  return sel;
}

void bootstrapCleanup(Bootstrap *bs) {
  free(bs->ws_sel_argv);
  bs->ws_sel_argv = NULL;
}

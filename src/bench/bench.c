/*
 * bench.c — porting benchmark workspace bench/ws.sh ke C (design/bench_c.md).
 *
 * Fase:
 *   1. parse     : argv `-f key=value` -> Bench (tanpa menyentuh filesystem).
 *   2. prepare   : direktori kerja, shim gcc/ld, kandidat tool.
 *   3. execute   : skenario cold/noop/leaf/bade/mod/touch/hedit/sel/dep/
 *                  libtouch/libhdr/rel (+ strace opsional).
 *   4. report    : median per skenario, validation log, success rate, artefak.
 *
 * Tidak ada bash: generator, mutator, snapshot artefak, penyalinan prime,
 * pengukuran, dan shim compiler semuanya di C. Proses di-spawn langsung
 * (fork/exec); shim gcc/ld adalah binary satria ini lewat symlink.
 */
#include "bench.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bench_internal.h"
#include "../portability.h"

#ifdef _WIN32
int cmdBench(const char *const argv[], int argc) {
  (void)argv; (void)argc;
  fprintf(stderr, "bench: hanya tersedia di POSIX (fork/exec, symlink, strace)\n");
  return 2;
}
int benchEmbedSource(const char *const argv[], int argc) { (void)argv; (void)argc; return 2; }
#else

/* ---------- util kecil ---------- */

static const char *benchBase(const char *path) {
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}

static void benchSanitize(const char *label, char *out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; label[i] && j + 1 < n; i++) {
    char c = label[i];
    out[j++] = (c == '/' || c == ' ' || c == ':') ? '_' : c;
  }
  out[j] = '\0';
}

static bool benchListHas(const char *list, const char *s) {
  char tmp[256];
  copyStr(tmp, sizeof(tmp), list);
  for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
    if (strcmp(tok, s) == 0) return true;
  return false;
}

/* Skenario yang dikenal tiap mode; `all` = daftar ini (bukan semua nama). */
static bool benchScenarioAllowed(const Bench *b, const char *s) {
  static const char *const ws[] = {"cold", "noop", "leaf", "bade", "mod", "touch", "hedit",
                                  "sel", "dep", "libtouch", "libhdr", "rel", "strace"};
  static const char *const single[] = {"cold",     "noop",  "touch", "edit",
                                      "editrel", "compdb", "strace"};
  const char *const *list = b->workspace ? ws : single;
  size_t n = b->workspace ? sizeof(ws) / sizeof(ws[0]) : sizeof(single) / sizeof(single[0]);
  for (size_t i = 0; i < n; i++)
    if (strcmp(list[i], s) == 0) return true;
  return false;
}

static bool benchHaveScenario(const Bench *b, const char *s) {
  if (!b->scenarios[0] || strcmp(b->scenarios, "all") == 0)
    return benchScenarioAllowed(b, s);
  if (benchListHas(b->scenarios, s)) return true;
  /* `scenario=trace` adalah alias `strace` (nama skenario di bash = strace). */
  if (strcmp(s, "strace") == 0 && benchListHas(b->scenarios, "trace")) return true;
  return false;
}

static bool benchAddTool(Bench *b, const char *label, BenchKind kind, const char *bin) {
  if (b->ntools >= BENCH_MAX_TOOLS) return false;
  /* Label unik: direktori workspace diturunkan dari label, jadi dua kandidat
     dengan basename sama (mis. bin A dan B sama-sama `satria`) diberi akhiran. */
  char uniq[BENCH_LABEL_LEN];
  copyStr(uniq, sizeof(uniq), label);
  for (int suffix = 2; suffix < 100; suffix++) {
    bool dup = false;
    for (int i = 0; i < b->ntools; i++)
      if (strcmp(b->tools[i].label, uniq) == 0) dup = true;
    if (!dup) break;
    snprintf(uniq, sizeof(uniq), "%s-%d", label, suffix);
  }
  BenchTool *t = &b->tools[b->ntools++];
  memset(t, 0, sizeof(*t));
  copyStr(t->label, sizeof(t->label), uniq);
  t->kind = kind;
  if (bin) copyStr(t->bin, sizeof(t->bin), bin);
  char safe[BENCH_LABEL_LEN];
  benchSanitize(uniq, safe, sizeof(safe));
  benchPathCat(t->ws, sizeof(t->ws), b->workdir, "/");
  benchPathCat(t->ws, sizeof(t->ws), t->ws, safe);
  return true;
}

/* Daftar koma sederhana; tanda [ ] ' " dan spasi diabaikan (mendukung
   bentuk bin=['a','b'] seperti pada contoh pemakaian). */
static int benchSplitList(const char *spec, char out[][BENCH_PATH_LEN], int max) {
  int n = 0;
  char raw[1024];
  copyStr(raw, sizeof(raw), spec ? spec : "");
  char *save = NULL;
  for (char *tok = strtok_r(raw, ",", &save); tok && n < max; tok = strtok_r(NULL, ",", &save)) {
    char clean[BENCH_PATH_LEN];
    size_t j = 0;
    for (size_t i = 0; tok[i] && j + 1 < sizeof(clean); i++) {
      char c = tok[i];
      if (c == '[' || c == ']' || c == '\'' || c == '"' || c == ' ' || c == '\t') continue;
      clean[j++] = c;
    }
    clean[j] = '\0';
    if (clean[0]) copyStr(out[n++], BENCH_PATH_LEN, clean);
  }
  return n;
}

/* ---------- parse opsi ---------- */

static void benchDefaults(Bench *b) {
  memset(b, 0, sizeof(*b));
  b->source = 80;
  b->header = 20;
  b->module = 40;
  b->runs = 5;
  b->jobs = cpuCount();
  b->warmup = true;
  b->trace = false;
  b->artifacts = true;
  b->keep = false;
  b->rolling = true;
  copyStr(b->scenarios, sizeof(b->scenarios), "all");
  copyStr(b->toolsSpec, sizeof(b->toolsSpec), "satria,cmake");
  copyStr(b->compilers, sizeof(b->compilers), "gcc");
  b->workspace = false;
}

static void benchUsage(void) {
  fputs("Pemakaian:\n"
        "  satria bench [--compare] [-f key=value ...]\n"
        "  satria bench --workspace [--compare] [-f key=value ...]\n"
        "\n"
        "Mode:\n"
        "  (default)               satu proyek: N source berantai H header\n"
        "  --workspace             graf 3 proyek: rupamod, core, bade (WsBuildfile)\n"
        "\n"
        "Kunci -f (kedua mode):\n"
        "  tools=...               satria,cmake/cninja,ninja,make,bear,meson,\n"
        "                          xmake,tup (default: semua; yang tidak ada dilewati)\n"
        "  source=80               jumlah source (.c) per proyek\n"
        "  header=20               jumlah header berantai per proyek\n"
        "  runs=5                  pengulangan + median\n"
        "  scenario=all            single: cold,noop,touch,edit,editrel,compdb\n"
        "                          (+ gcc-floor otomatis untuk edit/editrel)\n"
        "                          workspace: cold,noop,leaf,bade,mod,touch,hedit,\n"
        "                          sel,dep,libtouch,libhdr,rel\n"
        "  warmup=true             build+gate sekali per tool lalu pulihkan\n"
        "  trace=false             syscall profiling (strace -c)\n"
        "  artifact=true           tulis results/view/work log\n"
        "  keep=false              simpan workspace setelah selesai\n"
        "  rolling=true            urutan A/B bergantian per repetisi (abba)\n"
        "  bin=a,b                 binary satria A/B yang dibandingkan\n"
        "  jobs=N                  job paralel (default: jumlah core)\n"
        "  compiler=gcc            nilai `compiler =` di Buildfile\n"
        "  satria=\"args\"           argumen tambahan untuk satria\n"
        "  out=DIR                 direktori kerja (default: bench.tmp)\n"
        "\n"
        "Kunci -f (workspace):\n"
        "  module=40               jumlah file .rp di rupamod\n"
        "\n"
        "Contoh:\n"
        "  satria bench -f tools=cmake,satria -f source=40 -f header=10\n"
        "  satria bench -f tools=ninja,make,bear,satria -f scenario=editrel\n"
        "  satria bench --workspace --compare -f tools=cmake,satria -f source=10 \\\n"
        "    -f header=10 -f module=10 -f scenario=all -f warmup=false \\\n"
        "    -f artifact=true -f keep=true -f rolling=true -f bin=a,b\n",
        stdout);
}

static bool benchApplyOpt(Bench *b, const char *kv, char *err, size_t errN) {
  const char *eq = strchr(kv, '=');
  if (!eq) {
    snprintf(err, errN, "opsi -f harus key=value (diberikan '%s')", kv);
    return false;
  }
  char key[64];
  size_t kl = (size_t)(eq - kv);
  if (kl >= sizeof(key)) kl = sizeof(key) - 1;
  memcpy(key, kv, kl);
  key[kl] = '\0';
  const char *val = eq + 1;

  /* buang kutip pembungkus */
  char vbuf[1024];
  copyStr(vbuf, sizeof(vbuf), val);
  size_t vl = strlen(vbuf);
  if (vl >= 2 && ((vbuf[0] == '\'' && vbuf[vl - 1] == '\'') ||
                  (vbuf[0] == '"' && vbuf[vl - 1] == '"'))) {
    vbuf[vl - 1] = '\0';
    memmove(vbuf, vbuf + 1, vl - 1);
  }
  val = vbuf;

  bool bv = false;
  if (strcmp(key, "tools") == 0) {
    copyStr(b->toolsSpec, sizeof(b->toolsSpec), val);
    b->toolsExplicit = true;
  } else if (strcmp(key, "bin") == 0) {
    copyStr(b->binsSpec, sizeof(b->binsSpec), val);
  } else if (strcmp(key, "scenario") == 0 || strcmp(key, "scenarios") == 0) {
    copyStr(b->scenarios, sizeof(b->scenarios), val);
  } else if (strcmp(key, "compiler") == 0) {
    copyStr(b->compilers, sizeof(b->compilers), val);
  } else if (strcmp(key, "satria") == 0) {
    copyStr(b->satriaArgs, sizeof(b->satriaArgs), val);
  } else if (strcmp(key, "out") == 0) {
    copyStr(b->workdir, sizeof(b->workdir), val);
  } else if (strcmp(key, "source") == 0 || strcmp(key, "header") == 0 ||
             strcmp(key, "module") == 0 || strcmp(key, "runs") == 0 ||
             strcmp(key, "jobs") == 0) {
    char *end = NULL;
    long n = strtol(val, &end, 10);
    if (end == val || *end || n < 1) {
      snprintf(err, errN, "opsi '%s' butuh angka >= 1 (nilai '%s')", key, val);
      return false;
    }
    if (strcmp(key, "source") == 0) b->source = (int)n;
    if (strcmp(key, "header") == 0) b->header = (int)n;
    if (strcmp(key, "module") == 0) b->module = (int)n;
    if (strcmp(key, "runs") == 0) b->runs = (int)n;
    if (strcmp(key, "jobs") == 0) b->jobs = (int)n;
  } else if (strcmp(key, "warmup") == 0 || strcmp(key, "trace") == 0 ||
             strcmp(key, "artifact") == 0 || strcmp(key, "keep") == 0 ||
             strcmp(key, "rolling") == 0) {
    if (!parseBool(val, &bv)) {
      snprintf(err, errN, "opsi '%s' harus true/false (nilai '%s')", key, val);
      return false;
    }
    if (strcmp(key, "warmup") == 0) b->warmup = bv;
    if (strcmp(key, "trace") == 0) b->trace = bv;
    if (strcmp(key, "artifact") == 0) b->artifacts = bv;
    if (strcmp(key, "keep") == 0) b->keep = bv;
    if (strcmp(key, "rolling") == 0) b->rolling = bv;
  } else {
    snprintf(err, errN, "kunci -f tidak dikenal: '%s' (lihat: satria bench --help)", key);
    return false;
  }
  return true;
}

/* ---------- versi tool ---------- */

static void benchVersionOf(const char *cmd, char *out, size_t n) {
  out[0] = '\0';
  FILE *p = popenRB(cmd);
  if (!p) return;
  char line[256];
  if (fgets(line, sizeof(line), p)) {
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
    copyStr(out, n, line);
  }
  pcloseRB(p);
}

static void benchVersionTool(BenchTool *t) {
  char cmd[BENCH_PATH_LEN + 32];
  cmd[0] = '\0';
  switch (t->kind) {
    case BENCH_SATRIA:
      snprintf(cmd, sizeof(cmd), "\"%s\" version", t->bin);
      break;
    case BENCH_CMAKE:
      copyStr(cmd, sizeof(cmd), "cmake --version");
      break;
    case BENCH_NINJA:
      copyStr(cmd, sizeof(cmd), "ninja --version");
      break;
    case BENCH_MAKE:
      copyStr(cmd, sizeof(cmd), "make --version");
      break;
    case BENCH_BEAR:
      copyStr(cmd, sizeof(cmd), "bear --version");
      break;
    case BENCH_MESON:
      copyStr(cmd, sizeof(cmd), "meson --version");
      break;
    case BENCH_XMAKE:
      copyStr(cmd, sizeof(cmd), "xmake --version");
      break;
    case BENCH_TUP:
      copyStr(cmd, sizeof(cmd), "tup --version");
      break;
    case BENCH_FLOOR:
      copyStr(cmd, sizeof(cmd), "gcc -dumpfullversion");
      break;
  }
  if (!cmd[0]) return;
  benchVersionOf(cmd, t->version, sizeof(t->version));
  if (t->kind == BENCH_SATRIA) {
    const char *v = strstr(t->version, "satria ");
    if (v) memmove(t->version, v + 7, strlen(v + 7) + 1);
  } else if (t->kind == BENCH_FLOOR && t->version[0]) {
    char tmp[sizeof(t->version) + 8];
    snprintf(tmp, sizeof(tmp), "gcc %s", t->version);
    copyStr(t->version, sizeof(t->version), tmp);
  }
}

/* ---------- reset ---------- */

static void benchResetWorkspace(const char *ws) {
  static const char *const paths[] = {
    "core/build", "core/bin", "bade/build", "bade/bin", "rupamod/build",
    "rupamod/modules", "modules", ".satria", "core/lib", "bade/lib",
    ".ninja_log", ".ninja_deps", "build", "bin", "lib", "dist", ".cmake-build",
  };
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    char p[BENCH_PATH_LEN];
    snprintf(p, sizeof(p), "%s/%s", ws, paths[i]);
    fsRemoveTree(p);
  }
}

/* ---------- catatan hasil ---------- */

static BenchRec *benchNewRec(Bench *b) {
  if (b->nrec == b->caprec) {
    int nc = b->caprec ? b->caprec * 2 : 64;
    BenchRec *nv = realloc(b->recs, (size_t)nc * sizeof(*nv));
    if (!nv) return NULL;
    b->recs = nv;
    b->caprec = nc;
  }
  BenchRec *r = &b->recs[b->nrec++];
  memset(r, 0, sizeof(*r));
  return r;
}

static bool benchFileBig(const char *path, long minSize) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return false;
  }
  long n = ftell(f);
  fclose(f);
  return n > minSize;
}

static void benchRemoveCompdb(const char *ws) {
  static const char *const rel[] = {"/compile_commands.json",
                                    "/build-cmake/compile_commands.json",
                                    "/build-meson/compile_commands.json"};
  for (size_t i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
    char path[BENCH_PATH_LEN];
    benchPathCat(path, sizeof(path), ws, rel[i]);
    remove(path);
  }
}

static const char *benchCompdbRel(BenchKind kind) {
  if (kind == BENCH_CMAKE) return "/build-cmake/compile_commands.json";
  if (kind == BENCH_MESON) return "/build-meson/compile_commands.json";
  return "/compile_commands.json";
}

static int benchCountLog(const Bench *b, char kind) {
  FILE *f = fopen(b->cclog, "rb");
  if (!f) return 0;
  int n = 0;
  char line[2048];
  while (fgets(line, sizeof(line), f))
    if (line[0] == kind && line[1] == '|') n++;
  fclose(f);
  return n;
}

/* ---------- expectation (bench/ws/expect.sh) ---------- */

static void benchExpectWorkspace(const Bench *b, const BenchTool *t, BenchRec *r) {
  int expectObj = 2 * (b->source + 1) + 1 + (b->source == 1 ? 2 : 0);
  int expectCc = 2 * (b->source + 1) + 2 * (b->source == 1 ? 1 : 0);
  int deps = 6;
  bool cm = t->kind == BENCH_CMAKE;
  r->expObj = r->expArc = r->expLib = r->expBin = r->expCc = -1;
  char miss[256] = {0};

#define EXP_SET(o, a, l, bn, c)                                   \
  do {                                                            \
    r->expObj = (o); r->expArc = (a); r->expLib = (l);            \
    r->expBin = (bn); r->expCc = (c);                             \
  } while (0)

#define EXP_CHK(name, val, want)                                                    \
  do {                                                                              \
    if ((want) >= 0 && (val) != (want)) {                                           \
      char tmp[64];                                                                 \
      snprintf(tmp, sizeof(tmp), " %s=%d(harap %d)", name, (val), (want));          \
      strncat(miss, tmp, sizeof(miss) - strlen(miss) - 1);                          \
    }                                                                               \
  } while (0)

  const char *s = r->scen;
  if (strcmp(s, "cold") == 0) {
    /* +1 codegen embed carray (satria & CMake sama); CMake menambah 1 probe
       compiler identification saat configure cold. */
    EXP_SET(expectObj, 1, 4, 2, expectCc + 1 + (cm ? 1 : 0));
  } else if (strcmp(s, "noop") == 0) {
    EXP_SET(0, 0, 0, 0, 0);
  } else if (strcmp(s, "leaf") == 0 || strcmp(s, "bade") == 0) {
    EXP_SET(1, 0, 2, 2, 1);
  } else if (strcmp(s, "mod") == 0) {
    EXP_SET(1, 1, 2, 2, 1);
  } else if (strcmp(s, "touch") == 0) {
    if (cm) EXP_SET(deps, 0, 4, 2, deps);
    else EXP_SET(0, 0, 0, 0, 0);
  } else if (strcmp(s, "hedit") == 0) {
    EXP_SET(deps, 0, 4, 2, deps);
  } else if (strcmp(s, "sel") == 0) {
    EXP_SET(1, 0, 2, cm ? 0 : 1, 1);
  } else if (strcmp(s, "dep") == 0) {
    EXP_SET(1, 1, 2, 1, 1);
  } else if (strcmp(s, "libtouch") == 0) {
    if (cm) EXP_SET(2, -1, -1, -1, 2);
    else EXP_SET(0, 0, 0, 0, 0);
  } else if (strcmp(s, "libhdr") == 0) {
    EXP_SET(2, 0, 4, 2, 2);
  } else if (strcmp(s, "rel") == 0) {
    EXP_SET(0, -1, -1, -1, 0);
  }
  EXP_CHK("obj", r->obj, r->expObj);
  EXP_CHK("arc", r->arc, r->expArc);
  EXP_CHK("lib", r->lib, r->expLib);
  EXP_CHK("bin", r->bin, r->expBin);
  EXP_CHK("cc", r->cc, r->expCc);

  if (miss[0]) {
    r->miss = true;
    fprintf(stderr, "%s/%s: kerja berbeda dari harapan:%s\n", r->label, r->scen, miss);
  }
#undef EXP_CHK
#undef EXP_SET
}

/* Harapan mode single (bench/compare/scen.sh + audit). CMake tidak di-hard-check
   untuk cc (configure probe) dan touch (mtime -> cakupan rebuild berbeda). */
static void benchExpectSingle(const Bench *b, const BenchTool *t, BenchRec *r) {
  int n = b->source + 1;
  bool sat = t->kind == BENCH_SATRIA;
  bool xmake = t->kind == BENCH_XMAKE;
  bool floor = t->kind == BENCH_FLOOR;
  r->expObj = r->expArc = r->expLib = r->expBin = r->expCc = -1;
  const char *s = r->scen;
  if (strcmp(s, "cold") == 0) {
    r->expObj = n;
    r->expBin = xmake ? -1 : 1; /* output xmake tool-dependent */
    if (sat) r->expCc = n;
  } else if (strcmp(s, "noop") == 0) {
    r->expObj = r->expCc = 0;
    /* xmake menyalin ulang target tiap no-op; objek tetap 0. */
    r->expBin = xmake ? -1 : 0;
  } else if (strcmp(s, "touch") == 0) {
    /* Hanya satria yang menjanjikan 0 recompile (content-based). Tool lain
       boleh rebuild; laporan tetap menandai jumlah objeknya. */
    if (sat) r->expObj = r->expBin = r->expCc = 0;
  } else if (strcmp(s, "edit") == 0) {
    r->expObj = 1;
    if (sat || floor) r->expCc = 1;
    /* satria boleh no-relink; lantai selalu link, tapi bin tidak dipatok
       di sini (lihat editrel untuk pembanding link yang jujur). */
  } else if (strcmp(s, "editrel") == 0) {
    r->expObj = 1;
    r->expBin = xmake ? -1 : 1;
    if (sat || floor) r->expCc = 1;
  }
  char miss[256] = {0};
#define EXP_CHK(name, val, want)                                                    \
  do {                                                                              \
    if ((want) >= 0 && (val) != (want)) {                                           \
      char tmp[64];                                                                 \
      snprintf(tmp, sizeof(tmp), " %s=%d(harap %d)", name, (val), (want));          \
      strncat(miss, tmp, sizeof(miss) - strlen(miss) - 1);                          \
    }                                                                               \
  } while (0)
  EXP_CHK("obj", r->obj, r->expObj);
  EXP_CHK("bin", r->bin, r->expBin);
  EXP_CHK("cc", r->cc, r->expCc);
  if (miss[0]) {
    r->miss = true;
    fprintf(stderr, "%s/%s: kerja berbeda dari harapan:%s\n", r->label, r->scen, miss);
  }
#undef EXP_CHK
}

static void benchExpect(const Bench *b, const BenchTool *t, BenchRec *r) {
  if (b->workspace)
    benchExpectWorkspace(b, t, r);
  else
    benchExpectSingle(b, t, r);
}

/* ---------- satu run terukur ---------- */

static void benchRecordRun(Bench *b, BenchTool *t, const char *scen, const char *ver,
                           BenchArgv *cmd, int mode) {
  char cwd[BENCH_PATH_LEN];
  copyStr(cwd, sizeof(cwd), t->ws);
  BenchRec *r = benchNewRec(b);
  if (!r) return;
  copyStr(r->label, sizeof(r->label), t->label);
  copyStr(r->scen, sizeof(r->scen), scen);

  BenchSnap before = {0}, after = {0};
  benchSnapTake(t->ws, &before);
  FILE *log = fopen(b->cclog, "wb");
  if (log) fclose(log);

  double ms = 0;
  int rc = 0;
  bool cold = mode == BENCH_CMD_FULL && strcmp(scen, "cold") == 0;
  bool configure = t->kind == BENCH_CMAKE || t->kind == BENCH_MESON;
  if (mode == BENCH_CMD_COMPDB && t->kind == BENCH_NINJA) {
    /* `ninja -t compdb cc` menulis JSON ke stdout. */
    char out[BENCH_PATH_LEN];
    benchPathCat(out, sizeof(out), cwd, "/compile_commands.json");
    benchRunArgvOut(cwd, cmd->argv, b->errlog, out, &rc, &ms);
  } else if (mode == BENCH_CMD_FLOOR) {
    benchFloorBuild(b, cwd, &rc, &ms);
  } else if (cold && (configure || t->kind == BENCH_NINJA)) {
    double t0 = monotonicSeconds();
    if (t->kind == BENCH_NINJA) {
      benchGenNinjaGraph(b, cwd);
    } else {
      BenchArgv conf;
      benchToolCommand(b, t, BENCH_CMD_CONFIGURE, NULL, &conf);
      benchRunArgv(cwd, conf.argv, b->errlog, &rc, NULL);
    }
    if (rc == 0) benchRunArgv(cwd, cmd->argv, b->errlog, &rc, NULL);
    ms = (monotonicSeconds() - t0) * 1000.0;
  } else {
    benchRunArgv(cwd, cmd->argv, b->errlog, &rc, &ms);
  }

  BenchDelta d;
  benchSnapTake(t->ws, &after);
  benchDelta(&before, &after, &d);
  benchSnapFree(&before);
  benchSnapFree(&after);

  r->rc = rc;
  r->ms = (long)(ms + 0.5);
  r->obj = d.obj;
  r->arc = d.arc;
  r->lib = d.lib;
  r->bin = d.bin;
  r->cc = benchCountLog(b, 'C');
  r->lnk = benchCountLog(b, 'L') + benchCountLog(b, 'S') + benchCountLog(b, 'D') +
           benchCountLog(b, 'E');

  if (rc == 0 && strcmp(scen, "rel") == 0) {
    char archive[BENCH_PATH_LEN], sums[BENCH_PATH_LEN];
    const char *v = ver ? ver : "0.0.1";
    char rel[BENCH_PATH_LEN];
    snprintf(rel, sizeof(rel), "/dist/satria-v%s.tar.gz", v);
    benchPathCat(archive, sizeof(archive), t->ws, rel);
    benchPathCat(sums, sizeof(sums), archive, ".sha256");
    if (!benchPathExists(archive) || !benchPathExists(sums)) {
      r->rc = 1;
    } else {
      /* tanpa shell: jalankan sha256sum -c dengan cwd dist */
      char dist[BENCH_PATH_LEN];
      benchPathCat(dist, sizeof(dist), t->ws, "/dist");
      char *sv[] = {(char *)"sha256sum", (char *)"-c", sums, NULL};
      int vrc = 0;
      if (benchRunArgv(dist, sv, b->errlog, &vrc, NULL) && vrc != 0) r->rc = 1;
    }
  }

  if (r->rc == 0 && mode == BENCH_CMD_COMPDB) {
    char cpath[BENCH_PATH_LEN];
    benchPathCat(cpath, sizeof(cpath), t->ws, benchCompdbRel(t->kind));
    if (!benchFileBig(cpath, 2)) {
      r->rc = 98;
      FILE *ef = fopen(b->errlog, "ab");
      if (ef) {
        fprintf(ef, "%s/compdb: compile_commands.json tidak dihasilkan\n", t->label);
        fclose(ef);
      }
    }
  }

  if (r->rc == 0 && mode != BENCH_CMD_COMPDB && !benchRuntimeOk(b, t)) {
    r->rc = 96;
    FILE *ef = fopen(b->errlog, "ab");
    if (ef) {
      fprintf(ef, "%s/%s: runtime check gagal (circular / object basi)\n", t->label, scen);
      fclose(ef);
    }
  }

  benchExpect(b, t, r);
  if (r->rc != 0) {
    r->fail = true;
    b->mismatchRuns++;
  } else if (r->miss) {
    b->notAcc++;
  }

  printf("> %s/%-7s %5ldms obj:%d cc:%d lib:%d bin:%d%s\n", t->label, scen, r->ms,
         r->obj, r->cc, r->lib, r->bin,
         (r->rc != 0) ? "  GAGAL" : (r->miss ? "  Not in accordance" : ""));

  /* work log ringkas */
  FILE *w = fopen(b->worklog, "ab");
  if (w) {
    fprintf(w, "## %s/%s rc=%d\n", t->label, scen, r->rc);
    FILE *cf = fopen(b->cclog, "rb");
    if (cf) {
      char line[1024];
      while (fgets(line, sizeof(line), cf)) fprintf(w, "   %s", line);
      fclose(cf);
    }
    fclose(w);
  }
}

/* ---------- prime ---------- */

static bool benchPrimeWorkspace(Bench *b, BenchTool *t) {
  benchGenWorkspace(b, t);
  BenchArgv cmd;
  benchToolCommand(b, t, BENCH_CMD_FULL, NULL, &cmd);
  int rc = 0;
  if (t->kind == BENCH_CMAKE) {
    BenchArgv conf;
    benchToolCommand(b, t, BENCH_CMD_CONFIGURE, NULL, &conf);
    if (!benchRunArgv(t->ws, conf.argv, b->errlog, &rc, NULL) || rc != 0) return false;
  }
  if (!benchRunArgv(t->ws, cmd.argv, b->errlog, &rc, NULL) || rc != 0) return false;
  BenchArgv warm;
  benchToolCommand(b, t, BENCH_CMD_FULL, NULL, &warm);
  if (!benchRunArgv(t->ws, warm.argv, b->errlog, &rc, NULL) || rc != 0) return false;
  if (!benchRuntimeOk(b, t)) return false;

  char snap[BENCH_PATH_LEN];
  benchPathCat(snap, sizeof(snap), b->primedir, "/");
  benchPathCat(snap, sizeof(snap), snap, t->label);
  mkdirs(b->primedir);
  return benchCopyTree(t->ws, snap);
}

static bool benchPrimeSingle(Bench *b, BenchTool *t) {
  benchGenProject(b, t);
  int rc = 0;
  if (t->kind == BENCH_CMAKE || t->kind == BENCH_MESON) {
    BenchArgv conf;
    benchToolCommand(b, t, BENCH_CMD_CONFIGURE, NULL, &conf);
    if (!benchRunArgv(t->ws, conf.argv, b->errlog, &rc, NULL) || rc != 0) return false;
  }
  BenchArgv cmd;
  benchToolCommand(b, t, BENCH_CMD_FULL, NULL, &cmd);
  if (!benchRunArgv(t->ws, cmd.argv, b->errlog, &rc, NULL) || rc != 0) return false;
  BenchArgv warm;
  benchToolCommand(b, t, BENCH_CMD_FULL, NULL, &warm);
  if (!benchRunArgv(t->ws, warm.argv, b->errlog, &rc, NULL) || rc != 0) return false;
  if (!benchRuntimeOk(b, t)) return false;

  char snap[BENCH_PATH_LEN];
  benchPathCat(snap, sizeof(snap), b->primedir, "/");
  benchPathCat(snap, sizeof(snap), snap, t->label);
  mkdirs(b->primedir);
  return benchCopyTree(t->ws, snap);
}

static bool benchPrime(Bench *b, BenchTool *t) {
  return b->workspace ? benchPrimeWorkspace(b, t) : benchPrimeSingle(b, t);
}

/* Lantai gcc: objek disiapkan lewat make (seperti prime_tool make di
   compare.sh), lalu tiap run hanya compile 1 file + link. */
static bool benchFloorMake(Bench *b, BenchTool *t, bool warm) {
  char jbuf[32];
  snprintf(jbuf, sizeof(jbuf), "-j%d", b->jobs);
  char *argv[] = {(char *)"make", jbuf, NULL};
  int rc = 0;
  if (!benchRunArgv(t->ws, argv, b->errlog, &rc, NULL) || rc != 0) return false;
  if (warm && (!benchRunArgv(t->ws, argv, b->errlog, &rc, NULL) || rc != 0)) return false;
  return benchRuntimeOk(b, t);
}

static bool benchPrimeFloor(Bench *b) {
  BenchTool *t = &b->floor;
  benchGenProject(b, t);
  if (!benchFloorMake(b, t, true)) return false;
  char snap[BENCH_PATH_LEN];
  benchPathCat(snap, sizeof(snap), b->primedir, "/");
  benchPathCat(snap, sizeof(snap), snap, t->label);
  mkdirs(b->primedir);
  return benchCopyTree(t->ws, snap);
}

static bool benchSetupFloor(Bench *b) {
  benchGenProject(b, &b->floor);
  return benchFloorMake(b, &b->floor, false);
}

/* Setup tanpa snapshot: dipakai saat warmup=false dan `cold` tidak dipilih,
   supaya skenario incremental tetap punya state build awal. */
static bool benchSetupSingle(Bench *b, BenchTool *t) {
  benchGenProject(b, t);
  int rc = 0;
  if (t->kind == BENCH_CMAKE || t->kind == BENCH_MESON) {
    BenchArgv conf;
    benchToolCommand(b, t, BENCH_CMD_CONFIGURE, NULL, &conf);
    if (!benchRunArgv(t->ws, conf.argv, b->errlog, &rc, NULL) || rc != 0) return false;
  }
  BenchArgv cmd;
  benchToolCommand(b, t, BENCH_CMD_FULL, NULL, &cmd);
  return benchRunArgv(t->ws, cmd.argv, b->errlog, &rc, NULL) && rc == 0;
}

/* ---------- laporan ---------- */

typedef struct {
  const char *scen;
  const char *desc;
} BenchScenDesc;

static const BenchScenDesc kScenDescWs[] = {
  {"cold", "build dari nol, 3 proyek"},
  {"noop", "tanpa perubahan"},
  {"leaf", "edit 1 source core"},
  {"bade", "edit 1 source bade"},
  {"mod", "edit 1 .rp rupamod -> arsip -> codegen embed -> bade+rupa"},
  {"touch", "touch include/a.h pusat, konten sama"},
  {"hedit", "edit isi include/a.h pusat"},
  {"sel", "edit core+bade, cakupan selektif core (setara -w core)"},
  {"dep", "edit .rp, cakupan selektif bade (setara -w bade)"},
  {"libtouch", "touch include/ext.h (header internal), konten sama"},
  {"libhdr", "edit isi include/ext.h (header internal)"},
  {"rel", "release tar+sha256 (satria-only)"},
};

static const BenchScenDesc kScenDescSingle[] = {
  {"cold", "build dari nol (CMake/Meson termasuk configure)"},
  {"noop", "tanpa perubahan"},
  {"touch", "touch 1 header, konten sama"},
  {"edit", "edit 1 source (komentar) — satria boleh tanpa relink; gcc-floor terpisah"},
  {"editrel", "edit 1 source + fungsi baru — wajib relink; gcc-floor = lantai"},
  {"compdb", "compile_commands.json (bear = full clean build)"},
};

static const BenchScenDesc *benchScenList(const Bench *b, int *n) {
  if (b->workspace) {
    *n = (int)(sizeof(kScenDescWs) / sizeof(kScenDescWs[0]));
    return kScenDescWs;
  }
  *n = (int)(sizeof(kScenDescSingle) / sizeof(kScenDescSingle[0]));
  return kScenDescSingle;
}

static int benchCmpLong(const void *a, const void *b) {
  long x = *(const long *)a, y = *(const long *)b;
  return (x > y) - (x < y);
}

static void benchWriteCsv(const Bench *b) {
  if (!b->artifacts) return;
  FILE *f = fopen(b->csv, "wb");
  if (f) {
    fputs("label,scenario,ms,rc,obj,arc,lib,bin,cc,lnk\n", f);
    for (int i = 0; i < b->nrec; i++) {
      const BenchRec *r = &b->recs[i];
      fprintf(f, "%s,%s,%ld,%d,%d,%d,%d,%d,%d,%d\n", r->label, r->scen, r->ms, r->rc, r->obj,
              r->arc, r->lib, r->bin, r->cc, r->lnk);
    }
    fclose(f);
  }
  f = fopen(b->viewcsv, "wb");
  if (f) {
    fputs("label,scenario,rc,ms,obj,arc,lib,bin,cc,eobj,earc,elib,ebin,ecc,miss\n", f);
    for (int i = 0; i < b->nrec; i++) {
      const BenchRec *r = &b->recs[i];
      fprintf(f, "%s,%s,%d,%ld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", r->label, r->scen, r->rc,
              r->ms, r->obj, r->arc, r->lib, r->bin, r->cc, r->expObj, r->expArc, r->expLib,
              r->expBin, r->expCc, r->miss ? 1 : 0);
    }
    fclose(f);
  }
}

static long benchMedianFor(const Bench *b, const char *scen, const char *label, int *badOut) {
  long vals[256];
  int n = 0, bad = 0;
  for (int i = 0; i < b->nrec; i++) {
    const BenchRec *r = &b->recs[i];
    if (strcmp(r->scen, scen) != 0 || strcmp(r->label, label) != 0) continue;
    if (r->rc == 0) {
      if (n < (int)(sizeof(vals) / sizeof(vals[0]))) vals[n] = r->ms;
      n++;
    } else {
      bad++;
    }
  }
  if (badOut) *badOut = bad;
  int usable = n < (int)(sizeof(vals) / sizeof(vals[0])) ? n : (int)(sizeof(vals) / sizeof(vals[0]));
  if (usable == 0) return -1;
  qsort(vals, (size_t)usable, sizeof(long), benchCmpLong);
  return (usable % 2) ? vals[usable / 2] : (vals[usable / 2 - 1] + vals[usable / 2]) / 2;
}

/* Deskripsi biaya konfigurasi cold per jenis tool — dipakai pernyataan di
   skenario cold. Beda tool menyertakan biaya yang BEDA besar: ninja hanya
   membaca graph statis (murah), satria mem-parse Buildfile + menyusun
   startup state, cmake/meson menjalankan configure penuh (paling mahal).
   NULL bila tidak ada deskripsi. */
static const char *benchColdCost(BenchKind kind) {
  switch (kind) {
    case BENCH_NINJA: return "ninja: membaca graph statis (jauh lebih murah)";
    case BENCH_SATRIA: return "satria: parse Buildfile + startup state";
    case BENCH_CMAKE: return "cmake: configure penuh";
    case BENCH_MESON: return "meson: configure penuh";
    case BENCH_MAKE: return "make: membaca Makefile yang sudah jadi";
    case BENCH_BEAR: return "bear-make: membaca Makefile + merekam exec";
    case BENCH_XMAKE: return "xmake: konfigurasi ulang";
    case BENCH_TUP: return "tup: membaca database Tupfile";
    default: return NULL;
  }
}

static void benchReportMedian(const Bench *b) {
  printf("\nBENCHMARK REPORT (Median)\n");
  printf("-------------------------\n");
  int nscen = 0;
  const BenchScenDesc *descs = benchScenList(b, &nscen);
  for (int si = 0; si < nscen; si++) {
    const char *scen = descs[si].scen;
    long med[BENCH_MAX_TOOLS];
    int bad[BENCH_MAX_TOOLS];
    char labels[BENCH_MAX_TOOLS][BENCH_LABEL_LEN];
    BenchKind kinds[BENCH_MAX_TOOLS];
    int ntool = 0;
    bool any = false;
    for (int t = 0; t < b->ntools; t++) {
      int nbad = 0;
      long m = benchMedianFor(b, scen, b->tools[t].label, &nbad);
      if (m < 0 && nbad == 0) continue;
      any = true;
      int pos = ntool++;
      while (pos > 0 && med[pos - 1] > m) {
        med[pos] = med[pos - 1];
        bad[pos] = bad[pos - 1];
        copyStr(labels[pos], sizeof(labels[pos]), labels[pos - 1]);
        kinds[pos] = kinds[pos - 1];
        pos--;
      }
      med[pos] = m;
      bad[pos] = nbad;
      copyStr(labels[pos], sizeof(labels[pos]), b->tools[t].label);
      kinds[pos] = b->tools[t].kind;
    }
    if (!any) continue;
    printf("\n%s — %s\n", scen, descs[si].desc);
    long best = med[0] > 0 ? med[0] : 1;
    long mx = med[ntool - 1] > 0 ? med[ntool - 1] : 1;
    /* Tanda ★ hanya untuk pemenang tunggal; bila waktu teratas setara,
       tidak ada tool yang ditandai. */
    bool tie = med[0] >= 0 && ntool > 1 && med[1] == med[0];
    for (int k = 0; k < ntool; k++) {
      if (med[k] < 0) {
        printf("  ✗ %-11s semua run gagal\n", labels[k]);
        continue;
      }
      int width = (int)(med[k] * 16 / mx);
      if (width < 1) width = 1;
      printf("  %s %-11s ", (!tie && k == 0 && med[0] >= 0) ? "★" : " ", labels[k]);
      for (int j = 0; j < width; j++) fputs("█", stdout);
      printf(" %6ld ms %5.2fx%s\n", med[k], (double)med[k] / (double)best,
             bad[k] ? "  ✗" : "");
    }
    if (b->haveFloor &&
        (strcmp(scen, "edit") == 0 || strcmp(scen, "editrel") == 0)) {
      int fbad = 0;
      long fm = benchMedianFor(b, scen, b->floor.label, &fbad);
      if (fbad > 0) {
        printf("  ✗ %-11s semua run gagal\n", b->floor.label);
      } else if (fm >= 0) {
        if (strcmp(scen, "editrel") == 0)
          printf("  ┄ %-11s %6ld ms  lantai gcc -c + link langsung; selisih = overhead tool\n",
                 b->floor.label, fm);
        else
          printf("  ┄ lantai gcc diukur terpisah di sini (satria boleh no-relink); "
                 "lihat editrel\n");
      }
    }
    if (strcmp(scen, "cold") == 0 && ntool > 1) {
      /* Perbandingan cold tidak setara antar-tool: angka cold menyertakan
         biaya konfigurasi milik masing-masing tool, dan besarnya BEDA —
         graph ninja statis (murah), Buildfile satria dip-parse ulang,
         configure cmake/meson paling mahal. Jangan menyamakan biaya ini. */
      printf("  ┄ catatan: pembanding ");
      for (int k = 0; k < ntool; k++)
        printf("%s%s", k ? "," : "", labels[k]);
      printf(" tidak setara — tiap tool menyertakan biaya konfigurasi cold "
             "yang berbeda\n");
      bool seen[BENCH_TUP + 1] = {false};
      for (int k = 0; k < ntool; k++) {
        const char *cost = benchColdCost(kinds[k]);
        if (!cost || seen[kinds[k]]) continue;
        seen[kinds[k]] = true;
        printf("  ┄   %s\n", cost);
      }
    }
  }
}

static void benchValidationLog(const Bench *b) {
  printf("\n[Detailed Validation Log]\n\n");
  printf("%-11s | %-9s | %s\n", "tool", "execution", "status");
  int nscen = 0;
  const BenchScenDesc *descs = benchScenList(b, &nscen);
  for (int si = 0; si < nscen; si++) {
    const char *scen = descs[si].scen;
    for (int t = 0; t < b->ntools; t++) {
      bool seen = false, acc = true;
      for (int i = 0; i < b->nrec; i++) {
        const BenchRec *r = &b->recs[i];
        if (strcmp(r->scen, scen) != 0 || strcmp(r->label, b->tools[t].label) != 0) continue;
        seen = true;
        if (r->rc != 0 || r->miss) acc = false;
      }
      if (!seen) continue;
      printf("%-11s | %-9s | %s\n", b->tools[t].label, scen,
             acc ? "In accordance" : "Not in accordance");
    }
  }
  if (b->haveFloor) {
    for (int si = 0; si < nscen; si++) {
      const char *scen = descs[si].scen;
      if (strcmp(scen, "edit") != 0 && strcmp(scen, "editrel") != 0) continue;
      bool seen = false, acc = true;
      for (int i = 0; i < b->nrec; i++) {
        const BenchRec *r = &b->recs[i];
        if (strcmp(r->scen, scen) != 0 || strcmp(r->label, b->floor.label) != 0) continue;
        seen = true;
        if (r->rc != 0 || r->miss) acc = false;
      }
      if (seen)
        printf("%-11s | %-9s | %s\n", b->floor.label, scen,
               acc ? "In accordance" : "Not in accordance");
    }
  }
}

/* ---------- orkestrasi ---------- */

int cmdBench(const char *const argv[], int argc) {
  if (argc > 0 && strcmp(argv[0], "--embed-source") == 0)
    return benchEmbedSource(argv + 1, argc - 1);

  Bench b;
  benchDefaults(&b);
  char err[256];
  for (int i = 0; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--compare") == 0) {
      b.compare = true; /* tabel perbandingan; kedua mode */
    } else if (strcmp(a, "--workspace") == 0) {
      b.workspace = true;
    } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 || strcmp(a, "help") == 0) {
      benchUsage();
      return 0;
    } else if (strcmp(a, "-f") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "bench: -f butuh key=value\n");
        return 2;
      }
      if (!benchApplyOpt(&b, argv[++i], err, sizeof(err))) {
        fprintf(stderr, "bench: %s\n", err);
        return 2;
      }
    } else {
      fprintf(stderr, "bench: argumen tidak dikenal '%s' (lihat: satria bench --help)\n", a);
      return 2;
    }
  }
  /* Default tool bergantung mode: workspace hanya punya satria+CMake. */
  if (!b.toolsExplicit)
    copyStr(b.toolsSpec, sizeof(b.toolsSpec),
            b.workspace ? "satria,cmake"
                        : "satria,cmake,ninja,make,bear,meson,xmake,tup");

  /* self binary */
  ssize_t sl = readlink("/proc/self/exe", b.self, sizeof(b.self) - 1);
  if (sl > 0)
    b.self[sl] = '\0';
  else
    copyStr(b.self, sizeof(b.self), argv[0] ? argv[0] : "satria");

  if (!b.workdir[0]) copyStr(b.workdir, sizeof(b.workdir), "bench.tmp");
  mkdirs(b.workdir);
  char cwd[BENCH_PATH_LEN];
  if (fsGetCwd(cwd, sizeof(cwd))) {
    char abs[BENCH_PATH_LEN];
    if (b.workdir[0] != '/') {
      benchPathCat(abs, sizeof(abs), cwd, "/");
      benchPathCat(abs, sizeof(abs), abs, b.workdir);
      copyStr(b.workdir, sizeof(b.workdir), abs);
    }
  }
  benchPathCat(b.errlog, sizeof(b.errlog), b.workdir, "/stderr.log");
  benchPathCat(b.csv, sizeof(b.csv), b.workdir, "/results.csv");
  benchPathCat(b.viewcsv, sizeof(b.viewcsv), b.workdir, "/view.csv");
  benchPathCat(b.worklog, sizeof(b.worklog), b.workdir, "/work.log");
  benchPathCat(b.cclog, sizeof(b.cclog), b.workdir, "/.cc.log");
  benchPathCat(b.syscsv, sizeof(b.syscsv), b.workdir, "/workspace-syscalls.csv");
  benchPathCat(b.wrapdir, sizeof(b.wrapdir), b.workdir, "/.wrap");
  benchPathCat(b.primedir, sizeof(b.primedir), b.workdir, "/.prime");
  fsRemoveFile(b.errlog);
  fsRemoveFile(b.worklog);

  /* kandidat tool */
  char tools[BENCH_MAX_TOOLS][BENCH_PATH_LEN];
  int ntools = benchSplitList(b.toolsSpec, tools, BENCH_MAX_TOOLS);
  char bins[BENCH_MAX_TOOLS][BENCH_PATH_LEN];
  int nbins = benchSplitList(b.binsSpec, bins, BENCH_MAX_TOOLS);

  for (int i = 0; i < ntools; i++) {
    if (strcmp(tools[i], "satria") == 0) {
      if (nbins > 0) {
        for (int k = 0; k < nbins; k++)
          benchAddTool(&b, benchBase(bins[k]), BENCH_SATRIA, bins[k]);
      } else {
        benchAddTool(&b, "satria", BENCH_SATRIA, b.self);
      }
    } else {
      BenchKind kind;
      const char *label = NULL, *probe = tools[i];
      if (strcmp(tools[i], "cmake") == 0 || strcmp(tools[i], "cninja") == 0) {
        kind = BENCH_CMAKE;
        label = "cmake";
        probe = "cmake";
      } else if (strcmp(tools[i], "ninja") == 0) {
        kind = BENCH_NINJA;
        label = "ninja";
      } else if (strcmp(tools[i], "make") == 0) {
        kind = BENCH_MAKE;
        label = "make";
      } else if (strcmp(tools[i], "bear") == 0 || strcmp(tools[i], "bear-make") == 0) {
        kind = BENCH_BEAR;
        label = "bear-make";
        probe = "bear";
      } else if (strcmp(tools[i], "meson") == 0 || strcmp(tools[i], "meson-ninja") == 0) {
        kind = BENCH_MESON;
        label = "meson-ninja";
        probe = "meson";
      } else if (strcmp(tools[i], "xmake") == 0) {
        kind = BENCH_XMAKE;
        label = "xmake";
      } else if (strcmp(tools[i], "tup") == 0) {
        kind = BENCH_TUP;
        label = "tup";
      } else {
        fprintf(stderr,
                "bench: tool tidak dikenal '%s' (satria, cmake, ninja, make, bear,\n"
                "       meson, xmake, tup)\n",
                tools[i]);
        return 2;
      }
      if (b.workspace && kind != BENCH_CMAKE) {
        fprintf(stderr, "bench: tool '%s' belum didukung di mode --workspace\n", tools[i]);
        return 2;
      }
      if (!probeAvailable(probe)) {
        fprintf(stderr, "bench: %s tidak tersedia — dilewati\n", tools[i]);
        continue;
      }
      benchAddTool(&b, label, kind, NULL);
    }
  }
  if (b.ntools == 0) {
    fprintf(stderr, "bench: pilih minimal satu tool\n");
    return 2;
  }
  for (int i = 0; i < b.ntools; i++) {
    /* Tool dieksekusi dengan cwd = workspace: path binary relatif harus
       diabsolutkan lebih dulu. */
    if (b.tools[i].kind == BENCH_SATRIA && b.tools[i].bin[0] != '/') {
      char abs[BENCH_PATH_LEN];
      benchPathCat(abs, sizeof(abs), cwd, "/");
      benchPathCat(abs, sizeof(abs), abs, b.tools[i].bin);
      copyStr(b.tools[i].bin, sizeof(b.tools[i].bin), abs);
    }
    if (b.tools[i].kind == BENCH_SATRIA && !benchPathExists(b.tools[i].bin)) {
      fprintf(stderr, "bench: binary satria tidak ditemukan: %s\n", b.tools[i].bin);
      return 1;
    }
  }

  /* Lantai gcc hanya relevan untuk edit/editrel dan hanya di mode single. */
  b.haveFloor = !b.workspace &&
                (benchHaveScenario(&b, "edit") || benchHaveScenario(&b, "editrel"));
  if (b.haveFloor) {
    memset(&b.floor, 0, sizeof(b.floor));
    copyStr(b.floor.label, sizeof(b.floor.label), "gcc-floor");
    b.floor.kind = BENCH_FLOOR;
    char safe[BENCH_LABEL_LEN];
    benchSanitize(b.floor.label, safe, sizeof(safe));
    benchPathCat(b.floor.ws, sizeof(b.floor.ws), b.workdir, "/");
    benchPathCat(b.floor.ws, sizeof(b.floor.ws), b.floor.ws, safe);
    benchVersionTool(&b.floor);
  }

  /* shim gcc/ld */
  fsRemoveTree(b.wrapdir);
  mkdirs(b.wrapdir);
  char linkGcc[BENCH_PATH_LEN], linkLd[BENCH_PATH_LEN];
  benchPathCat(linkGcc, sizeof(linkGcc), b.wrapdir, "/gcc");
  benchPathCat(linkLd, sizeof(linkLd), b.wrapdir, "/ld");
  if (symlink(b.self, linkGcc) != 0 || symlink(b.self, linkLd) != 0) {
    fprintf(stderr, "bench: gagal membuat shim di %s: %s\n", b.wrapdir, strerror(errno));
    return 1;
  }
  const char *realGcc = getenv("RB_BENCH_REAL_GCC");
  const char *realLd = getenv("RB_BENCH_REAL_LD");
  if (!realGcc || !*realGcc) {
    /* cari gcc asli di PATH tanpa direktori wrap */
    const char *path = getenv("PATH");
    static char gccPath[BENCH_PATH_LEN], ldPath[BENCH_PATH_LEN];
    gccPath[0] = ldPath[0] = '\0';
    char tmp[BENCH_PATH_LEN * 2];
    copyStr(tmp, sizeof(tmp), path ? path : "");
    char *save = NULL;
    for (char *dir = strtok_r(tmp, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
      char cand[BENCH_PATH_LEN];
      snprintf(cand, sizeof(cand), "%s/gcc", dir);
      if (!gccPath[0] && access(cand, X_OK) == 0) copyStr(gccPath, sizeof(gccPath), cand);
      snprintf(cand, sizeof(cand), "%s/ld", dir);
      if (!ldPath[0] && access(cand, X_OK) == 0) copyStr(ldPath, sizeof(ldPath), cand);
    }
    if (!gccPath[0]) {
      fprintf(stderr, "bench: gcc tidak ditemukan di PATH\n");
      return 1;
    }
    realGcc = gccPath;
    if (!ldPath[0]) copyStr(ldPath, sizeof(ldPath), "/usr/bin/ld");
    realLd = ldPath;
  }
  setenv("RB_BENCH_REAL_GCC", realGcc, 1);
  setenv("RB_BENCH_REAL_LD", realLd, 1);
  setenv("RB_BENCH_LOG", b.cclog, 1);
  setenv("RB_BENCH_SHIM", "1", 1);
  {
    const char *old = getenv("PATH");
    char newPath[BENCH_PATH_LEN * 2];
    snprintf(newPath, sizeof(newPath), "%s:%s", b.wrapdir, old ? old : "");
    setenv("PATH", newPath, 1);
  }
  setenv("SATRIA_WS_PAR", "1", 1);
  /* Meson memilih compiler dari CC; samakan dengan `compiler =` di Buildfile. */
  if (!b.workspace && b.compilers[0] && !strchr(b.compilers, ','))
    setenv("CC", b.compilers, 1);

  /* versi */
  char verGcc[128] = "";
  for (int i = 0; i < b.ntools; i++) benchVersionTool(&b.tools[i]);
  benchVersionOf("gcc -dumpfullversion", verGcc, sizeof(verGcc));

  if (b.workspace) {
    printf("▌ satria workspace benchmark · %d tool · %d run · median\n", b.ntools, b.runs);
    printf("Workspace : rupamod (%d .rp) · core (%d .c) · bade (%d .c)\n", b.module,
           b.source, b.source);
  } else {
    printf("▌ satria benchmark · %d tool · %d run · median\n", b.ntools, b.runs);
    printf("Project   : %d source · %d header · gcc · gnu11\n", b.source, b.header);
  }
  printf("Directory : %s\n", b.workdir);
  printf("Versions  :\n");
  for (int i = 0; i < b.ntools; i++)
    printf("  %-11s : %s\n", b.tools[i].label, b.tools[i].version[0] ? b.tools[i].version : "?");
  if (b.haveFloor)
    printf("  %-11s : %s\n", b.floor.label, b.floor.version[0] ? b.floor.version : "?");
  printf("  %-11s : %s\n", "gcc", verGcc[0] ? verGcc : "?");

  /* strace */
  b.haveStrace = false;
  if (b.trace) {
    if (getenv("PROOT") || getenv("PROOT_L2S_DIR"))
      copyStr(b.straceSkip, sizeof(b.straceSkip), "proot terdeteksi");
    else if (!probeAvailable("strace"))
      copyStr(b.straceSkip, sizeof(b.straceSkip), "strace tidak tersedia");
    else
      b.haveStrace = true;
  }

  /* prime (warmup) */
  if (b.warmup) {
    printf("\n[Prime]\n");
    for (int i = 0; i < b.ntools; i++) {
      BenchTool *t = &b.tools[i];
      if (benchPrime(&b, t)) {
        t->primeOk = true;
        printf("  ✓ %-9s prime OK\n", t->label);
      } else {
        t->primeFail = true;
        printf("  ✗ %-9s prime gagal (lihat %s)\n", t->label, b.errlog);
      }
    }
    if (b.haveFloor) {
      if (benchPrimeFloor(&b)) {
        b.floor.primeOk = true;
        printf("  ✓ %-9s prime OK\n", b.floor.label);
      } else {
        b.floor.primeFail = true;
        printf("  ✗ %-9s prime gagal (lihat %s)\n", b.floor.label, b.errlog);
      }
    }
  }

  /* Tanpa warmup dan tanpa `cold`, skenario incremental butuh state build
     awal yang tidak dihitung (sama seperti prime_tool di compare.sh). */
  if (!b.workspace && !b.warmup && !benchHaveScenario(&b, "cold")) {
    for (int i = 0; i < b.ntools; i++)
      if (!benchSetupSingle(&b, &b.tools[i])) {
        b.tools[i].primeFail = true;
        printf("  ✗ %-8s setup awal gagal (lihat %s)\n", b.tools[i].label, b.errlog);
      }
    if (b.haveFloor && !benchSetupFloor(&b)) {
      b.floor.primeFail = true;
      printf("  ✗ %-8s setup awal gagal (lihat %s)\n", b.floor.label, b.errlog);
    }
  }

  printf("\n[Execution Stream]\n");

  /* ---- mode single project ---- */
  if (!b.workspace) {
    static const char *const order[] = {"cold",   "noop", "touch",
                                       "edit",   "editrel", "compdb"};
    for (size_t si = 0; si < sizeof(order) / sizeof(order[0]); si++) {
      const char *scen = order[si];
      if (!benchHaveScenario(&b, scen)) continue;
      for (int r = 1; r <= b.runs; r++) {
        bool reverse = b.rolling && b.ntools == 2 && (r % 2 == 0);
        for (int oi = 0; oi < b.ntools; oi++) {
          int i = reverse ? b.ntools - 1 - oi : oi;
          BenchTool *t = &b.tools[i];
          /* make/tup tidak punya compdb native; jangan ukur/dicatat sama sekali. */
          if (strcmp(scen, "compdb") == 0 &&
              (t->kind == BENCH_MAKE || t->kind == BENCH_TUP))
            continue;
          if (t->primeFail) {
            BenchRec *rec = benchNewRec(&b);
            if (rec) {
              copyStr(rec->label, sizeof(rec->label), t->label);
              copyStr(rec->scen, sizeof(rec->scen), scen);
              rec->rc = 95;
              rec->ms = -1;
              rec->fail = true;
              b.mismatchRuns++;
              benchExpect(&b, t, rec);
            }
            continue;
          }
          if (strcmp(scen, "compdb") == 0) {
            /* bear butuh build bersih untuk merekam compile yang benar-benar jalan. */
            if (t->kind == BENCH_BEAR) {
              benchGenProject(&b, t);
            } else {
              if (b.warmup && t->primeOk) {
                char snap[BENCH_PATH_LEN];
                benchPathCat(snap, sizeof(snap), b.primedir, "/");
                benchPathCat(snap, sizeof(snap), snap, t->label);
                benchCopyTree(snap, t->ws);
              }
              benchRemoveCompdb(t->ws);
            }
            if (t->kind == BENCH_SATRIA) benchBuildfileCompdb(t->ws, true);
            BenchArgv cmd;
            benchToolCommand(&b, t, BENCH_CMD_COMPDB, NULL, &cmd);
            benchRecordRun(&b, t, scen, NULL, &cmd, BENCH_CMD_COMPDB);
            if (t->kind == BENCH_SATRIA) benchBuildfileCompdb(t->ws, false);
            continue;
          }
          if (strcmp(scen, "cold") == 0) {
            benchGenProject(&b, t);
            BenchArgv cmd;
            benchToolCommand(&b, t, BENCH_CMD_FULL, NULL, &cmd);
            benchRecordRun(&b, t, scen, NULL, &cmd, BENCH_CMD_FULL);
            continue;
          }
          if (b.warmup && t->primeOk) {
            char snap[BENCH_PATH_LEN];
            benchPathCat(snap, sizeof(snap), b.primedir, "/");
            benchPathCat(snap, sizeof(snap), snap, t->label);
            benchCopyTree(snap, t->ws);
          }
          benchMutateSingle(&b, scen, r, t->label, t->ws);
          BenchArgv cmd;
          benchToolCommand(&b, t, BENCH_CMD_FULL, NULL, &cmd);
          benchRecordRun(&b, t, scen, NULL, &cmd, BENCH_CMD_FULL);
        }
      }

      /* Lantai gcc setelah semua tool pada skenario edit/editrel. */
      if (b.haveFloor && (strcmp(scen, "edit") == 0 || strcmp(scen, "editrel") == 0)) {
        BenchTool *f = &b.floor;
        for (int r = 1; r <= b.runs; r++) {
          if (f->primeFail) {
            BenchRec *rec = benchNewRec(&b);
            if (rec) {
              copyStr(rec->label, sizeof(rec->label), f->label);
              copyStr(rec->scen, sizeof(rec->scen), scen);
              rec->rc = 95;
              rec->ms = -1;
              rec->fail = true;
              b.mismatchRuns++;
              benchExpect(&b, f, rec);
            }
            continue;
          }
          if (b.warmup && f->primeOk) {
            char snap[BENCH_PATH_LEN];
            benchPathCat(snap, sizeof(snap), b.primedir, "/");
            benchPathCat(snap, sizeof(snap), snap, f->label);
            benchCopyTree(snap, f->ws);
          }
          benchMutateSingle(&b, scen, r, f->label, f->ws);
          BenchArgv cmd;
          benchToolCommand(&b, f, BENCH_CMD_FULL, NULL, &cmd);
          benchRecordRun(&b, f, scen, NULL, &cmd, BENCH_CMD_FLOOR);
        }
      }
    }
  } else {
    /* ---- mode workspace ---- */
    static const char *const scenOrder[] = {"cold", "noop", "leaf", "bade", "mod", "touch",
                                            "hedit", "sel", "dep", "libtouch", "libhdr", "rel"};
    for (size_t si = 0; si < sizeof(scenOrder) / sizeof(scenOrder[0]); si++) {
      const char *scen = scenOrder[si];
      if (!benchHaveScenario(&b, scen)) continue;
      if (strcmp(scen, "rel") == 0) {
        /* satria saja */
        for (int i = 0; i < b.ntools; i++) {
          BenchTool *t = &b.tools[i];
          if (t->kind != BENCH_SATRIA) continue;
          for (int r = 1; r <= b.runs; r++) {
            if (b.warmup) {
              char snap[BENCH_PATH_LEN];
              snprintf(snap, sizeof(snap), "%s/%s", b.primedir, t->label);
              if (t->primeOk) benchCopyTree(snap, t->ws);
            }
            char ver[32];
            snprintf(ver, sizeof(ver), "0.0.%d", r);
            BenchArgv cmd;
            benchToolCommand(&b, t, BENCH_CMD_REL, ver, &cmd);
            benchRecordRun(&b, t, scen, ver, &cmd, BENCH_CMD_REL);
          }
        }
        continue;
      }

      for (int r = 1; r <= b.runs; r++) {
        bool reverse = b.rolling && b.ntools == 2 && (r % 2 == 0);
        for (int oi = 0; oi < b.ntools; oi++) {
          int i = reverse ? b.ntools - 1 - oi : oi;
          BenchTool *t = &b.tools[i];
          if (t->primeFail) {
            BenchRec *rec = benchNewRec(&b);
            if (rec) {
              copyStr(rec->label, sizeof(rec->label), t->label);
              copyStr(rec->scen, sizeof(rec->scen), scen);
              rec->rc = 95;
              rec->ms = -1;
              rec->fail = true;
              b.mismatchRuns++;
              benchExpect(&b, t, rec);
            }
            continue;
          }

          if (strcmp(scen, "cold") == 0) {
            benchGenWorkspace(&b, t);
            BenchArgv cmd;
            benchToolCommand(&b, t, BENCH_CMD_FULL, NULL, &cmd);
            benchRecordRun(&b, t, scen, NULL, &cmd, BENCH_CMD_FULL);
            continue;
          }

          if (b.warmup && t->primeOk) {
            char snap[BENCH_PATH_LEN];
            benchPathCat(snap, sizeof(snap), b.primedir, "/");
            benchPathCat(snap, sizeof(snap), snap, t->label);
            benchCopyTree(snap, t->ws);
          }
          benchMutate(&b, scen, r, t->label, t->ws);
          int mode = BENCH_CMD_FULL;
          if (strcmp(scen, "sel") == 0) mode = BENCH_CMD_SEL;
          else if (strcmp(scen, "dep") == 0) mode = BENCH_CMD_DEP;
          BenchArgv cmd;
          benchToolCommand(&b, t, mode, NULL, &cmd);
          benchRecordRun(&b, t, scen, NULL, &cmd, mode);
        }
      }
    }
  } /* akhir mode workspace */

  /* strace (sekali per tool) */
  if (b.haveStrace && benchHaveScenario(&b, "strace")) {
    printf("\n[Trace]\n");
    FILE *sf = fopen(b.syscsv, "wb");
    if (sf) fputs("tool,scenario,run,rank,percent,seconds,usecs_per_call,calls,errors,syscall\n", sf);
    for (int i = 0; i < b.ntools; i++) {
      BenchTool *t = &b.tools[i];
      if (b.workspace) {
        benchResetWorkspace(t->ws);
      } else if (b.warmup && t->primeOk) {
        char snap[BENCH_PATH_LEN];
        benchPathCat(snap, sizeof(snap), b.primedir, "/");
        benchPathCat(snap, sizeof(snap), snap, t->label);
        benchCopyTree(snap, t->ws);
      } else {
        benchSetupSingle(&b, t);
      }
      char report[BENCH_PATH_LEN];
      benchPathCat(report, sizeof(report), b.workdir,
                   b.workspace ? "/workspace.strace." : "/single.strace.");
      benchPathCat(report, sizeof(report), report, t->label);
      benchPathCat(report, sizeof(report), report, ".txt");
      int rc = benchTraceTool(&b, t, report);
      if (rc != 0) {
        printf("  ✗ %-8s strace GAGAL\n", t->label);
        b.mismatchRuns++;
        continue;
      }
      FILE *rf = fopen(report, "rb");
      if (!rf) continue;
      char line[1024];
      long totalCalls = 0;
      double totalSec = 0;
      int rank = 0;
      while (fgets(line, sizeof(line), rf)) {
        /* Kolom strace -c: %time seconds usecs/call calls errors syscall.
           Baris `total` kadang tidak punya kolom errors (tanda `-`), jadi
           dipecah per token, bukan sscanf. */
        char *tok[8];
        int nt = 0;
        char tmp[sizeof(line)];
        copyStr(tmp, sizeof(tmp), line);
        for (char *t2 = strtok(tmp, " \t\r\n"); t2 && nt < 8; t2 = strtok(NULL, " \t\r\n")) tok[nt++] = t2;
        if (nt >= 4 && strcmp(tok[nt - 1], "total") == 0) {
          totalSec = strtod(tok[1], NULL);
          totalCalls = strtol(tok[3], NULL, 10);
          continue;
        }
        if (rank >= 10 || nt < 6) continue;
        if (tok[5][0] == '-' || !tok[5][0]) continue;
        double pct = strtod(tok[0], NULL), sec = strtod(tok[1], NULL);
        long usecs = strtol(tok[2], NULL, 10), calls = strtol(tok[3], NULL, 10);
        long errs = strcmp(tok[4], "-") == 0 ? 0 : strtol(tok[4], NULL, 10);
        if (calls <= 0) continue;
        rank++;
        if (sf)
          fprintf(sf, "%s,strace,1,%d,%.2f,%.6f,%ld,%ld,%ld,%s\n", t->label, rank, pct, sec,
                  usecs, calls, errs, tok[5]);
      }
      fclose(rf);
      printf("  ✓ %-8s strace total=%.2fs calls=%ld\n", t->label, totalSec, totalCalls);
    }
    if (sf) fclose(sf);
  }

  /* laporan */
  benchReportMedian(&b);
  benchValidationLog(&b);
  printf("\nSuccess Rate:\n");
  for (int t = 0; t < b.ntools; t++) {
    int ok = 0, total = 0;
    int nscen = 0;
    const BenchScenDesc *descs = benchScenList(&b, &nscen);
    for (int si = 0; si < nscen; si++) {
      bool seen = false, acc = true;
      for (int i = 0; i < b.nrec; i++) {
        const BenchRec *r = &b.recs[i];
        if (strcmp(r->scen, descs[si].scen) != 0) continue;
        if (strcmp(r->label, b.tools[t].label) != 0) continue;
        seen = true;
        if (r->rc != 0 || r->miss) acc = false;
      }
      if (seen) {
        total++;
        if (acc) ok++;
      }
    }
    if (total > 0)
      printf("  %-11s : %d/%d scenarios passed\n", b.tools[t].label, ok, total);
  }
  if (b.haveFloor) {
    int ok = 0, total = 0;
    const char *fs[] = {"edit", "editrel"};
    for (int k = 0; k < 2; k++) {
      bool seen = false, acc = true;
      for (int i = 0; i < b.nrec; i++) {
        const BenchRec *r = &b.recs[i];
        if (strcmp(r->scen, fs[k]) != 0 || strcmp(r->label, b.floor.label) != 0) continue;
        seen = true;
        if (r->rc != 0 || r->miss) acc = false;
      }
      if (seen) {
        total++;
        if (acc) ok++;
      }
    }
    if (total) printf("  %-11s : %d/%d scenarios passed\n", b.floor.label, ok, total);
  }
  if (b.trace) {
    if (b.haveStrace)
      printf("  strace   : diukur\n");
    else
      printf("  strace   : dilewati (%s)\n", b.straceSkip[0] ? b.straceSkip : "tidak tersedia");
  }

  benchWriteCsv(&b);
  if (b.artifacts) {
    printf("\nArtifacts :\n");
    printf("  hasil lengkap : %s\n", b.csv);
    printf("  view data     : %s\n", b.viewcsv);
    printf("  bukti kerja   : %s\n", b.worklog);
    if (b.haveStrace && benchPathExists(b.syscsv)) printf("  syscall detail: %s\n", b.syscsv);
    printf("  stderr tool   : %s\n", b.errlog);
  } else {
    fsRemoveFile(b.csv);
    fsRemoveFile(b.viewcsv);
    fsRemoveFile(b.worklog);
    fsRemoveFile(b.syscsv);
    fsRemoveFile(b.errlog);
  }

  if (!b.keep) {
    for (int i = 0; i < b.ntools; i++) fsRemoveTree(b.tools[i].ws);
    fsRemoveTree(b.primedir);
    fsRemoveTree(b.wrapdir);
  }

  free(b.recs);
  return (b.mismatchRuns == 0 && b.notAcc == 0) ? 0 : 1;
}

#endif /* !_WIN32 */

/*
 * nc_convert — inti konversi build.ninja -> Buildfile (format "use alias").
 *
 * Dipindah apa adanya dari ninjaconv.c: klasifikasi output edge (biner,
 * library, chain embedded generator->source->object), tiga pass pengumpulan
 * (klasifikasi non-compile, edge compile, token -l), dan emitter Buildfile.
 * Parser & ekspansi template ada di nc_parse.c / nc_expand.c.
 */
#include "nc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ninjaconv.h"
#include "../portability.h"

/* Root source terkecil ("src" bila semua di src/, "." bila campur). */
static void ncCommonRoot(const NcList *srcs, char *out, size_t n) {
  if (srcs->count == 0) {
    snprintf(out, n, "src");
    return;
  }
  char root[SATRIA_MAX_PATH];
  snprintf(root, sizeof(root), "%s", srcs->items[0]);
  char *slash = strrchr(root, '/');
  if (!slash) {
    snprintf(out, n, ".");
    return;
  }
  *slash = '\0';
  for (int i = 1; i < srcs->count; i++) {
    if (strncmp(srcs->items[i], root, strlen(root)) != 0 || srcs->items[i][strlen(root)] != '/') {
      snprintf(out, n, ".");
      return;
    }
  }
  snprintf(out, n, "%s", root);
}

/* ==================== hasil konversi terkumpul (dua pass) ==================== */

typedef struct {
  char libName[128];  /* dari lib/lib<name>.a / lib<name>.so / <name>.dll */
  bool libShared;     /* true bila varian shared yang cocok */
  bool libStatic;     /* true bila varian statis yang cocok */
} NcLibTarget;

/* Nama ada di daftar (dipakai klasifikasi library sistem per platform). */
static bool ncInList(const char *s, const char **list) {
  for (int k = 0; list[k]; k++)
    if (strcmp(s, list[k]) == 0) return true;
  return false;
}

/* Output biner EXE: "bin/rupa" / "satria.exe" -> "rupa" (isi buf), NULL bila
   bukan bentuk itu (arsip .tar.gz, library lib*.a/.so, file .c/.o, dst). */
static const char *ncBinBase(const char *out, char *buf, size_t n) {
  const char *slash = strrchr(out, '/');
  const char *base = slash ? slash + 1 : out;
  const char *dot = strrchr(base, '.');
  /* tanpa ekstensi = target biner biasa (POSIX); .exe (Windows) diizinkan */
  if (dot && strcmp(dot, ".exe") != 0) return NULL;
  if (strlen(base) >= n) return NULL;
  snprintf(buf, n, "%s", base);
  return buf;
}

/* Output library: "lib/librupa.so" -> "rupa" (bukan .exe/biner). */
static const char *ncLibBaseFromOut(const char *out) {
  const char *slash = strrchr(out, '/');
  const char *base = slash ? slash + 1 : out;
  if (strncmp(base, "lib", 3) != 0) return NULL; /* konvensi: lib<name>.<ext> */
  const char *dot = strrchr(base, '.');
  if (!dot || dot == base + 3) return NULL;
  if (strcmp(dot, ".so") != 0 && strcmp(dot, ".a") != 0 && strcmp(dot, ".dylib") != 0 &&
      strcmp(dot, ".dll") != 0 && strcmp(dot, ".lib") != 0)
    return NULL;
  return base + 3; /* panggilan salin sampai dot */
}

/* Kumpulkan token -l dari teks command/template; nilai $var digali satu
   tingkat (mis. libs = -lssl -lcrypto di build.ninja root). */
static void ncScanLibTokens(const NcFile *f, const char *s, int depth, NcList *out) {
  if (!s || depth > 4) return;
  for (const char *p = s; *p;) {
    if (p[0] == '$' && p[1] == '$') {
      p += 2;
      continue;
    }
    if (p[0] == '$' && (p[1] == '{' || ((p[1] >= 'A' && p[1] <= 'Z') ||
                                        (p[1] >= 'a' && p[1] <= 'z')))) {
      char name[64];
      size_t nl = 0;
      const char *q;
      if (p[1] == '{') {
        q = strchr(p + 2, '}');
        if (!q) break;
        nl = (size_t)(q - (p + 2));
        if (nl >= sizeof(name)) nl = sizeof(name) - 1;
        memcpy(name, p + 2, nl);
        name[nl] = '\0';
        p = q + 1;
      } else {
        q = p + 1;
        while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
               (*q >= '0' && *q <= '9') || *q == '_') {
          if (nl < sizeof(name) - 1) name[nl] = *q;
          nl++;
          q++;
        }
        name[nl] = '\0';
        p = q;
      }
      ncScanLibTokens(f, ncVarGet(f, name), depth + 1, out);
      continue;
    }
    if (p[0] == '-' && p[1] == 'l' && p[2] && p[2] != '-') {
      char name[64];
      size_t k = 0;
      const char *q = p + 2;
      while (*q && *q != ' ' && *q != '\t' && *q != '\n' && k < sizeof(name) - 1)
        name[k++] = *q++;
      name[k] = '\0';
      if (k) ncAdd(out, name);
      p = q;
      continue;
    }
    p++;
  }
}

/* Chain embedded generator: arsip (.tar/.gz) -> file sumber yang DIHASILKAN
   (.c) -> object-nya. satria punya primitif setara (embedded.<name>.src /
   .with.tar/.with.ext / .dir / .name) yang menghasilkan object yang sama,
   jadi chain seperti itu ditranslasi alih-alih dianggap source biasa. */
static bool ncLinkInHas(NcFile *f, const char *path);

static void ncDetectEmbedded(NcFile *f, int idx, NcList *embFiles) {
  NcEdge *arc = &f->edges[idx];
  const NcRule *rArc = ncRuleFind(f, arc->rule);
  if (!arc->in || !arc->out || !rArc || strstr(rArc->command, "-c")) return;

  /* Step 2: edge generator APAPUN yang mengonsumsi arsip (mis. rule embed
     yang menulis file .c). Generator biasanya bukan rule compile. */
  for (int j = 0; j < f->nedges; j++) {
    if (j == idx) continue;
    NcEdge *gen = &f->edges[j];
    if (!gen->in || !gen->out || strcmp(gen->in, arc->out) != 0) continue;

    /* Step 3: edge compile (-c) yang mengkompilasi file hasil generator;
       object-nya harus masuk link — baru layak dianggap chain embed. */
    for (int k = 0; k < f->nedges; k++) {
      if (k == idx || k == j) continue;
      NcEdge *comp = &f->edges[k];
      if (!comp->in || !comp->out || strcmp(comp->in, gen->out) != 0) continue;
      const NcRule *rComp = ncRuleFind(f, comp->rule);
      if (!rComp || !strstr(rComp->command, "-c")) continue;
      if (!ncLinkInHas(f, comp->out)) continue;

      /* Tandai SELURUH anggota chain via path (bukan pola nama): arsip &
         object. Object di-skip dari source; arsip untuk tahu nama entri. */
      ncAdd(embFiles, arc->out);
      ncAdd(embFiles, comp->out);
      return;
    }
  }
}

/* True bila edge compile ke-i menghasilkan object milik chain embedded
   (keanggotaan path eksak pada daftar hasil deteksi) — source-nya file
   hasil-generate, jadi tidak boleh masuk daftar source Buildfile. */
static bool ncEdgeInEmbeddedChain(NcFile *f, int idx, const NcList *embFiles) {
  NcEdge *e = &f->edges[idx];
  if (!e || !e->out || embFiles->count == 0) return false;
  for (int b = 0; b < embFiles->count; b++)
    if (strcmp(embFiles->items[b], e->out) == 0) return true;
  return false;
}

/* True bila path jadi input eksplisit salah satu edge link. */
static bool ncLinkInHas(NcFile *f, const char *path) {
  size_t pl = strlen(path);
  for (int i = 0; i < f->nedges; i++) {
    NcEdge *e = &f->edges[i];
    if (!e->ins || !e->out) continue;
    const NcRule *r = ncRuleFind(f, e->rule);
    if (!r || !r->command || strstr(r->command, "-c")) continue;
    for (const char *p = e->ins; (p = strstr(p, path)) != NULL; p++) {
      bool leftOk = p == e->ins || p[-1] == ' ' || p[-1] == '\t';
      char right = p[pl];
      bool rightOk = right == '\0' || right == ' ' || right == '\t';
      if (leftOk && rightOk) return true;
    }
  }
  return false;
}

bool ninjaToBuildfile(const char *ninjaPath, const char *outPath, char *err, size_t errCap) {
  char ninjaDir[SATRIA_MAX_PATH];
  snprintf(ninjaDir, sizeof(ninjaDir), "%s", ninjaPath);
  char *slash = strrchr(ninjaDir, '/');
  if (slash)
    *slash = '\0';
  else
    snprintf(ninjaDir, sizeof(ninjaDir), ".");

  NcFile f;
  memset(&f, 0, sizeof(f));
  if (!ncParseInto(&f, ninjaPath, ".", 0)) {
    if (err) snprintf(err, errCap, "cannot read %s", ninjaPath);
    return false;
  }

  NcList srcs = {0};
  NcList allFlags = {0};
  NcList allIncs = {0};
  NcList libraries = {0};
  NcList embFiles = {0}; /* path arsip & object milik chain embedded */
  char std[SATRIA_MAX_PATH * 4] = {0};
  char binName[128] = {0};
  NcLibTarget lib = {0};
  int converted = 0;

  /* ===== pass 1: klasifikasi edge non-compile — biner, library, chain
     embedded. Harus lebih dulu: source hasil-generate (& object-nya) baru
     bisa dikenali dan disisihkan dari daftar source di pass 2. ===== */
  for (int i = 0; i < f.nedges; i++) {
    NcEdge *e = &f.edges[i];
    if (!e->out) continue;
    const NcRule *r = ncRuleFind(&f, e->rule);
    if (!r || !r->command) continue;
    if (strstr(r->command, "-c")) continue; /* edge compile: urusan pass 2 */

    /* Output tanpa ekstensi (atau .exe) = link EXE -> kandidat biner utama
       (yang pertama menang). Edge tar/arsip tak lolos ncBinBase. */
    char binCand[128];
    if (ncBinBase(e->out, binCand, sizeof(binCand))) {
      if (!binName[0]) snprintf(binName, sizeof(binName), "%s", binCand);
      continue;
    }

    /* lib/lib<name>.a|.so|.dll|.dylib|.lib -> output.libraryName. */
    const char *libBase = ncLibBaseFromOut(e->out);
    if (libBase) {
      size_t bl2 = (size_t)(strrchr(libBase, '.') - libBase);
      if (!lib.libName[0] && bl2 < sizeof(lib.libName)) {
        memcpy(lib.libName, libBase, bl2);
        lib.libName[bl2] = '\0';
      }
      if (strstr(e->out, ".so") || strstr(e->out, ".dylib") || strstr(e->out, ".dll"))
        lib.libShared = true;
      else
        lib.libStatic = true;
      continue;
    }

    /* Rule non-compile lainnya: kandidat chain embedded (arsip -> file
       hasil-generate -> object masuk link). */
    ncDetectEmbedded(&f, i, &embFiles);
  }

  /* ===== pass 2: edge compile — kumpulkan source/flags/includes/std ===== */
  for (int i = 0; i < f.nedges; i++) {
    NcEdge *e = &f.edges[i];
    const NcRule *r = ncRuleFind(&f, e->rule);
    if (!r || !r->command || !e->in) continue;
    if (!strstr(r->command, "-c")) continue; /* link/arsip: bukan source */
    /* Source yang DIHASILKAN generator (mis. build/x.c dari arsip) & object
       embed-nya: tidak ada di tree source — ditangani embedded satria. */
    if (ncEdgeInEmbeddedChain(&f, i, &embFiles)) continue;

    /* include edge: path -I dibuat relatif sebelum substitusi */
    char *incsFixed = NULL;
    size_t incsLen = 0, incsCap = 0;
    bool incsOk = true;
    if (e->incs) {
      char *dup = ncStrndup(e->incs, e->incs + strlen(e->incs));
      if (!dup) incsOk = false;
      char *cur = dup, *tok;
      while (incsOk && (tok = ncNextTok(&cur)) != NULL) {
        char fixed[SATRIA_MAX_PATH * 2];
        ncFixIncludeTok(tok, ninjaDir, true, fixed, sizeof(fixed)); /* sudah pre-fix */
        if (incsLen && !ncAppendText(&incsFixed, &incsLen, &incsCap, " "))
          incsOk = false;
        if (incsOk && !ncAppendText(&incsFixed, &incsLen, &incsCap, fixed))
          incsOk = false;
        free(tok);
      }
      free(dup);
    }
    if (!incsOk) {
      free(incsFixed);
      continue;
    }
    if (!incsFixed) {
      incsFixed = calloc(1, 1);
      if (!incsFixed) continue;
    }

    char *cmd = ncExpand(&f, r->command, e->in, e->out, e->flags, incsFixed, e->defs,
                         e->depfile);
    free(incsFixed);
    if (!cmd) continue;

    NcCompile cc;
    if (ncParseCompile(cmd, &cc, ninjaDir)) {
      ncAdd(&srcs, cc.src);
      for (int k = 0; k < cc.flags.count; k++)
        ncAdd(&allFlags, cc.flags.items[k]);
      for (int k = 0; k < cc.includes.count; k++) {
        /* buang penanda \x01 dari include yang sudah di-pre-fix */
        const char *p = cc.includes.items[k];
        ncAdd(&allIncs, p[0] == '\x01' ? p + 1 : p);
      }
      if (!std[0] && cc.std[0]) snprintf(std, sizeof(std), "%s", cc.std);
      converted++;
      ncFree(&cc.flags);
      ncFree(&cc.includes);
    } else if (!binName[0]) {
      /* Template non-gcc yang gagal di-parse tapi output-nya bentuk biner
         (mis. "$(CC) ... -o out"): ambil nama target-nya sebagai biner. */
      const char *s2 = strrchr(e->out, '/');
      const char *b2 = s2 ? s2 + 1 : e->out;
      const char *dot2 = strrchr(b2, '.');
      if ((!dot2 || strcmp(dot2, ".exe") == 0) && strlen(b2) < sizeof(binName))
        snprintf(binName, sizeof(binName), "%s", b2);
    }
    free(cmd);
  }

  /* ===== pass 3: edge link & template rule — kumpulkan library (-l) ===== */
  for (int i = 0; i < f.nedges; i++) {
    NcEdge *e = &f.edges[i];
    const NcRule *r = ncRuleFind(&f, e->rule);
    if (!r || !r->command) continue;
    if (strstr(r->command, "-c")) continue;
    if (ncLibBaseFromOut(e->out)) continue; /* pembuatan library bukan link EXE */
    ncScanLibTokens(&f, r->command, 0, &libraries);
    if (e->flags) ncScanLibTokens(&f, e->flags, 0, &libraries);
  }

  if (converted == 0) {
    if (err) snprintf(err, errCap, "no compilable edges found in %s", ninjaPath);
    ncFileFree(&f);
    ncFree(&srcs);
    ncFree(&allFlags);
    ncFree(&allIncs);
    ncFree(&libraries);
    ncFree(&embFiles);
    return false;
  }

  FILE *out = fopen(outPath, "w");
  if (!out) {
    if (err) snprintf(err, errCap, "cannot create %s", outPath);
    ncFileFree(&f);
    ncFree(&srcs);
    ncFree(&allFlags);
    ncFree(&allIncs);
    ncFree(&libraries);
    ncFree(&embFiles);
    return false;
  }

  char root[SATRIA_MAX_PATH];
  ncCommonRoot(&srcs, root, sizeof(root));

  fprintf(out, "use alias\n\n");
  fprintf(out, "clean as c\noutput as o\n");
  if (libraries.count) fprintf(out, "library as lib\n");
  fprintf(out, "\n");
  fprintf(out, "# Dikonversi otomatis dari %s oleh `satria -xf`.\n\n", ninjaPath);
  fprintf(out, "root = .\n\n");
  if (strcmp(root, ".") != 0) {
    fprintf(out, "sources = %s\n\n", root);
  } else {
    fprintf(out, "sources =");
    for (int i = 0; i < srcs.count; i++)
      fprintf(out, "%s %s", i ? "," : "", srcs.items[i]);
    fprintf(out, "\n\n");
  }
  fprintf(out, "flags =");
  for (int i = 0; i < allFlags.count; i++)
    fprintf(out, "%s %s", i ? "," : "", allFlags.items[i]);
  fprintf(out, "%s MMD, MP # pelacakan header satria\n\n", allFlags.count ? "," : "");
  fprintf(out, "std = %s\n\n", std[0] ? std : "gnu11");
  if (allIncs.count) {
    fprintf(out, "headers =");
    for (int i = 0; i < allIncs.count; i++)
      fprintf(out, "%s %s", i ? "," : "", allIncs.items[i]);
    fprintf(out, "\n\n");
  }
  fprintf(out, "progress.bar = true\nprogress.error = always\n\n");
  fprintf(out, "o.binaryName = %s\n", binName[0] ? binName : "app");
  fprintf(out, "o.binaryDir = bin\n");
  fprintf(out, "o.buildDir = build\n");
  fprintf(out, "o.compileCommands = auto\n");

  if (lib.libName[0]) {
    fprintf(out, "\n# Library dari output lib/* pada build.ninja.\n");
    fprintf(out, "o.libraryName = %s\n", lib.libName);
    if (!lib.libStatic) fprintf(out, "o.libraryStatic = false\n");
    if (lib.libShared) fprintf(out, "o.libraryShared = true\n");
  }
  /* Library per platform: -l "sistem" (m, pthread, ...) masuk lib.linux/
     lib.macos; sisanya (ssl, crypto, ...) lib.default — semua platform.
     lib.windows tak bisa ditebak dari -l GNU: diisi bila ada kandidat
     umum, sisanya disunting manual. configFinalize menggabungkan default
     + platform sesuai host (urutan link tetap seperti build.ninja). */
  if (libraries.count) {
    static const char *kPortBoth[] = {"m", "dl", NULL};
    static const char *kPortLinux[] = {"pthread", "rt", "nsl", "resolv", NULL};
    fprintf(out, "\n# Library dari token -l pada build.ninja.\n");
    fprintf(out, "lib.default =");
    int ndef = 0, nlin = 0, nmac = 0;
    for (int i = 0; i < libraries.count; i++) {
      const char *l = libraries.items[i];
      bool mac = ncInList(l, kPortBoth);
      bool lin = mac || ncInList(l, kPortLinux);
      if (!lin) fprintf(out, "%s %s", ndef++ ? "," : "", l);
    }
    fprintf(out, "\nlib.linux =");
    for (int i = 0; i < libraries.count; i++) {
      const char *l = libraries.items[i];
      if (ncInList(l, kPortBoth) || ncInList(l, kPortLinux))
        fprintf(out, "%s %s", nlin++ ? "," : "", l);
    }
    fprintf(out, "\nlib.macos =");
    for (int i = 0; i < libraries.count; i++) {
      if (ncInList(libraries.items[i], kPortBoth))
        fprintf(out, "%s %s", nmac++ ? "," : "", libraries.items[i]);
    }
    fprintf(out, "\n# lib.windows perlu disunting manual (tak bisa ditebak dari -l GNU).\n");
  }

  /* Chain embedded: entri di embFiles berisi path arsip & object. Yang
     diproses hanya path arsip (ada edge non-compile dengan output itu);
     src = input rule arsip, nama entri = basis nama arsip tanpa ekstensi. */
  for (int b = 0; b < embFiles.count; b++) {
    const char *arcPath = embFiles.items[b];
    NcEdge *arc = NULL;
    for (int i = 0; i < f.nedges && !arc; i++) {
      NcEdge *e2 = &f.edges[i];
      if (!e2->out || !e2->in || strcmp(e2->out, arcPath) != 0) continue;
      const NcRule *r = e2->rule ? ncRuleFind(&f, e2->rule) : NULL;
      if (!r || !r->command || strstr(r->command, "-c")) continue;
      arc = e2;
    }
    if (!arc) continue; /* object chain — bukan arsip */

    const char *ab2 = strrchr(arc->out, '/');
    ab2 = ab2 ? ab2 + 1 : arc->out;
    char name[128];
    snprintf(name, sizeof(name), "%s", ab2);
    char *dot3 = strchr(name, '.');
    if (dot3) *dot3 = '\0';
    if (!name[0]) continue;

    fprintf(out, "\n# Embedded: dari chain %s pada build.ninja.\n", name);
    fprintf(out, "embedded.%s.src = %s\n", name, arc->in);
    /* Arsip di build/ — bukan di dalam src-nya sendiri (tar: "file changed
       as we read it" bila output ikut terbaca), dan ikut bersih saat
       `satria clean`. */
    fprintf(out, "embedded.%s.dir = build\n", name);
    fprintf(out, "embedded.%s.with = tar, gz\n", name);
  }

  fclose(out);

  printf("> Converted  : %s -> %s (%d compile edge, %d source)\n", ninjaPath, outPath, converted,
         srcs.count);
  ncFileFree(&f);
  ncFree(&srcs);
  ncFree(&allFlags);
  ncFree(&allIncs);
  ncFree(&libraries);
  ncFree(&embFiles);
  return true;
}

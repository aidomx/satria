/*
 * adhoc.c — eksekutor mode ad-hoc CLI (design/compile.md §2A/§2B, §5).
 *
 * Filosofi: wrapper gcc-floor terstruktur. Mode A (-o/-b) menerjemahkan
 * 1:1 ke perintah compiler minimal — tanpa flag yang tidak diminta, tanpa
 * state. Mode B (-F) memakai konfigurasi ringan untuk flags/headers/paths:
 * compile memakai compileCmd yang sama dengan build normal (minus state),
 * link meniru bentuk link cmds_passfinish (libraries, langCpp, target).
 *
 * Tidak ada penulisan .satria apa pun: loadConfigEx(cacheWrite=false),
 * tidak ada fingerprint/state/verdict. Kegagalan compiler mengalir ke
 * stderr apa adanya; adhocRun mengembalikan rc 1.
 */

#include "adhoc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootstrap.h"
#include "compile.h"
#include "config.h"
#include "portability.h"
#include "util.h"

/* ---------- util path ---------- */

/* "dir" + "p": p absolut diteruskan apa adanya; else "<dir>/<p>".
   dir kosong berarti cwd (p apa adanya). Keluaran ke buf. */
static void joinDir(char *buf, size_t n, const char *dir, const char *p) {
  if (!dir || !*dir || p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':')) {
    snprintf(buf, n, "%s", p);
    return;
  }
  snprintf(buf, n, "%s/%s", dir, p);
}

/* Ambil nama file dari path (tanpa direktori). */
static const char *baseName(const char *p) {
  const char *s1 = strrchr(p, '/');
  const char *s2 = strrchr(p, '\\');
  const char *s = s1 > s2 ? s1 : s2;
  return s ? s + 1 : p;
}

/* Nama objek dari sumber: "x/y.c" -> "y.o" (tanpa direktori). */
static void objBaseName(char *buf, size_t n, const char *src) {
  const char *b = baseName(src);
  const char *dot = strrchr(b, '.');
  size_t blen = dot ? (size_t)(dot - b) : strlen(b);
  snprintf(buf, n, "%.*s.o", (int)blen, b);
}

/* ---------- command builders (mode A: cermin gcc-floor) ---------- */

/* <cc> -c <src> -o <obj>  (MSVC: cl /nologo /c <src> /Fo<obj>) */
static char *adhocCcCmd(const Config *c, const char *src, const char *obj) {
  size_t n = strlen(c->cc) + strlen(src) + strlen(obj) + 40;
  char *cmd = malloc(n);
  if (compilerIsMSVC(c))
    snprintf(cmd, n, "%s /nologo /c %s /Fo%s", c->cc, src, obj);
  else
    snprintf(cmd, n, "%s -c %s -o %s", c->cc, src, obj);
  return cmd;
}

/* <cc> <objs...> -o <bin>  — libraries mengikuti konvensi Buildfile
   ("ssl"/"-lssl" -> -lssl; path langsung diteruskan). */
static char *adhocLinkCmd(const Config *c, char *const *objs, int nobjs, const char *bin) {
  size_t n = strlen(c->cc) + strlen(bin) + 40;
  for (int i = 0; i < nobjs; i++) n += strlen(objs[i]) + 2;
  for (int i = 0; i < c->libraries.count; i++) n += strlen(c->libraries.items[i]) + 8;
  char *cmd = malloc(n);
  if (compilerIsMSVC(c)) {
    strcpy(cmd, c->cc);
    strcat(cmd, " /nologo");
    for (int i = 0; i < nobjs; i++) {
      strcat(cmd, " ");
      strcat(cmd, objs[i]);
    }
    strcat(cmd, " /Fe:");
    strcat(cmd, bin);
    return cmd;
  }
  strcpy(cmd, c->cc);
  for (int i = 0; i < nobjs; i++) {
    strcat(cmd, " ");
    strcat(cmd, objs[i]);
  }
  strcat(cmd, " -o ");
  strcat(cmd, bin);
  /* Driver C + sumber C++: runtime libstdc++ ditambahkan manual (sama
     dengan link normal); driver C++ sudah membawanya sendiri. */
  bool cpp = false;
  for (int i = 0; i < nobjs; i++) {
    size_t l = strlen(objs[i]);
    cpp = cpp || (l >= 4 && strcmp(objs[i] + l - 4, ".cpp") == 0);
  }
  if (cpp && !compilerIsCppDriver(c)) strcat(cmd, " -lstdc++");
  for (int i = 0; i < c->libraries.count; i++) {
    const char *lib = c->libraries.items[i];
    strcat(cmd, " ");
    if (strchr(lib, '/') || strchr(lib, '\\')) {
      strcat(cmd, lib); /* path artefak: teruskan apa adanya */
    } else {
      if (lib[0] == '-' || lib[0] == 'l')
        strcat(cmd, "-");
      else
        strcat(cmd, "-l");
      strcat(cmd, lib);
    }
  }
  return cmd;
}

/* ---------- eksekusi ---------- */

/* Satu slot kompilasi paralel: sumber, waktu mulai, objeknya, indeks sumber. */
typedef struct {
  int idx; /* indeks sumber — mengikuti kompaksi slot */
  char obj[SATRIA_MAX_PATH + 260];
  double start;
} AdhocSlot;

/* Kompilasi SERIAL — cermin gcc-floor, satu command per sumber. */
static bool compileListSerial(const Config *c, const AdhocSlot *slots, const char *const *srcs,
                              int n, bool useConfigFlags) {
  for (int i = 0; i < n; i++) {
    double t0 = portMonoMs();
    printf("[ %d/%d] cc   %s\n", i + 1, n, srcs[i]);
    fflush(stdout);
    mkparent(slots[i].obj); /* build/, bin/, path -o ber-direktori */
    char *cmd;
    if (useConfigFlags) {
      char *inc = includeFlags(c);
      char *wf = warningFlags(c); /* gabungan `flags =` konfigurasi */
      cmd = compileCmd(c, inc, wf, srcs[i], slots[i].obj);
      free(inc);
      free(wf);
    } else {
      cmd = adhocCcCmd(c, srcs[i], slots[i].obj);
    }
    bool ok = runCmd(cmd);
    free(cmd);
    if (!ok) {
      fprintf(stderr, "satria: kompilasi %s gagal\n", srcs[i]);
      return false;
    }
    printf("       %5.0f ms  %s\n", portMonoMs() - t0, slots[i].obj);
  }
  return true;
}

/* Kompilasi PARALEL (-jN, pola cmdsRunParallelJobs): procStartCapture per
   job — output compiler (warning/error) ditahan di file sementara dan
   dicetak UTUH saat job selesai, urutan terjaga, blok tidak tercampur job
   lain (anti-interleaving). Gagal/ctrl+C: berhenti meluncurkan, tunggu
   yang aktif, kembalikan false. */
static bool compileListParallel(const Config *c, AdhocSlot *slots, const char *const *srcs, int n,
                                int jobs, bool useConfigFlags) {
  ProcHandle *handles = calloc((size_t)jobs, sizeof(ProcHandle));
  if (!handles) return false;
  int next = 0, active = 0, done = 0, failed = 0;

  while (next < n || active > 0) {
    while (active < jobs && next < n && !procInterrupted()) {
      mkparent(slots[next].obj);
      char *cmd;
      if (useConfigFlags) {
        char *inc = includeFlags(c);
        char *wf = warningFlags(c);
        cmd = compileCmd(c, inc, wf, srcs[next], slots[next].obj);
        free(inc);
        free(wf);
      } else {
        cmd = adhocCcCmd(c, srcs[next], slots[next].obj);
      }
      bool started = procStartCapture(cmd, &handles[active]);
      free(cmd);
      if (!started) {
        fprintf(stderr, "satria: tidak bisa memulai compiler untuk %s\n", srcs[next]);
        slots[next].idx = -1; /* tandai gagal mulai: dihitung failed di bawah */
        failed++;
        next++;
        continue;
      }
      slots[next].start = portMonoMs();
      slots[next].idx = next;
      next++;
      active++;
    }
    if (active == 0) break;

    ProcHandle *fin = NULL;
    int w = procWaitAny(handles, active, &fin);
    if (w < 0) break; /* tidak ada job tersisa / Ctrl+C */

    AdhocSlot *s = &slots[w]; /* slot paralel disimpan sejajar handle */
    done++;
    printf("[%3d/%3d] %-4s %5.0f ms  %s\n", done, n, fin->ok ? "OK" : "FAIL",
           portMonoMs() - s->start, srcs[s->idx]);
    fflush(stdout);
    {
      size_t blen = 0;
      char *out = procCaptureTake(fin, &blen);
      if (out && blen) fwrite(out, 1, blen, stdout);
      free(out);
    }
    if (!fin->ok) failed++;

    /* kompaksi: slot & handle terakhir mengisi tempat yang kosong */
    handles[w] = handles[active - 1];
    slots[w] = slots[active - 1];
    active--;
  }

  for (int i = 0; i < active; i++) {
    size_t blen = 0;
    free(procCaptureTake(&handles[i], &blen)); /* buang tangkapan tersisa */
  }
  free(handles);
  return failed == 0;
}

static bool compileList(const Config *c, AdhocSlot *slots, const char *const *srcs, int n,
                        int jobs, bool useConfigFlags) {
  if (jobs > 1 && n > 1) return compileListParallel(c, slots, srcs, n, jobs, useConfigFlags);
  return compileListSerial(c, slots, srcs, n, useConfigFlags);
}

int adhocRun(const Bootstrap *bs) {
  Config c;
  const char *cfgDir = ""; /* direktori file -F untuk path relatif */

  if (bs->fconfig) {
    /* Mode B: konfigurasi ringan. cacheWrite=false -> .satria tidak
       tersentuh (design §6: parser aman melewatkan blok tak relevan). */
    if (!loadConfigEx(&c, bs->fconfig, false)) {
      fprintf(stderr, "satria: gagal membaca konfigurasi '%s'\n", bs->fconfig);
      return 1;
    }
    static char dirbuf[SATRIA_MAX_PATH];
    snprintf(dirbuf, sizeof(dirbuf), "%s", bs->fconfig);
    char *slash = strrchr(dirbuf, '/');
    if (slash) {
      *slash = '\0';
      cfgDir = dirbuf;
    }
  } else {
    /* Mode A: tanpa konfigurasi sama sekali — deteksi compiler urutan
       design §5.4: cc, gcc, clang, tcc, cl (Windows: cl dulu). */
    c = configDefaults();
    c.std[0] = '\0';
#ifdef _WIN32
    listAdd(&c.compilers, "cl");
    listAdd(&c.compilers, "cc");
    listAdd(&c.compilers, "gcc");
    listAdd(&c.compilers, "clang");
#else
    listAdd(&c.compilers, "cc");
    listAdd(&c.compilers, "gcc");
    listAdd(&c.compilers, "clang");
    listAdd(&c.compilers, "tcc");
    listAdd(&c.compilers, "cl");
#endif
  }

  if (!resolveCompiler(&c)) return 1; /* pesan dari resolveCompiler */

  int n = bs->source_count;
  char objDir[SATRIA_MAX_PATH];
  char binPath[SATRIA_MAX_PATH * 2];

  if (bs->fconfig) {
    /* Keputusan -F: compile + link. Objek -> o.buildDir/<base>.o;
       binary -> o.binaryDir/o.binaryName. Path relatif konfigurasi
       di-resolve terhadap direktori file konfigurasi (design §6). */
    joinDir(objDir, sizeof(objDir), cfgDir, c.outBuildDir[0] ? c.outBuildDir : "build");
    joinDir(binPath, sizeof(binPath), cfgDir, c.outBinaryDir[0] ? c.outBinaryDir : "bin");
    size_t bl = strlen(binPath);
    snprintf(binPath + bl, sizeof(binPath) - bl, "/%s",
             c.outBinaryName[0] ? c.outBinaryName : "a.out");
  } else {
    /* Mode A: -o = path objek penuh (satu sumber) ATAU direktori objek
       (multi-sumber) — keputusan "cermin gcc". */
    snprintf(objDir, sizeof(objDir), "%s", bs->objOut);
    snprintf(binPath, sizeof(binPath), "%s", bs->binOut ? bs->binOut : "");
  }

  /* Objek per sumber. baseName + ".o" <= 255; objDir <= SATRIA_MAX_PATH
     (joinDir memotung aman) — gabungan selalu muat di buffer slot. */
  int slotCap = n > 0 ? n : 1;
  AdhocSlot *slots = malloc((size_t)slotCap * sizeof(AdhocSlot));
  for (int i = 0; i < n; i++) {
    slots[i].idx = i;
    slots[i].start = 0;
    if (bs->fconfig || n > 1) {
      char base[256];
      objBaseName(base, sizeof(base), bs->sources[i]);
      snprintf(slots[i].obj, sizeof(slots[i].obj), "%s/%s", objDir, base);
    } else {
      snprintf(slots[i].obj, sizeof(slots[i].obj), "%s", objDir);
    }
  }
  char **objs = malloc((size_t)slotCap * sizeof(char *));
  for (int i = 0; i < n; i++) objs[i] = strdup(slots[i].obj);
  /* objs salinan mandiri: compileListParallel mengkompaksi slots[] saat
     job selesai — pointer ke slots[i].obj akan tertukar pasangan objeknya. */

  const bool link = bs->fconfig || bs->binOut;
  if (link) mkparent(binPath);

  /* -jN (opsi global -j, tambahan design/compile.md 15/11): kompilasi
     paralel pola build normal; tanpa -j / -j1 tetap serial. */
  int jobs = bs->jobs > 1 ? bs->jobs : 1;
  if (!compileList(&c, slots, bs->sources, n, jobs, !!bs->fconfig)) {
    for (int i = 0; i < n; i++) free(objs[i]);
    free(objs);
    free(slots);
    return 1;
  }

  if (link) {
    printf("Linking : %s\n", binPath);
    fflush(stdout);
    char *cmd = adhocLinkCmd(&c, objs, n, binPath);
    bool ok = runCmd(cmd);
    if (!ok) fprintf(stderr, "satria: link command: %s\n", cmd);
    free(cmd);
    if (!ok) {
      for (int i = 0; i < n; i++) free(objs[i]);
      free(objs);
      free(slots);
      return 1;
    }
  }

  printf("Status    : Success\n");
  for (int i = 0; i < n; i++) free(objs[i]);
  free(objs);
  free(slots);
  return 0;
}

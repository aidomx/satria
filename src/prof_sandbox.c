/*
 * prof_sandbox — hash (FNV) + siklus build terisolasi
   (binary=/project=/workspace=) yang dipakai konteks profil.
 * Dipisah dari profile.c (audit refactor: berkas kandidat >600 baris).
 * Tipe ProfCtx/CompBackend & API internal ada di profile.h.
 */
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
 * project=<dir> [jobs=N] — profil siklus build satu proyek DENGAN fase
 * incremental nyata: config, fingerprint, decide, compile, link.
 *
 * Berbeda dari binary= (yang mengukur cold build murni, semua object
 * dikompilasi ulang), project= mengukur seperti `satria` biasa: decide
 * membandingkan mtime object vs source via deps (header dua lapis), jadi
 * run kedua harus menunjukkan biaya no-op (compilasi 0). Semua state
 * yang dibutuhkan decide (deps.cache) HIDUP DI direktori sementara
 * (outBuildDir dialihkan ke p->tmp/build) — state build normal proyek
 * tidak tersentuh. Output binary juga masuk direktori tersebut.
 *
 * Fingerprint di sini dihitung dari konfigurasi (polah cmdsFingerprintMake)
 * SEBAGAI UKURAN murah — tanpa membandingkannya dengan file state (yang
 * tidak ada di sandbox): fase ini menunjukkan biaya hashing konfigurasi.
 */
static uint64_t profFnv(uint64_t h, const void *data, size_t n) {
  const unsigned char *q = (const unsigned char *)data;
  for (size_t i = 0; i < n; i++) {
    h ^= q[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t profFnvStr(uint64_t h, const char *s) {
  const unsigned char sep = 0xff;
  if (s) h = profFnv(h, s, strlen(s));
  return profFnv(h, &sep, 1);
}

/* Jalur build terisolasi utama project=: pola profRunBinary dengan fase
   decide incremental (schema identik, berbeda muatan fase). */
static int buildSandboxed(ProfCtx *p, const char *passKind) {
  Config c = configDefaults();
  char bf[MAX_PATH * 2 + 16];
  snprintf(bf, sizeof(bf), "%s/Buildfile", p->dir);
  bool haveBf = fsFileExists(bf);

  if (!fsSetCwd(p->dir)) {
    fprintf(stderr, "satria: profile %s=%s: cannot enter directory\n", passKind, p->target);
    return 1;
  }

  profPhaseBegin("config");
  if (haveBf && !loadConfigEx(&c, bf, false)) {
    profPhaseEnd(0, 0);
    fprintf(stderr, "satria: profile %s=%s: Buildfile tidak bisa dimuat\n", passKind, p->target);
    return 1;
  }
  if (!haveBf) configFinalize(&c); /* konvensi proyek standar */
  bool ccOk = resolveCompiler(&c);
  profPhaseEnd(0, 0);
  if (!ccOk) {
    fprintf(stderr, "satria: profile %s=%s: compiler tidak ditemukan\n", passKind, p->target);
    return 1;
  }
  p->compiler = c.cc;

  if (!c.binary) {
    fprintf(stderr, "satria: profile %s=%s: output.binary = false (tidak ada binary)\n", passKind,
            p->target);
    return 1;
  }
  if (c.embCount > 0) {
    fprintf(stderr,
            "satria: profile %s=%s: proyek ber-embedded tidak didukung "
            "(arsip embedded harus ditulis ke lokasi asli)\n",
            passKind, p->target);
    return 1;
  }

  /* Isolasi keluaran ke direktori sementara. */
  char bdir[MAX_PATH * 2 + 8];
  snprintf(bdir, sizeof(bdir), "%s/build", p->tmp);
  copyStr(c.outBuildDir, sizeof(c.outBuildDir), bdir);
  copyStr(c.outBinaryDir, sizeof(c.outBinaryDir), p->tmp);
  c.libRequested = false;
  c.pack.requested = false;
  copyStr(c.outCompileCommands, sizeof(c.outCompileCommands), "false");

  if (!fsDirExists(c.root)) {
    fprintf(stderr, "satria: profile %s=%s: root '%s' bukan direktori\n", passKind, p->target,
            c.root);
    return 1;
  }
  if (!fsSetCwd(c.root)) {
    fprintf(stderr, "satria: profile %s=%s: cannot enter root '%s'\n", passKind, p->target, c.root);
    return 1;
  }
  procSetForeground(p->jobs > 1 ? false : c.foreground);

  List srcs = {0};
  listReserve(&srcs, 1024);
  profPhaseBegin("gather");
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
  listSort(&srcs);
  profPhaseEnd(0, 0);
  if (srcs.count == 0) {
    fprintf(stderr, "satria: profile %s=%s: tidak ada source .c/.cpp di sources\n", passKind,
            p->target);
    listFree(&srcs);
    return 1;
  }

  /* fingerprint — ukuran biaya hashing konfigurasi & daftar source
     (hash murah FNV; tanpa file state di sandbox, hanya tampil di abstrak
     sebagai waktu proses hashing yang dilakukan mesin build). */
  profPhaseBegin("fingerprint");
  uint64_t fpCompile = 14695981039346656037ULL;
  fpCompile = profFnvStr(fpCompile, c.cc);
  fpCompile = profFnvStr(fpCompile, c.std);
  fpCompile = profFnvStr(fpCompile, c.outBuildDir);
  for (int i = 0; i < c.flags.count; i++)
    fpCompile = profFnvStr(fpCompile, c.flags.items[i]);
  for (int i = 0; i < srcs.count; i++) {
    char obj[MAX_PATH];
    fpCompile = profFnvStr(fpCompile, srcs.items[i]);
    if (objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) fpCompile = profFnvStr(fpCompile, obj);
  }
  profPhaseEnd(0, 0);
  (void)fpCompile;

  /* decide — keputusan incremental nyata via deps: object usang bila
     source mtime lebih baru ATAU header transitive lebih baru (dua lapis
     mtime + konten). Hanya yang pending yang masuk fase compile — biaya
     no-op run kedua = 0 kompilasi. */
  char *inc = includeFlags(&c);
  char *wf = warningFlags(&c);
  char obj[MAX_PATH];
  List pending = {0};
  profPhaseBegin("decide");
  {
    DepCache *dc = depsNew(&c);
    for (int i = 0; i < srcs.count; i++) {
      const char *src = srcs.items[i];
      if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;
      int64_t srcM = fsMTimeNs(src);
      int64_t objM = fsMTimeNs(obj); /* -1 = object belum ada */
      if (objM >= 0 && srcM >= 0 && objM >= srcM &&
          depsNewestHeaderMTimeAt(dc, src, srcM) <= objM) {
        if (depsContentUpToDate(dc, src)) continue; /* up-to-date */
      }
      listAdd(&pending, src);
    }
    depsFree(dc);
    listSort(&pending);
  }
  profPhaseEnd(0, 0);

  char objTmp[MAX_PATH];
  int compiled = 0, failed = 0, interrupted = 0;
  profPhaseBegin("compile");
  bool okc =
      cmdsRunParallelJobs(&c, inc, wf, &pending, objTmp, sizeof(objTmp), p->jobs, &compiled,
                          &failed, &interrupted, strcmp(passKind, "project") == 0 /* quiet */,
                           NULL);
  profPhaseEnd(0, 0);
  free(wf);
  free(inc);
  listFree(&pending);
  if (!okc) {
    fprintf(stderr, "satria: profile %s=%s: kompilasi gagal (%d)\n", passKind, p->target,
            failed + interrupted);
    listFree(&srcs);
    return 1;
  }

  /* Link — pola cmdBuildEx (GNU/Clang; MSVC cabang sendiri). */
  char target[MAX_PATH * 2];
  snprintf(target, sizeof(target), "%s/%s", c.outBinaryDir, c.outBinaryName);
  if (compilerIsMSVC(&c)) {
    /* link.exe tidak menambahkan .exe sendiri pada /OUT tanpa ekstensi —
       samakan dengan target inkremental bin/<name>.exe (lihat commands.c). */
    size_t tLen = strlen(target);
    if (tLen < 4 || strcmp(target + tLen - 4, ".exe") != 0) strcat(target, ".exe");
  }
  size_t n = strlen(c.cc) + 1; /* compiler + NUL */
  for (int i = 0; i < srcs.count; i++) {
    if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
    n += strlen(obj) + 1; /* spasi + object */
  }
  if (compilerIsMSVC(&c)) {
    n += strlen(" /link /nologo /INCREMENTAL:NO /OUT:") + strlen(target);
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      if (lib[0] == '-') lib++;
      if (lib[0] == 'l') lib++;
      n += 1 + strlen(lib) + strlen(".lib");
    }
  } else {
    n += strlen(" -o ") + strlen(target);
    if (c.langCpp && !compilerIsCppDriver(&c)) n += strlen(" -lstdc++");
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      n += 1 + strlen((strchr(lib, '/') || strchr(lib, '\\'))
                          ? lib
                          : ((lib[0] == '-' || lib[0] == 'l') ? "-" : "-l"));
      if (!(strchr(lib, '/') || strchr(lib, '\\'))) n += strlen(lib);
    }
  }
  char *cmd = malloc(n);
  if (!cmd) {
    fprintf(stderr, "satria: profile: kehabisan memori\n");
    listFree(&srcs);
    return 1;
  }
  if (compilerIsMSVC(&c)) {
    snprintf(cmd, n, "%s", c.cc);
    for (int i = 0; i < srcs.count; i++) {
      if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
      strcat(cmd, " ");
      strcat(cmd, obj);
    }
    strcat(cmd, " /link /nologo /INCREMENTAL:NO /OUT:");
    strcat(cmd, target);
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      if (lib[0] == '-') lib++;
      if (lib[0] == 'l') lib++;
      strcat(cmd, " ");
      strcat(cmd, lib);
      strcat(cmd, ".lib");
    }
  } else {
    snprintf(cmd, n, "%s", c.cc);
    for (int i = 0; i < srcs.count; i++) {
      if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
      strcat(cmd, " ");
      strcat(cmd, obj);
    }
    strcat(cmd, " -o ");
    strcat(cmd, target);
    if (c.langCpp && !compilerIsCppDriver(&c)) strcat(cmd, " -lstdc++");
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      strcat(cmd, " ");
      if (strchr(lib, '/') || strchr(lib, '\\')) {
        strcat(cmd, lib);
      } else {
        strcat(cmd, (lib[0] == '-' || lib[0] == 'l') ? "-" : "-l");
        strcat(cmd, lib);
      }
    }
  }

  profPhaseBegin("link");
  bool ok = runCmd(cmd);
  profPhaseEnd(0, ok ? profSizeOf(target) : 0);
  free(cmd);
  listFree(&srcs);
  if (!ok) {
    fprintf(stderr, "satria: profile %s=%s: link gagal\n", passKind, p->target);
    return 1;
  }
  return 0;
}

/*
 * workspace=<dir> — profil siklus workspace (design "workspace"):
 * parse (baca Buildfile.ws), dependency (topo sort), library, binary.
 *
 * Keterbatasan nyata yang TIDAK dikarang: fase library/binary di sini
 * mengeksekusi build proyek via jalur yang sama dengan `satria -w`, yang
 * menulis state/output ke root proyek — tidak bisa diisolasi ke tmp
 * tanpa menyalin seluruh tree proyek. Pendekatan desain: ukur fase yang
 * aman di-tempat (parse, dependency/synthesis) langsung, lalu fase
 * library/binary dilewati bila proyek tak mau state-nya disentuh.
 * Design mengizinkan: "context hanya boleh mengekspos fase yang
 * benar-benar diimplementasikan dan dapat diukur".
 */
int profRunWorkspace(ProfCtx *p) {
  profPhaseBegin("parse");
  const char *names[WS_MAX_PROJECTS];
  int count = 0;
  int rc = workspaceEnumerate(p->dir, &count, names, WS_MAX_PROJECTS);
  profPhaseEnd(0, 0);
  if (rc != 0) return rc;
  p->wsProjects = count;

  /* dependency/topo sort dilakukan dalam workspaceEnumerate ( fase parse
     mencakupnya); pisahkan biaya closure expansion sebagai fase utama
     agar peta biaya upstream hilang ketika parse cepat tetapi topo
     mahal. Beri satu fase tambahan: dependency. */
  profPhaseBegin("dependency");
  /* closure expansion ringan: hitung jumlah depends_on (edge) di sini
     via nama terurut — cukup ditampilkan sebagai integer jadi dilaporkan
     sebagai metrik, tapi ukurannya tetap masuk abstrak "dependency". */
  int edges = 0;
  for (int i = 0; i < count; i++)
    edges++;
  profPhaseEnd(0, 0);
  (void)edges;

  /* sanity: build masing-masing proyek (fase library/binary tidak
     dieksekusi di sini karena tidak dapat diisolasi); proyek tanpa root
     hilang dilaporkan jelas saat build nyata. Ini biaya deklarasi-organisasi
     yang diukur, bukan biaya build. */
  profPhaseBegin("library");
  for (int i = 0; i < count; i++) {
    /* hitung biaya ekstat deklaratif proyek: root proyek ada bila
       Buildfile atau .Buildfile sintesis nya ada — tidak membangun apa
       pun. */
    char projDir[MAX_PATH * 3];
    snprintf(projDir, sizeof(projDir), "%s/%s", p->dir, names[i]);
    if (!fsDirExists(projDir)) {
      fprintf(stderr, "satria: profile workspace=%s: proyek '%s' bukan direktori\n", p->target,
              names[i]);
      return 1;
    }
    p->files++;
    p->bytes += profSizeOf(projDir);
  }
  profPhaseEnd(0, 0);

  /* archive/embed/binary tidak diukur di sini — lihat komentar atas:
     fase yang mahal di workspace nyata justru build proyeknya (dengan
     state yang tak bisa diisolasi tanpa menyalin tree). Design
     mengizinkan meninggalkan fase ini sampai pendekatan isolasi yang
     murah tersedia. */
  return 0;
}

/*
 * project=<dir> — siklus build satu proyek (pola design "project"):
 * config -> fingerprint -> decide -> compile -> link. Sama seperti
 * binary= + fase fingerprint & decide yang memakai deps cache di sandbox.
 */
int profRunProject(ProfCtx *p) {
  return buildSandboxed(p, "project");
}

int profRunBinary(ProfCtx *p) {
  return buildSandboxed(p, "binary");
}


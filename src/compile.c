#include "compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"
#include "util.h"

/* Keluarga toolchain (CompilerKind) didefinisikan di compile.h — dipakai
   config.c untuk suffix compiler (design/flags.md). */

CompilerKind compilerKind(const char *cc) {
  const char *base = strrchr(cc, '/');
  base = base ? base + 1 : cc;
  const char *dot = strrchr(base, '.');
  char basebuf[128];
  size_t bl = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  if (bl >= sizeof(basebuf)) bl = sizeof(basebuf) - 1;
  memcpy(basebuf, base, bl);
  basebuf[bl] = '\0';

  if (strcmp(basebuf, "cl") == 0) return CC_MSVC;
  if (bl >= 2 && basebuf[bl - 2] == '+' && basebuf[bl - 1] == '+') return CC_GXX;
  if (strncmp(basebuf, "clang", 5) == 0) return CC_CLANG; /* clang, clang-*, dst. */
  return CC_GCC;                                          /* gcc, cc, mingw32-gcc, dst. */
}

bool compilerIsMSVC(const Config *c) {
  return compilerKind(c->cc) == CC_MSVC;
}

/* cc adalah driver C++ (g++/clang++/cl) — runtime C++ sudah di-link-nya.
   Driver tak dikenal dianggap C-driver: -lstdc++ tetap ditambahkan
   (tak berbahaya bila driver ternyata memang C++). */
bool compilerIsCppDriver(const Config *c) {
  CompilerKind k = compilerKind(c->cc);
  return k == CC_GXX || k == CC_MSVC;
}

/*
 * Flag arsitektur untuk Buildfile: target — "" bila target kosong/tak
 * dikenal (fallback host). Nilai yang dikenal: x86_64, arm64, riscv64.
 * GNU/Clang: -march/-mabi (x86_64 juga menerima -m64); Apple Clang: -arch;
 * MSVC: /ARCH (x86_64 = default, tidak perlu flag).
 */
char *targetFlags(const Config *c) {
  const char *t = c->target;
  if (!t || !*t) return NULL;

#if defined(__APPLE__)
  if (strcmp(t, "x86_64") == 0) return strdup("-arch x86_64 ");
  if (strcmp(t, "arm64") == 0 || strcmp(t, "aarch64") == 0) return strdup("-arch arm64 ");
  return NULL;
#else
  if (strcmp(t, "x86_64") == 0 || strcmp(t, "amd64") == 0)
    return strdup(compilerKind(c->cc) == CC_MSVC ? "" : "-m64 ");
  if (strcmp(t, "arm64") == 0 || strcmp(t, "aarch64") == 0)
    return strdup(compilerKind(c->cc) == CC_MSVC ? "" : "-march=armv8-a ");
  if (strcmp(t, "riscv64") == 0)
    return strdup(compilerKind(c->cc) == CC_MSVC ? "" : "-march=rv64gc -mabi=lp64d ");
  return NULL;
#endif
}

bool resolveCompiler(Config *c) {
  if (c->compilers.count == 0) {
    /* Konvensi bahasa: proyek C++ (langCpp, diturunkan dari sources .cpp)
       default ke driver C++; sisanya gcc/clang seperti biasa. */
#ifdef _WIN32
    if (c->langCpp) {
      listAdd(&c->compilers, "cl");
      listAdd(&c->compilers, "g++");
      listAdd(&c->compilers, "clang++");
    } else {
      listAdd(&c->compilers, "cl");
      listAdd(&c->compilers, "gcc");
      listAdd(&c->compilers, "clang");
    }
#else
    if (c->langCpp) {
      listAdd(&c->compilers, "g++");
      listAdd(&c->compilers, "clang++");
    } else {
      listAdd(&c->compilers, "gcc");
      listAdd(&c->compilers, "clang");
    }
#endif
  }
  for (int i = 0; i < c->compilers.count; i++) {
    if (probeAvailable(c->compilers.items[i])) {
      copyStr(c->cc, sizeof(c->cc), c->compilers.items[i]);
      configApplyCompilerSuffix(c); /* suffix .gcc/.clang/.cl (design/flags.md) */
      return true;
    }
  }
  fprintf(stderr, "satria: no usable compiler found (tried compiler list)\n");
  return false;
}

/*
 * Resolusi entri headers menjadi direktori polos:
 *   "-Ifoo" diteruskan apa adanya sebagai flag lengkap, "I." adalah
 *   shorthand terdokumentasi untuk ".", (meniru library-nya "lm" = -lm),
 *   selain itu dianggap sebagai direktori itu sendiri.
 */
static const char *headerEntryDir(const char *entry) {
  if (entry[0] == '-') return entry; /* flag lengkap, teruskan */
  if (entry[0] == 'I' && entry[1] == '.') return entry + 1;
  return entry;
}

/*
 * includeFlags & warningFlags memakai sintaks GNU (-I/-W) sebagai bentuk
 * kanonik di seluruh program; translasi ke sintaks MSVC (/I, /W4, /W3)
 * dilakukan satu titik di compileOne() — termasuk kompilasi object embed.
 */

/* "-Iinclude -I." dari headers.public (menerima bentuk flat & nested) */
char *includeFlags(const Config *c) {
  size_t n = 1;
  for (int i = 0; i < c->headerPublic.count; i++)
    n += strlen(c->headerPublic.items[i]) + 8;
  char *s = malloc(n);
  s[0] = '\0';
  for (int i = 0; i < c->headerPublic.count; i++) {
    const char *entry = c->headerPublic.items[i];
    if (entry[0] == '-') {
      strcat(s, entry);
    } else {
      strcat(s, "-I");
      strcat(s, headerEntryDir(entry));
    }
    strcat(s, " ");
  }
  return s;
}

void includeDirs(const Config *c, List *out) {
  for (int i = 0; i < c->headerPublic.count; i++) {
    const char *entry = c->headerPublic.items[i];
    if (entry[0] == '-') {
      if (entry[1] == 'I' && entry[2]) listAdd(out, entry + 2);
      continue; /* flag lain bukan direktori */
    }
    listAdd(out, headerEntryDir(entry));
  }
}

/* Inti warningFlags: gabungkan flag (tambah '-' bila belum ada). */
static char *joinFlags(const List *flags) {
  size_t n = 1;
  for (int i = 0; i < flags->count; i++)
    n += strlen(flags->items[i]) + 8;
  char *s = malloc(n);
  s[0] = '\0';
  for (int i = 0; i < flags->count; i++) {
    const char *f = flags->items[i];
    if (f[0] != '-') strcat(s, "-");
    strcat(s, f);
    strcat(s, " ");
  }
  return s;
}

/* "-Wall -Wextra" dari flags; entri mempertahankan '-' di depan bila ada */
char *warningFlags(const Config *c) {
  return joinFlags(&c->flags);
}

/* warningFlags + -fPIC — dipakai compileOne & kompilasi paralel saat
   library shared diminta (GNU/Clang; MSVC tidak butuh flag). */
char *picWarningFlags(const Config *c) {
  char *wf = warningFlags(c);
  char *picwf = malloc(strlen(wf) + 16);
  strcpy(picwf, wf);
  strcat(picwf, "-fPIC ");
  free(wf);
  return picwf;
}

/*
 * defines (design/flags.md §2.1): "FOO" -> "-DFOO " (GNU/Clang) atau
 * "/DFOO " (MSVC, tanpa lewat translateFlagsToMsvc — kanoniknya memang /D).
 * Nilai yang sudah berprefix (-DFOO, /DFOO) dinormalisasi; token lain yang
 * diawali '-'/'/' adalah flag penuh — diteruskan apa adanya.
 * NULL bila kosong (malloc gagal juga mengembalikan NULL; pemanggil
 * memakai NULL sebagai "tanpa defines", aman).
 */
char *definesFlags(const Config *c) {
  if (c == NULL || c->defines.count == 0) return NULL;
  const char *pfx = compilerKind(c->cc) == CC_MSVC ? "/D" : "-D";
  size_t n = 1;
  for (int i = 0; i < c->defines.count; i++) n += strlen(c->defines.items[i]) + 4;
  char *s = malloc(n);
  if (!s) return NULL;
  s[0] = '\0';
  for (int i = 0; i < c->defines.count; i++) {
    const char *it = c->defines.items[i];
    if ((it[0] == '-' || it[0] == '/') && it[1] == 'D') {
      it += 2; /* sudah berprefix -D / /D: strip, ganti dengan kanonik */
    } else if (it[0] == '-' || it[0] == '/') {
      strcat(s, it); /* flag penuh non-D (langka): teruskan */
      strcat(s, " ");
      continue;
    }
    strcat(s, pfx);
    strcat(s, it);
    strcat(s, " ");
  }
  return s;
}

/* Path sumber relatif root proyek: "bade/manager/main.c" -> "manager/main.c". */
static const char *projectRelPath(const Config *c, const char *src) {
  size_t pl = strlen(c->projRoot);
  if (pl == 0) return src;
  if (strncmp(src, c->projRoot, pl) == 0 && src[pl] == '/') return src + pl + 1;
  return src;
}

/* Root sumber dalam mode workspace diawali root proyek (mis. "bade/manager"
   untuk root proyek "bade"). Object harus memakai path relatif terhadap root
   proyek: build/bade/manager/main.o, bukan build/bade/bade/manager/main.o. */
static const char *projectRelRoot(const Config *c, const char *root) {
  const char *r = root;
  size_t pl = strlen(c->projRoot);
  if (pl > 0) {
    if (strcmp(root, c->projRoot) == 0) r = ".";
    else if (strncmp(root, c->projRoot, pl) == 0 && root[pl] == '/') r = root + pl + 1;
  }
  /* Root "src" adalah root sumber utama: object-nya diratakan ke build/<proyek>/
     (bade/src/bade.c -> build/bade/bade.o), sedangkan root lain, mis. "manager",
     tetap memakai subfolder (build/bade/manager/main.o). */
  if (strcmp(r, "src") == 0) return ".";
  return r;
}

bool objectPathFor(const Config *c, const char *src, char *out, size_t n) {
  for (int i = 0; i < c->sources.count; i++) {
    const char *root = c->sources.items[i];
    size_t rl = strlen(root);
    const char *rest = NULL;
    if (strcmp(root, ".") == 0)
      rest = src;
    else if (strncmp(src, root, rl) == 0 && (src[rl] == '/' || src[rl] == '\\'))
      rest = src + rl + 1;
    else if (strcmp(src, root) == 0) {
      /* Entri sources berupa FILE (mis. `sources = main.c` hasil `satria
         -xf` untuk build.ninja flat): object = nama file tanpa ekstensi
         langsung di bawah build dir. */
      char rel[MAX_PATH];
      snprintf(rel, sizeof(rel), "%s", src);
      char *dot = strrchr(rel, '.');
      if (dot) *dot = '\0';
      snprintf(out, n, "%s/%s.o", c->outBuildDir, rel);
      return true;
    }
    if (!rest) continue;

    char rel[MAX_PATH];
    snprintf(rel, sizeof(rel), "%s", rest);
    char *dot = strrchr(rel, '.');
    if (dot) *dot = '\0';

    /* Keep the source-root in the object path when multiple source roots are
       configured. This prevents collisions such as src/install.c and
       manager/install.c both becoming build/install.o. A single-root build
       keeps the historical flat layout for compatibility. */
    const char *oroot = projectRelRoot(c, root);
    if (c->sources.count > 1 && strcmp(oroot, ".") != 0)
      snprintf(out, n, "%s/%s/%s.o", c->outBuildDir, oroot, rel);
    else
      snprintf(out, n, "%s/%s.o", c->outBuildDir, rel);
    return true;
  }
  return false;
}

/* src/x/y.c -> build/y.d — file dependensi (-MMD) milik object-nya. */
bool dotDPathFor(const Config *c, const char *src, char *out, size_t n) {
  if (!objectPathFor(c, src, out, n)) return false;
  size_t len = strlen(out);
  if (len + 2 >= n) return false;
  out[len - 1] = 'd'; /* .o -> .d */
  out[len] = '\0';
  return true;
}

/*
 * exclude: nama file, path relatif, atau direktori. Pencocokan:
 *   - basename sama persis ("main.c" vs "src/main.c"), atau
 *   - path sama persis, atau diakhiri "/<entry>".
 */
bool excludedSource(const Config *c, const char *src) {
  if (!c || c->excludes.count == 0 || !src) return false;
  const char *base = strrchr(src, '/');
  base = base ? base + 1 : src;
  /* exclude di Buildfile relatif root PROYEK (design/workspace.md), sedangkan
     src di workspace relatif root workspace: cocokkan keduanya. */
  const char *rel = projectRelPath(c, src);
  for (int i = 0; i < c->excludes.count; i++) {
    const char *e = c->excludes.items[i];
    if (!e || !*e) continue;
    if (strcmp(src, e) == 0 || strcmp(rel, e) == 0 || strcmp(base, e) == 0) return true;
    size_t el = strlen(e);
    /* Entri berakhiran '/' = prefix direktori: semua src di bawahnya. */
    if (el > 0 && e[el - 1] == '/' && (strncmp(src, e, el) == 0 || strncmp(rel, e, el) == 0))
      return true;
  }
  return false;
}

/* Kompilasi satu source untuk library: identik compileOne, plus -fPIC
   bila library shared diminta (GNU/Clang; MSVC tidak butuh flag). */
bool compileLibraryOne(const Config *c, const char *inc, const char *wf, const char *src,
                       const char *obj) {
#ifndef _WIN32
  if (c->libShared && compilerKind(c->cc) != CC_MSVC) {
    char *picwf = picWarningFlags(c);
    bool ok = compileOne(c, inc, picwf, src, obj);
    free(picwf);
    return ok;
  }
#else
  (void)c;
#endif
  return compileOne(c, inc, wf, src, obj);
}

/* "token1 token2" -> "token1","token2" — pecah per spasi lalu terjemahkan:
   -I<dir> -> /I<dir>, -W* -> /W4 (Wall/Wextra) atau /W3 (lainnya),
   token lain diteruskan (mis. /std dari pemanggil). */
static char *translateFlagsToMsvc(const char *flags) {
  size_t n = strlen(flags) + 16;
  char *out = malloc(n);
  out[0] = '\0';
  const char *p = flags;
  while (*p) {
    while (*p == ' ')
      p++;
    const char *start = p;
    while (*p && *p != ' ')
      p++;
    size_t len = (size_t)(p - start);
    if (len == 0) continue;
    if (len >= 2 && start[0] == '-' && start[1] == 'I') {
      strncat(out, "/I", n - strlen(out) - 1);
      strncat(out, start + 2, n - strlen(out) - 1);
    } else if (len >= 2 && start[0] == '-' && start[1] == 'W') {
      /* -Wall/-Wextra -> /W4; warning lain dinormalisasi ke /W3 */
      strcat(out, (len == 5 && strncmp(start, "-Wall", 5) == 0) ||
                          (len == 7 && strncmp(start, "-Wextra", 7) == 0)
                      ? "/W4"
                      : "/W3");
    } else if (len >= 2 && start[0] == '-' && start[1] == 'M') {
      /* -MMD/-MP/-MF/-MT: opsi dep-file GCC/Clang tanpa padanan langsung di
         MSVC — dilewati agar tidak salah diterjemahkan jadi /W3. */
      continue;
    } else {
      strncat(out, start, len);
    }
    strcat(out, " ");
  }
  return out;
}

/*
 * Susun command kompilasi satu source — dipakai compileOne (runtime juga
 * untuk jalur embed fallback) dan jalur kompilasi paralel (-jN).
 * Hasil di buffer malloc; pemanggil yang membebaskan.
 */
char *compileCmd(const Config *c, const char *inc, const char *wf, const char *src,
                 const char *obj) {
  char *tf = targetFlags(c); /* NULL bila tidak ada target */
  size_t tflen = tf ? strlen(tf) : 0;

  if (compilerKind(c->cc) == CC_MSVC) {
    /* MSVC (cl.exe): flag GNU diterjemahkan ke /I, /W4|/W3; gnu11/c11 ->
       /std:c11, c17 -> /std:c17; object -> /Fo<path>. Flag -D diteruskan
       (MSVC menerima -DNAME juga). Proyek C++: /std:c++14 (std generik
       tanpa "++"), /std:c++17, /std:c++20 ("2" di dalamnya), /std:c++latest. */
    char msvcstd[16];
    strcpy(msvcstd, c->langCpp ? "c++14" : "c11");
    if (strstr(c->std, "++")) {
      if (strstr(c->std, "latest"))
        strcpy(msvcstd, "c++latest");
      else if (strstr(c->std, "2"))
        strcpy(msvcstd, "c++20");
      else if (strstr(c->std, "17"))
        strcpy(msvcstd, "c++17");
    } else if (strstr(c->std, "17"))
      strcpy(msvcstd, "c17");
    else if (strstr(c->std, "2"))
      strcpy(msvcstd, "clatest");

    char *minc = translateFlagsToMsvc(inc);
    char *mwf = translateFlagsToMsvc(wf);
    char *mtf = tf ? translateFlagsToMsvc(tf) : NULL;
    char *mdf = definesFlags(c); /* sudah sintaks /D untuk MSVC */
    size_t n = strlen(minc) + strlen(mwf) + (mtf ? strlen(mtf) : 0) +
               (mdf ? strlen(mdf) : 0) + 2 * strlen(src) + strlen(obj) + 128;
    char *cmd = malloc(n);
    snprintf(cmd, n, "cl /nologo %s %s %s %s/std:%s /c %s /Fo%s", mtf ? mtf : "", minc, mwf,
             mdf ? mdf : "", msvcstd, src, obj);
    free(minc);
    free(mwf);
    free(mtf);
    free(mdf);
    free(tf);
    return cmd;
  }

  char *df = definesFlags(c); /* -D... utk GNU/Clang (design/flags.md §2.1) */
  size_t dflen = df ? strlen(df) : 0;
  char *cmd = malloc(strlen(c->cc) + strlen(inc) + strlen(wf) + tflen + dflen +
                     strlen(c->std) + 2 * strlen(src) + strlen(obj) + 64);
  sprintf(cmd, "%s %s %s %s%s-std=%s -c %s -o %s", c->cc, tf ? tf : "", inc, wf, df ? df : "",
          c->std, src, obj);
  free(df);
  free(tf);
  return cmd;
}

bool compileOne(const Config *c, const char *inc, const char *wf, const char *src,
                const char *obj) {
  mkparent(obj);
  char *cmd = compileCmd(c, inc, wf, src, obj);
  bool ok = runCmd(cmd);
  free(cmd);
  return ok;
}

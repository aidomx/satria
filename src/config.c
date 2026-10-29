#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg/config_cache.h"
#include "compile.h"
#include "embed.h"
#include "pack/pack_internal.h"
#include "uses/alias.h"
#include "uses/use.h"

#define SECTION_LEN 64

#include "portability.h"
#include "util.h"

/* true bila salah satu entri sources memuat source .cpp — dipakai untuk
   default bahasa C++ (std, compiler, linker). Dipanggil SEBELUM
   configFinalize menambahkan default "src", jadi di sini belum ada
   sources sintetis. */
static bool sourcesHaveCpp(const Config *c) {
  for (int i = 0; i < c->sources.count; i++) {
    const char *entry = c->sources.items[i];
    size_t len = strlen(entry);
    if (len >= 4 && strcmp(entry + len - 4, ".cpp") == 0) return true;
    if (!fsDirExists(entry)) continue;
    /* Semua .cpp di bawah entry (rekursif), sama dengan scan manual sebelumnya. */
    List all = {0};
    stateWalkDirCached(entry, ".cpp", &all);
    bool found = all.count > 0;
    listFree(&all);
    if (found) return true;
  }
  return false;
}

/*
 * ==================== Alias (format "use alias") ====================
 *
 * Mesin alias `X as Y` dipindah ke uses/alias.c agar dipakai bersama oleh
 * `use project` (parser di bawah) dan `use workspace` (uses/workspace.c).
 * Semantik tak berubah — lihat komentar di uses/alias.h.
 */

/* ==================== Default & finalisasi ==================== */

Config configDefaults(void) {
  Config c = {0};
  copyStr(c.root, sizeof(c.root), ".");
  /* c.std sengaja kosong: default bahasa (gnu11 / c++17) ditentukan
     configFinalize setelah bahasa proyek diketahui (lihat langCpp). */
  c.binary = true;
  c.cleanBuildDir = true;
  c.cleanCompileCommands = false;
  c.progressBar = true;
  c.progressErrorAlways = true;
  c.foreground = true;
  /* outBinaryName sengaja kosong: konvensi proyek standar memakai nama
     folder proyek (diisi configFinalize; fallback "satria"). */
  copyStr(c.outBinaryDir, sizeof(c.outBinaryDir), "bin");
  copyStr(c.outBuildDir, sizeof(c.outBuildDir), "build");
  copyStr(c.outCompileCommands, sizeof(c.outCompileCommands), "auto");
  copyStr(c.outLibDir, sizeof(c.outLibDir), "lib");
  /* c.aliases zero-init via Config c = {0} — List tanpa konstruktor. */
  /* default embedded per-entri; tidak ada yang di-preset di sini */
  return c;
}

void addCommaList(List *dst, const char *value) {
  char buf[2048];
  copyStr(buf, sizeof(buf), value);
  for (char *save = buf;;) {
    char *comma = strchr(save, ',');
    if (comma) *comma = '\0';
    char *item = trim(save);
    if (*item) listAdd(dst, item);
    if (!comma) break;
    save = comma + 1;
  }
}

/* ==================== pack (pengemasan artefak) ==================== */

/* Helper parsing pack.* (packSetStr/packAddFiles/packApplyDeb/packApply)
   ada di pack/pack_config.c — dipakai lewat pack/pack_internal.h. */

/* ==================== Suffix platform/compiler (design/flags.md) ==================== */

/*
 * Nama suffix -> target enum; -1 = tak dikenal. Pemanggil merutekan item
 * ke jalur flat (perilaku lama untuk nama tak dikenal: `flags.foo`,
 * `library.default`, dst. tetap diterapkan tanpa syarat). Alias lama
 * libraryListForPlatform ikut didukung: darwin, win32, mingw.
 */
int sfxTarget(const char *name) {
  if (!name || !*name) return -1;
  if (strcmp(name, "linux") == 0) return SFX_LINUX;
  if (strcmp(name, "android") == 0) return SFX_ANDROID;
  if (strcmp(name, "macos") == 0 || strcmp(name, "macOS") == 0 ||
      strcmp(name, "darwin") == 0)
    return SFX_MACOS;
  if (strcmp(name, "windows") == 0 || strcmp(name, "win32") == 0 ||
      strcmp(name, "mingw") == 0)
    return SFX_WINDOWS;
  if (strcmp(name, "freebsd") == 0) return SFX_FREEBSD;
  if (strcmp(name, "unix") == 0 || strcmp(name, "posix") == 0) return SFX_UNIX;
  if (strcmp(name, "gcc") == 0 || strcmp(name, "g++") == 0) return SFX_GCC;
  if (strcmp(name, "clang") == 0 || strcmp(name, "clang++") == 0) return SFX_CLANG;
  if (strcmp(name, "cl") == 0 || strcmp(name, "msvc") == 0) return SFX_CL;
  return -1;
}

/* Target platform host — deteksi compile-time di awal parse (design §6). */
static int hostSfxPlatform(void) {
#if defined(__ANDROID__)
  return SFX_ANDROID;
#elif defined(__APPLE__)
  return SFX_MACOS;
#elif defined(_WIN32)
  return SFX_WINDOWS;
#elif defined(__FreeBSD__)
  return SFX_FREEBSD;
#elif defined(__linux__)
  return SFX_LINUX;
#else
  return -1; /* OS tak dikenal: hanya suffix unix (fallback POSIX) */
#endif
}

/* Dedup mengabaikan '-' di depan: "Wall" dan "-Wall" dianggap sama. */
static bool sfxSameItem(const char *a, const char *b) {
  while (*a == '-')
    a++;
  while (*b == '-')
    b++;
  return strcmp(a, b) == 0;
}

void configMergeUnique(List *dst, const List *src) {
  for (int i = 0; i < src->count; i++) {
    bool dup = false;
    for (int j = 0; j < dst->count && !dup; j++)
      if (sfxSameItem(dst->items[j], src->items[i])) dup = true;
    if (!dup) listAdd(dst, src->items[i]);
  }
}

/* true bila bucket[idx] ada isinya dan sudah di-append (dedup) ke dst. */
static bool mergeSfxBucket(List *dst, List bucket[SFX_COUNT], int idx) {
  if (idx < 0 || idx >= SFX_COUNT || bucket[idx].count == 0) return false;
  configMergeUnique(dst, &bucket[idx]);
  return true;
}

/*
 * Merge suffix PLATFORM ke nilai final (design/flags.md §3: global dulu,
 * suffix di-append, dedup otomatis). Urutan: bucket spesifik host → unix
 * (fallback POSIX bila tak ada yang cocok; Windows bukan POSIX). Android
 * match bucket android DAN linux (linux mencakup Android/Termux).
 * std skalar: suffix platform meng-override nilai global/konvensi.
 */
static void configFinalizeSuffix(Config *c) {
  int plat = hostSfxPlatform();
#ifdef _WIN32
  bool posix = false;
#else
  bool posix = true; /* termasuk OS tak dikenal (kebanyakan POSIX) */
#endif

  List *props[] = {&c->flags, &c->libraries, &c->headerPublic, &c->defines};
  List *buckets[] = {c->sfxFlags, c->sfxLibrary, c->sfxHeaders, c->sfxDefines};
  for (int p = 0; p < 4; p++) {
    bool any = false;
    if (plat == SFX_ANDROID) any = mergeSfxBucket(props[p], buckets[p], SFX_ANDROID) || any;
    if (plat >= 0) any = mergeSfxBucket(props[p], buckets[p], plat) || any;
    if (plat == SFX_ANDROID) any = mergeSfxBucket(props[p], buckets[p], SFX_LINUX) || any;
    if (!any && posix) mergeSfxBucket(props[p], buckets[p], SFX_UNIX);
  }

  const char *s = NULL;
  if (plat == SFX_ANDROID) {
    if (c->sfxStd[SFX_ANDROID][0]) s = c->sfxStd[SFX_ANDROID];
    else if (c->sfxStd[SFX_LINUX][0])
      s = c->sfxStd[SFX_LINUX];
  } else if (plat >= 0 && c->sfxStd[plat][0]) {
    s = c->sfxStd[plat];
  }
  if (!s && posix && c->sfxStd[SFX_UNIX][0]) s = c->sfxStd[SFX_UNIX];
  if (s) copyStr(c->std, sizeof(c->std), s);
}

/*
 * Merge suffix COMPILER <prop>.gcc/.clang/.cl — dipanggil resolveCompiler
 * setelah cc terpilih (design/flags.md §3: paling spesifik, berlaku
 * terakhir). Idempotent via ccSuffixDone.
 */
void configApplyCompilerSuffix(Config *c) {
  if (!c || c->ccSuffixDone || !c->cc[0]) return;
  c->ccSuffixDone = true; /* idempotent: resolveCompiler bisa dipanggil ulang */

  int t;
  switch (compilerKind(c->cc)) {
    case CC_MSVC:
      t = SFX_CL;
      break;
    case CC_CLANG:
      t = SFX_CLANG;
      break;
    case CC_GCC:
    case CC_GXX: /* g++ memakai bucket .gcc (design hanya punya gcc/clang/cl) */
      t = SFX_GCC;
      break;
    default:
      return; /* driver tak dikenal: tanpa suffix compiler */
  }

  List *props[] = {&c->flags, &c->libraries, &c->headerPublic, &c->defines};
  List *buckets[] = {c->sfxFlags, c->sfxLibrary, c->sfxHeaders, c->sfxDefines};
  for (int p = 0; p < 4; p++) configMergeUnique(props[p], &buckets[p][t]);
  if (c->sfxStd[t][0]) copyStr(c->std, sizeof(c->std), c->sfxStd[t]);
}

/*
 * configFinalize — terapkan konvensi proyek standar pada Config hasil parse
 * (dipanggil loadConfig; juga dipakai jalur TANPA Buildfile — `satria profile
 * binary=<dir>` — sehingga konvensi sources=src dst. tetap berlaku).
 * MURNI derivasi in-memory + stat direktori; tidak menulis file apa pun.
 */
void configFinalize(Config *c) {
  for (int i = 0; i < c->embCount; i++)
    configFinalizeEntry(&c->emb[i], c->outBuildDir);

  /* Konvensi proyek standar — Buildfile minimal cukup `use project`:
     - sources -> src (bila tidak dideklarasikan)
     - headers -> include (bila folder ada dan tidak dideklarasikan)
     - binary  -> nama folder proyek (cwd saat load; bila tidak diset)
     Proyek non-standar tetap bebas mendeklarasikan semuanya secara
     eksplisit — defaults hanya mengisi yang kosong. */
  if (c->sources.count == 0) listAdd(&c->sources, "src");
  if (c->headerPublic.count == 0 && fsDirExists("include")) listAdd(&c->headerPublic, "include");
  /* Output-dir proyek selalu ada di include path: header generated (embedded.h,
     version.h) bisa di-#include tanpa headers.each (design/embedded.md, "Path &
     include"). Ditambahkan SETELAH default "include" agar penghapusan default
     telanjang di wsBuildConfig (cek items[0]=="include") tetap berfungsi. */
  if (c->outBuildDir[0]) {
    bool present = false;
    for (int i = 0; i < c->headerPublic.count; i++)
      if (strcmp(c->headerPublic.items[i], c->outBuildDir) == 0) present = true;
    if (!present) listAdd(&c->headerPublic, c->outBuildDir);
  }

  /* Deteksi bahasa C++ dari sources final (termasuk default "src" yang baru
     diisi — proyek standar tanpa deklarasi pun terdeteksi). Proyek campuran
     C/C++ ikut jalur C++ (runtime libstdc++ diselesaikan linker C++). */
  c->langCpp = sourcesHaveCpp(c);

  /* Konvensi bahasa C++: sources memuat .cpp -> std c++17 + toolchain C++
     (g++/clang++), tanpa perlu menulis std/compiler di Buildfile.
     Nilai eksplisit selalu menang — hanya mengisi yang kosong. */
  if (!c->std[0]) copyStr(c->std, sizeof(c->std), c->langCpp ? "c++17" : "gnu11");
  if (c->langCpp && c->compilers.count == 0) {
    listAdd(&c->compilers, "g++");
    listAdd(&c->compilers, "clang++");
  }

  if (!c->outBinaryName[0]) {
    char cwd[MAX_PATH];
    if (fsGetCwd(cwd, sizeof(cwd))) {
      const char *base = strrchr(cwd, '/');
#ifdef _WIN32
      const char *bs = strrchr(cwd, '\\');
      if (bs && (!base || bs > base)) base = bs;
#endif
      base = base ? base + 1 : cwd;
      char name[128];
      size_t w = 0;
      for (const char *p = base; *p && w + 1 < sizeof(name); p++) {
        char ch = *p;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        name[w++] = ok ? ch : '_';
      }
      name[w] = '\0';
      if (w > 0 && strcmp(name, ".") != 0 && strcmp(name, "..") != 0)
        copyStr(c->outBinaryName, sizeof(c->outBinaryName), name);
    }
  }
  if (!c->outBinaryName[0]) copyStr(c->outBinaryName, sizeof(c->outBinaryName), "satria");

  /* pack: nilai default + penanda proyek pengemasan. Key yang hanya
     mengisi metadata (name/version/deb.*) tidak mengaktifkan pack —
     cukup files/output yang disebut. */
  if (c->pack.files.count > 0 || c->pack.output[0]) {
    c->pack.requested = true;
    if (!c->pack.name[0]) copyStr(c->pack.name, sizeof(c->pack.name), c->outBinaryName);
    if (!c->pack.version[0]) copyStr(c->pack.version, sizeof(c->pack.version), "0.0.0");
    if (!c->pack.output[0])
      copyStr(c->pack.output, sizeof(c->pack.output), "dist/{name}-v{version}.tar.gz");
    if (!c->pack.debInstallPrefix[0])
      copyStr(c->pack.debInstallPrefix, sizeof(c->pack.debInstallPrefix), "/usr/local");
    /* Proyek kemasan tanpa sources tidak punya yang bisa dikompilasi —
       sama seperti archive.*, jalur !binary di cmdBuild yang menangani. */
    if (c->sources.count == 0) c->binary = false;
  }

  /*
   * Merge suffix platform TERAKHIR (design/flags.md): setelah defaults
   * konvensi — headers.<target> di-append setelah fallback include/, dan
   * std.<target> meng-override default bahasa (gnu11/c++17).
   */
  configFinalizeSuffix(c);
}


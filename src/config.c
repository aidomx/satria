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

static void addCommaList(List *dst, const char *value) {
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
static int sfxTarget(const char *name) {
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

/* ==================== Penerapan konfigurasi ==================== */

/*
 * Buildfile sengaja berupa file konfigurasi YAML-like kecil. Kita hanya
 * mem-parse subset yang dimiliki satria: section, dua level nesting, dan
 * pasangan key/value skalar (mis. embedded -> archive -> with). Opsi yang
 * tidak dikenal tetap aman diabaikan.
 */
static void configApply(Config *c, const char *section, const char *sub, const char *subsub,
                        const char *key, const char *value) {
  if (!c || !key || !value) return;
  bool b;

  if (!section) { /* skalar top-level */
    if (strcmp(key, "root") == 0)
      copyStr(c->root, sizeof(c->root), value);
    else if (strcmp(key, "std") == 0)
      copyStr(c->std, sizeof(c->std), value);
    else if (strcmp(key, "log") == 0 && parseBool(value, &b))
      c->logEnabled = b;
    else if (strcmp(key, "foreground") == 0 && parseBool(value, &b))
      c->foreground = b;
    else if (strcmp(key, "target") == 0)
      copyStr(c->target, sizeof(c->target),
              value); /* Key bare hasil sintesis Buildfile.ws (prefix projects.<n>. dan
       aliasnya dilepas): name/version/files/output/... -> pack. Tanpa
       files/output proyek biasa tidak terpengaruh (pack tak aktif). */
    else if (strcmp(key, "name") == 0 || strcmp(key, "version") == 0 ||
             strcmp(key, "output") == 0 || strcmp(key, "compress") == 0 ||
             strcmp(key, "checksum") == 0 || strcmp(key, "format") == 0 ||
             strcmp(key, "files") == 0 || strcmp(key, "exclude") == 0)
      packApply(c, key, value);
    return;
  }

  if (strcmp(section, "clean") == 0) {
    /* clean.compdb (dulu clean.compileCommands — nama lama tetap diterima,
       selaras clean.build yang dulu clean.buildDir). */
    if (strcmp(key, "build") == 0 && parseBool(value, &b))
      c->cleanBuildDir = b;
    else if ((strcmp(key, "compdb") == 0 || strcmp(key, "compileCommands") == 0) &&
             parseBool(value, &b))
      c->cleanCompileCommands = b;
    return;
  }

  if (strcmp(section, "std") == 0) {
    /* std.<target> (design/flags.md) — suffix, bukan key std biasa;
       skalar, jadi di-merge sebagai override saat finalize. */
    int t = sfxTarget(key);
    if (t >= 0) copyStr(c->sfxStd[t], sizeof(c->sfxStd[t]), value);
    return;
  }

  if (strcmp(section, "progress") == 0) {
    if (strcmp(key, "bar") == 0 && parseBool(value, &b))
      c->progressBar = b;
    else if (strcmp(key, "error") == 0)
      c->progressErrorAlways = (strcmp(value, "always") == 0);
    return;
  }

  if (strcmp(section, "output") == 0) {
    if (strcmp(key, "binary") == 0 && parseBool(value, &b)) c->binary = b;
    if (strcmp(key, "binaryName") == 0)
      copyStr(c->outBinaryName, sizeof(c->outBinaryName), value);
    else if (strcmp(key, "binaryDir") == 0)
      copyStr(c->outBinaryDir, sizeof(c->outBinaryDir), value);
    else if (strcmp(key, "buildDir") == 0)
      copyStr(c->outBuildDir, sizeof(c->outBuildDir), value);
    else if (strcmp(key, "compileCommands") == 0)
      copyStr(c->outCompileCommands, sizeof(c->outCompileCommands), value);
    else if (strcmp(key, "libraryName") == 0) {
      /* output.libraryName menandai library diminta + menetapkan namanya. */
      c->libRequested = true;
      c->libStatic = true;
      copyStr(c->outLibName, sizeof(c->outLibName), value);
    } else if (strcmp(key, "libDir") == 0)
      copyStr(c->outLibDir, sizeof(c->outLibDir), value);
    else if (strcmp(key, "libraryStatic") == 0 && parseBool(value, &b))
      c->libStatic = b;
    else if (strcmp(key, "libraryShared") == 0 && parseBool(value, &b)) {
      c->libShared = b;
      if (b) c->libRequested = true;
    }
    return;
  }

  if (strcmp(section, "embedded") == 0) {
    /* sub adalah nama entri (key apa pun pilihan project); lazily ditambahkan */
    if (!sub) return;

    EmbeddedEntry *e = NULL;
    for (int i = 0; i < c->embCount; i++) {
      if (strcmp(c->emb[i].name, sub) == 0) {
        e = &c->emb[i];
        break;
      }
    }
    if (!e) {
      if (c->embCount >= MAX_EMBEDDED) return;
      e = &c->emb[c->embCount++];
      memset(e, 0, sizeof(*e));
      copyStr(e->name, sizeof(e->name), sub);
      e->enable = true;
      e->tar = true;
      copyStr(e->archiveDir, sizeof(e->archiveDir), "modules");
      copyStr(e->archiveName, sizeof(e->archiveName), sub);
      copyStr(e->ext, sizeof(e->ext), "gz");
      listAdd(&e->excludes, ".git");
      /* State/cache satria bukan aset: tanpa exclude ini scan freshness
         menganggap arsip basi setiap run (folder .satria berubah) dan
         tar diulang percuma — biaya no-op workspace membesar. */
      listAdd(&e->excludes, ".satria");
    }

    if (subsub && strcmp(subsub, "with") == 0) {
      if (strcmp(key, "tar") == 0 && parseBool(value, &b))
        e->tar = b;
      else if (strcmp(key, "ext") == 0)
        copyStr(e->ext, sizeof(e->ext), value);
    } else if (subsub && strcmp(subsub, "archive") == 0) {
      if (strcmp(key, "exclude") == 0) addCommaList(&e->excludes, value);
    } else if (strcmp(key, "src") == 0) {
      copyStr(e->src, sizeof(e->src), value);
    } else if (strcmp(key, "extract") == 0) {
      copyStr(e->extract, sizeof(e->extract), value);
    } else if (strcmp(key, "pattern") == 0) {
      copyStr(e->pattern, sizeof(e->pattern), value);
    } else if (strcmp(key, "enable") == 0 && parseBool(value, &b)) {
      e->enable = b;
    } else if (strcmp(key, "dir") == 0) {
      copyStr(e->archiveDir, sizeof(e->archiveDir), value);
    } else if (strcmp(key, "name") == 0) {
      copyStr(e->archiveName, sizeof(e->archiveName), value);
    } else if (strcmp(key, "file") == 0) {
      /* embedded.<n>.file: pakai arsip jadi (skip tar). Ditangani lebih
         lanjut di configFinalizeEntry. */
      e->usePrebuilt = true;
      copyStr(e->prebuiltPath, sizeof(e->prebuiltPath), value);
    } else if (strcmp(key, "variable") == 0) {
      /* Override prefix macro EMBED_<N>_ — mis. BUILTIN_MODULES. */
      copyStr(e->variable, sizeof(e->variable), value);
    }
    return;
  }

  if (strcmp(section, "pack") == 0) {
    /* Proyek pengemasan: pack.<key> (name/version/output/...),
       pack.files (daftar koma), atau pack.deb.<key>. */
    if (sub && strcmp(sub, "deb") == 0) {
      packApplyDeb(c, key, value);
      return;
    }
    packApply(c, key, value);
    return;
  }

  if (strcmp(section, "release") == 0) {
    /* Section release.* — kekuatan pack penuh, scope workspace/proyek
       (design/release.md revisi 2). Model identik dengan pack.*, jadi
       cukup dilaporkan sebagai pack. Pencetus section ini adalah
       pengaktif pack: sekali release.* diset, fase release berjalan. */
    if (sub && strcmp(sub, "deb") == 0) {
      packApplyDeb(c, key, value);
    } else {
      packApply(c, key, value);
    }
    c->pack.requested = true;
    return;
  }

  if (strcmp(section, "deb") == 0) {
    /* Key bare deb.<key> (hasil sintesis Buildfile.ws: kunci
       `deb.maintainer = ...` di-rute sebagai section "deb"). */
    packApplyDeb(c, key, value);
    return;
  }

  if (strcmp(section, "archive") == 0) {
    /*
     * Proyek kemasan (workspace): memproduksi arsip saja, tanpa binary.
     * Key set ini MEMAKAI entri embedded sebagai penyimpanan setting tar
     * (nama/with.tar/with.ext/dir), memakai arsip jadi via file, lalu
     * menonaktifkan binary (output.binary = false secara implisit).
     * Aset yang diarsipkan: archive.src.
     */
    if (!c->embCount) {
      if (c->embCount >= MAX_EMBEDDED) return;
      EmbeddedEntry *e = &c->emb[c->embCount++];
      memset(e, 0, sizeof(*e));
      copyStr(e->name, sizeof(e->name), "archive");
      e->enable = true;
      e->tar = true;
      copyStr(e->archiveDir, sizeof(e->archiveDir), "dist");
      copyStr(e->archiveName, sizeof(e->archiveName), "archive");
      copyStr(e->ext, sizeof(e->ext), "gz");
      copyStr(e->pattern, sizeof(e->pattern), "");
      listAdd(&e->excludes, ".git");
      listAdd(&e->excludes, ".satria"); /* state/cache satria bukan aset */
    }
    EmbeddedEntry *e = &c->emb[0];

    if (subsub && strcmp(subsub, "with") == 0) {
      if (strcmp(key, "tar") == 0 && parseBool(value, &b))
        e->tar = b;
      else if (strcmp(key, "ext") == 0)
        copyStr(e->ext, sizeof(e->ext), value);
      return;
    }
    if (strcmp(key, "with") == 0) {
      /* with = tar | tar, gz | tar, xz | with.tar = true + with.ext = <e>
       * `tar` polos (tanpa koma) = tar TANPA kompresi: ext default "gz"
       * harus dikosongkan, otherwise buildEmbeddedArchiveEntry memilih
       * flag `z` (tar czf) dan nama arsip tetap <name>.tar.gz (bug).
       */
      char buf[128];
      copyStr(buf, sizeof(buf), value);
      char *comma = strchr(buf, ',');
      if (comma) {
        *comma = '\0';
        char *format = trim(buf);
        char *ext = trim(comma + 1);
        e->tar = strcmp(format, "tar") == 0;
        copyStr(e->ext, sizeof(e->ext), ext);
      } else if (strcmp(trim(buf), "tar") == 0) {
        e->tar = true;
        e->ext[0] = '\0'; /* tanpa .tar.<ext> — tar murni */
      } else {
        e->tar = false;
        copyStr(e->ext, sizeof(e->ext), trim(buf));
      }
      return;
    }
    if (strcmp(key, "exclude") == 0) {
      addCommaList(&e->excludes, value);
      return;
    }
    if (strcmp(key, "src") == 0) {
      copyStr(e->src, sizeof(e->src), value);
    } else if (strcmp(key, "pattern") == 0) {
      copyStr(e->pattern, sizeof(e->pattern), value);
    } else if (strcmp(key, "name") == 0) {
      copyStr(e->archiveName, sizeof(e->archiveName), value);
    } else if (strcmp(key, "dir") == 0) {
      copyStr(e->archiveDir, sizeof(e->archiveDir), value);
    }
    c->binary = false; /* proyek kemasan tidak memproduksi binary */
    c->libRequested = false;
    return;
  }
}

static void listForSection(Config *c, const char *section, const char *sub, const char *item) {
  int sfx = sfxTarget(sub);
  if (strcmp(section, "sources") == 0)
    listAdd(&c->sources, item);
  else if (strcmp(section, "flags") == 0) {
    if (sfx >= 0)
      listAdd(&c->sfxFlags[sfx], item);
    else
      listAdd(&c->flags, item);
  } else if (strcmp(section, "defines") == 0) {
    if (sfx >= 0)
      listAdd(&c->sfxDefines[sfx], item);
    else
      listAdd(&c->defines, item);
  } else if (strcmp(section, "library") == 0) {
    if (sfx >= 0)
      listAdd(&c->sfxLibrary[sfx], item);
    else
      listAdd(&c->libraries, item); /* flat + nama tak dikenal (default, ...) */
  } else if (strcmp(section, "compiler") == 0)
    listAdd(&c->compilers, item);
  else if (strcmp(section, "exclude") == 0)
    listAdd(&c->excludes, item);
  else if (strcmp(section, "headers") == 0) {
    if (sub && strcmp(sub, "internal") == 0)
      listAdd(&c->headerInternal, item);
    else if (sfx >= 0)
      listAdd(&c->sfxHeaders[sfx], item);
    else
      listAdd(&c->headerPublic, item);
  } else if (strcmp(section, "pack") == 0) {
    /* Format colon: section pack + item list = entri pack.files. */
    listAdd(&c->pack.files, item);
  }
}

/*
 * configApplyResolved — routing SATU baris `key = value` format `use
 * project` SETELAH alias di-resolve: key bertitik dipecah menjadi
 * section/sub/subsub, lalu dirutekan ke listForSection (section list:
 * sources/flags/compiler/exclude/headers/library/defines — value dipotong
 * koma di tempat) atau configApply (section lain). key & value MUTABLE dan
 * dipotong di tempat — pemanggil menyiapkan buffer sendiri.
 */
static void configApplyResolved(Config *c, char *key, char *v) {
  char *parts[8];
  int np = 0;
  for (char *tok = key; tok && np < 8;) {
    char *dot = strchr(tok, '.');
    parts[np++] = tok;
    if (!dot) break;
    *dot = '\0';
    tok = dot + 1;
  }

  bool isListSection = strcmp(parts[0], "sources") == 0 || strcmp(parts[0], "flags") == 0 ||
                       strcmp(parts[0], "compiler") == 0 || strcmp(parts[0], "exclude") == 0 ||
                       strcmp(parts[0], "headers") == 0 || strcmp(parts[0], "library") == 0 ||
                       strcmp(parts[0], "defines") == 0;
  if (isListSection) {
    if (!*v) return;
    for (char *save = v;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item) listForSection(c, parts[0], np >= 2 ? parts[1] : NULL, item);
      if (!comma) break;
      save = comma + 1;
    }
    return;
  }

  if (np == 1)
    configApply(c, NULL, NULL, NULL, parts[0], v);
  else if (np == 2)
    configApply(c, parts[0], NULL, NULL, parts[1], v);
  else if (np == 3)
    configApply(c, parts[0], parts[1], NULL, parts[2], v);
  else if (np == 4)
    configApply(c, parts[0], parts[1], parts[2], parts[3], v);
  else if (np >= 5 && strcmp(parts[np - 2], "with") == 0)
    configApply(c, parts[0], parts[1], "with", parts[np - 1], v);
}

void configApplyLine(Config *c, const char *key, const char *value) {
  if (!c || !key || !value) return;
  char k[256], v[2048];
  if (strlen(key) >= sizeof(k)) return;
  copyStr(k, sizeof(k), key);
  copyStr(v, sizeof(v), value);
  configApplyResolved(c, k, v);
}

static void parseLine(Config *c, char *section, char *sub, char *subsub, int *subIndent,
                      int *subsubIndent, char *text, int indent, bool useAlias) {
  bool isItem = text[0] == '-';
  if (isItem) text = trim(text + 1);

  /* ---- format `use alias` / `use project`: key = value (kualifikasi
     titik, alias aktif) ----
     Baris `X as Y` = definisi alias, bukan pengaturan. Baris `a = b`:
     key tanpa titik mewarisi prefix section aktif (baris menjorok), lalu
     alias di-resolve, dan hasilnya dirutekan ke configApply/listForSection
     yang sama dengan format lama — tidak ada jalur konfigurasi kedua.
     `use project` adalah nama baru untuk jalur ini; `use alias` tetap
     diterima (kompatibilitas). */
  if (useAlias && !isItem) {
    /* Format baru menerima komentar // (selain # yang sudah dibuang
       loadConfig). Dipotong sebelum apapun — nilai berisi "//" (URL) tak
       didukung di format ini. */
    char *slash2 = strstr(text, "//");
    if (slash2) *slash2 = '\0';
    if (!*trim(text)) return;

    if (aliasLineAdd(&c->aliases, text)) return;
    char *eq = strchr(text, '=');
    if (eq) {
      *eq = '\0';
      char *k = trim(text);
      char *v = trim(eq + 1);
      if (!*k) return;

      char key[ALIAS_KEY_MAX];
      if (strlen(k) >= sizeof(key)) return; /* key tidak masuk akal — abaikan */
      copyStr(key, sizeof(key), k);
      if (indent > 0 && section[0] && !strchr(key, '.')) {
        char qual[3 * SECTION_LEN + 4];
        if (subsub[0])
          snprintf(qual, sizeof(qual), "%s.%s.%s", section, sub, subsub);
        else if (sub[0])
          snprintf(qual, sizeof(qual), "%s.%s", section, sub);
        else
          snprintf(qual, sizeof(qual), "%s", section);
        char full[sizeof(qual) + sizeof(key)];
        snprintf(full, sizeof(full), "%s.%s", qual, key);
        copyStr(key, sizeof(key), full);
      }
      aliasResolve(&c->aliases, key, sizeof(key));
      configApplyResolved(c, key, v);
      return;
    }
    /* tanpa '=' dan tanpa ' as ': jatuh ke jalur parse lama di bawah */
  }

  char *colon = strchr(text, ':');
  if (!colon) {
    if (!isItem || !section[0]) return; /* teks nyasar, abaikan */
    listForSection(c, section, sub[0] ? sub : NULL, text);
    return;
  }

  *colon = '\0';
  char *key = trim(text);
  char *value = trim(colon + 1);

  if (!*value) {
    if (indent == 0) {
      snprintf(section, SECTION_LEN, "%s", key);
      sub[0] = '\0';
      subsub[0] = '\0';
    } else if (!sub[0] || indent <= *subIndent) {
      snprintf(sub, SECTION_LEN, "%s", key);
      *subIndent = indent;
      subsub[0] = '\0';
    } else {
      snprintf(subsub, SECTION_LEN, "%s", key);
      *subsubIndent = indent;
    }
    return;
  }

  if (indent == 0) {
    configApply(c, NULL, NULL, NULL, key, value);
    return;
  }
  if (!section[0]) return;
  if (subsub[0] && indent > *subsubIndent)
    configApply(c, section, sub, subsub, key, value);
  else
    configApply(c, section, sub, NULL, key, value);
}

bool loadConfigEx(Config *c, const char *path, bool cacheWrite) {
  char cachePath[MAX_PATH];
  cfgCachePathFor(path, cachePath, sizeof(cachePath));

  /* Cache validation only stats Buildfile; the Buildfile itself is not opened
     on a cache hit. */
  if (cfgCacheLoad(c, path, cachePath)) return true;

  /* A corrupt/stale cache may have partially populated c before failing. */
  *c = configDefaults();

  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "satria: cannot open %s\n", path);
    return false;
  }

  char line[1024];
  char section[SECTION_LEN] = {0};
  char sub[SECTION_LEN] = {0};
  char subsub[SECTION_LEN] = {0};
  int subIndent = 0, subsubIndent = 0;
  /* Format baru: `use alias` (nama lama) atau `use project` (nama baru,
     sinonim persis) — tanpa baris itu, parse lama. */
  bool useAlias = false;
  while (fgets(line, sizeof(line), fp)) {
    char *comment = strchr(line, '#');
    if (comment) *comment = '\0';

    int indent = 0;
    for (char *q = line; *q && (*q == ' ' || *q == '\t'); q++)
      indent += (*q == '\t') ? 2 : 1;

    char *text = trim(line);
    if (!*text) continue;
    if (indent == 0) {
      if (useLineMode(text) == USE_PROJECT) {
        useAlias = true;
        continue;
      }
      /* `use workspace` / `use` tak dikenal: baris `use` BUKAN setting —
         jangan biarkan jatuh ke parse lama sebagai section. */
      if (strncmp(text, "use", 3) == 0 && (text[3] == '\0' || text[3] == ' ')) continue;
    }
    parseLine(c, section, sub, subsub, &subIndent, &subsubIndent, text, indent, useAlias);
  }
  fclose(fp);
  configFinalize(c);
  if (cacheWrite) cfgCacheSave(c, path, cachePath);
  return true;
}

bool loadConfig(Config *c, const char *path) { return loadConfigEx(c, path, true); }

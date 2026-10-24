/*
 * pack — orkestrasi pengemasan (lihat pack.h & pack_internal.h).
 *
 * Urutan: validasi entri -> resolusi template -> cek freshness (mtime
 * input vs artefak) -> bangun tarball dan/atau .deb -> checksum. Artefak
 * ditulis ke file sementara di .satria/pack/ lalu direname agar output
 * lama tidak pernah setengah jadi.
 */
#include "pack_internal.h"

#include <stdio.h>
#include <string.h>

#include "../portability.h"
#include "../util.h"
#include "pack.h"
#include "../cache/artifact.h" /* daftarkan paket/checksum ke artifact.cache (§9) */
#include "../cmds/cmds_internal.h"
#include "pack_stage.h"

/* ---- nilai {os}/{arch} host (host target satria sendiri) ---- */

static const char *packOsName(void) {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(__unix__) || defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

static const char *packArchName(void) {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__i386__) || defined(_M_X86)
  return "i386";
#elif defined(__riscv) && defined(__riscv_xlen) && __riscv_xlen == 64
  return "riscv64";
#elif defined(__arm__)
  return "arm";
#else
  return "unknown";
#endif
}

void packTemplate(const Config *c, const char *tpl, char *out, size_t n) {
  size_t o = 0;
  for (const char *p = tpl; *p && o + 1 < n; p++) {
    if (*p != '{') {
      out[o++] = *p;
      continue;
    }
    const char *close = strchr(p, '}');
    if (!close) { /* tanpa penutup: salin apa adanya */
      out[o++] = *p;
      continue;
    }
    char token[32];
    size_t tl = (size_t)(close - p - 1);
    if (tl == 0 || tl >= sizeof(token)) { /* {} atau token gila: salin utuh */
      for (const char *q = p; q <= close && o + 1 < n; q++) out[o++] = *q;
      p = close;
      continue;
    }
    memcpy(token, p + 1, tl);
    token[tl] = '\0';
    const char *val = NULL;
    if (strcmp(token, "name") == 0) val = c->pack.name;
    else if (strcmp(token, "version") == 0) val = c->pack.version;
    else if (strcmp(token, "os") == 0) val = packOsName();
    else if (strcmp(token, "arch") == 0) val = packArchName();
    if (!val) { /* token tak dikenal: salin utuh */
      for (const char *q = p; q <= close && o + 1 < n; q++) out[o++] = *q;
      p = close;
      continue;
    }
    for (const char *q = val; *q && o + 1 < n; q++) out[o++] = *q;
    p = close;
  }
  out[o] = '\0';
}

void packDebName(const char *tarPath, char *out, size_t n) {
  copyStr(out, n, tarPath);
  static const char *exts[] = {".tar.gz", ".tar.xz", ".tar.bz2", ".tgz", ".tar", NULL};
  for (int i = 0; exts[i]; i++) {
    size_t el = strlen(exts[i]), len = strlen(out);
    if (len > el && strcmp(out + len - el, exts[i]) == 0) {
      out[len - el] = '\0';
      break;
    }
  }
  size_t len = strlen(out);
  snprintf(out + len, n - len, ".deb");
}

void packStageDir(char *out, size_t n) { snprintf(out, n, ".satria/pack"); }

int64_t packPathMTimeNs(const char *path) { return fsMTimeNs(path); }

int64_t packNewestInput(const Config *c) {
  int64_t newest = -1;
  for (int i = 0; i < c->pack.files.count; i++) {
    char src[MAX_PATH * 2], dst[MAX_PATH * 2];
    if (!packEntryParts(c->pack.files.items[i], src, sizeof(src),
                        dst, sizeof(dst))) continue;
    /* Fallback .exe (bin/satria -> bin/satria.exe di Windows) agar relink
       binary tetap memicu kemasan ulang. */
    char eff[MAX_PATH * 2];
    if (!packResolveSource(src, eff, sizeof(eff))) continue;
    if (fsFileExists(eff)) {
      int64_t m = fsMTimeNs(eff);
      if (m > newest) newest = m;
    } else if (fsDirExists(eff)) {
      /* folder: semua file di dalamnya + mtime folder itu sendiri.
         Mode workspace: daftar file dari cache walk (sources.cache, divalidasi mtime
         folder) dan mtime lewat memo, sehingga embed freshness ikut memakainya. */
      List files = {0};
      if (wsModeActive()) {
        cmdsStatMemoSetCwd();
        stateWalkDirCached(eff, "", &files);
      } else {
        walkDir(eff, "", &files);
      }
      for (int j = 0; j < files.count; j++) {
        /* Sama dengan staging: isi folder yang di-exclude (mis. build/) adalah
           output, bukan input. Tanpa filter ini, output build yang lebih baru
           dari arsip membuat arsip dianggap basi di setiap run. */
        const char *suffix = files.items[j] + strlen(eff);
        if (*suffix == '/') suffix++;
        if (*suffix && packExcluded(c, suffix)) continue;
        int64_t m = wsModeActive() ? cmdsMTimeDecide(files.items[j]) : fsMTimeNs(files.items[j]);
        if (m > newest) newest = m;
      }
      listFree(&files);
      int64_t dm = wsModeActive() ? cmdsMTimeDecide(eff) : fsMTimeNs(eff);
      if (dm > newest) newest = dm;
    }
    /* entri yang tidak ada diabaikan di sini; packRun melaporkannya. */
  }
  return newest;
}

/* ---- kompresi tarball ---- */

static bool endsWith(const char *s, const char *suffix) {
  size_t l1 = strlen(s), l2 = strlen(suffix);
  return l1 >= l2 && strcmp(s + l1 - l2, suffix) == 0;
}

static void packDeriveCompress(const Config *c, PackArtifact *a) {
  const char *v = c->pack.compress;
  if (v[0]) { /* nilai eksplisit; "bzip2" diterima sebagai "bz2" */
    copyStr(a->compress, sizeof(a->compress), strcmp(v, "bzip2") == 0 ? "bz2" : v);
    return;
  }
  if (endsWith(a->path, ".gz") || endsWith(a->path, ".tgz"))
    copyStr(a->compress, sizeof(a->compress), "gzip");
  else if (endsWith(a->path, ".xz"))
    copyStr(a->compress, sizeof(a->compress), "xz");
  else if (endsWith(a->path, ".bz2"))
    copyStr(a->compress, sizeof(a->compress), "bz2");
  else
    copyStr(a->compress, sizeof(a->compress), "none");
}

/* ---- checksum ---- */

static bool packChecksumApply(const Config *c, const char *artifactPath) {
  if (strcmp(c->pack.checksum, "sha256") != 0) {
    if (c->pack.checksum[0] && strcmp(c->pack.checksum, "none") != 0)
      fprintf(stderr, "satria: pack: checksum '%s' tidak dikenal (sha256)\n", c->pack.checksum);
    return true;
  }
  char sumPath[MAX_PATH * 2 + 8];
  snprintf(sumPath, sizeof(sumPath), "%s.sha256", artifactPath);
  artifactCacheAdd(sumPath); /* checksum = artefak tanpa kode C (§9) */
  int64_t artM = fsMTimeNs(artifactPath), sumM = fsMTimeNs(sumPath);
  if (artM >= 0 && sumM >= artM) {
    printf("%s Checksum: %s (up-to-date)\n", repPrefix(), sumPath);
    return true;
  }
  if (!packWriteSha256(artifactPath)) {
    fprintf(stderr, "satria: pack: gagal menulis %s\n", sumPath);
    return false;
  }
  printf("%s Checksum: %s\n", repPrefix(), sumPath);
  return true;
}

/* ---- packRun ---- */

/* Status rebuild packRun terakhir — lihat packLastRebuilt() di pack.h. */
static bool g_packRebuilt = false;

bool packLastRebuilt(void) { return g_packRebuilt; }

bool packRunAt(const Config *c) {
  /* Tidak ada fast state terpisah untuk fase release: freshness kemasan
     sudah divalidasi lewat mtime input di packRun (mekanisme standar). */
  return packRun(c);
}

bool packRun(const Config *c) {
  g_packRebuilt = false;
  if (!c->pack.requested) return true;

  /* Validasi entri dulu: paket dengan file yang hilang harus gagal jelas,
     bukan diam-diam memaketkan sisanya. packResolveSource menoleransi
     entri portabel tanpa .exe (bin/satria -> bin/satria.exe di Windows). */
  for (int i = 0; i < c->pack.files.count; i++) {
    char src[MAX_PATH * 2], dst[MAX_PATH * 2];
    if (!packEntryParts(c->pack.files.items[i], src, sizeof(src),
                        dst, sizeof(dst))) {
      fprintf(stderr, "satria: pack: mapping '%s' tidak valid\n", c->pack.files.items[i]);
      return false;
    }
    char eff[MAX_PATH * 2];
    if (!packResolveSource(src, eff, sizeof(eff))) {
      fprintf(stderr, "satria: pack: '%s' tidak ditemukan\n", src);
      return false;
    }
  }
  if (c->pack.files.count == 0) {
    fprintf(stderr, "satria: pack: pack.files kosong\n");
    return false;
  }

  char stage[64];
  packStageDir(stage, sizeof(stage));
  mkdirs(".satria");
  mkdirs(stage);

  int64_t newest = packNewestInput(c);
  bool wantDeb = strcmp(c->pack.format, "deb") == 0;

  char tarPath[MAX_PATH * 2];
  packTemplate(c, c->pack.output, tarPath, sizeof(tarPath));
  bool outputIsDeb = endsWith(tarPath, ".deb");

  /* Tarball dilewati hanya bila output template sendiri berakhiran .deb
     (pemakaian deb-only yang eksplisit). */
  if (!outputIsDeb) {
    PackArtifact a = {0};
    copyStr(a.path, sizeof(a.path), tarPath);
    packDeriveCompress(c, &a);
    snprintf(a.tmp, sizeof(a.tmp), "%s/out.tar.part", stage);

    int64_t artM = fsMTimeNs(a.path);
    if (newest >= 0 && artM >= newest) {
      printf("%s Package : %s (up-to-date)\n", repPrefix(), a.path);
    } else if (!packBuildTar(c, &a)) {
      return false;
    } else {
      long long sz = fsFileSize(a.path);
      printf("%s Package : %s (%.1fKB)\n", repPrefix(), a.path, sz > 0 ? (double)sz / 1024.0 : 0);
      g_packRebuilt = true;
    }
    artifactCacheAdd(a.path);
    if (!packChecksumApply(c, a.path)) return false;
  }

  if (wantDeb || outputIsDeb) {
    PackArtifact a = {0};
    a.isDeb = true;
    if (outputIsDeb)
      copyStr(a.path, sizeof(a.path), tarPath);
    else
      packDebName(tarPath, a.path, sizeof(a.path));
    snprintf(a.tmp, sizeof(a.tmp), "%s/out.deb.part", stage);

    int64_t artM = fsMTimeNs(a.path);
    if (newest >= 0 && artM >= newest) {
      printf("%s Package : %s (up-to-date)\n", repPrefix(), a.path);
    } else if (!packBuildDeb(c, &a)) {
      return false;
    } else {
      long long sz = fsFileSize(a.path);
      printf("%s Package : %s (%.1fKB)\n", repPrefix(), a.path, sz > 0 ? (double)sz / 1024.0 : 0);
      g_packRebuilt = true;
    }
    artifactCacheAdd(a.path);
    if (!packChecksumApply(c, a.path)) return false;
  } else if (c->pack.format[0] && strcmp(c->pack.format, "tar") != 0 &&
             strcmp(c->pack.format, "none") != 0) {
    fprintf(stderr, "satria: pack: format '%s' tidak dikenal (tar, deb)\n", c->pack.format);
    return false;
  }
  return true;
}

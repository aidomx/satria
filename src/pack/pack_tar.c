/*
 * pack_tar — tarball pack.output via shell tar.
 *
 * Entri pack.files diteruskan ke tar apa adanya (file maupun folder,
 * rekursif oleh tar sendiri); path tersimpan di arsip persis seperti yang
 * ditulis (bin/app tetap bin/app). Kompresi mengikuti PackArtifact:
 *   gzip -> cz, xz -> cJ, bz2 -> cj, none -> c.
 * c->pack.optimize=true: dua langkah — tar lalu compressor level ter-FASTEST
 * (gzip -1 / xz -1 / bzip2 -1); ukuran lebih besar, biaya jauh lebih murah
 * (design/release.md). Hasil ditulis ke a->tmp lalu direname ke a->path
 * (output lama tidak pernah setengah jadi).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../portability.h"
#include "../util.h"
#include "pack_internal.h"
#include "pack_stage.h"

static const char *tarFlag(const char *compress) {
  if (strcmp(compress, "gzip") == 0) return "cz";
  if (strcmp(compress, "xz") == 0) return "cJ";
  if (strcmp(compress, "bz2") == 0) return "cj";
  return "c";
}

/* compress -1 (level ter-FASTEST) atas file flat: gzip|xz|bz2 dari a.
   Nama biner mengikuti compress; akhiran .gz otomatis ditambahkan tool. */
static bool fastCompress(const char *compress, const char *flat) {
  const char *bin = strcmp(compress, "xz") == 0 ? "xz"
                    : strcmp(compress, "bz2") == 0 ? "bzip2" : "gzip";
  size_t n = strlen(bin) + strlen(flat) + 32;
  char *cmd = malloc(n);
  if (!cmd) {
    fprintf(stderr, "satria: pack: kehabisan memori\n");
    return false;
  }
  snprintf(cmd, n, "%s -1 -kf %s", bin, flat);
  bool ok = runCmd(cmd);
  free(cmd);
  return ok;
}

bool packBuildTar(const Config *c, const PackArtifact *a) {
  fsRemoveFile(a->tmp);
  char stage[128];
  snprintf(stage, sizeof(stage), ".satria/pack/tar-data");
  if (!packStageEntries(c, stage)) return false;

  /* optimize=true: dua langkah — tar lalu compressor -1. Default tetap
     satu langkah tar czf/cJ/cj (perilaku lama tidak tersentuh). */
  bool optimize = c->pack.optimize && strcmp(a->compress, "none") != 0;

  size_t n = strlen(a->tmp) + strlen(stage) + 256;
  for (int i = 0; i < c->pack.files.count; i++) {
    char src[SATRIA_MAX_PATH * 2], dst[SATRIA_MAX_PATH * 2];
    if (!packEntryParts(c->pack.files.items[i], src, sizeof(src),
                        dst, sizeof(dst))) continue;
    n += strlen(dst) + 2;
  }
  char *cmd = malloc(n);
  if (!cmd) {
    fprintf(stderr, "satria: pack: kehabisan memori\n");
    fsRemoveTree(stage);
    return false;
  }

  snprintf(cmd, n, "tar %s-%sf %s -C %s", tarDetFlags(),
           optimize ? "c" : tarFlag(a->compress), a->tmp, stage);
  for (int i = 0; i < c->pack.files.count; i++) {
    char src[SATRIA_MAX_PATH * 2], dst[SATRIA_MAX_PATH * 2];
    if (!packEntryParts(c->pack.files.items[i], src, sizeof(src),
                        dst, sizeof(dst))) continue;
    strcat(cmd, " ");
    strcat(cmd, dst);
  }

  mkparent(a->tmp);
  bool ok = runCmd(cmd);
  free(cmd);
  fsRemoveTree(stage);
  if (!ok) {
    fsRemoveFile(a->tmp);
    fprintf(stderr, "satria: pack: tar gagal untuk %s\n", a->path);
    return false;
  }

  mkparent(a->path);
  if (optimize) {
    /* compressor -1 menulis <a->tmp>.gz (gzip/xz/bzip2 -k otomatis
       menambah akhiran); arsip final ditempatkan dari situ. */
    char gz[SATRIA_MAX_PATH * 2 + 4];
    snprintf(gz, sizeof(gz), "%s.gz", a->tmp);
    fsRemoveFile(gz);
    if (!fastCompress(a->compress, a->tmp) || !fsFileExists(gz)) {
      fsRemoveFile(a->tmp);
      fsRemoveFile(gz);
      fprintf(stderr, "satria: pack: kompresi %s -1 gagal untuk %s\n", a->compress, a->path);
      return false;
    }
    fsRemoveFile(a->tmp);
    if (!fsRenameReplace(gz, a->path)) {
      fsRemoveFile(gz);
      fprintf(stderr, "satria: pack: tidak bisa menempatkan %s\n", a->path);
      return false;
    }
    return true;
  }
  if (!fsRenameReplace(a->tmp, a->path)) {
    fsRemoveFile(a->tmp);
    fprintf(stderr, "satria: pack: tidak bisa menempatkan %s\n", a->path);
    return false;
  }
  return true;
}

/*
 * pack_tar — tarball pack.output via shell tar.
 *
 * Entri pack.files diteruskan ke tar apa adanya (file maupun folder,
 * rekursif oleh tar sendiri); path tersimpan di arsip persis seperti yang
 * ditulis (bin/app tetap bin/app). Kompresi mengikuti PackArtifact:
 *   gzip -> cz, xz -> cJ, bz2 -> cj, none -> c.
 * Hasil ditulis ke a->tmp lalu direname ke a->path (output lama tidak
 * pernah setengah jadi).
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

bool packBuildTar(const Config *c, const PackArtifact *a) {
  fsRemoveFile(a->tmp);
  char stage[128];
  snprintf(stage, sizeof(stage), ".satria/pack/tar-data");
  if (!packStageEntries(c, stage)) return false;

  size_t n = strlen(a->tmp) + strlen(stage) + 64;
  for (int i = 0; i < c->pack.files.count; i++) {
    char src[MAX_PATH * 2], dst[MAX_PATH * 2];
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

  snprintf(cmd, n, "tar %sf %s -C %s", tarFlag(a->compress), a->tmp, stage);
  for (int i = 0; i < c->pack.files.count; i++) {
    char src[MAX_PATH * 2], dst[MAX_PATH * 2];
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
  if (!fsRenameReplace(a->tmp, a->path)) {
    fsRemoveFile(a->tmp);
    fprintf(stderr, "satria: pack: tidak bisa menempatkan %s\n", a->path);
    return false;
  }
  return true;
}

/*
 * embed_archive — pembangunan arsip tiap entri (fase sebelum codegen) dan
 * finalisasi entri (configFinalizeEntry).
 * Dipisah dari embed.c (audit refactor: berkas kandidat >600 baris).
 */
#include "embed.h"
#include "cmds/cmds_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache/cache_store.h"
#include "compile.h"
#include "portability.h"
#include "util.h"
/* ==================== pembangunan arsip (fase sebelum codegen) ====================
   Object per-entrilama (ld -r -b binary / <name>.embed.c) sudah DIHAPUS:
   konversi arsip -> object kini lewat codegen embedded.c (source biasa).
   Tugas fase ini hanya memastikan arsip tiap entri ada & segar sebelum
   emitEmbeddedSource membacanya. */

static bool buildEmbeddedArchiveEntryEx(const EmbeddedEntry *e, bool *rebuilt) {
  if (rebuilt) *rebuilt = false;
  if (e->usePrebuilt) {
    if (fsFileExists(e->archivePath)) return true;
    /* Embedded.<n>.file hilang HARUS gagal jelas (design: silent-swallow
       audit) — tanpa pesan ini, build "berhenti" tanpa arahan dan fase
       berikutnya memakai object embed lama. */
    fprintf(stderr,
            "satria: %s: embedded file '%s' tidak ditemukan\n",
            e->name, e->archivePath);
    return false;
  }
  if (!fsDirExists(e->src)) {
    fprintf(stderr, "satria: %s: src '%s' not found\n", e->name, e->src);
    return false;
  }
  if (embArchiveFresh(e)) {
    embEmbedCacheRecordIfMissing(e); /* self-healing bila entri hilang */
    return true;
  }

  /* File sementara (tmp arsip dan list) dibuat di .satria/, bukan di folder sumber.
     Rename tmp keluar dari folder sumber mengubah mtime folder itu tepat setelah
     arsip ditulis, sehingga arsip selalu tampak basi di run berikutnya. .satria
     tidak pernah masuk walk freshness. */
  mkdirs(".satria");
  char tmpArchive[MAX_PATH * 2 + 32];
  snprintf(tmpArchive, sizeof(tmpArchive), ".satria/embed-%s.tmp", e->name);
  fsRemoveFile(tmpArchive);
  bool ok;
  if (e->tar) {
    const char *z = (strcmp(e->ext, "gz") == 0) ? "z" : "";

    /* Snapshot the files before invoking tar.  This is required when
       archive.src is "."/"./": the archive output directory and satria's        temporary files can live inside the source tree, so `tar ... .`
       otherwise observes the tree changing while it is being read. */
    List files = {0};
    if (e->pattern[0])
      stateWalkDirCached(e->src, e->pattern, &files);
    else
      stateWalkDirCached(e->src, "", &files);
    /* Listfile tar menentukan urutan entri arsip: sortir sekali pada data
       lengkap agar arsip reprodusible antar mesin (urutan readdir tidak
       lagi melekat pada hasil). */
    listSort(&files);

    char listFile[MAX_PATH * 2 + 32];
    snprintf(listFile, sizeof(listFile), ".satria/embed-%s.list", e->name);
    FILE *lf = fopen(listFile, "w");
    if (!lf) {
      listFree(&files);
      return false;
    }

    char rel[MAX_PATH * 2];
    for (int i = 0; i < files.count; i++) {
      const char *path = files.items[i];
      embRelPath(e, path, rel, sizeof(rel));
      if (*rel && !embExcluded(e, rel)) fprintf(lf, "%s\n", rel);
    }
    fclose(lf);
    listFree(&files);

    size_t cap = strlen(tmpArchive) + strlen(e->src) + strlen(listFile) + 96;
    char *cmd = malloc(cap);
    if (!cmd) {
      fsRemoveFile(listFile);
      return false;
    }
    snprintf(cmd, cap, "tar c%sf %s -C %s -T %s", z, tmpArchive, e->src, listFile);
    ok = runCmd(cmd);
    free(cmd);
    fsRemoveFile(listFile);
  } else {
    char *cmd = malloc(strlen(tmpArchive) + strlen(e->src) + 64);
    if (!cmd) return false;
    sprintf(cmd, "tar czf %s -C %s .", tmpArchive, e->src);
    ok = runCmd(cmd);
    free(cmd);
  }
  if (!ok) {
    fsRemoveFile(tmpArchive);
    fprintf(stderr, "satria: %s: failed to archive %s\n", e->name, e->src);
    return false;
  }
  mkparent(e->archivePath);
  if (!fsRenameReplace(tmpArchive, e->archivePath)) {
    fsRemoveFile(tmpArchive);
    fprintf(stderr, "satria: %s: cannot move archive into place (%s)\n", e->name, e->archivePath);
    return false;
  }
  embArchiveStateSave(e);
  embEmbedCacheRecord(e); /* entri grup + input di caches/embed.cache */
  if (rebuilt) *rebuilt = true;
  return true;
}

bool buildEmbeddedArchivesEx(const Config *c, bool *anyRebuilt) {
  bool any = false;
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    bool rebuilt = false;
    if (!buildEmbeddedArchiveEntryEx(&c->emb[i], &rebuilt)) return false;
    if (rebuilt) any = true;
  }
  if (anyRebuilt) *anyRebuilt = any;
  return true;
}

bool buildEmbeddedArchives(const Config *c) {
  return buildEmbeddedArchivesEx(c, NULL);
}

/* ==================== finalisasi entri ==================== */

/*
 * configFinalizeEntry — turunkan archivePath & objectPath dari setting
 * arsip entri. Dipindah apa adanya dari config.c: pengetahuan penamaan
 * arsip (<dir>/<name>[.tar.<ext>]) lebih cocok tinggal di modul embed;
 * config.c tinggal memanggilnya dari configFinalize() per entri.
 */
void configFinalizeEntry(EmbeddedEntry *e, const char *buildDir) {
  if (e->usePrebuilt && e->prebuiltPath[0]) {
    /* Arsip jadi (embedded.<n>.file): jalurnya apa adanya; object tetap di
       build dir dengan nama entri. */
    copyStr(e->archivePath, sizeof(e->archivePath), e->prebuiltPath);
    snprintf(e->objectPath, sizeof(e->objectPath), "%s/%s.o", buildDir, e->name);
    return;
  }

  /* archive path: <archiveDir>/<n>[.tar.<ext>] tergantung with.tar/with.ext */
  if (e->tar) {
    if (e->ext[0])
      snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s.tar.%s", e->archiveDir,
               e->archiveName, e->ext);
    else
      snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s.tar", e->archiveDir, e->archiveName);
  } else if (e->ext[0]) {
    snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s.%s", e->archiveDir, e->archiveName,
             e->ext);
  } else {
    snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s", e->archiveDir, e->archiveName);
  }

  /* object ada di bawah build dir, mis. build/rupa_modules.o */
  snprintf(e->objectPath, sizeof(e->objectPath), "%s/%s.o", buildDir, e->archiveName);
}
/* stamp-meta: pemicu rebuild verifikasi */

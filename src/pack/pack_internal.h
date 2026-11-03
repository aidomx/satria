#ifndef SATRIA_V0_1_0_PACK_INTERNAL_H
#define SATRIA_V0_1_0_PACK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../config.h"

/*
 * pack_internal — kontrak antar-file modul pack/ (publik: pack.h).
 *
 *   pack.c       : template, freshness, checksum, dispatch artefak.
 *   pack_tar.c   : tarball via shell tar (cz/c/cJ/cj).
 *   pack_deb.c   : staging control+data, tar cz, ar archive (debian-binary,
 *                  control.tar.gz, data.tar.gz) ditulis sendiri di C.
 *   pack_sha256.c: SHA-256 murni C + penulis file .sha256.
 */

/* Satu artefak hasil resolusi template pack.output. */

/* packResolveSource — resolve path SUMBER entri pack.files. Entri boleh
   ditulis portabel tanpa akhiran .exe: di Windows binary link menghasilkan
   <name>.exe, sehingga `bin/satria:bin/satria` menunjuk bin/satria.exe. Path apa
   adanya dicek dulu; bila tidak ada, coba dengan akhiran .exe (fallback
   lintas host, tanpa #ifdef, agar Buildfile yang sama jalan di mana pun).
   Return true bila path efektif (di out) ada; false bila keduanya tidak —
   pemanggil melaporkan error memakai path asli. */
bool packResolveSource(const char *src, char *out, size_t n);
typedef struct {
  char path[SATRIA_MAX_PATH * 2]; /* output final, relatif root proyek */
  char tmp[SATRIA_MAX_PATH * 2];  /* file sementara, direname setelah jadi */
  bool isDeb;
  char compress[16]; /* gzip|none|xz|bz2 — tarball saja */
} PackArtifact;

/* Ganti {name} {version} {os} {arch} di template `tpl` dengan nilai host. */
void packTemplate(const Config *c, const char *tpl, char *out, size_t n);

/* Nama .deb dari path tarball: ekstensi tar dikenal (.tar.gz/.tgz/.tar.xz/
   .tar.bz2/.tar) diganti .deb; tanpa ekstensi dikenal -> <path>.deb. */
void packDebName(const char *tarPath, char *out, size_t n);

/* Direktori staging bersama: ".satria/pack". */
void packStageDir(char *out, size_t n);

/* checksum sha256 -> "<path>.sha256" berisi "<hex>  <basename>" (dua spasi,
   kompatibel `sha256sum -c`). */
bool packWriteSha256(const char *path);

/* Pembangun artefak: tulis file final di a->path, true bila sukses. */
bool packBuildTar(const Config *c, const PackArtifact *a);
bool packBuildDeb(const Config *c, const PackArtifact *a);

/* mtime ns terbesar di antara source entri pack.files (folder di-walk rekursif);
   -1 bila tidak ada entri valid. */
int64_t packNewestInput(const Config *c);

/* mtime ns file, -1 bila tidak ada. */
int64_t packPathMTimeNs(const char *path);

/* ============ parsing (pack_config.c; dipakai config.c) ============ */

/* Setel string pack; sepasang kutip pembuka/penutup dilepas. */
void packSetStr(char *dst, size_t n, const char *value);
/* pack.files = a, b, c — daftar dipisah koma. */
void packAddFiles(PackConfig *p, const char *value);
/* Key deb.* (pack.deb.* dan key bare deb.*). */
void packApplyDeb(Config *c, const char *key, const char *value);
/* Key pack.* tanpa sub (section pack, section release, maupun key bare
   top-level). */
void packApply(Config *c, const char *key, const char *value);
/* List List milik PackConfig (files, excludes) — dipanggil pemakai yang
   memakai ulang Config (mis. fase release workspace). */
void packFreeConfig(PackConfig *p);

#endif /* SATRIA_V0_1_0_PACK_INTERNAL_H */

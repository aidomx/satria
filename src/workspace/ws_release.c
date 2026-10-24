/* ws_release.c — Fase release workspace: kemas satu paket dari root workspace lewat pack engine.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"
#include "../cache/artifact.h"


/*
 * wsRelease — fase release workspace (design/release.md revisi 2):
 * kemas SATU paket dari root workspace lewat pack engine yang sama,
 * dengan konfigurasi release.* (key identik pack.*).
 *
 * Sumber file:
 *   1. Baris "files = ..." di release.* (entri polos relatif root
 *      workspace; entri ../<n>/... di-strip ../ jadi <n>/...).
 *   2. Bila tanpa baris files: default = artefak build semua proyek —
 *      bin/<binaryName> tiap proyek binary + lib/<libraryName>.a/.so
 *      tiap proyek library. Proyek archive/tanpa output tidak menyumbang.
 *
 * Baris release lain (name/version/output/format/compress/checksum/
 * exclude/deb.*) diteruskan apa adanya ke Config — pack engine yang
 * mengartikan. prefix dikonversi ke pack.* agar parser lama mengenali.
 */
int wsRelease(WsModel *m, int jobs, const char *wsDir) {
  (void)jobs;

  Config c = configDefaults();

  /* Baris release -> pack.* ke Config. Baris files butuh perawatan
     path: ../<n>/x di-strip ../ (root workspace = induk folder proyek). */
  for (int i = 0; i < m->release.lines.count; i++) {
    char buf[WS_LINE_LEN * 2];
    copyStr(buf, sizeof(buf), m->release.lines.items[i]);
    char *eq = strchr(buf, '=');
    if (!eq) continue;
    *eq = '\0';
    char *k = trim(buf);
    char *v = trim(eq + 1);
    if (!*k) continue;
    if (strcmp(k, "files") == 0) {
      for (char *save = v;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item) {
          const char *path = item;
          if (strncmp(item, "../", 3) == 0) path = item + 3;
          if (!listAdd(&c.pack.files, path)) goto oom;
          /* Sisi src (sebelum ':') -> artifact.cache (§9): input release di
             luar fingerprint (mis. file teks) ikut divalidasi no-op. */
          char art[WS_LINE_LEN * 2];
          copyStr(art, sizeof(art), path);
          char *colon = strchr(art, ':');
          if (colon) *colon = '\0';
          if (art[0]) artifactCacheAdd(art);
        }
        if (!comma) break;
        save = comma + 1;
      }
    } else {
      if (strcmp(k, "name") == 0)
        packApply(&c, "name", v);
      else if (strcmp(k, "version") == 0)
        packApply(&c, "version", v);
      else if (strcmp(k, "output") == 0)
        packApply(&c, "output", v);
      else if (strcmp(k, "compress") == 0)
        packApply(&c, "compress", v);
      else if (strcmp(k, "checksum") == 0)
        packApply(&c, "checksum", v);
      else if (strcmp(k, "format") == 0)
        packApply(&c, "format", v);
      else if (strcmp(k, "exclude") == 0)
        packApply(&c, "exclude", v);
      else if (strncmp(k, "deb.", 4) == 0)
        packApplyDeb(&c, k + 4, v);
      else
        fprintf(stderr, "satria: release: key '%s' tidak dikenal (padanan pack.*)\n", k);
    }
  }

  /* Default files: artefak build semua proyek (bila tidak ada files). */
  if (c.pack.files.count == 0) {
    for (int i = 0; i < m->projectCount; i++) {
      const WsProject *pp = &m->projects[i];
      /* isArchive dihitung penuh lebih dulu (helper bersama): proyek
         kemasan tidak pernah punya entri lib default, apa pun urutan
         baris di Buildfile.ws. */
      bool isArchive = wsProjectArchiveOnly(pp);
      for (int l = 0; l < pp->lines.count; l++) {
        const char *ln = pp->lines.items[l];
        if (strncmp(ln, "output.libraryName = ", 21) == 0 && !isArchive) {
          /* Library di root workspace: lib/lib<name>.a/.so (design v1). */
          char libEntry[WS_LINE_LEN * 2];
          const char *libName = ln + 21;
          int w1 = snprintf(libEntry, sizeof(libEntry), "lib/lib%s.a", libName);
          if (w1 > 0 && (size_t)w1 < sizeof(libEntry) && !listAdd(&c.pack.files, libEntry))
            goto oom;
          char shEntry[WS_LINE_LEN * 4];
          int w2 = snprintf(shEntry, sizeof(shEntry), "lib/lib%s.so", libName);
          if (w2 > 0 && (size_t)w2 < sizeof(shEntry) && !listAdd(&c.pack.files, shEntry)) goto oom;
        }
      }
      if (!isArchive) {
        /* Binary di root workspace: bin/<binaryName>; default = nama proyek
           (sama dengan wsBuildConfig). */
        const char *binName = NULL;
        for (int l = 0; l < pp->lines.count; l++) {
          const char *ln = pp->lines.items[l];
          if (strncmp(ln, "output.binaryName = ", 20) == 0) binName = ln + 20;
        }
        if (!binName) binName = pp->name;
        char binEntry[WS_LINE_LEN * 3];
        int w = snprintf(binEntry, sizeof(binEntry), "bin/%s", binName);
        if (w > 0 && (size_t)w < sizeof(binEntry) && !listAdd(&c.pack.files, binEntry)) goto oom;
      }
    }
  }

  c.pack.requested = true;
  if (!c.pack.name[0]) {
    /* produk utter: nama folder workspace */
    char cwd[MAX_PATH];
    if (fsGetCwd(cwd, sizeof(cwd))) {
      const char *base = strrchr(cwd, '/');
      base = base ? base + 1 : cwd;
      copyStr(c.pack.name, sizeof(c.pack.name), base);
    }
  }
  if (!c.pack.version[0]) copyStr(c.pack.version, sizeof(c.pack.version), "0.0.0");
  if (!c.pack.output[0])
    copyStr(c.pack.output, sizeof(c.pack.output), "dist/{name}-v{version}.tar.gz");
  if (!c.pack.debInstallPrefix[0])
    copyStr(c.pack.debInstallPrefix, sizeof(c.pack.debInstallPrefix), "/usr/local");

  printf("\n%s Release : %s (workspace%s%s)\n", repPrefix(), c.pack.name,
         c.pack.format[0] ? ", format " : "", c.pack.format[0] ? c.pack.format : "");

  /* packRunAt berjalan di CWD sekarang — pastikan itu root workspace. */
  if (!fsSetCwd(wsDir)) {
    fprintf(stderr, "satria: cannot enter workspace root\n");
    packFreeConfig(&c.pack);
    return 1;
  }
  bool ok = packRunAt(&c);
  packFreeConfig(&c.pack);
  if (!ok) fprintf(stderr, "satria: workspace: release gagal\n");
  return ok ? 0 : 1;

oom:
  fprintf(stderr, "satria: workspace: kehabisan memori (release)\n");
  packFreeConfig(&c.pack);
  return 1;
}

/* ws_validate.c — Validasi model: pack.merge per proyek, validasi release, dan lintas proyek.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"

static bool wsProjectPackFiles(const WsProject *p, List *out);

/* ==================== pack.merge ==================== */
static bool wsProjectPackFiles(const WsProject *p, List *out) {
  bool found = false;
  for (int i = 0; i < p->lines.count; i++) {
    const char *ln = p->lines.items[i];
    if (strncmp(ln, "pack.files = ", 13) != 0) continue;
    const char *v = ln + 13;
    char buf[WS_LINE_LEN * 2];
    if (strlen(v) >= sizeof(buf)) return false;
    copyStr(buf, sizeof(buf), v);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item && !listAdd(out, item)) return false;
      if (!comma) break;
      save = comma + 1;
    }
    found = true;
  }
  return found;
}
bool wsValidatePackMerges(WsModel *m) {
  for (int i = 0; i < m->projectCount; i++) {
    WsProject *owner = &m->projects[i];
    for (int j = 0; j < owner->lines.count; j++) {
      const char *ln = owner->lines.items[j];
      if (strncmp(ln, "pack.merge = ", 13) != 0) continue;
      const char *v = ln + 13;
      char buf[WS_LINE_LEN * 2];
      if (strlen(v) >= sizeof(buf)) return false;
      copyStr(buf, sizeof(buf), v);
      for (char *save = buf;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item) {
          int dep = wsFindProject(m, item);
          if (dep < 0) {
            fprintf(stderr, "satria: workspace: '%s' pack.merge unknown project '%s'\n", owner->name,
                    item);
            return false;
          }
          List files = {0};
          bool ok = wsProjectPackFiles(&m->projects[dep], &files);
          listFree(&files);
          if (!ok) {
            fprintf(stderr, "satria: workspace: '%s' pack.merge project '%s' has no pack.files\n",
                    owner->name, item);
            return false;
          }
          /* merge is also a build dependency: the merged package's files
             must exist before the owner's pack phase runs. */
          bool exists = false;
          for (int k = 0; k < m->depends[i].count; k++)
            if (strcmp(m->depends[i].items[k], item) == 0) exists = true;
          if (!exists && !listAdd(&m->depends[i], item)) return false;
        }
        if (!comma) break;
        save = comma + 1;
      }
    }
  }
  return true;
}
/* ==================== validasi release ==================== */
/* release.* konfigurasi tunggal — validasi ringan: format (bila diset)
   harus tar/deb. Paket tanpa files -> fase release pakai default artefak
   build proyek (packNewestInput waktu itu juga dilaporkan). */
bool wsValidateRelease(WsModel *m) {
  for (int i = 0; i < m->release.lines.count; i++) {
    const char *ln = m->release.lines.items[i];
    if (strncmp(ln, "format = ", 9) != 0) continue;
    const char *fmt = ln + 9;
    if (*fmt && strcmp(fmt, "tar") != 0 && strcmp(fmt, "deb") != 0 && strcmp(fmt, "none") != 0) {
      fprintf(stderr, "satria: workspace: release.format '%s' tidak dikenal (tar, deb)\n", fmt);
      return false;
    }
    if (strcmp(fmt, "none") == 0) {
      fprintf(stderr, "satria: workspace: release.format 'none' tidak bermakna di workspace\n");
      return false;
    }
  }
  return true;
}

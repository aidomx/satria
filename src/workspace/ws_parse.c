/* ws_parse.c — Parser Buildfile.ws: baris setting, section engine, dan pembacaan file.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"

static void wsRecordLine(WsModel *m, const char *key, const char *value);

/* Section engine yang dikenali Config — segmen pertama key dengan salah
   satu nama ini adalah default bersama, bukan nama proyek. Nama proyek
   tidak boleh memakai nama-nama ini. */
bool wsIsEngineSection(const char *seg) {
  static const char *names[] = {"root",       "std",      "foreground", "target",  "clean",
                                "progress",   "output",   "embedded",   "archive", "sources",
                                "flags",      "compiler", "exclude",    "headers", "library",
                                "depends_on", "release",  NULL};
  for (int i = 0; names[i]; i++)
    if (strcmp(seg, names[i]) == 0) return true;
  return false;
}
/* ==================== parse Buildfile.ws ==================== */
/* Satu baris setting. Tiga bentuk diterima (alias di-resolve lebih dulu):
     projects.<nama>.<setting> = v   (bentuk kanonik)
     <nama>.<setting> = v            (use workspace mewakili namespace —
                                      segmen pertama yang bukan section
                                      engine = nama proyek; sama persis
                                      dengan use project)
     <section> = v                   (default bersama semua proyek) */
static void wsRecordLine(WsModel *m, const char *key, const char *value) {
  char name[WS_NAME_LEN];
  const char *setting = NULL;

  if (strcmp(key, "projects") == 0) return; /* daftar proyek, bukan setting */

  /* releases = rupa, ruka — sintaks revisi 1 (daftar release terikat
     proyek) SUDAH DIHAPUS (design/release.md revisi 2). Pesan migrasi
     di sini supaya Buildfile.ws lama gagal JELAS, bukan diam-diam
     salah kemas. */
  if (strcmp(key, "releases") == 0 || strncmp(key, "releases.", 9) == 0) {
    if (!wsGuardQuiet)
      fprintf(stderr, "satria: workspace: sintaks 'releases' tidak lagi didukung\n"
                      "        gunakan section release.* — SATU paket workspace\n"
                      "        (release.name/files/exclude/format; lihat design/release.md)\n");
    return;
  }

  /* release.* — konfigurasi release tunggal (baris disimpan apa adanya,
     tanpa prefix, karena fase release memakai parser pack standar). */
  if (strcmp(key, "release") == 0) return; /* section marker saja */
  if (strncmp(key, "release.", 8) == 0) {
    m->release.present = true;
    char buf[WS_LINE_LEN];
    snprintf(buf, sizeof(buf), "%s = %s", key + 8, value);
    listAdd(&m->release.lines, buf);
    return;
  }

  if (strncmp(key, "projects.", 9) == 0) {
    const char *rest = key + 9;
    const char *dot = strchr(rest, '.');
    if (!dot || dot == rest) return;
    size_t nl = (size_t)(dot - rest);
    if (nl >= sizeof(name)) return;
    memcpy(name, rest, nl);
    name[nl] = '\0';
    setting = dot + 1;
  } else {
    const char *dot = strchr(key, '.');
    if (!dot) {
      /* root di level workspace DIBAULKAN (warning + abaikan): build
         selalu berjalan dengan cwd = folder proyek, jadi root berlaku
         per proyek — nilai workspace-level tidak mengagregasi output
         ke root workspace. Agregasi lewat path per proyek (../bin). */
      if (strcmp(key, "root") == 0) {
        if (!wsGuardQuiet)
          fprintf(stderr, "satria: workspace: 'root' diabaikan (root berlaku per proyek — "
                          "deklarasikan per proyek: <nama>.root = ...; output tetap "
                          "root-centric otomatis: bin/, build/, lib/ di root workspace)\n");
        return;
      }
      if (strcmp(key, "log") == 0 || wsIsEngineSection(key))
        wsAddCommon(m, key, value);
      return;
    }
    size_t nl = (size_t)(dot - key);
    if (nl == 0 || nl >= sizeof(name)) return;
    memcpy(name, key, nl);
    name[nl] = '\0';
    if (wsIsEngineSection(name)) {
      wsAddCommon(m, key, value);
      return;
    }
    setting = dot + 1;
  }

  WsProject *p = wsEnsureProject(m, name);
  if (!p) return;
  /* <n>.root: dieksekusi oleh wsExecOne (chdir dari root workspace) dan
     TIDAK ditulis ke Buildfile sintesis — file sintesis selalu root = "."
     terhadap cwd proyek, jadi root di situ diterapkan dua kali. */
  if (strcmp(setting, "root") == 0) {
    copyStr(p->root, sizeof(p->root), value);
    return;
  }
  char buf[WS_LINE_LEN];
  snprintf(buf, sizeof(buf), "%s = %s", setting, value);
  listAdd(&p->lines, buf);
}
/* Parse file workspace di cwd. Alias di-resolve dengan mesin alias
   yang sama dengan use project (projects.rupamod as mod). */
bool wsParse(WsModel *m, const char *path) {
  memset(m, 0, sizeof(*m));

  FILE *fp = fopen(path, "r");
  if (!fp) {
    if (!wsGuardQuiet) fprintf(stderr, "satria: cannot open %s\n", path);
    return false;
  }

  List aliases = {0};
  char line[WS_LINE_LEN];
  while (fgets(line, sizeof(line), fp)) {
    char *comment = strchr(line, '#');
    if (comment) *comment = '\0';
    char *text = trim(line);
    if (!*text) continue;

    /* Komentar // (gaya use alias) dibuang sebelum apa pun. */
    char *slash2 = strstr(text, "//");
    if (slash2) *slash2 = '\0';
    if (!*trim(text)) continue;

    char *eq = strchr(text, '=');
    if (!eq) {
      aliasLineAdd(&aliases, text); /* baris alias "X as Y" */
      continue;
    }
    *eq = '\0';
    char *k = trim(text);
    char *v = trim(eq + 1);
    if (!*k) continue;

    char key[ALIAS_KEY_MAX];
    if (strlen(k) >= sizeof(key)) continue;
    copyStr(key, sizeof(key), k);
    if (strcmp(key, "projects") == 0) {
      for (char *save = v;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item && !wsEnsureProject(m, item)) {
          if (!wsGuardQuiet)
            fprintf(stderr, "satria: workspace: invalid project name '%s'\n", item);
          fclose(fp);
          listFree(&aliases);
          return false;
        }
        if (!comma) break;
        save = comma + 1;
      }
      continue;
    }
    aliasResolve(&aliases, key, sizeof(key));
    wsRecordLine(m, key, v);
  }
  fclose(fp);
  listFree(&aliases);
  return true;
}

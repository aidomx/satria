/* ws_config.c — Susun Config per proyek di memori dari model (library shorthand, baris, BuildConfig).
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"

static void wsApplyLine(const WsModel *m, const WsProject *p, Config *c, const char *key,
                        const char *value, bool global);

/* ==================== sintesis Buildfile per proyek ==================== */
/* Ubah shorthand library workspace: `library.linux = ruka` menjadi path
   artifact project (`../ruka/lib/libruka.a`). Nilai project tidak boleh
   diterjemahkan menjadi `-lruka`, karena linker berada di root project
   consumer dan tidak otomatis mengetahui libDir project lain. */
bool wsLibraryValue(const WsModel *m, const WsProject *owner, const char *value, char *out,
                    size_t n) {
  (void)owner;
  out[0] = '\0';
  const char *p = value;
  bool first = true;
  while (*p) {
    const char *comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    while (len && (p[0] == ' ' || p[0] == '\t')) {
      p++;
      len--;
    }
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
      len--;
    char item[WS_LINE_LEN];
    if (len >= sizeof(item)) return false;
    memcpy(item, p, len);
    item[len] = '\0';
    const WsProject *dep = NULL;
    for (int i = 0; i < m->projectCount; i++) {
      if (strcmp(m->projects[i].name, item) == 0) {
        dep = &m->projects[i];
        break;
      }
    }
    char mapped[WS_LINE_LEN];
    if (dep) {
      char libName[WS_NAME_LEN];
      char libDir[WS_LINE_LEN];
      snprintf(libName, sizeof(libName), "%s", dep->name);
      snprintf(libDir, sizeof(libDir), "lib");
      for (int j = 0; j < dep->lines.count; j++) {
        const char *ln = dep->lines.items[j];
        if (strncmp(ln, "output.libraryName = ", 21) == 0)
          snprintf(libName, sizeof(libName), "%s", ln + 21);
        else if (strncmp(ln, "output.libDir = ", 16) == 0)
          snprintf(libDir, sizeof(libDir), "%s", ln + 16);
      }
      /* Path artifact lintas-proyek relatif ROOT WORKSPACE: library
         ditulis ke <libDir>/ di root workspace (design/workspace.md,
         "lib/ di root"), jadi path-nya <libDir>/lib<name>.a — tanpa nama
         folder proyek di depannya. Size dicek dulu agar path panjang
         tidak terpotong snprintf. */
      {
        const char *libPrefix = "/lib";
        const char *suffix = ".a";
        size_t need = strlen(libDir) + strlen(libPrefix) + strlen(libName) + strlen(suffix) + 1;
        if (need > sizeof(mapped)) return false;
        char *dst = mapped;
        const char *parts[] = {libDir, libPrefix, libName, suffix};
        for (size_t k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
          size_t partLen = strlen(parts[k]);
          memcpy(dst, parts[k], partLen);
          dst += partLen;
        }
        *dst = '\0';
      }
    } else {
      snprintf(mapped, sizeof(mapped), "%s", item);
    }
    if (!first) strncat(out, ", ", n - strlen(out) - 1);
    strncat(out, mapped, n - strlen(out) - 1);
    first = false;
    if (!comma) break;
    p = comma + 1;
  }
  return true;
}
/* pack.* per proyek di luar scope v1 — peringatan sekali per run. */
static bool wsPackWarned = false;
/* Terapkan SATU baris model (key sudah kanonik) ke Config dengan skoping
   per-kunci (lihat komentar seksi di atas). */
static void wsApplyLine(const WsModel *m, const WsProject *p, Config *c, const char *key,
                        const char *value, bool global) {
  char mapped[WS_LINE_LEN * 4];

  if (strncmp(key, "pack.", 5) == 0) {
    if (!wsPackWarned) {
      wsPackWarned = true;
      fprintf(stderr, "satria: workspace: pack.* per proyek diabaikan di mode workspace "
                      "(lihat design/workspace_v1.md)\n");
    }
    return;
  }
  if (strcmp(key, "cdeps") == 0) return; /* dikonsumsi wsApplyCdeps */
  /* headers.each = X: X diterapkan ke SETIAP proyek, diresolve relatif root
     tiap proyek (I. = <proyek>/include). Setara dengan menulis <proyek>.headers = X
     untuk setiap proyek, tanpa mengulang penulisan. */
  if (strcmp(key, "headers.each") == 0) {
    /* headers.each = [nama-proyek, ...] [I. | auto | path, ...]
       - Token yang cocok dengan nama proyek = PENYARING: hanya proyek itu yang
         menerima sisanya. Tanpa nama = semua proyek.
       - I. = <proyek>/include (-I), auto = I. bila include/ ada, path lain =
         dilacak relatif root proyek.
       - Token yang bukan nama proyek dan tidak ada sebagai folder di proyek mana
         pun diberi peringatan (salah tulis tidak lolos diam-diam). */
    char list[WS_LINE_LEN];
    if (strlen(value) >= sizeof(list)) return;
    snprintf(list, sizeof(list), "%s", value);
    bool hasNames = false, applies = false;
    char others[WS_LINE_LEN] = "";
    char *save = NULL;
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
      while (*t == ' ' || *t == '\t')
        t++;
      size_t tl = strlen(t);
      while (tl > 0 && (t[tl - 1] == ' ' || t[tl - 1] == '\t'))
        t[--tl] = '\0';
      if (tl == 0) continue;
      bool isName = false;
      for (int k = 0; k < m->projectCount; k++)
        if (strcmp(m->projects[k].name, t) == 0) {
          isName = true;
          break;
        }
      if (isName) {
        hasNames = true;
        if (strcmp(p->name, t) == 0) applies = true;
        continue;
      }
      /* bukan nama proyek: harus berupa flag (I./auto) atau folder yang ada */
      if (strcmp(t, "I.") != 0 && strcmp(t, "auto") != 0) {
        bool found = false;
        for (int k = 0; k < m->projectCount && !found; k++) {
          const char *r = m->projects[k].root[0] ? m->projects[k].root : ".";
          char probe[WS_PATH_LEN];
          if ((size_t)snprintf(probe, sizeof(probe), "%s/%s", r, t) < sizeof(probe) &&
              fsDirExists(probe))
            found = true;
        }
        /* peringatan dicetak sekali per nama (baris ini dipanggil per proyek/pass) */
        static char warnedTok[32][WS_LINE_LEN];
        static int nwarned = 0;
        bool already = false;
        for (int w = 0; w < nwarned; w++)
          if (strcmp(warnedTok[w], t) == 0) already = true;
        if (!found && !already && nwarned < 32) {
          snprintf(warnedTok[nwarned++], WS_LINE_LEN, "%s", t);
          fprintf(stderr,
                  "satria: workspace: headers.each: '%s' bukan nama proyek dan tidak ada "
                  "sebagai folder di proyek mana pun, dilewati\n",
                  t);
        }
      }
      if (strlen(others) + strlen(t) + 4 < sizeof(others)) {
        if (others[0]) strcat(others, ", ");
        strcat(others, t);
      }
    }
    if (hasNames && !applies) return;
    if (others[0] == '\0') return;
    /* auto: I. hanya bila include/ proyek ada */
    char finalv[WS_LINE_LEN] = "";
    char tmp[WS_LINE_LEN];
    snprintf(tmp, sizeof(tmp), "%s", others);
    char *s2 = NULL;
    for (char *t = strtok_r(tmp, ",", &s2); t; t = strtok_r(NULL, ",", &s2)) {
      while (*t == ' ')
        t++;
      if (strcmp(t, "auto") == 0) {
        char inc[WS_PATH_LEN];
        const char *r = p->root[0] ? p->root : ".";
        if ((size_t)snprintf(inc, sizeof(inc), "%s/include", r) >= sizeof(inc)) continue;
        if (!fsDirExists(inc)) continue;
        t = (char *)"I.";
      }
      if (strlen(finalv) + strlen(t) + 4 < sizeof(finalv)) {
        if (finalv[0]) strcat(finalv, ", ");
        strcat(finalv, t);
      }
    }
    if (finalv[0]) wsApplyLine(m, p, c, "headers", finalv, false);
    return;
  }
  /* headers.internal = folder shared di root workspace -> publik lewat
     `headers` supaya ikut -I (kriteria: shared headers di-root ditambahkan
     ke -I). Value relatif root workspace — tanpa prefix. */
  if (strcmp(key, "headers.internal") == 0) {
    configApplyLine(c, "headers.internal", value); /* dilacak, bukan -I */
    return;
  }

  const char *root = p->root[0] ? p->root : ".";

  if (strncmp(key, "sources", 7) == 0 && (key[7] == '\0' || key[7] == '.')) {
    if (wsMapList(root, value, false, mapped, sizeof(mapped))) configApplyLine(c, key, mapped);
    return;
  }
  if (strncmp(key, "headers", 7) == 0 && (key[7] == '\0' || key[7] == '.')) {
    char pub[WS_LINE_LEN], intl[WS_LINE_LEN];
    bool split = key[7] == '\0';
    if (wsMapHeaders(root, value, global, p->name, split, pub, intl, sizeof(pub))) {
      if (pub[0]) configApplyLine(c, key, pub);
      if (intl[0]) configApplyLine(c, "headers.internal", intl);
    }
    return;
  }
  if (strcmp(key, "archive.src") == 0) {
    if (wsIsRelPath(value)) {
      if (strcmp(value, ".") == 0)
        snprintf(mapped, sizeof(mapped), "%s", root);
      else if ((size_t)snprintf(mapped, sizeof(mapped), "%s/%s", root, value) >= sizeof(mapped))
        return;
      configApplyLine(c, key, mapped);
      return;
    }
    configApplyLine(c, key, value);
    return;
  }
  if (strncmp(key, "embedded.", 9) == 0) {
    /* embedded.<n>.src = konten proyek (prefix root proyek); .file/.dir/
       .extract dst lintas-proyek (apa adanya, relatif root workspace). */
    size_t kl = strlen(key);
    if (kl >= 4 && strcmp(key + kl - 4, ".src") == 0 && wsIsRelPath(value)) {
      if (strcmp(value, ".") == 0)
        snprintf(mapped, sizeof(mapped), "%s", root);
      else if ((size_t)snprintf(mapped, sizeof(mapped), "%s/%s", root, value) >= sizeof(mapped))
        return;
      configApplyLine(c, key, mapped);
      return;
    }
    configApplyLine(c, key, value);
    return;
  }
  if (strncmp(key, "library", 7) == 0 && (key[7] == '\0' || key[7] == '.')) {
    /* library.<os> = <nama proyek> -> path artifact; item non-proyek
       (ssl, m, pthread, ...) lewat apa adanya. */
    if (wsLibraryValue(m, p, value, mapped, sizeof(mapped))) configApplyLine(c, key, mapped);
    return;
  }
  /* Lainnya (compiler/flags/defines/std/exclude/output./clean./progress/
     foreground/target/depends_on): diterapkan apa adanya. */
  configApplyLine(c, key, value);
}
/* Susun Config proyek `idx` dari model Buildfile.ws — tanpa file sintesis,
   tanpa loadConfig. Urutan: defaults -> baris bersama -> baris proyek ->
   cdeps -> fixup output/konvensi -> configFinalize -> resolveCompiler
   (dipanggil pemanggil, wsRunProject). */
bool wsBuildConfig(const WsModel *m, int idx, Config *c) {
  const WsProject *p = &m->projects[idx];
  if (!p->root[0]) return false;
  *c = configDefaults();

  for (int pass = 0; pass < 2; pass++) {
    const List *lines = pass == 0 ? &m->commonLines : &p->lines;
    for (int i = 0; i < lines->count; i++) {
      char buf[WS_LINE_LEN * 2];
      char key[WS_LINE_LEN];
      if (strlen(lines->items[i]) >= sizeof(buf)) continue;
      copyStr(buf, sizeof(buf), lines->items[i]);
      char *val = NULL;
      if (!wsSplitKV(buf, key, sizeof(key), &val)) continue;
      wsApplyLine(m, p, c, key, val, pass == 0);
    }
  }
  wsApplyCdeps(m, p, c);

  const char *root = p->root[0] ? p->root : ".";
  /* Root proyek anggota -> projRoot (basis path sumber/object). `root` tetap
     "." agar tidak dianggap cwd build (cmdBuildEx). */
  if (p->root[0] && strcmp(p->root, ".") != 0)
    copyStr(c->projRoot, sizeof(c->projRoot), p->root);
  /* Output root-centric: object di build/<proyek> (agar nama source antar-proyek tidak bertabrakan); bin/ & lib/ sudah root lewat default. */
  if (strcmp(c->outBuildDir, "build") == 0)
    snprintf(c->outBuildDir, sizeof(c->outBuildDir), "build/%s", p->name);
  /* Nama binary default = nama proyek (bukan basename cwd = root ws). */
  if (!c->outBinaryName[0]) copyStr(c->outBinaryName, sizeof(c->outBinaryName), p->name);
  /* Konvensi sources/headers berbasis root PROYEK — configFinalize berjalan
     dengan cwd root workspace, jadi defaults disediakan dahulu. */
  if (c->sources.count == 0) {
    char defSrc[MAX_PATH * 2];
    if (strcmp(root, ".") == 0)
      snprintf(defSrc, sizeof(defSrc), "src");
    else
      snprintf(defSrc, sizeof(defSrc), "%s/src", root);
    listAdd(&c->sources, defSrc);
  }
  /* Workspace-root include/ is conventional, even with explicit headers. */
  if (fsDirExists("include")) {
    bool present = false;
    for (int i = 0; i < c->headerPublic.count; i++)
      if (strcmp(c->headerPublic.items[i], "include") == 0) present = true;
    if (!present) listAdd(&c->headerPublic, "include");
  }
  bool hdrDeclared = c->headerPublic.count > 0;
  if (!hdrDeclared) {
    char incDir[MAX_PATH * 2];
    if (strcmp(root, ".") == 0)
      snprintf(incDir, sizeof(incDir), "include");
    else
      snprintf(incDir, sizeof(incDir), "%s/include", root);
    if (fsDirExists(incDir)) listAdd(&c->headerPublic, incDir);
  }
  int hpBefore = c->headerPublic.count;

  configFinalize(c);

  /* configFinalize bisa menyuntik default "include" telanjang (berbasis
     cwd root workspace) bila proyek tak mendeklarasikan headers — buang;
     default sah hanya <root>/include di atas. */
  if (!hdrDeclared && c->headerPublic.count > hpBefore && c->headerPublic.count > 0 &&
      strcmp(c->headerPublic.items[0], "include") == 0) {
    memmove(c->headerPublic.items, c->headerPublic.items + 1,
            sizeof(char *) * (size_t)(c->headerPublic.count - 1));
    c->headerPublic.count--;
  }
  return true;
}

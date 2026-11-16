/* ws_parse.c — Parser WsBuildfile: baris setting, section engine, dan pembacaan file.
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
/* ==================== parse WsBuildfile ==================== */
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
     di sini supaya workspace lama gagal JELAS, bukan diam-diam
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
      /* `background = true` (design/background.md): setting global yang
         hanya berlaku di root workspace — diterima seperti `log`. */
      if (strcmp(key, "log") == 0 || strcmp(key, "background") == 0 ||
          wsIsEngineSection(key))
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
  /* <proyek>.lang = c|cpp|c++|hybrid: override bahasa per proyek. Nilai salah
     ditolak dengan pesan — bukan diabaikan diam-diam (proyek lalu jatuh ke
     default `use workspace`). */
  if (strcmp(setting, "lang") == 0) {
    WsLang l;
    if (!wsParseLang(value, &l)) {
      if (!wsGuardQuiet)
        fprintf(stderr,
                "satria: workspace: '%s.lang = %s' tidak dikenal (pakai: c | cpp | c++ | hybrid) "
                "— diabaikan, memakai default workspace\n",
                name, value);
      return;
    }
  }
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
/* ---- daftar release.files multi-baris ----------------------------------
   Format rapi yang lazim:

       release.files =
       \tbin/rupa,
       \tbin/bade:bin/bade

   atau nilai awal + lanjutan ber-indentasi. Setiap baris lanjutan (tanpa
   '=') digabung dengan koma; koma/spasi/tab pemisah tidak pernah menjadi
   bagian nama entri. Tanpa ini baris lanjutan dibuang diam-diam dan entri
   yang hilang membuat daftar jatuh ke default (semua artefak proyek). */
static void wsFilesAppend(char *buf, size_t n, size_t *len, const char *item) {
  char tmp[WS_LINE_LEN];
  copyStr(tmp, sizeof(tmp), item);
  char *s = trim(tmp);
  size_t l = strlen(s);
  while (l > 0 && (s[l - 1] == ',' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = '\0';
  s = trim(s);
  if (!*s) return;
  int w = snprintf(buf + *len, *len < n ? n - *len : 0, "%s%s", *len ? ", " : "", s);
  if (w > 0) {
    size_t add = (size_t)w;
    *len = (*len + add < n) ? *len + add : n - 1;
  }
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
  int lineno = 0;
  /* Penampung `release.files` yang nilainya berlanjut di baris berikutnya. */
  bool filesOpen = false;
  char filesVal[WS_LINE_LEN * 4] = "";
  size_t filesLen = 0;
  while (fgets(line, sizeof(line), fp)) {
    lineno++;
    char *comment = strchr(line, '#');
    if (comment) *comment = '\0';
    int indent = 0;
    for (const char *q = line; *q == ' ' || *q == '\t'; q++)
      indent += (*q == '\t') ? 2 : 1;
    char *text = trim(line);
    if (!*text) continue;

    /* Komentar // (gaya use alias) dibuang sebelum apa pun. */
    char *slash2 = strstr(text, "//");
    if (slash2) *slash2 = '\0';
    if (!*trim(text)) continue;

    /* `use workspace [c|cpp|c++|hybrid]` — bahasa default workspace. Tanpa
       argumen = hybrid (deteksi otomatis per proyek, perilaku lama). Bahasa
       tak dikenal dilaporkan, bukan dibuang diam-diam. */
    if (strncmp(text, "use workspace", 13) == 0 &&
        (text[13] == '\0' || text[13] == ' ' || text[13] == '\t')) {
      const char *arg = text + 13;
      while (*arg == ' ' || *arg == '\t') arg++;
      if (*arg) {
        WsLang l;
        if (wsParseLang(arg, &l))
          m->lang = l;
        else if (!wsGuardQuiet)
          fprintf(stderr,
                  "satria: %s:%d: bahasa '%s' pada 'use workspace' tidak dikenal "
                  "(pakai: c | cpp | c++ | hybrid) — dipakai hybrid\n",
                  path, lineno, arg);
      }
      continue;
    }

    char *eq = strchr(text, '=');
    if (!eq) {
      /* Baris lanjutan release.files: indentasi menandakan masih bagian
         daftar. Baris alias tetap di level-0. */
      if (filesOpen && indent > 0) {
        wsFilesAppend(filesVal, sizeof(filesVal), &filesLen, text);
        continue;
      }
      if (filesOpen) {
        wsRecordLine(m, "release.files", filesVal);
        filesOpen = false;
        filesLen = 0;
      }
      aliasLineAdd(&aliases, text); /* baris alias "X as Y" */
      continue;
    }
    /* Baris setting baru menutup daftar yang sedang berjalan. */
    if (filesOpen) {
      wsRecordLine(m, "release.files", filesVal);
      filesOpen = false;
      filesLen = 0;
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
    /* cdeps level atas: grup {a,b}, {b,c} (design/cdeps_group.md). Diekspansi
       setelah seluruh file dibaca, karena nama proyek bisa datang di baris mana pun. */
    if (strcmp(key, "cdeps") == 0) {
      char gbuf[WS_LINE_LEN];
      snprintf(gbuf, sizeof(gbuf), "%d\t%s", lineno, v);
      listAdd(&m->cdepsGroups, gbuf);
      continue;
    }
    aliasResolve(&aliases, key, sizeof(key));
    /* release.files: kumpulkan dulu — mungkin berlanjut di baris berikutnya
       (nilai kosong atau trailing comma untuk format rapi). */
    if (strcmp(key, "release.files") == 0) {
      filesOpen = true;
      filesLen = 0;
      filesVal[0] = '\0';
      wsFilesAppend(filesVal, sizeof(filesVal), &filesLen, v);
      continue;
    }
    /* cdeps.<kunci> (mis. cdeps.preshared): kebijakan workspace yang belum
       didukung. Ditolak eksplisit, bukan diterima diam-diam sebagai proyek
       implisit bernama `cdeps` (lihat problems/05_11_2026_cdeps.md). */
    if (strncmp(key, "cdeps.", 6) == 0) {
      if (!wsGuardQuiet)
        fprintf(stderr, "satria: %s:%d: '%s' belum didukung (kebijakan cdeps belum tersedia)\n",
                path, lineno, key);
      fclose(fp);
      listFree(&aliases);
      return false;
    }
    wsRecordLine(m, key, v);
  }
  if (filesOpen) wsRecordLine(m, "release.files", filesVal);
  fclose(fp);
  listFree(&aliases);
  return wsExpandCdepsGroups(m, path);
}

/* ws_member.c — Guard direktori anggota: deteksi cwd sebagai anggota workspace induk.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"

static void wsPathCanon(char *s);
bool wsMemberCwdDetect(char *wsRootOut, size_t wsRootN, char *memberOut, size_t memberN);

/* Guard direktori anggota (wsMemberCwdDetect): selama keanggotaan divalidasi
   lewat model induk, pesan parser workspace dibungkam — induk yang rusak
   berarti "bukan anggota", bukan kesalahan run ini. Definisi & pemakaian di
   bagian guard (dekat akhir file). */
bool wsGuardQuiet = false;
/* ==================== guard direktori anggota ======================== */

/* Batas naik ke parent saat mencari Buildfile.ws induk — jauh melebihi
   kedalaman workspace wajar, tetapi tetap terbatas supaya tidak pernah
   menembus ke root filesystem. */
/* Normalisasi path ("<ws>/a/../b/./c" -> "<ws>/b/c") secara leksikal atas
   string — cukup karena semua kandidat path dibangun dari fsGetCwd (sudah
   dinormalkan OS) + penyusunan string, tanpa resolusi symlink. */
static void wsPathCanon(char *s) {
  bool abs = s[0] == '/';
  const char *seg[128];
  int top = 0;
  bool overflow = false;
  char *p = s + (abs ? 1 : 0);
  while (*p && !overflow) {
    char *start = p;
    while (*p && *p != '/') p++;
    if (*p) *p++ = '\0';
    if (strcmp(start, ".") == 0) continue;
    if (strcmp(start, "..") == 0) {
      if (top > 0 && strcmp(seg[top - 1], "..") != 0)
        top--; /* pop segmen biasa */
      else if (abs)
        continue; /* ".." di atas root filesystem: buang */
      else if (top < 128)
        seg[top++] = start; /* path relatif: ".." dipertahankan */
      else
        overflow = true;
      continue;
    }
    if (!*start) continue;
    if (top < 128)
      seg[top++] = start;
    else
      overflow = true;
  }
  if (overflow) return; /* path absurd — biarkan apa adanya */

  char *w = s;
  if (abs) *w++ = '/';
  for (int i = 0; i < top; i++) {
    size_t l = strlen(seg[i]);
    if (i) *w++ = '/';
    memmove(w, seg[i], l); /* selalu mundur: aman in-place */
    w += l;
  }
  if (w == s) *w++ = '.';
  *w = '\0';
}
/*
 * wsMemberCwdDetect — true bila cwd berada DI DALAM direktori proyek anggota
 * sebuah workspace INDUK (Buildfile.ws di salah satu parent). Keanggotaan
 * divalidasi lewat model Buildfile.ws induk (parse daftar projects + root) —
 * keberadaan file workspace di parent saja TIDAK cukup (design/
 * workspace_v1.md aturan 1 & 5); proyek standalone di luar workspace tidak
 * terpengaruh (jalur single-project tidak berubah).
 *
 * Bila true: wsRootOut = root workspace induk (absolut), memberOut = nama
 * proyek anggota sesuai deklarasi. Cwd pada root workspace sendiri selalu
 * false — jalur mode workspace (workspaceRun) yang menanganinya.
 */
bool wsMemberCwdDetect(char *wsRootOut, size_t wsRootN, char *memberOut, size_t memberN) {
  if (workspaceFileName()) return false; /* cwd = root workspace sendiri */
  char cwd[WS_PATH_LEN];
  if (!fsGetCwd(cwd, sizeof(cwd))) return false;
  wsPathCanon(cwd);

  char probe[WS_PATH_LEN];
  copyStr(probe, sizeof(probe), cwd);
  for (int up = 0; up < WS_MEMBER_MAX_UP; up++) {
    char *slash = strrchr(probe, '/');
    if (!slash || slash == probe) break; /* mencapai root filesystem */
    *slash = '\0';
    if (!probe[0]) break;

    /* Kandidat induk: Buildfile.ws (atau nama lama) di parent. */
    char wsf[WS_PATH_LEN + 40];
    const char *wsName = NULL;
    snprintf(wsf, sizeof(wsf), "%s/%s", probe, WORKSPACE_FILENAME);
    if (fsFileExists(wsf))
      wsName = WORKSPACE_FILENAME;
    else {
      snprintf(wsf, sizeof(wsf), "%s/%s", probe, WORKSPACE_FILENAME_LEGACY);
      if (fsFileExists(wsf)) wsName = WORKSPACE_FILENAME_LEGACY;
    }
    if (!wsName) continue;

    /* Validasi keanggotaan lewat model — parse TENANG; file rusak/gagal
       parse = bukan workspace valid, lanjut naik ke parent berikutnya. */
    WsModel m;
    wsGuardQuiet = true;
    char scache[WS_PATH_LEN + 40];
    snprintf(scache, sizeof(scache), "%s/%s", probe, WS_STARTUP_FILE);
    /* startUp() versi baca-saja (design/startup.md §2): state startup yang
       valid (hash Buildfile.ws cocok) = model identik dengan parse; deteksi
       anggota tidak pernah menulis cache. */
    bool parsed = wsStartupLoad(&m, wsf, wsName, scache);
    if (!parsed) parsed = wsParse(&m, wsf);
    wsGuardQuiet = false;
    if (!parsed) {
      wsModelFree(&m);
      continue;
    }

    for (int i = 0; i < m.projectCount; i++) {
      const char *root = m.projects[i].root[0] ? m.projects[i].root : m.projects[i].name;
      /* Buffer muat worst case "probe/root" tanpa potong (bukti statis
         untuk snprintf — tanpa warning format-truncation). */
      char mem[WS_PATH_LEN + MAX_PATH + 4];
#ifdef _WIN32
      bool absRoot = root[0] == '/' || (root[0] && root[1] == ':');
#else
      bool absRoot = root[0] == '/';
#endif
      if (absRoot)
        snprintf(mem, sizeof(mem), "%s", root);
      else
        snprintf(mem, sizeof(mem), "%s/%s", probe, root);
      wsPathCanon(mem);
      size_t ml = strlen(mem);
      /* cwd == root anggota, atau di bawahnya (boundary '/'). */
      bool hit = strcmp(cwd, mem) == 0 || (strncmp(cwd, mem, ml) == 0 && cwd[ml] == '/');
      if (hit) {
        copyStr(wsRootOut, wsRootN, probe);
        copyStr(memberOut, memberN, m.projects[i].name);
        wsModelFree(&m);
        return true;
      }
    }
    wsModelFree(&m);
  }
  return false;
}

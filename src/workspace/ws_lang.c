/* ws_lang.c — Aturan bahasa workspace: `use workspace c|cpp|c++|hybrid` dan
 * override per proyek `<proyek>.lang = ...`.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 *
 * Satu titik aturan: wsProjectLang() dipakai wsBuildConfig (mengisi
 * Config.langDeclared/langCpp), sehingga walk sumber (configWalkSources),
 * std default, dan pemilihan driver compiler mengikuti nilai yang sama.
 * HYBRID = perilaku lama (deteksi otomatis per proyek) — default, jadi
 * workspace tanpa deklarasi bahasa tidak berubah perilakunya.
 */
#include "ws_internal.h"

static bool wsLangEq(const char *v, const char *want) {
  size_t i = 0;
  for (; v[i] && want[i]; i++) {
    char a = v[i], b = want[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a | 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b | 32);
    if (a != b) return false;
  }
  return v[i] == '\0' && want[i] == '\0';
}

bool wsParseLang(const char *value, WsLang *out) {
  if (!value) return false;
  while (*value == ' ' || *value == '\t') value++;
  char buf[16];
  size_t n = 0;
  while (value[n] && value[n] != ' ' && value[n] != '\t' && n + 1 < sizeof(buf)) {
    buf[n] = value[n];
    n++;
  }
  buf[n] = '\0';
  if (n == 0) return false;
  if (wsLangEq(buf, "c")) {
    *out = WS_LANG_C;
    return true;
  }
  if (wsLangEq(buf, "cpp") || wsLangEq(buf, "c++")) {
    *out = WS_LANG_CPP;
    return true;
  }
  if (wsLangEq(buf, "hybrid")) {
    *out = WS_LANG_HYBRID;
    return true;
  }
  return false;
}

WsLang wsProjectLang(const WsModel *m, int idx) {
  if (idx < 0 || idx >= m->projectCount) return m->lang;
  const WsProject *p = &m->projects[idx];
  for (int i = 0; i < p->lines.count; i++) {
    const char *ln = p->lines.items[i];
    if (strncmp(ln, "lang = ", 7) != 0) continue;
    WsLang l;
    if (wsParseLang(ln + 7, &l)) return l;
  }
  return m->lang; /* HYBRID bila tak ada deklarasi: perilaku lama */
}

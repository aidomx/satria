/* ws_model.c — Model workspace: validasi nama, daftar proyek, pemasangan baris umum, pembebasan model.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"


/* ==================== util kecil ==================== */
bool wsValidName(const char *s) {
  if (!s || !*s) return false;
  for (const char *p = s; *p; p++) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}
int wsFindProject(const WsModel *m, const char *name) {
  for (int i = 0; i < m->projectCount; i++)
    if (strcmp(m->projects[i].name, name) == 0) return i;
  return -1;
}
WsProject *wsEnsureProject(WsModel *m, const char *name) {
  int idx = wsFindProject(m, name);
  if (idx >= 0) return &m->projects[idx];
  if (m->projectCount >= WS_MAX_PROJECTS) return NULL;
  if (!wsValidName(name) || wsIsEngineSection(name)) return NULL;
  idx = m->projectCount++;
  memset(&m->projects[idx], 0, sizeof(WsProject));
  copyStr(m->projects[idx].name, sizeof(m->projects[idx].name), name);
  snprintf(m->projects[idx].root, sizeof(m->projects[idx].root), "%s", name);
  return &m->projects[idx];
}
void wsAddCommon(WsModel *m, const char *key, const char *value) {
  char buf[WS_LINE_LEN];
  snprintf(buf, sizeof(buf), "%s = %s", key, value);
  listAdd(&m->commonLines, buf);
}
/* Bebaskan seluruh isi model (dipakai di semua jalur keluar workspaceRun
   agar tidak ada List yang bocor saat parse/topo/needed gagal). */
void wsModelFree(WsModel *m) {
  for (int i = 0; i < WS_MAX_PROJECTS; i++) {
    listFree(&m->depends[i]);
    listFree(&m->libDeps[i]);
  }
  listFree(&m->order);
  for (int i = 0; i < m->projectCount; i++)
    listFree(&m->projects[i].lines);
  listFree(&m->commonLines);
  listFree(&m->release.lines);
}

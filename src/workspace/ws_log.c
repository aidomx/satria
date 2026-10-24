/* ws_log.c — workspace_log: snapshot sumber, hash konfigurasi/isi, dan entri log per proyek.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"
#include "../cache/state_blob.h"

static uint64_t wsFnv1a(uint64_t h, const void *data, size_t n);
static uint64_t wsFnv1aStr(uint64_t h, const char *s);

/* Fast-hit per proyek pada run berjalan (wsRunProject): proyek yang
   lolos fast-state source-nya dijamin tak berubah oleh stamp folder di
   ws.state — memungkinkan workspace_log memuat ulang daftar source dari
   snapshot <n>.src tanpa re-scan (keputusan (a) workspace_log). */
bool wsLogFastOk[WS_MAX_PROJECTS];
/* ==================== workspace_log + CompDB merged ==================== */
/* Indeks no-op & CompDB (design/workspace_v1.md §No-op dan CompDB): satu
   blok per proyek — (2) hash konfigurasi Buildfile.ws, (3) fingerprint
   daftar source dari snapshot <n>.src, (4) hash isi compile_commands.json
   terakhir (sama untuk seluruh workspace). Hanya dua keputusan: (a) proyek
   fast-hit + (2)+(3) identik -> daftar source dimuat dari snapshot tanpa
   re-scan; (b) (4) identik + file ada -> CompDB tidak ditulis ulang. */
static uint64_t wsFnv1a(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}
static uint64_t wsFnv1aStr(uint64_t h, const char *s) {
  h = wsFnv1a(h, s, strlen(s));
  const unsigned char sep = 0xff;
  return wsFnv1a(h, &sep, 1);
}
/* Hash konfigurasi proyek (field 2): baris bersama + root override + baris
   proyek — semua baris Buildfile.ws yang memengaruhinya. */
uint64_t wsLogCfgHash(const WsModel *m, int idx) {
  uint64_t h = 1469598103934665603ULL;
  for (int i = 0; i < m->commonLines.count; i++)
    h = wsFnv1aStr(h, m->commonLines.items[i]);
  h = wsFnv1aStr(h, m->projects[idx].root);
  for (int i = 0; i < m->projects[idx].lines.count; i++)
    h = wsFnv1aStr(h, m->projects[idx].lines.items[i]);
  return h;
}
/* Stamp proyek untuk metadata grup P (design/startup.md §4): hash
   konfigurasi (baris bersama + root + baris proyek) + daftar sumber dalam
   urutan kanonik. Dipakai SAMA oleh save (wsCacheSave) dan gate no-op
   (jalur penuh): input identik -> hash identik. Proyek kemasan
   (archive-only) tidak punya daftar sumber -> konfigurasi + root saja. */
uint64_t wsProjStamp(const WsModel *m, int idx) {
  uint64_t h = wsLogCfgHash(m, idx);
  if (!wsProjectArchiveOnly(&m->projects[idx])) {
    List srcs = {0};
    if (wsLogLoadSrcs(m->projects[idx].name, &srcs)) {
      listSort(&srcs);
      for (int i = 0; i < srcs.count; i++) h = wsFnv1aStr(h, srcs.items[i]);
    }
    listFree(&srcs);
  }
  return h;
}
/* Hash isi file — 0 bila tak bisa dibaca/kosong. */
uint64_t wsLogFileHash(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  uint64_t h = 1469598103934665603ULL;
  char buf[4096];
  size_t n;
  bool any = false;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    h = wsFnv1a(h, buf, n);
    any = true;
  }
  fclose(f);
  return any ? h : 0;
}
/* Indeks CompDB per proyek disimpan di state.cache, bukan workspace_log.
   Versi payload mencegah pembacaan state lama sebagai indeks baru. */
#define WS_COMPDB_LEAF "compdb-index-v1"

static bool wsLogDecode(const char *data, size_t len, uint64_t *cfg,
                        uint64_t *src, uint64_t *cdb) {
  unsigned long long a = 0, b = 0, c = 0;
  if (len != 52 || data[0] != '1' || data[1] != ' ' ||
      sscanf(data, "1 %16llx %16llx %16llx", &a, &b, &c) != 3)
    return false;
  *cfg = (uint64_t)a;
  *src = (uint64_t)b;
  *cdb = (uint64_t)c;
  return true;
}

bool wsLogReadEntry(const char *name, uint64_t *cfg, uint64_t *src, uint64_t *cdb) {
  char *data = NULL;
  size_t len = 0;
  if (!stateBlobGet(name, WS_COMPDB_LEAF, &data, &len)) return false;
  bool ok = wsLogDecode(data, len, cfg, src, cdb);
  free(data);
  return ok;
}

uint64_t wsLogCompdbFp(void) {
  char *data = NULL;
  size_t len = 0;
  if (!stateBlobGet(WS_COMPDB_GLOBAL, "fingerprint-v1", &data, &len)) return 0;
  unsigned long long value = 0;
  bool ok = len == 16 && sscanf(data, "%16llx", &value) == 1;
  free(data);
  return ok ? (uint64_t)value : 0;
}

/* Muat daftar source dari snapshot <n>.src TANPA re-scan (baris "S <path>").
   Pemanggil hanya memanggil bila fast-state proyek valid DAN hash snapshot
   cocok dengan log — source set dijamin tak berubah. */
bool wsLogLoadSrcs(const char *name, List *out) {
  char *blob = NULL;
  size_t bn = 0;
  if (!stateBlobGet(name, "src", &blob, &bn)) return false;
  size_t pos = 0;
  char line[WS_PATH_LEN + 32];
  bool ok = stateBufGets(blob, bn, &pos, line, sizeof(line)) != NULL &&
            strncmp(line, "SATRIAWSLIST1", 12) == 0;
  while (ok && stateBufGets(blob, bn, &pos, line, sizeof(line))) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = '\0';
    if (strncmp(line, "S ", 2) == 0)
      ok = listAdd(out, line + 2);
    else if (line[0])
      ok = false;
  }
  free(blob);
  if (!ok || out->count == 0) {
    listFree(out);
    return false;
  }
  return true;
}
/* Simpan hanya entri berubah; stateBlobFlush melakukan commit atomik sekali
   di akhir workspaceRun, termasuk jalur `-g compdb`. */
void wsLogWrite(const WsModel *m, const uint64_t *cfgH, const uint64_t *srcH,
                uint64_t cdb) {
  for (int i = 0; i < m->projectCount; i++) {
    const char *name = m->projects[i].name;
    uint64_t a = 0, b = 0, c = 0;
    if (wsLogReadEntry(name, &a, &b, &c) &&
        a == cfgH[i] && b == srcH[i] && c == cdb) continue;
    char entry[64];
    int n = snprintf(entry, sizeof(entry), "1 %016llx %016llx %016llx",
                     (unsigned long long)cfgH[i],
                     (unsigned long long)srcH[i], (unsigned long long)cdb);
    if (n > 0 && (size_t)n < sizeof(entry))
      stateBlobPut(name, WS_COMPDB_LEAF, entry, (size_t)n);
  }
  if (wsLogCompdbFp() != cdb) {
    char entry[24];
    int n = snprintf(entry, sizeof(entry), "%016llx", (unsigned long long)cdb);
    if (n > 0 && (size_t)n < sizeof(entry))
      stateBlobPut(WS_COMPDB_GLOBAL, "fingerprint-v1", entry, (size_t)n);
  }
}

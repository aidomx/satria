/* ws_startup_state.c — Persistensi startup.state: simpan dan muat model hasil parse, dengan validasi hash.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"

static char *wsStartupIndexedRest(char *line, int *idx);
static char *wsStartupPayload(char *line);

/* ==================== startup.state (design/startup.md §2–§4) =============
 *
 * Konfigurasi hasil penerjemahan Buildfile.ws dipersistenskan ke
 * `<wsDir>/.satria/startup.state`: run berikutnya MEMULIHKAN model workspace
 * tanpa parse ulang (resolusi alias, pemecahan namespace, pemilihan baris
 * per proyek sudah selesai saat penyimpanan) — startup penuh hanya saat
 * pertama kali atau saat Buildfile.ws berubah.
 *
 * Validasi cache (§4):
 *   - magic + versi SATRIA-STARTUP-1;
 *   - FNV-1a 64 atas isi file workspace == hash tersimpan (perubahan satu
 *     byte pun pada Buildfile.ws -> miss dan parse ulang);
 *   - nama file workspace ikut di header (Buildfile.ws vs nama lama);
 *   - seluruh entri terbaca utuh dan dalam batas (file terpotong/ganjal
 *     -> miss, bukan model setengah jadi).
 *
 * Yang diserialisasi HANYA hasil parse mentah (nama/root/baris per proyek,
 * default bersama, konfigurasi release). Turunan (depends, libDeps, urutan
 * topo) tetap dihitung ulang pemanggil lewat wsTopoSort, dan
 * wsValidatePackMerges tetap dijalankan pada kedua jalur — model hasil
 * cache-hit dan cache-miss identik, hanya waktunya lebih pendek.
 *
 * Penulisan atomik (tmp lalu rename); gagal menulis = miss berikutnya, bukan
 * kegagalan build. Jalur baca-saja (`satria profile`, deteksi anggota) TIDAK
 * menulis cache — kontrak non-mutating, sama dengan loadConfigEx(cacheWrite=false).
 */
/* "K <idx> <sisa-baris>" -> sisa baris setelah indeks (boleh kosong). */
static char *wsStartupIndexedRest(char *line, int *idx) {
  char *p = line + 1;
  while (*p == ' ') p++;
  char *end = NULL;
  long v = strtol(p, &end, 10);
  if (end == p || v < 0 || v > WS_MAX_PROJECTS) return NULL;
  *idx = (int)v;
  p = end;
  while (*p == ' ') p++;
  return p;
}
/* "K <sisa-baris>" tanpa indeks (baris C/T). */
static char *wsStartupPayload(char *line) {
  char *p = line + 1;
  while (*p == ' ') p++;
  return p;
}
/* Simpan model (parse mentah) + hash Buildfile.ws ke startup.state —
   atomik: tulis .tmp lalu rename. */
void wsStartupSave(const WsModel *m, const char *wsFile, const char *wsName,
                          const char *cacheFile) {
  uint64_t hash = wsStartupHash(wsFile);
  if (!hash) return; /* file workspace tak terbaca: tidak ada yang valid */
  char dir[WS_PATH_LEN];
  snprintf(dir, sizeof(dir), "%s", cacheFile);
  char *slash = strrchr(dir, '/');
  if (slash) *slash = '\0';
  mkdirs(dir);
  char tmp[WS_PATH_LEN + 8];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", cacheFile) >= (int)sizeof(tmp)) return;
  FILE *fp = fopen(tmp, "w");
  if (!fp) return;
  fprintf(fp, "%s %016llx %lld %s\n", WS_STARTUP_MAGIC, (unsigned long long)hash,
          (long long)time(NULL), wsName);
  fprintf(fp, "count %d\n", m->projectCount);
  for (int i = 0; i < m->projectCount; i++) {
    fprintf(fp, "P %d %s\n", i, m->projects[i].name);
    fprintf(fp, "R %d %s\n", i, m->projects[i].root);
    for (int j = 0; j < m->projects[i].lines.count; j++)
      fprintf(fp, "L %d %s\n", i, m->projects[i].lines.items[j]);
  }
  for (int i = 0; i < m->commonLines.count; i++) fprintf(fp, "C %s\n", m->commonLines.items[i]);
  fprintf(fp, "X %d\n", m->release.present ? 1 : 0);
  for (int i = 0; i < m->release.lines.count; i++) fprintf(fp, "T %s\n", m->release.lines.items[i]);
  fprintf(fp, "END\n");
  if (fclose(fp) == 0) {
    if (!fsRenameReplace(tmp, cacheFile)) fsRemoveFile(tmp);
  } else
    fsRemoveFile(tmp);
}
/* Muat startup.state ke `m`. true hanya bila magic+hash+seluruh entri
   tervalidasi; false = `m` dibebaskan dan DIKOSONGKAN — aman dilanjutkan
   wsParse. Truncation, indeks liar, atau hash beda = miss. */
bool wsStartupLoad(WsModel *m, const char *wsFile, const char *wsName,
                          const char *cacheFile) {
  FILE *fp = fopen(cacheFile, "r");
  if (!fp) return false;
  uint64_t want = wsStartupHash(wsFile);
  if (!want) {
    fclose(fp);
    return false;
  }

  memset(m, 0, sizeof(*m));
  bool ok = true;
  char line[WS_LINE_LEN * 2 + 64];
  unsigned long long hash = 0;
  long long ts = 0;
  char fname[128] = "";
  if (!fgets(line, sizeof(line), fp) ||
      sscanf(line, WS_STARTUP_MAGIC " %llx %lld %127s", &hash, &ts, fname) != 3 ||
      hash != (unsigned long long)want || strcmp(fname, wsName) != 0)
    ok = false;

  int count = -1;
  if (ok &&
      (!fgets(line, sizeof(line), fp) || sscanf(line, "count %d", &count) != 1 || count < 0 ||
       count > WS_MAX_PROJECTS))
    ok = false;
  if (ok) m->projectCount = count;

  int expectP = 0; /* P harus berurutan 0..count-1 — urutan = urutan parse */
  bool sawEnd = false;
  bool sawRelease = false;
  while (ok && fgets(line, sizeof(line), fp)) {
    size_t len = strlen(line);
    if (len && line[len - 1] == '\n')
      line[--len] = '\0';
    else if (!feof(fp)) {
      ok = false; /* baris terpotong */
      break;
    }
    if (strcmp(line, "END") == 0) {
      sawEnd = true;
      break;
    }
    switch (line[0]) {
      case 'P': {
        int i = -1;
        char nm[WS_NAME_LEN];
        if (sscanf(line, "P %d %63s", &i, nm) != 2 || i != expectP || i >= count ||
            !wsValidName(nm) || wsFindProject(m, nm) >= 0) {
          ok = false;
          break;
        }
        copyStr(m->projects[i].name, sizeof(m->projects[i].name), nm);
        expectP++;
        break;
      }
      case 'R': {
        int i = -1;
        char *rest = wsStartupIndexedRest(line, &i);
        if (!rest || i < 0 || i >= count || !m->projects[i].name[0] ||
            strlen(rest) >= sizeof(m->projects[i].root)) {
          ok = false;
          break;
        }
        copyStr(m->projects[i].root, sizeof(m->projects[i].root), rest);
        break;
      }
      case 'L': {
        int i = -1;
        char *rest = wsStartupIndexedRest(line, &i);
        if (!rest || i < 0 || i >= count || !m->projects[i].name[0] || !*rest ||
            !listAdd(&m->projects[i].lines, rest)) {
          ok = false;
          break;
        }
        break;
      }
      case 'C': {
        char *rest = wsStartupPayload(line);
        if (!*rest || !listAdd(&m->commonLines, rest)) {
          ok = false;
          break;
        }
        break;
      }
      case 'X': {
        int v = -1;
        if (sscanf(line, "X %d", &v) != 1 || (v != 0 && v != 1) || sawRelease) {
          ok = false;
          break;
        }
        sawRelease = true;
        m->release.present = v != 0;
        break;
      }
      case 'T': {
        char *rest = wsStartupPayload(line);
        if (!sawRelease || !m->release.present || !*rest || !listAdd(&m->release.lines, rest)) {
          ok = false;
          break;
        }
        break;
      }
      default: ok = false; break;
    }
  }
  if (ok && (expectP != count || !sawEnd)) ok = false;
  fclose(fp);
  if (!ok) {
    wsModelFree(m);
    memset(m, 0, sizeof(*m));
    return false;
  }
  (void)ts;
  return true;
}

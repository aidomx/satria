/* ws_util.c — Utilitas bersama workspace: join path, format dinamis, split key=value, list, hash startup.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"


/* Sudah ada di list? (dedup persis string — cukup untuk path folder). */
bool wsListHas(const List *l, const char *s) {
  for (int i = 0; i < l->count; i++)
    if (strcmp(l->items[i], s) == 0) return true;
  return false;
}
/* ==================== Susun Config di memori (WsConfig) ====================
 *
 * Menggantikan sintesis file + loadConfig (design/workspace_v1.md §Pemisahan
 * Jalur Build): tiap proyek memperoleh Config hasil resolusi Buildfile.ws
 * di memori — tanpa file .Buildfile sintesis, tanpa chdir. Skoping path
 * per-kunci:
 *   - konten-proyek (sources, headers, archive.src, embedded.*.src) ->
 *     relatif root proyek (di-prefix <root>/);
 *   - lintas-proyek/workspace (headers.internal, archive.dir,
 *     embedded.*.file, library.<os> = <proyek>) -> relatif root workspace;
 *   - output: build/<proyek>, bin, lib di root workspace.
 */
/* true bila item adalah path relatif yang harus ditempel ke root proyek
   (bukan flag -I/-D, bukan path absolut POSIX maupun Windows). */
bool wsIsRelPath(const char *item) {
  if (!item || !*item) return false;
  if (item[0] == '-' || item[0] == '/') return false;
  if (item[1] == ':' && item[2]) return false; /* C:\... / C:/... */
  return true;
}
/* "key = value" -> key kanonik (ter-trim) + pointer value di buffer
   mutable milik pemanggil. */
bool wsSplitKV(char *line, char *key, size_t kn, char **val) {
  char *eq = strchr(line, '=');
  if (!eq) return false;
  *eq = '\0';
  char *k = trim(line);
  char *v = trim(eq + 1);
  if (!*k || strlen(k) >= kn) return false;
  memcpy(key, k, strlen(k) + 1);
  *val = v;
  return true;
}
/* FNV-1a 64 atas isi file (streaming 4KB). 0 = file tak terbaca; 0 juga
   nilai yang mustahil bagi konten nyata, jadi 0 = "tidak terhitung". */
uint64_t wsStartupHash(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  uint64_t h = 14695981039346656037ULL;
  unsigned char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    for (size_t i = 0; i < n; i++) {
      h ^= buf[i];
      h *= 1099511628211ULL;
    }
  fclose(fp);
  return h;
}

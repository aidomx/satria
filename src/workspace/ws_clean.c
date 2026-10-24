/* ws_clean.c — clean workspace: kosongkan isi folder output root (bin/ build/ lib/ modules/).
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"


/* clean di workspace: true bila proyek mana pun menghapus sesuatu (untuk satu
   pesan "Nothing to clean" di akhir, bukan per proyek). */
bool wsCleanTouched = false;
/* Kosongkan isi root output workspace (bin/ build/ lib/ modules/, plus build/
   yang menampung sisa proyek lepasan). Folder tetap ada. true bila ada entri
   yang terhapus atau gagal dihapus. */
bool wsCleanRootOutputs(const char *wsDir) {
  static const char *const rootOut[] = {"build", "bin", "lib", "modules"};
  bool any = false;
  for (int i = 0; i < (int)(sizeof(rootOut) / sizeof(rootOut[0])); i++) {
    char dir[WS_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/%s", wsDir, rootOut[i]);
    if (!fsDirExists(dir)) continue;
    int removed = 0;
    bool ok = cmdsEmptyDir(dir, &removed);
    if (removed || !ok) {
      printf("> Emptied   : %s (%d entri%s)\n", rootOut[i], removed, ok ? "" : ", sebagian gagal");
      any = true;
    }
  }
  return any;
}

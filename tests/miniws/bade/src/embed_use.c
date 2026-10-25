/* Penggunaan nyata embedded: konsumen cukup memanggil getEmbed(key)
   tanpa hutan makro EMBED_*, tanpa ukuran dari selisih pointer
   (design/embedded.md). Menulis arsip modul yang tertanam di binary ke
   EXTRACT_DIR lalu membongkarnya ke EXTRACT_DIR/extracted (seperti loader
   lama: `tar xf`). Dilewati bila arsip dengan ukuran sama dan hasil
   bongkarannya sudah ada. Tanpa output supaya keluaran bin/bade tidak
   berubah. */
#include <embedded.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int badeExtractSystem(void) {
  const Embedded *e = getEmbed("modules"); /* key default = nama entri */
  if (!e || !e->data || !e->size) {
    fprintf(stderr, "Tidak ada!\n");
    return 0;
  }
  char archive[512], outdir[512], cmd[1200];
  snprintf(archive, sizeof(archive), "%s/%s", e->extract, e->name);
  snprintf(outdir, sizeof(outdir), "%s/extracted", e->extract);

  struct stat st, dst;
  bool archiveOk = stat(archive, &st) == 0 && (size_t)st.st_size == e->size;
  if (archiveOk && stat(outdir, &dst) == 0 && S_ISDIR(dst.st_mode))
    return 1; /* sudah terekstrak dari run sebelumnya */

  snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", outdir, e->extract);
  if (system(cmd) != 0) return 0;

  FILE *f = fopen(archive, "wb");
  if (!f) return 0;
  bool written = fwrite(e->data, 1, e->size, f) == e->size;
  fclose(f);
  if (!written) return 0;

  snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && tar xf '%s' -C '%s'", outdir, archive, outdir);
  return system(cmd) == 0;
}

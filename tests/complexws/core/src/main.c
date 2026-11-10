#include <rupa.h>

#include <stdio.h>
#include <string.h>

static void usage(FILE *out) {
  fputs("pakai: rupa <perintah> [argumen]\n"
        "\n"
        "perintah:\n"
        "  notes add <teks>  tambah satu catatan ($RUPA_HOME, default .rupa)\n"
        "  notes list        cetak catatan bernomor\n"
        "  notes count       jumlah catatan\n"
        "  notes clear       hapus semua catatan\n"
        "  version           versi rupa beserta nilai core/bade\n"
        "  help              tampilkan bantuan ini\n"
        "\n"
        "tanpa perintah: cetak ringkasan workspace (perilaku lama fixture)\n",
        out);
}

int main(int argc, char **argv) {
  const char *cmd;

  if (argc < 2) {
    thisBade();
    printf("core=%d bade=%d\n", core_value(), bade_value());
    return 0;
  }

  cmd = argv[1];
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
    usage(stdout);
    return 0;
  }
  if (strcmp(cmd, "version") == 0 || strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0) {
    printf("rupa %s (core %d bade %d)\n", CORE_VERSION, core_value(), bade_value());
    return 0;
  }
  if (strcmp(cmd, "notes") == 0) return core_cli_notes(argc, argv);

  fprintf(stderr, "rupa: perintah tidak dikenal: %s\n", cmd);
  usage(stderr);
  return 2;
}

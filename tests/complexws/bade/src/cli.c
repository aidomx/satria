#include <bade.h>
#include <core.h>

#include <stdio.h>
#include <string.h>

static void usage(FILE *out) {
  fputs("pakai: bade <perintah> [argumen]\n"
        "\n"
        "perintah:\n"
        "  extract           ekstrak modul tertanam ke direktori ekstraksi fixture\n"
        "  embed             ringkas entri embed \"modules\" (nama, ukuran, direktori)\n"
        "  notes <sub>       catatan: add <teks> | list | count | clear\n"
        "  version           versi bade beserta nilai bade/core\n"
        "  help              tampilkan bantuan ini\n"
        "\n"
        "tanpa perintah: ekstrak modul lalu cetak \"bade=7 core=8\" (perilaku lama)\n",
        out);
}

int bade_cli(int argc, char **argv) {
  const char *cmd;

  if (argc < 2) {
    thisCore();
    badeExtractSystem();
    printf("bade=%d core=%d\n", bade_value(), core_value());
    return 0;
  }

  cmd = argv[1];
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
    usage(stdout);
    return 0;
  }
  if (strcmp(cmd, "version") == 0 || strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0) {
    printf("bade %s (bade %d core %d)\n", BADE_VERSION, bade_value(), core_value());
    return 0;
  }
  if (strcmp(cmd, "extract") == 0) {
    if (!badeExtractSystem()) {
      fputs("bade: ekstraksi modul gagal\n", stderr);
      return 1;
    }
    puts("extracted");
    return 0;
  }
  if (strcmp(cmd, "embed") == 0) {
    if (!badeEmbedInfo()) {
      fputs("bade: entri embed \"modules\" tidak tersedia\n", stderr);
      return 1;
    }
    return 0;
  }
  if (strcmp(cmd, "notes") == 0) return core_cli_notes(argc, argv);

  fprintf(stderr, "bade: perintah tidak dikenal: %s\n", cmd);
  usage(stderr);
  return 2;
}

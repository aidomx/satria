#include <rupa.h>

#include <stdio.h>
#include <string.h>

static int notes_fail(const char *what) {
  fprintf(stderr, "notes %s gagal (direktori: %s)\n", what, core_store_dir());
  return 1;
}

/* Gabungkan argv[from..] menjadi satu baris, dipisah spasi. */
static int join_args(int argc, char **argv, int from, char *out, size_t size) {
  size_t len = 0;
  int i;

  out[0] = '\0';
  for (i = from; i < argc; i++) {
    int w = snprintf(out + len, size - len, "%s%s", i > from ? " " : "", argv[i]);
    if (w < 0) return -1;
    len += (size_t)w;
    if (len >= size - 1) return 0; /* terpotong: masih bisa disimpan */
  }
  return 0;
}

int core_cli_notes(int argc, char **argv) {
  const char *dir = core_store_dir();
  const char *sub = (argc > 2) ? argv[2] : "";
  char text[512];
  int n;

  if (strcmp(sub, "add") == 0) {
    if (argc < 4) {
      fputs("notes add: butuh teks catatan\n", stderr);
      return 2;
    }
    if (join_args(argc, argv, 3, text, sizeof text) != 0) return 2;
    if (notes_add(dir, text) != 0) return notes_fail("add");
    printf("added: %s\n", text);
    return 0;
  }
  if (strcmp(sub, "list") == 0) {
    n = notes_list(dir);
    if (n < 0) return notes_fail("list");
    if (n == 0) puts("(belum ada catatan)");
    return 0;
  }
  if (strcmp(sub, "count") == 0) {
    n = notes_count(dir);
    if (n < 0) return notes_fail("count");
    printf("%d\n", n);
    return 0;
  }
  if (strcmp(sub, "clear") == 0) {
    if (notes_clear(dir) != 0) return notes_fail("clear");
    return 0;
  }
  fprintf(stderr, "notes: subperintah tidak dikenal: %s\n", sub);
  fputs("pakai: notes add <teks> | notes list | notes count | notes clear\n", stderr);
  return 2;
}

/* parser_cli.c — antarmuka CLI parser DSL untuk `satria test --parse …`.
   Padanan `--check/--suite` di scripts/lib/parser.sh; inti parser ada di
   parser.c (tanpa dependensi OS) agar bisa diuji terpisah. */

#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../portability.h"
#include "../util.h"

static const char *baseName(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool hasSuffix(const char *s, const char *suffix) {
  size_t n = strlen(s);
  size_t m = strlen(suffix);
  return n >= m && strcmp(s + n - m, suffix) == 0;
}

static int cmpPath(const void *a, const void *b) {
  const char *pa = *(const char *const *)a;
  const char *pb = *(const char *const *)b;
  return strcmp(baseName(pa), baseName(pb));
}

static void usage(void) {
  fputs("pemakaian: satria test --parse <file.test>     # jejak event TSV\n"
        "           satria test --parse --check <file>  # sunyi; rc 0=sah 1=salah\n"
        "           satria test --parse --suite <dir>   # --check tiap *.test di dir\n",
        stderr);
}

/* Baris pertama error (untuk ringkasan --suite). */
static void firstLine(FILE *cap, char *out, size_t n) {
  out[0] = '\0';
  if (!cap) return;
  if (fflush(cap) != 0) return;
  rewind(cap);
  if (!fgets(out, (int)n, cap)) {
    out[0] = '\0';
    return;
  }
  size_t len = strlen(out);
  while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
}

static int runSuite(const char *dir) {
  if (!fsDirExists(dir)) {
    fprintf(stderr, "parser: direktori tidak ada: %s\n", dir);
    return 2;
  }

  List dirs;
  List files;
  memset(&dirs, 0, sizeof(dirs));
  memset(&files, 0, sizeof(files));
  fsListDir(dir, &dirs, &files);

  /* Gabung nama (glob bash tidak memfilter tipe); urutkan per basename. */
  List names;
  memset(&names, 0, sizeof(names));
  for (int i = 0; i < dirs.count; i++) listAdd(&names, dirs.items[i]);
  for (int i = 0; i < files.count; i++) listAdd(&names, files.items[i]);
  listFree(&dirs);
  listFree(&files);
  if (names.count > 1) qsort(names.items, (size_t)names.count, sizeof(*names.items), cmpPath);

  int ok = 0, no = 0;
  const char *suffixes[2] = {".test", ".t"};
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < names.count; i++) {
      const char *path = names.items[i];
      const char *name = baseName(path);
      if (!hasSuffix(name, suffixes[pass])) continue;
      /* `*` pada glob tidak mencocokkan titik di awal nama. */
      if (name[0] == '.') continue;

      FILE *cap = tmpfile();
      int rc = testParseFile(path, false, NULL, cap ? cap : stderr);
      char msg[4096];
      firstLine(cap, msg, sizeof(msg));
      if (cap) fclose(cap);
      if (rc == 0) {
        printf("OK    %s\n", name);
        ok++;
      } else {
        printf("GAGAL %s — %s\n", name, msg);
        no++;
      }
    }
  }
  listFree(&names);

  printf("Ringkasan parser: lulus: %d gagal: %d\n", ok, no);
  return no == 0 ? 0 : 1;
}

int testParserMain(int argc, const char *const *argv) {
  if (argc < 1) {
    usage();
    return 2;
  }
  if (strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0) {
    usage();
    return 2;
  }
  if (strcmp(argv[0], "--check") == 0) {
    if (argc != 2) {
      usage();
      return 2;
    }
    return testParseFile(argv[1], false, stdout, stderr);
  }
  if (strcmp(argv[0], "--suite") == 0) {
    if (argc != 2) {
      usage();
      return 2;
    }
    return runSuite(argv[1]);
  }
  return testParseFile(argv[0], true, stdout, stderr);
}

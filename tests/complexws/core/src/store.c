#include <rupa.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static int notes_path(const char *dir, char *out, size_t size) {
  if (!dir || !*dir) return -1;
  if (snprintf(out, size, "%s/notes.txt", dir) >= (int)size) return -1;
  return 0;
}

static int ensure_dir(const char *dir) {
  if (mkdir(dir, 0777) == 0) return 0;
  return errno == EEXIST ? 0 : -1;
}

const char *core_store_dir(void) {
  const char *home = getenv("RUPA_HOME");
  return (home && *home) ? home : ".rupa";
}

int notes_add(const char *dir, const char *text) {
  char path[1024];
  FILE *f;
  int ok;

  if (!text || !*text) return -1;
  if (ensure_dir(dir) != 0) return -1;
  if (notes_path(dir, path, sizeof path) != 0) return -1;
  f = fopen(path, "a");
  if (!f) return -1;
  ok = fprintf(f, "%s\n", text) > 0;
  if (fclose(f) != 0) ok = 0;
  return ok ? 0 : -1;
}

int notes_count(const char *dir) {
  char path[1024];
  char line[512];
  FILE *f;
  int n = 0;

  if (notes_path(dir, path, sizeof path) != 0) return -1;
  f = fopen(path, "r");
  if (!f) return errno == ENOENT ? 0 : -1;
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (*line) n++;
  }
  fclose(f);
  return n;
}

int notes_list(const char *dir) {
  char path[1024];
  char line[512];
  FILE *f;
  int n = 0;

  if (notes_path(dir, path, sizeof path) != 0) return -1;
  f = fopen(path, "r");
  if (!f) return errno == ENOENT ? 0 : -1;
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (*line) printf("%2d  %s\n", ++n, line);
  }
  fclose(f);
  return n;
}

int notes_clear(const char *dir) {
  char path[1024];

  if (notes_path(dir, path, sizeof path) != 0) return -1;
  if (remove(path) == 0 || errno == ENOENT) return 0;
  return -1;
}

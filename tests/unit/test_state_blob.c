/* Uji unit state_blob (caches/state.cache). Jalankan dari root repo:
     gcc -std=c11 -Wall -Wextra -Isrc/cache -o /tmp/tsb tests/unit/test_state_blob.c \
         src/cache/state_blob.c src/cache/cache_store.c && /tmp/tsb */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "state_blob.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void writeFile(const char *p, const char *t) { FILE *f = fopen(p, "w"); if (f) { fputs(t, f); fclose(f); } }

static bool getEq(const char *proj, const char *leaf, const char *want, size_t wantLen) {
  char *b = NULL; size_t n = 0;
  bool ok = stateBlobGet(proj, leaf, &b, &n) && n == wantLen && memcmp(b, want, wantLen) == 0;
  free(b);
  return ok;
}

static void testRoundtrip(void) {
  const char tricky[] = "a b\\c\nd\re\x01\x02z\\n";
  CHECK(stateBlobPut("core", "ws.state", tricky, sizeof(tricky) - 1));
  CHECK(getEq("core", "ws.state", tricky, sizeof(tricky) - 1));
  CHECK(stateBlobPut("core", "src", "", 0));
  CHECK(getEq("core", "src", "", 0));
  CHECK(!stateBlobGet("core", "missing", &(char *){0}, &(size_t){0}));
  CHECK(!stateBlobGet("nope", "src", &(char *){0}, &(size_t){0}));
}

static void testDeleteAndHash(void) {
  CHECK(stateBlobPut("bade", "build.fingerprint", "1 2 3\n", 6));
  uint64_t h = stateBlobHash("bade", "build.fingerprint");
  CHECK(h != 0);
  CHECK(stateBlobDelete("bade", "build.fingerprint"));
  CHECK(!stateBlobGet("bade", "build.fingerprint", &(char *){0}, &(size_t){0}));
  CHECK(stateBlobHash("bade", "build.fingerprint") == 0);
  CHECK(stateBlobHash("bade", "never") == 0);
  CHECK(stateBlobPut("bade", "src", "abc", 3));
  CHECK(stateBlobHash("bade", "src") == stateBlobHash("bade", "src"));
  /* konsisten dengan wsLogFileHash (seed sama): isi berbeda -> hash berbeda */
  CHECK(stateBlobPut("bade", "src", "abd", 3));
  CHECK(stateBlobHash("bade", "src") != h);
}

static void testFlushAndReload(void) {
  CHECK(stateBlobPut("core", "ws.state", "SATRIAWS3 x\nF 1 2 p\n", 20));
  CHECK(stateBlobPut("bade", "src", "SATRIAWSLIST1 x\nS a.c\n", 22));
  CHECK(stateBlobFlush());
  struct stat sb;
  CHECK(stat(STATE_CACHE_FILE, &sb) == 0);
  /* setelah flush, pembacaan datang dari file */
  CHECK(getEq("core", "ws.state", "SATRIAWS3 x\nF 1 2 p\n", 20));
  CHECK(getEq("bade", "src", "SATRIAWSLIST1 x\nS a.c\n", 22));
  /* hapus lalu flush: hilang dari file */
  CHECK(stateBlobDelete("bade", "src"));
  CHECK(stateBlobFlush());
  CHECK(!stateBlobGet("bade", "src", &(char *){0}, &(size_t){0}));
  CHECK(getEq("core", "ws.state", "SATRIAWS3 x\nF 1 2 p\n", 20));
}

static void testForkStaging(void) {
  CHECK(stateBlobPut("core", "build.fingerprint", "A", 1));
  CHECK(stateBlobFlush());
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    stateBlobForkChild();
    if (!getEq("core", "build.fingerprint", "A", 1)) fprintf(stderr, "child: fp read failed\n");
    stateBlobPut("bade", "src", "CHILD", 5);
    stateBlobDelete("core", "build.fingerprint");
    int ok = stateBlobExport("/tmp/state_test_stage.cache");
    if (!ok) fprintf(stderr, "child: export failed\n");
    _exit(ok && failures == 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(stateBlobImport("/tmp/state_test_stage.cache"));
  struct stat sb;
  CHECK(stat("/tmp/state_test_stage.cache", &sb) != 0); /* stage dihapus */
  CHECK(getEq("bade", "src", "CHILD", 5));
  CHECK(!stateBlobGet("core", "build.fingerprint", &(char *){0}, &(size_t){0}));
  CHECK(stateBlobFlush());
  CHECK(getEq("bade", "src", "CHILD", 5));
  CHECK(!stateBlobGet("core", "build.fingerprint", &(char *){0}, &(size_t){0}));
}

static void testCorruptFile(void) {
  writeFile(STATE_CACHE_FILE, "garbage here\n");
  stateBlobForkChild(); /* buang snapshot lama: seperti proses baru yang membaca file rusak */
  CHECK(!stateBlobGet("core", "src", &(char *){0}, &(size_t){0}));
  CHECK(stateBlobPut("core", "src", "fresh", 5));
  CHECK(stateBlobFlush());
  CHECK(getEq("core", "src", "fresh", 5));
}

static void testBufGets(void) {
  const char *t = "line1\nlongline_xxxxxxxxxxxxxxxxxxxx\nlast";
  size_t len = strlen(t);
  char line[12], acc[256];
  size_t pos = 0, k = 0;
  acc[0] = '\0';
  /* fgets-like: setiap potongan <= n-1 karakter, berhenti setelah '\n'; gabungannya = teks asli */
  while (stateBufGets(t, len, &pos, line, sizeof(line))) {
    CHECK(strlen(line) <= sizeof(line) - 1);
    strcat(acc + k, line);
    k = strlen(acc);
  }
  CHECK(strcmp(acc, t) == 0);
  CHECK(pos == len);
}

int main(void) {
  char dir[256];
  snprintf(dir, sizeof(dir), "/tmp/state_blob_test_%d", (int)getpid());
  mkdir(dir, 0755);
  if (chdir(dir) != 0) return 2;
  mkdir(".satria", 0755);
  mkdir(".satria/caches", 0755);
  testRoundtrip();
  testDeleteAndHash();
  testFlushAndReload();
  testForkStaging();
  testCorruptFile();
  testBufGets();
  printf("state_blob: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

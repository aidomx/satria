/* Uji unit cache_store. Jalankan dari root repo:
     gcc -std=c11 -Wall -Wextra -Isrc/cache -o /tmp/tcs tests/unit/test_cache_store.c src/cache/cache_store.c
     /tmp/tcs
   Exit 0 = semua lulus. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cache_store.h"

static int failures = 0, checks = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    checks++;                                                             \
    if (!(cond)) {                                                        \
      failures++;                                                         \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                     \
  } while (0)

static char dir[256];

static void writeText(const char *path, const char *text) {
  FILE *fp = fopen(path, "w");
  if (fp) {
    fputs(text, fp);
    fclose(fp);
  }
}

static char *readAll(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, fp);
  buf[got] = '\0';
  fclose(fp);
  return buf;
}

static void path(char *out, size_t n, const char *name) {
  snprintf(out, n, "%s/%s", dir, name);
}

static void testHash(void) {
  CHECK(csHash("") == 0xcbf29ce484222325ULL);  /* FNV offset basis */
  CHECK(csHash("a") == 0xaf63dc4c8601ec8cULL); /* FNV-1a 64 untuk "a" */
  CHECK(csHash("core") != csHash("bade"));
}

static void testMissingFileIsEmpty(void) {
  char p[512];
  path(p, sizeof(p), "missing.cache");
  CsStore *st = csOpen(p);
  CHECK(st != NULL);
  CHECK(csCount(st) == 0);
  CHECK(!csIsDirty(st));
  CHECK(csCommit(st));         /* tanpa perubahan: tidak menulis file */
  struct stat sb;
  CHECK(stat(p, &sb) != 0);    /* file belum dibuat */
  csClose(st);
}

static void testPutGetPatchDeleteAndPersist(void) {
  char p[512];
  path(p, sizeof(p), "fingerprint.cache");
  uint64_t core = csHash("core"), bade = csHash("bade");
  uint64_t loader = csHash("core/src/bootstrap/loader.c"), main = csHash("bade/src/main.c");

  CsStore *st = csOpen(p);
  CHECK(st != NULL);
  CsEntry e1 = {core, loader, 0x91b151cd599dd53aULL, 1792314999281163016LL, 1034, NULL};
  CsEntry e2 = {bade, main, 0x8643f83449509d9bULL, 1792314999281329828LL, 812, NULL};
  CHECK(csPut(st, &e1));
  CHECK(csPut(st, &e2));
  CHECK(csIsDirty(st));
  CHECK(csCommit(st));
  CHECK(!csIsDirty(st));
  csClose(st);

  /* Muat ulang: entri harus persis sama. */
  st = csOpen(p);
  CHECK(st != NULL);
  CHECK(csCount(st) == 2);
  CsEntry got;
  CHECK(csGet(st, core, loader, &got));
  CHECK(got.content == e1.content && got.time == e1.time && got.size == e1.size);

  /* touch: PATCH hanya time -> content & size tetap. */
  CsPatch touch = {false, true, false, 0, 1792314999290000000LL, 0, false, NULL};
  CHECK(csPatch(st, core, loader, &touch));
  CHECK(csGet(st, core, loader, &got));
  CHECK(got.time == 1792314999290000000LL && got.content == e1.content && got.size == e1.size);

  /* edit: PATCH content + size. */
  CsPatch edit = {true, false, true, 0xdeadbeefULL, 0, 2048, false, NULL};
  CHECK(csPatch(st, core, loader, &edit));
  CHECK(csGet(st, core, loader, &got));
  CHECK(got.content == 0xdeadbeefULL && got.size == 2048 && got.time == 1792314999290000000LL);

  /* PATCH entri yang tidak ada gagal. */
  CHECK(!csPatch(st, core, csHash("nope"), &touch));

  /* DELETE entri bade. */
  CHECK(csDelete(st, bade, main));
  CHECK(!csDelete(st, bade, main));
  CHECK(!csGet(st, bade, main, &got));
  CHECK(csCommit(st));
  csClose(st);

  st = csOpen(p);
  CHECK(st != NULL);
  CHECK(csCount(st) == 1);
  CHECK(csGet(st, core, loader, &got));
  CHECK(got.content == 0xdeadbeefULL && got.size == 2048);
  CHECK(!csGet(st, bade, main, &got));
  csClose(st);

  /* Tidak ada file tmp yang tertinggal. */
  char tmp[1100];
  snprintf(tmp, sizeof(tmp), "%s.tmp", p);
  struct stat sb;
  CHECK(stat(tmp, &sb) != 0);
}

static void testExactFormat(void) {
  char p[512];
  path(p, sizeof(p), "format.cache");
  CsStore *st = csOpen(p);
  CsEntry e = {0x12ULL, 0x34ULL, 0xabULL, 100, 7, NULL};
  CHECK(csPut(st, &e));
  CHECK(csCommit(st));
  csClose(st);
  char *txt = readAll(p);
  CHECK(txt != NULL);
  const char *want = "P 0000000000000012\n"
                     "PUT 0000000000000012 0000000000000034 00000000000000ab 100 7\n";
  CHECK(txt && strcmp(txt, want) == 0);
  free(txt);
}

static void testGroupingByProject(void) {
  char p[512];
  path(p, sizeof(p), "group.cache");
  CsStore *st = csOpen(p);
  for (uint64_t f = 1; f <= 3; f++) {
    CsEntry a = {10, f, f, 1, 1, NULL};
    CsEntry b = {20, f, f, 1, 1, NULL};
    CHECK(csPut(st, &a));
    CHECK(csPut(st, &b));
  }
  CHECK(csCommit(st));
  csClose(st);
  char *txt = readAll(p);
  int headers = 0;
  for (char *q = txt; (q = strstr(q, "\nP ")) != NULL; q++) headers++;
  if (txt && strncmp(txt, "P ", 2) == 0) headers++;
  CHECK(headers == 2); /* satu P per proyek, bukan per entri */
  free(txt);
}

static void testRejectsCorruptFiles(void) {
  const char *bad[] = {
      "PUT 0000000000000012 0000000000000034 00000000000000ab 100 7\n", /* entri sebelum P */
      "P 0000000000000012\nBOGUS 0000000000000012 0000000000000034 1 1 1\n", /* OP tak dikenal */
      "P 0000000000000012\nPUT 0000000000000013 0000000000000034 1 1 1\n",  /* proyek salah */
      "P 0000000000000012\nPUT 0000000000000012 0000000000000034 1 1\n",    /* kolom kurang */
      "P 0000000000000012\nPUT 0000000000000012 0000000000000034 zz 1 1\n", /* hex rusak */
      "P 0000000000000012\nPUT 0000000000000012 0000000000000034 1 1 1\n"
      "PUT 0000000000000012 0000000000000034 2 2 2\n",                       /* duplikat */
  };
  char p[512];
  path(p, sizeof(p), "bad.cache");
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    writeText(p, bad[i]);
    CsStore *st = csOpen(p);
    CHECK(st == NULL);
    if (st) csClose(st);
  }
  /* DELETE di file tidak ditulis; kalau ada, dianggap OP tak dikenal -> rusak. */
  writeText(p, "P 0000000000000012\nDELETE 0000000000000012 0000000000000034 1 1 1\n");
  CsStore *st = csOpen(p);
  CHECK(st == NULL);
  if (st) csClose(st);
}

static bool countVisit(const CsEntry *e, CsOp op, void *ctx) {
  (void)e; (void)op;
  (*(size_t *)ctx)++;
  return true;
}

static void testForEach(void) {
  char p[512];
  path(p, sizeof(p), "foreach.cache");
  CsStore *st = csOpen(p);
  for (uint64_t i = 1; i <= 7; i++) {
    CsEntry e = {i % 2 ? 1 : 2, i, i, 1, 1, NULL};
    CHECK(csPut(st, &e));
  }
  size_t seen = 0;
  CHECK(csForEach(st, countVisit, &seen));
  CHECK(seen == 7);
  csClose(st);
}

static void testPayload(void) {
  char p[512];
  path(p, sizeof(p), "payload.cache");
  uint64_t proj = csHash("core");
  uint64_t file = csHash("core/src/a.c");

  /* Payload dengan spasi dan '\x01' (pemisah edges). */
  CsStore *st = csOpen(p);
  CHECK(st != NULL);
  const char *pay = "include/a.h\x01include/deep dir/b.h";
  CsEntry e = {proj, file, 0x11ULL, 42, 7, pay};
  CHECK(csPut(st, &e));
  /* Entris tanpa payload tetap sah dalam store yang sama. */
  CsEntry bare = {proj, csHash("core/src/b.c"), 0x22ULL, 43, 8, NULL};
  CHECK(csPut(st, &bare));
  CHECK(csCommit(st));
  csClose(st);

  st = csOpen(p);
  CHECK(st != NULL && csCount(st) == 2);
  CsEntry got;
  CHECK(csGet(st, proj, file, &got));
  CHECK(got.payload != NULL && strcmp(got.payload, pay) == 0);
  CHECK(csGet(st, proj, csHash("core/src/b.c"), &got));
  CHECK(got.payload == NULL);

  /* PATCH payload: ganti, lalu kosongkan. */
  CsPatch repl = {0};
  repl.hasPayload = true;
  repl.payload = "x/y.h";
  CHECK(csPatch(st, proj, file, &repl));
  CHECK(csGet(st, proj, file, &got));
  CHECK(got.payload && strcmp(got.payload, "x/y.h") == 0);
  CsPatch clear = {0};
  clear.hasPayload = true;
  clear.payload = NULL;
  CHECK(csPatch(st, proj, file, &clear));
  CHECK(csGet(st, proj, file, &got));
  CHECK(got.payload == NULL);
  CHECK(csCommit(st));
  csClose(st);

  st = csOpen(p); /* muat ulang: payload lama tidak lagi ada */
  CHECK(st != NULL);
  CHECK(csGet(st, proj, file, &got));
  CHECK(got.payload == NULL);
  csClose(st);
}

static void testPayloadLongLine(void) {
  /* Payload >> CS_LINE_MAX lama (512): baris panjang harus terbaca utuh
     (readLineGrow), termasuk payload yang menyentuh batas buffer. */
  char p[512];
  path(p, sizeof(p), "long.cache");
  size_t N = 100000;
  char *big = malloc(N + 1);
  CHECK(big != NULL);
  if (!big) return;
  for (size_t i = 0; i < N; i++) big[i] = (char)('a' + (i % 26));
  big[N] = '\0';

  CsStore *st = csOpen(p);
  CHECK(st != NULL);
  CsEntry e = {1, 2, 3, 4, 5, big};
  CHECK(csPut(st, &e));
  CHECK(csCommit(st));
  csClose(st);

  st = csOpen(p);
  CHECK(st != NULL && csCount(st) == 1);
  CsEntry got;
  CHECK(csGet(st, 1, 2, &got));
  CHECK(got.payload != NULL && strlen(got.payload) == N && strcmp(got.payload, big) == 0);
  csClose(st);
  free(big);
}

static void testRacy(void) {
  CHECK(csRacy(100, 100));  /* sama detik = racy: wajib hash */
  CHECK(csRacy(101, 100));
  CHECK(!csRacy(99, 100));
}

static void testScale(void) {
  char p[512];
  path(p, sizeof(p), "scale.cache");
  CsStore *st = csOpen(p);
  const size_t N = 5000;
  char name[64];
  for (size_t i = 0; i < N; i++) {
    snprintf(name, sizeof(name), "proj%zu/src/f%zu.c", i % 50, i);
    CsEntry e = {csHash(name) >> 1, csHash(name), (uint64_t)i, (long long)i, (long long)(i * 3), NULL};
    CHECK(csPut(st, &e));
  }
  CHECK(csCount(st) == N);
  CHECK(csCommit(st));
  csClose(st);

  st = csOpen(p);
  CHECK(st != NULL && csCount(st) == N);
  CsEntry got;
  size_t hits = 0;
  for (size_t i = 0; i < N; i++) {
    snprintf(name, sizeof(name), "proj%zu/src/f%zu.c", i % 50, i);
    if (csGet(st, csHash(name) >> 1, csHash(name), &got) && got.content == i && got.size == (long long)(i * 3))
      hits++;
  }
  CHECK(hits == N);
  csClose(st);
}

int main(void) {
  snprintf(dir, sizeof(dir), "/tmp/cs_test_%d", (int)getpid());
  mkdir(dir, 0755);
  testHash();
  testMissingFileIsEmpty();
  testPutGetPatchDeleteAndPersist();
  testExactFormat();
  testGroupingByProject();
  testRejectsCorruptFiles();
  testPayload();
  testPayloadLongLine();
  testRacy();
  testForEach();
  testScale();
  printf("cache_store: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

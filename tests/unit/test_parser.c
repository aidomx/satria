/* Uji unit parser DSL (src/test/parser.c). Jalankan dari root repo:
     gcc -std=c11 -Wall -Wextra -Isrc -o /tmp/test_parser \
         tests/unit/test_parser.c src/test/parser.c
     /tmp/test_parser
   Exit 0 = semua lulus. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test/parser.h"

static int failures = 0, checks = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    checks++;                                                             \
    if (!(cond)) {                                                        \
      failures++;                                                         \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                     \
  } while (0)

static char *readAll(FILE *f) {
  if (fflush(f) != 0) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) return NULL;
  long n = ftell(f);
  if (n < 0) return NULL;
  rewind(f);
  char *buf = malloc((size_t)n + 1);
  if (!buf) return NULL;
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = '\0';
  return buf;
}

static FILE *fileFrom(const char *src) {
  FILE *f = tmpfile();
  if (!f) return NULL;
  fputs(src, f);
  rewind(f);
  return f;
}

/* bandingkan keluaran penuh; `wantOut`/`wantErr` NULL = lewati pengecekan. */
static void runCase(const char *name, const char *src, int wantRc, const char *wantOut,
                    const char *wantErr) {
  FILE *in = fileFrom(src);
  FILE *out = tmpfile();
  FILE *err = tmpfile();
  if (!in || !out || !err) {
    failures++;
    fprintf(stderr, "FAIL %s: tmpfile gagal\n", name);
    if (in) fclose(in);
    if (out) fclose(out);
    if (err) fclose(err);
    return;
  }
  int rc = testParseDsl(in, true, out, err);
  fclose(in);

  char *gotOut = readAll(out);
  char *gotErr = readAll(err);
  fclose(out);
  fclose(err);

  checks++;
  if (rc != wantRc) {
    failures++;
    fprintf(stderr, "FAIL %s: rc %d, harap %d\n", name, rc, wantRc);
  }
  if (wantOut && (!gotOut || strcmp(gotOut, wantOut) != 0)) {
    failures++;
    fprintf(stderr, "FAIL %s: jejak beda\n--- harap ---\n%s--- dapat ---\n%s", name,
            wantOut, gotOut ? gotOut : "(null)");
  }
  if (wantErr && (!gotErr || strcmp(gotErr, wantErr) != 0)) {
    failures++;
    fprintf(stderr, "FAIL %s: stderr beda\n--- harap ---\n%s--- dapat ---\n%s", name,
            wantErr, gotErr ? gotErr : "(null)");
  }
  free(gotOut);
  free(gotErr);
}

int main(void) {
  /* ---- jejak lengkap --------------------------------------------------- */
  runCase("lengkap",
          "# komentar\n"
          "@suite project\n"
          "@purpose \"contoh\"\n"
          "@requires cold\n"
          "@keep\n"
          "\n"
          "file.write Buildfile <<T\n"
          "use project\n"
          "# isi bukan komentar\n"
          "  indent\n"
          "T\n"
          "\n"
          "run.build as cold\n"
          "run.cmd -w -g compdb as base\n"
          "\n"
          "@response cold\n"
          "  rc.ok\n"
          "  out.contains \"Nothing\"\n"
          "@end\n",
          0,
          "CMT\t1\t# komentar\n"
          "HDR\t2\tsuite\tproject\n"
          "HDR\t3\tpurpose\t\"contoh\"\n"
          "HDR\t4\trequires\tcold\n"
          "HDR\t5\tkeep\t\n"
          "STMT\t7\tfile\twrite\tBuildfile <<T\n"
          "OPEN\t7\theredoc\tT\n"
          "TEXT\t8\tuse project\n"
          "TEXT\t9\t# isi bukan komentar\n"
          "TEXT\t10\t  indent\n"
          "CLOSE\t11\theredoc\tT\n"
          "STMT\t13\trun\tbuild\tas cold\n"
          "STMT\t14\trun\tcmd\t-w -g compdb as base\n"
          "BLOCK\t16\tresponse\tcold\n"
          "STMT\t17\trc\tok\t\n"
          "STMT\t18\tout\tcontains\t\"Nothing\"\n"
          "ENDB\t19\tresponse\n",
          "");

  runCase("heredoc-eof", "file.write x <<T\nabc", 0,
          "STMT\t1\tfile\twrite\tx <<T\n"
          "OPEN\t1\theredoc\tT\n"
          "TEXT\t2\tabc\n"
          "CLOSE\t2\theredoc\tT\n",
          "");

  runCase("kosong", "", 0, "", "");

  runCase("crlf-indent",
          "\r\n"
          "  # x\r\n"
          "\t@suite  project  \r\n"
          "run.build\tas cold  \r\n",
          0,
          "CMT\t2\t# x\n"
          "HDR\t3\tsuite\tproject\n"
          "STMT\t4\trun\tbuild\tas cold  \n",
          "");

  /* ---- direktif & blok ------------------------------------------------- */
  runCase("if-else", "@if a == 1\n@else\n@end\n", 0,
          "BLOCK\t1\tif\ta == 1\n"
          "DIR\t2\telse\t\n"
          "ENDB\t3\tif\n",
          "");
  runCase("for", "@for i in 1..3\n@end\n", 0,
          "BLOCK\t1\tfor\ti in 1..3\n"
          "ENDB\t2\tfor\n",
          "");
  runCase("include-as", "@include frag/root.test as r\n", 0,
          "DIR\t1\tinclude\tfrag/root.test as r\n", "");
  runCase("directives", "@return\n@set v \"a b\"\n@get v\n@find v *\n@guard x\n",
          0,
          "DIR\t1\treturn\t\n"
          "DIR\t2\tset\tv \"a b\"\n"
          "DIR\t3\tget\tv\n"
          "DIR\t4\tfind\tv *\n"
          "DIR\t5\tguard\tx\n",
          "");
  runCase("while", "@while x\n@end\n", 0, "BLOCK\t1\twhile\tx\nENDB\t2\twhile\n", "");

  /* ---- negatif --------------------------------------------------------- */
  runCase("objek-asing", "foo.bar\n", 1, NULL, "baris 1: objek tidak dikenal: foo\n");
  runCase("action-asing", "file.rewind x\n", 1, NULL,
          "baris 1: action tidak dikenal: file.rewind\n");
  runCase("else-di-luar", "@else\n", 1, NULL, "baris 1: @else di luar @if\n");
  runCase("header-lambat", "run.build\n@fixture x\n", 1, NULL,
          "baris 2: direktif header setelah badan\n");
  runCase("write-tanpa-heredoc", "file.write x\n", 1, NULL,
          "baris 1: file.write memerlukan heredoc: file.write <path> <<T\n");
  runCase("heredoc-action-lain", "file.append x <<T\nT\n", 1, NULL,
          "baris 1: heredoc hanya sah pada file.write\n"
          "baris 2: pernyataan tidak dipahami: T\n");
  runCase("set-tanpa-nilai", "@set alat\n", 1, NULL,
          "baris 1: bentuk: @set <nama> <ekspresi>\n");
  runCase("end-tanpa-blok", "@end\n", 1, NULL, "baris 1: @end tanpa blok terbuka\n");
  runCase("blok-tanpa-end", "@if x\nrun.build\n", 1, NULL,
          "baris 1: blok @if (mulai baris ini) tanpa @end\n");
  runCase("direktif-asing", "@bogus x\n", 1, NULL,
          "baris 1: direktif tidak dikenal: @bogus\n");
  runCase("heredoc-di-build", "run.build <<T\nT\n", 1, NULL,
          "baris 1: heredoc hanya sah pada file.write\n"
          "baris 2: pernyataan tidak dipahami: T\n");
  runCase("marker-cacat", "file.write x <<9\n", 1, NULL,
          "baris 1: marker heredoc tidak sah di: x <<9\n");
  runCase("heredoc-ganda", "file.write x <<A <<B\n", 1, NULL,
          "baris 1: hanya satu heredoc per pernyataan\n");
  runCase("else-kedua", "@if x\n@else\n@else\n@end\n", 1, NULL,
          "baris 3: @else kedua pada @if yang sama\n");
  runCase("response-dua", "@response a b\n", 1, NULL,
          "baris 1: bentuk: @response [label]\n");
  runCase("for-cacat", "@for x ny\n", 1, NULL,
          "baris 1: bentuk: @for <var> in <rentang|koleksi>\n");
  runCase("get-kosong", "@get\n", 1, NULL, "baris 1: bentuk: @get <nama>\n");
  runCase("find-satu", "@find a\n", 1, NULL, "baris 1: bentuk: @find <nama> <pola>\n");
  runCase("set-nama-cacat", "@set 1x y\n", 1, NULL,
          "baris 1: nama variabel tidak sah: 1x\n");
  runCase("requires-cacat", "@requires foo\n", 1, NULL,
          "baris 1: bentuk: @requires cold|snap\n");
  runCase("keep-arg", "@keep x\n", 1, NULL, "baris 1: @keep tidak menerima argumen\n");
  runCase("suite-kosong", "@suite\n", 1, NULL, "baris 1: @suite memerlukan argumen\n");
  runCase("include-dua", "@include a b\n", 1, NULL,
          "baris 1: bentuk: @include <path> [as <nama>]\n");
  runCase("include-as-kurang", "@include a as\n", 1, NULL,
          "baris 1: bentuk: @include <path> [as <nama>]\n");
  runCase("pernyataan-cacat", "a..b\n", 1, NULL,
          "baris 1: pernyataan tidak dipahami: a..b\n");

  /* ---- berkas ---------------------------------------------------------- */
  {
    FILE *err = tmpfile();
    int rc = testParseFile("/tidak/ada/berkas.test", true, stdout, err);
    CHECK(rc == 2);
    if (err) {
      char *s = readAll(err);
      CHECK(s && strcmp(s, "parser: berkas tidak ada: /tidak/ada/berkas.test\n") == 0);
      free(s);
      fclose(err);
    }
    err = tmpfile();
    rc = testParseFile(".", true, stdout, err);
    CHECK(rc == 2);
    if (err) fclose(err);
  }

  printf("%d cek, %d gagal\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

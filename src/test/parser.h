#pragma once
/* Parser berkas DSL `.test` (design/unit_test_dsl.md) — padanan C dari
   scripts/lib/parser.sh. Parse-only: tokenisasi per baris, mesin status
   heredoc, tumpukan blok, validasi objek/aksi, kesalahan bernomor baris.
   TIDAK mengeksekusi apa pun.

   Jejak event (dipisah TAB, satu baris per event):
     HDR   <baris> <direktif> <argumen>
     DIR   <baris> <direktif> <argumen>
     BLOCK <baris> <direktif> <label/ekspresi>
     ENDB  <baris> <direktif>
     STMT  <baris> <objek> <aksi> <argumen>
     OPEN  <baris> heredoc <marker>
     TEXT  <baris> <isi>
     CLOSE <baris> heredoc <marker>
     CMT   <baris> <teks> */

#include <stdbool.h>
#include <stdio.h>

/* Parse aliran `in`. Bila `trace`, tulis jejak ke `traceOut`; seluruh error
   (`baris N: …`) ke `errOut`. Return 0 = sah, 1 = salah parse. */
int testParseDsl(FILE *in, bool trace, FILE *traceOut, FILE *errOut);

/* Parse berkas. Return 0 = sah, 1 = salah parse, 2 = berkas tidak ada
   (bukan berkas biasa). */
int testParseFile(const char *path, bool trace, FILE *traceOut, FILE *errOut);

/* CLI `satria test --parse …` — argc/argv TANPA kata "--parse".
   `--check <file>`, `--suite <dir>`, atau `<file>` (jejak). */
int testParserMain(int argc, const char *const *argv);

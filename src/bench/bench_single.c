/*
 * bench_single.c — mode single project (porting bench/compare.sh).
 *
 * Generator satu proyek sintetis: N source berantai header H, gaya /tmp/bench,
 * seperti bench/compare/gen.sh:
 *   include/h1.h .. hH.h   (h_j meng-include h_{j-1})
 *   src/t1.c .. tN.c       (t_i meng-include h_{i % H + 1})
 *   src/main.c
 * Ditulis Buildfile (satria), build.ninja, Makefile, CMakeLists.txt,
 * meson.build, xmake.lua, dan Tupfile untuk pohon yang sama, sehingga tiap
 * tool diukur pada project yang setara.
 */
#include "bench_internal.h"

#ifdef _WIN32
void benchGenProject(Bench *b, BenchTool *t) { (void)b; (void)t; }
void benchMutateSingle(const Bench *b, const char *scen, int run, const char *label,
                       const char *ws) {
  (void)b; (void)scen; (void)run; (void)label; (void)ws;
}
#else

#include "../portability.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool benchSingleWrite(const char *path, const char *data) {
  mkparent(path);
  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "bench: tidak bisa menulis %s: %s\n", path, strerror(errno));
    return false;
  }
  fputs(data, f);
  fclose(f);
  return true;
}

static bool benchSingleAppend(const char *path, const char *data) {
  FILE *f = fopen(path, "ab");
  if (!f) return false;
  fputs(data, f);
  fclose(f);
  return true;
}

static void benchGenBuildfile(Bench *b, const char *ws) {
  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/Buildfile");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("use project c\n", f);
  fputs("\nclean as c\noutput as o\n\nroot = .\n\n", f);
  fputs("sources = src\n", f);
  fputs("headers = include\n", f);
  fputs("flags = O2, MMD, MP\n", f);
  fputs("std = gnu11\n", f);
  fprintf(f, "compiler = %s\n", b->compilers);
  fputs("c.compdb = false\n", f);
  fputs("foreground = false\n\n", f);
  fputs("o.binaryName = app\n", f);
  fputs("o.binaryDir = bin\n", f);
  fputs("o.buildDir = build\n", f);
  fputs("o.compileCommands = false\n", f);
  fclose(f);
}

/* Graph Ninja ditulis dari daftar source yang diketahui; cold mengukur
   pembuatan ulang graph ini (setara gen_ninja_graph di compare.sh). */
void benchGenNinjaGraph(const Bench *b, const char *ws) {
  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/build.ninja");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("cflags = -O2 -std=gnu11 -Iinclude\n\n", f);
  fputs("rule cc\n", f);
  fputs("  command = gcc $cflags -MMD -MP -MF $out.d -c $in -o $out\n", f);
  fputs("  depfile = $out.d\n", f);
  fputs("  deps = gcc\n", f);
  fputs("rule link\n", f);
  /* Daftar object bisa melewati ARG_MAX pada proyek besar. Simpan input
     linker ke response file agar command yang dijalankan Ninja tetap pendek. */
  fputs("  command = gcc @$out.rsp -o $out\n", f);
  fputs("  rspfile = $out.rsp\n", f);
  fputs("  rspfile_content = $in\n\n", f);
  for (int i = 1; i <= b->source; i++)
    fprintf(f, "build build/t%d.o: cc src/t%d.c\n", i, i);
  fputs("build build/main.o: cc src/main.c\n", f);
  fputs("build bin/app: link build/main.o", f);
  for (int i = 1; i <= b->source; i++) fprintf(f, " build/t%d.o", i);
  fputs("\n\ndefault bin/app\n", f);
  fclose(f);
}

static void benchGenMakefile(const Bench *b, const char *ws) {
  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/Makefile");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("CC      := gcc\n", f);
  fputs("CFLAGS  := -O2 -std=gnu11 -Iinclude\n", f);
  fputs("OBJS    := build/main.o", f);
  for (int i = 1; i <= b->source; i++) fprintf(f, " build/t%d.o", i);
  fputs("\n\n", f);
  fputs("bin/app: $(OBJS)\n", f);
  fputs("\t$(CC) $^ -o $@\n\n", f);
  fputs("build/%.o: src/%.c\n", f);
  fputs("\t@mkdir -p build\n", f);
  fputs("\t$(CC) $(CFLAGS) -MMD -MP -c $< -o $@\n\n", f);
  fputs("-include $(OBJS:.o=.d)\n", f);
  fclose(f);
}

static void benchGenMeson(const Bench *b, const char *ws) {
  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/meson.build");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("project('app', 'c',\n", f);
  fputs("        version : '1.0',\n", f);
  fputs("        default_options : ['c_std=gnu11', 'warning_level=0', 'optimization=2'])\n\n", f);
  fputs("inc = include_directories('include')\n", f);
  fputs("src = files(\n", f);
  fputs("  'src/main.c'", f);
  for (int i = 1; i <= b->source; i++) fprintf(f, ",\n  'src/t%d.c'", i);
  fputs("\n)\n\n", f);
  fputs("executable('app', src,\n", f);
  fputs("           include_directories : inc,\n", f);
  fputs("           install : false)\n", f);
  fclose(f);
}

static void benchGenXmake(const char *ws) {
  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/xmake.lua");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("set_project(\"app\")\n", f);
  fputs("set_version(\"1.0.0\")\n\n", f);
  fputs("set_languages(\"gnu11\")\n", f);
  fputs("set_warnings(\"none\")\n", f);
  fputs("set_optimize(\"fast\")\n\n", f);
  fputs("add_includedirs(\"include\")\n\n", f);
  fputs("target(\"app\")\n", f);
  fputs("    set_kind(\"binary\")\n", f);
  fputs("    add_files(\"src/*.c\")\n", f);
  fclose(f);
}

/* Satu rule eksplisit per source: output objek datar di build/, lhs glob
   pada link menangkap semuanya tanpa bergantung pada pola substitusi Tup. */
static void benchGenTupfile(const Bench *b, const char *ws) {
  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/Tupfile");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("CFLAGS = -O2 -std=gnu11 -Iinclude -MMD -MP\n\n", f);
  fprintf(f, ": src/main.c |> gcc $(CFLAGS) -c %%f -o build/main.o |> build/main.o\n");
  for (int i = 1; i <= b->source; i++)
    fprintf(f, ": src/t%d.c |> gcc $(CFLAGS) -c %%f -o build/t%d.o |> build/t%d.o\n", i, i, i);
  fputs("\n: build/*.o |> gcc %f -o %o |> bin/app\n", f);
  fclose(f);
}

/* Skenario compdb satria butuh output.compileCommands != false; Buildfile
   proyek benchmark mematikannya untuk build normal, jadi baris itu diganti
   sementara (seperti sed di compare.sh) lalu dikembalikan. */
bool benchBuildfileCompdb(const char *ws, bool enabled) {
  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/Buildfile");
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  char buf[16384];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = '\0';
  fclose(f);
  const char *key = "o.compileCommands =";
  char *p = strstr(buf, key);
  if (!p) return false;
  char *nl = strchr(p, '\n');
  size_t off = nl ? (size_t)(nl - buf) + 1 : n;
  const char *line = enabled ? "o.compileCommands = auto\n" : "o.compileCommands = false\n";
  FILE *o = fopen(path, "wb");
  if (!o) return false;
  fwrite(buf, 1, (size_t)(p - buf), o);
  fputs(line, o);
  fwrite(buf + off, 1, n - off, o);
  fclose(o);
  return true;
}

static void benchGenCmakeSingle(const char *ws) {
  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/CMakeLists.txt");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("cmake_minimum_required(VERSION 3.12)\n", f);
  fputs("project(app C)\n\n", f);
  fputs("set(CMAKE_C_STANDARD 11)\n", f);
  fputs("set(CMAKE_C_EXTENSIONS ON)\n", f);
  fputs("set(CMAKE_C_STANDARD_REQUIRED ON)\n", f);
  fputs("set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n", f);
  fputs("set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)\n\n", f);
  fputs("file(GLOB SOURCES CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/src/*.c\")\n", f);
  fputs("list(SORT SOURCES)\n", f);
  fputs("add_executable(app ${SOURCES})\n", f);
  fputs("target_include_directories(app PRIVATE include)\n", f);
  fputs("target_compile_options(app PRIVATE -O2 -MMD -MP)\n", f);
  fclose(f);
}

void benchGenProject(Bench *b, BenchTool *t) {
  const char *ws = t->ws;
  fsRemoveTree(ws);
  char dir[BENCH_PATH_LEN], path[BENCH_PATH_LEN];
  benchPathCat(dir, sizeof(dir), ws, "/src");
  mkdirs(dir);
  benchPathCat(dir, sizeof(dir), ws, "/include");
  mkdirs(dir);
  benchPathCat(dir, sizeof(dir), ws, "/bin");
  mkdirs(dir);
  benchPathCat(dir, sizeof(dir), ws, "/build");
  mkdirs(dir);
  mkdirs(ws);

  /* Rantai include antar header dibatasi kedalamannya lewat modulo (CAP),
     bukan linear h -> h-1: linear membuat kedalaman include == b->header,
     dan compiler menolak begitu melewati -fmax-include-depth (default 200)
     untuk b->header besar (mis. 1000+). Dengan wrap, setiap header tetap
     meng-include header lain (h != dirinya sendiri bila header > 1), tapi
     kedalaman rantai yang terbentuk tidak pernah melebihi CAP+1, berapa pun
     b->header-nya. Jumlah file dan nilai v%d per header tidak berubah. */
  const int BENCH_HEADER_CHAIN_CAP = 64;
  for (int h = 1; h <= b->header; h++) {
    char tail[32];
    snprintf(tail, sizeof(tail), "/include/h%d.h", h);
    benchPathCat(path, sizeof(path), ws, tail);
    FILE *f = fopen(path, "wb");
    if (!f) continue;
    fputs("#pragma once\n", f);
    if (h > 1) {
      int prev;
      if (h <= BENCH_HEADER_CHAIN_CAP) {
        prev = h - 1;
      } else {
        /* (h - CAP - 1) bisa 0 saat h == CAP + 1; modulo lalu +1 menjaga
           prev selalu di rentang [1, CAP], tidak pernah 0 atau negatif. */
        prev = (h - BENCH_HEADER_CHAIN_CAP - 1) % BENCH_HEADER_CHAIN_CAP + 1;
      }
      fprintf(f, "#include \"h%d.h\"\n", prev);
    }
    fprintf(f, "static const int v%d = %d;\n", h, h);
    fclose(f);
  }

  for (int i = 1; i <= b->source; i++) {
    int hv = i % b->header + 1;
    char tail[48];
    snprintf(tail, sizeof(tail), "/src/t%d.c", i);
    benchPathCat(path, sizeof(path), ws, tail);
    FILE *f = fopen(path, "wb");
    if (!f) continue;
    fprintf(f, "#include \"h%d.h\"\n", hv);
    /* Nilai balik = i (header tetap dipakai supaya depfile nyata); main
       membandingkan t1() == 1 agar runtime check lolos. */
    fprintf(f, "int t%d(void) { return v%d - v%d + %d; }\n", i, hv, hv, i);
    fclose(f);
  }

  benchPathCat(path, sizeof(path), ws, "/src/main.c");
  benchSingleWrite(path, "#include \"h1.h\"\nint t1(void);\nint main(void) { return t1() - 1; }\n");

  benchGenBuildfile(b, ws);
  benchGenNinjaGraph(b, ws);
  benchGenMakefile(b, ws);
  benchGenCmakeSingle(ws);
  benchGenMeson(b, ws);
  benchGenXmake(ws);
  benchGenTupfile(b, ws);

  /* Tup membutuhkan database proyek sebelum bisa dipakai. Ini bagian dari
     persiapan generator, bukan waktu build yang diukur. */
  if (t->kind == BENCH_TUP) {
    char *argv[] = {(char *)"tup", (char *)"init", NULL};
    int rc = 0;
    benchRunArgv(ws, argv, b->errlog, &rc, NULL);
  }
}

void benchMutateSingle(const Bench *b, const char *scen, int run, const char *label,
                       const char *ws) {
  char path[BENCH_PATH_LEN], text[256];
  if (strcmp(scen, "touch") == 0) {
    char tail[48];
    snprintf(tail, sizeof(tail), "/include/h%d.h", b->header / 2 + 1);
    benchPathCat(path, sizeof(path), ws, tail);
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = 0;
    ts[0].tv_nsec = ts[1].tv_nsec = UTIME_NOW;
    utimensat(AT_FDCWD, path, ts, 0);
  } else if (strcmp(scen, "edit") == 0) {
    benchPathCat(path, sizeof(path), ws, "/src/t1.c");
    snprintf(text, sizeof(text), "\n/* edit %s-%d */\n", label, run);
    benchSingleAppend(path, text);
  } else if (strcmp(scen, "editrel") == 0) {
    benchPathCat(path, sizeof(path), ws, "/src/t1.c");
    /* Label bisa mengandung '-' (gcc-floor): jadikan identifier C yang sah. */
    char ident[BENCH_LABEL_LEN];
    size_t j = 0;
    for (size_t i = 0; label[i] && j + 1 < sizeof(ident); i++) {
      char c = label[i];
      ident[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9')) ? c : '_';
    }
    ident[j] = '\0';
    snprintf(text, sizeof(text), "\nint bench_r%s_%d(void) { return %d; }\n", ident, run, run);
    benchSingleAppend(path, text);
  }
}

#endif /* !_WIN32 */

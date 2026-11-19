/*
 * bench_ws.c — generator workspace sintetis, mutator skenario, snapshot
 * artefak, dan salinan pohon (prime). Semua logika ini dulu ada di
 * bench/ws/gen.sh + scen.sh + measure.sh.
 */
#include "bench_internal.h"

#ifdef _WIN32
/* Benchmark hanya POSIX (fork/exec, symlink, utimensat). Build Windows tetap
   lolos karena fungsi kontrak di bench_internal.h punya stub di sini. */
bool benchPathExists(const char *path) { (void)path; return false; }
void benchGenWorkspace(Bench *b, BenchTool *t) { (void)b; (void)t; }
void benchMutate(const Bench *b, const char *scen, int run, const char *label, const char *ws) {
  (void)b; (void)scen; (void)run; (void)label; (void)ws;
}
bool benchCopyTree(const char *src, const char *dst) { (void)src; (void)dst; return false; }
bool benchSnapTake(const char *root, BenchSnap *out) { (void)root; memset(out, 0, sizeof(*out)); return false; }
void benchSnapFree(BenchSnap *s) { (void)s; }
void benchDelta(const BenchSnap *before, const BenchSnap *after, BenchDelta *d) {
  (void)before; (void)after; memset(d, 0, sizeof(*d));
}
#else

#include "../portability.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* struct stat macOS memakai st_*timespec; glibc memakai st_*tim.
   Alias agar snapshot mtime/ctime portable (pola sama dengan port_fs.c). */
#if defined(__APPLE__)
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#endif

bool benchPathExists(const char *path) {
  struct stat st;
  return path && *path && stat(path, &st) == 0;
}

static bool benchWriteFile(const char *path, const char *data) {
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

static bool benchAppendFile(const char *path, const char *data) {
  FILE *f = fopen(path, "ab");
  if (!f) return false;
  fputs(data, f);
  fclose(f);
  return true;
}

/* ---------- generator C (gen_c) ---------- */

static void benchGenC(Bench *b, const char *ws, const char *proj, const char *pre) {
  char path[BENCH_PATH_LEN], dir[BENCH_PATH_LEN];
  snprintf(dir, sizeof(dir), "%s/%s/src", ws, proj);
  mkdirs(dir);

  for (int h = 1; h <= b->header; h++) {
    char tail[32];
    snprintf(tail, sizeof(tail), "/h%d.h", h);
    benchPathCat(path, sizeof(path), dir, tail);
    FILE *f = fopen(path, "wb");
    if (!f) continue;
    fputs("#pragma once\n", f);
    if (h > 1) fprintf(f, "#include \"h%d.h\"\n", h / 2);
    fprintf(f, "static const int v%d = %d;\n", h, h);
    fclose(f);
  }
  for (int i = 1; i <= b->source; i++) {
    int hv = i % b->header + 1;
    char tail[96];
    snprintf(tail, sizeof(tail), "/%s%d.c", pre, i);
    benchPathCat(path, sizeof(path), dir, tail);
    FILE *f = fopen(path, "wb");
    if (!f) continue;
    fprintf(f, "#include \"h%d.h\"\n", hv);
    fprintf(f, "int %s%d(void) { return v%d + %d; }\n", pre, i, hv, i);
    fclose(f);
  }
  benchPathCat(path, sizeof(path), dir, "/main.c");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("#include \"h1.h\"\n", f);
  fprintf(f, "int %s1(void);\n", pre);
  fprintf(f, "int main(void) { return %s1() - 1; }\n", pre);
  fclose(f);
}

static void benchGenSatriaWsBuildfile(Bench *b, const char *ws) {
  char path[BENCH_PATH_LEN], tmp[BENCH_PATH_LEN];
  const char *env = getenv("TMPDIR");
  snprintf(tmp, sizeof(tmp), "%s/rupa-system", env && *env ? env : "/tmp");
  benchPathCat(path, sizeof(path), ws, "/WsBuildfile");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("use workspace c\n", f);
  fputs("projects.rupamod as mod\n", f);
  fputs("projects.core as core\n", f);
  fputs("projects = bade, core, rupamod\n", f);
  fprintf(f, "compiler = %s\n", b->compilers);
  fputs("flags = Wall, Wextra, MMD, MP, fPIC\n", f);
  fputs("headers.each = auto\n", f);
  fputs("std = gnu11\n", f);
  fputs("mod.output.binary = false\n", f);
  fputs("mod.archive.src = .\n", f);
  fputs("mod.archive.name = rupa_modules\n", f);
  fputs("mod.archive.with = tar\n", f);
  fputs("mod.archive.dir = modules\n", f);
  fputs("mod.archive.exclude = build, dist, LICENSE, README.md, self_archive\n", f);
  fputs("core.sources = src\n", f);
  fputs("core.exclude = main.c\n", f);
  fputs("core.output.binaryName = rupa\n", f);
  fputs("core.output.libraryName = core\n", f);
  fputs("core.output.libraryShared = true\n", f);
  fputs("bade.sources = manager, src\n", f);
  fputs("bade.exclude = manager/main.c\n", f);
  fputs("bade.output.binaryName = bade\n", f);
  fputs("bade.output.libraryName = bade\n", f);
  fputs("bade.output.libraryShared = true\n", f);
  fputs("bade.depends_on = rupamod\n", f);
  fputs("bade.embedded.modules.file = modules/rupa_modules.tar\n", f);
  fputs("bade.embedded.modules.variable = MODULES\n", f);
  fprintf(f, "bade.embedded.modules.extract = %s\n", tmp);
  fputs("bade.embedded.modules.pattern = .rp\n", f);
  fputs("bade.library.cdeps = core\n", f);
  fputs("core.library.cdeps = bade\n", f);
  fclose(f);
}

static void benchGenCmake(Bench *b, const char *ws) {
  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/CMakeLists.txt");
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs("cmake_minimum_required(VERSION 3.20)\n", f);
  fputs("project(wsbench C)\n", f);
  fputs("set(CMAKE_C_STANDARD 11)\n", f);
  fputs("set(CMAKE_C_EXTENSIONS ON)\n", f);
  fputs("add_compile_options(-Wall -Wextra -MMD -MP -fPIC)\n", f);
  fputs("include_directories(\"${CMAKE_SOURCE_DIR}/include\")\n", f);
  fputs("file(GLOB_RECURSE CORE_SRC CONFIGURE_DEPENDS \"${CMAKE_SOURCE_DIR}/core/src/*.c\")\n", f);
  fputs("file(GLOB_RECURSE BADE_SRC CONFIGURE_DEPENDS \"${CMAKE_SOURCE_DIR}/bade/src/*.c\")\n", f);
  fputs("list(REMOVE_ITEM CORE_SRC \"${CMAKE_SOURCE_DIR}/core/src/main.c\")\n", f);
  fputs("list(REMOVE_ITEM BADE_SRC \"${CMAKE_SOURCE_DIR}/bade/src/main.c\")\n", f);
  fputs("file(GLOB_RECURSE MOD_SRC CONFIGURE_DEPENDS \"${CMAKE_SOURCE_DIR}/rupamod/*.rp\")\n", f);
  fputs("set(ARCHIVE \"${CMAKE_SOURCE_DIR}/modules/rupa_modules.tar\")\n", f);
  fputs("add_custom_command(OUTPUT \"${ARCHIVE}\"\n", f);
  fputs("  COMMAND ${CMAKE_COMMAND} -E make_directory \"${CMAKE_SOURCE_DIR}/modules\"\n", f);
  fputs("  COMMAND tar -cf \"${ARCHIVE}\" -C \"${CMAKE_SOURCE_DIR}/rupamod\"\n", f);
  fputs("    --exclude=build --exclude=dist --exclude=LICENSE\n", f);
  fputs("    --exclude=README.md --exclude=self_archive .\n", f);
  fputs("  DEPENDS ${MOD_SRC} VERBATIM)\n", f);
  fputs("set(EMBED \"${CMAKE_CURRENT_BINARY_DIR}/modules/rupa_modules.o\")\n", f);
  fputs("add_custom_command(OUTPUT \"${EMBED}\"\n", f);
  fputs("  COMMAND ${CMAKE_COMMAND} -E make_directory \"${CMAKE_CURRENT_BINARY_DIR}/modules\"\n", f);
  fprintf(f, "  COMMAND \"%s\" bench --embed-source \"${ARCHIVE}\" \"${EMBED}.c\" "
             "_binary_modules_rupa_modules_tar\n",
          b->self);
  fputs("  COMMAND gcc -c -fPIC -o \"${EMBED}\" \"${EMBED}.c\"\n", f);
  fputs("  DEPENDS \"${ARCHIVE}\" VERBATIM)\n", f);
  fputs("set_source_files_properties(\"${EMBED}\" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)\n", f);
  fputs("add_library(core_obj OBJECT ${CORE_SRC})\n", f);
  fputs("add_library(bade_obj OBJECT ${BADE_SRC})\n", f);
  fputs("set_target_properties(core_obj bade_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)\n", f);
  fputs("add_library(core_static STATIC $<TARGET_OBJECTS:core_obj>)\n", f);
  fputs("add_library(bade_static STATIC $<TARGET_OBJECTS:bade_obj> \"${EMBED}\")\n", f);
  fputs("add_library(core_shared SHARED $<TARGET_OBJECTS:core_obj>)\n", f);
  fputs("add_library(bade_shared SHARED $<TARGET_OBJECTS:bade_obj> \"${EMBED}\")\n", f);
  fputs("set_target_properties(core_static PROPERTIES OUTPUT_NAME core\n", f);
  fputs("  ARCHIVE_OUTPUT_DIRECTORY \"${CMAKE_SOURCE_DIR}/core/build\")\n", f);
  fputs("set_target_properties(bade_static PROPERTIES OUTPUT_NAME bade\n", f);
  fputs("  ARCHIVE_OUTPUT_DIRECTORY \"${CMAKE_SOURCE_DIR}/bade/build\")\n", f);
  fputs("set_target_properties(core_shared PROPERTIES OUTPUT_NAME core\n", f);
  fputs("  LIBRARY_OUTPUT_DIRECTORY \"${CMAKE_SOURCE_DIR}/core/lib\")\n", f);
  fputs("set_target_properties(bade_shared PROPERTIES OUTPUT_NAME bade\n", f);
  fputs("  LIBRARY_OUTPUT_DIRECTORY \"${CMAKE_SOURCE_DIR}/bade/lib\")\n", f);
  fputs("add_executable(rupa core/src/main.c)\n", f);
  fputs("add_executable(bade bade/src/main.c)\n", f);
  fputs("set_target_properties(rupa PROPERTIES RUNTIME_OUTPUT_DIRECTORY "
        "\"${CMAKE_SOURCE_DIR}/core/bin\")\n", f);
  fputs("set_target_properties(bade PROPERTIES RUNTIME_OUTPUT_DIRECTORY "
        "\"${CMAKE_SOURCE_DIR}/bade/bin\")\n", f);
  fputs("foreach(t IN ITEMS rupa bade)\n", f);
  fputs("  target_link_options(${t} PRIVATE "
        "\"LINKER:--undefined=_binary_modules_rupa_modules_tar_start\")\n", f);
  fputs("  target_link_libraries(${t} PRIVATE -Wl,--start-group core_static bade_static "
        "-Wl,--end-group)\n", f);
  fputs("endforeach()\n", f);
  fclose(f);
}

void benchGenWorkspace(Bench *b, BenchTool *t) {
  const char *ws = t->ws;
  fsRemoveTree(ws);
  char dir[BENCH_PATH_LEN];
  benchPathCat(dir, sizeof(dir), ws, "/rupamod");
  mkdirs(dir);
  benchGenC(b, ws, "core", "r");
  benchGenC(b, ws, "bade", "k");

  mkdirs(ws);
  benchPathCat(dir, sizeof(dir), ws, "/include");
  mkdirs(dir);
  benchPathCat(dir, sizeof(dir), ws, "/modules");
  mkdirs(dir);

  char path[BENCH_PATH_LEN];
  benchPathCat(path, sizeof(path), ws, "/include/a.h");
  benchWriteFile(path, "#pragma once\n#define WS_SHARED_VALUE 7\n");
  benchPathCat(path, sizeof(path), ws, "/include/b.h");
  benchWriteFile(path, "#pragma once\n#include \"a.h\"\n#define WS_BADE_VALUE WS_SHARED_VALUE\n");
  benchPathCat(path, sizeof(path), ws, "/include/ext.h");
  benchWriteFile(path, "#pragma once\n#define WS_EXT_VALUE 3\n");

  char data[256];
  for (int i = 1; i <= b->module; i++) {
    char tail[48];
    snprintf(tail, sizeof(tail), "/rupamod/m%d.rp", i);
    benchPathCat(path, sizeof(path), ws, tail);
    snprintf(data, sizeof(data), "module m%d\nexport f%d\n", i, i);
    benchWriteFile(path, data);
  }

  /* Simbol circular nyata + jalur balik independen. */
  benchPathCat(path, sizeof(path), ws, "/core/src/r1.c");
  benchWriteFile(path,
                 "#include \"a.h\"\n"
                 "int bade_entry(void);\n"
                 "int core_entry(void) { return bade_entry(); }\n");
  benchPathCat(path, sizeof(path), ws, "/core/src/r2.c");
  benchWriteFile(path,
                 "#include \"a.h\"\n"
                 "#include \"ext.h\"\n"
                 "int ws_ext_value = WS_EXT_VALUE;\n"
                 "int core_helper(void) { return WS_SHARED_VALUE + ws_ext_value - WS_EXT_VALUE; }\n");
  benchPathCat(path, sizeof(path), ws, "/bade/src/k1.c");
  benchWriteFile(path,
                 "#include \"b.h\"\n"
                 "int core_helper(void);\n"
                 "int bade_entry(void) { return core_helper() + (WS_BADE_VALUE - WS_SHARED_VALUE); }\n");
  benchPathCat(path, sizeof(path), ws, "/bade/src/k2.c");
  benchWriteFile(path,
                 "#include \"b.h\"\n"
                 "#include \"ext.h\"\n"
                 "int core_entry(void);\n"
                 "int bade_ext_value = WS_EXT_VALUE;\n"
                 "int bade_reverse(void) { return core_entry() + (WS_BADE_VALUE - WS_SHARED_VALUE) + "
                 "bade_ext_value - WS_EXT_VALUE; }\n");
  benchPathCat(path, sizeof(path), ws, "/core/src/main.c");
  benchWriteFile(path,
                 "#include \"a.h\"\n"
                 "int core_entry(void);\n"
                 "int main(void) { return core_entry() == WS_SHARED_VALUE ? 0 : 1; }\n");
  benchPathCat(path, sizeof(path), ws, "/bade/src/main.c");
  benchWriteFile(path,
                 "#include \"b.h\"\n"
                 "int bade_reverse(void);\n"
                 "int main(void) { return bade_reverse() == WS_BADE_VALUE ? 0 : 1; }\n");

  if (t->kind == BENCH_SATRIA)
    benchGenSatriaWsBuildfile(b, ws);
  else
    benchGenCmake(b, ws);
}

/* ---------- mutator skenario (scen.sh) ---------- */

static bool benchTouch(const char *path) {
  struct timespec ts[2];
  ts[0].tv_sec = ts[1].tv_sec = 0;
  ts[0].tv_nsec = ts[1].tv_nsec = UTIME_NOW;
  return utimensat(AT_FDCWD, path, ts, 0) == 0;
}

/* Ganti `#define <key> <N>` menjadi N+1 (isi berubah, nilai tetap koheren). */
static bool benchBumpDefine(const char *path, const char *key) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  char buf[512];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  char needle[128];
  snprintf(needle, sizeof(needle), "#define %s ", key);
  char *p = strstr(buf, needle);
  if (!p) return false;
  char *v = p + strlen(needle);
  char *end = NULL;
  long val = strtol(v, &end, 10);
  if (end == v) return false;
  char out[512];
  int head = (int)(v - buf);
  snprintf(out, sizeof(out), "%.*s%ld%s", head, buf, val + 1, end);
  return benchWriteFile(path, out);
}

/* Label dipakai sebagai bagian nama fungsi C, jadi hanya [A-Za-z0-9_] yang
   boleh lolos; karakter lain (mis. '-' pada label A/B `satria-2`) jadi '_'. */
static void benchSanitizeLabel(const char *label, char *out, size_t n) {
  size_t j = 0;
  if (n == 0) return;
  if (label[0] >= '0' && label[0] <= '9' && j + 1 < n) out[j++] = '_';
  for (size_t i = 0; label[i] && j + 1 < n; i++) {
    char c = label[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_';
    out[j++] = ok ? c : '_';
  }
  out[j] = '\0';
}

void benchMutate(const Bench *b, const char *scen, int run, const char *label, const char *ws) {
  (void)b;
  char path[BENCH_PATH_LEN], text[256], safe[BENCH_LABEL_LEN];
  benchSanitizeLabel(label, safe, sizeof(safe));

  if (strcmp(scen, "leaf") == 0 || strcmp(scen, "sel") == 0) {
    benchPathCat(path, sizeof(path), ws, "/core/src/r1.c");
    snprintf(text, sizeof(text), "\nint bench_leaf_%d_%s(void) { return %d; }\n", run, safe, run);
    benchAppendFile(path, text);
  }
  if (strcmp(scen, "bade") == 0 || strcmp(scen, "sel") == 0) {
    benchPathCat(path, sizeof(path), ws, "/bade/src/k1.c");
    snprintf(text, sizeof(text), "\nint bench_bade_%d_%s(void) { return %d; }\n", run, safe, run);
    benchAppendFile(path, text);
  }
  if (strcmp(scen, "mod") == 0 || strcmp(scen, "dep") == 0) {
    benchPathCat(path, sizeof(path), ws, "/rupamod/m1.rp");
    snprintf(text, sizeof(text), "\n# edit %s-%d\n", label, run);
    benchAppendFile(path, text);
  }
  if (strcmp(scen, "touch") == 0) {
    benchPathCat(path, sizeof(path), ws, "/include/a.h");
    benchTouch(path);
  }
  if (strcmp(scen, "libtouch") == 0) {
    benchPathCat(path, sizeof(path), ws, "/include/ext.h");
    benchTouch(path);
  }
  if (strcmp(scen, "hedit") == 0) {
    benchPathCat(path, sizeof(path), ws, "/include/a.h");
    benchBumpDefine(path, "WS_SHARED_VALUE");
  }
  if (strcmp(scen, "libhdr") == 0) {
    benchPathCat(path, sizeof(path), ws, "/include/ext.h");
    benchBumpDefine(path, "WS_EXT_VALUE");
  }
}

/* ---------- salinan pohon (prime snapshot) ---------- */

static bool benchCopyDir(const char *src, const char *dst) {
  mkdirs(dst);
  List dirs = {0}, files = {0};
  fsListDir(src, &dirs, &files);
  bool ok = true;
  for (int i = 0; i < files.count && ok; i++) {
    FILE *in = fopen(files.items[i], "rb");
    if (!in) continue;
    const char *rel = files.items[i] + strlen(src);
    if (*rel == '/') rel++;
    char out[BENCH_PATH_LEN];
    snprintf(out, sizeof(out), "%s/%s", dst, rel);
    mkparent(out);
    FILE *fo = fopen(out, "wb");
    if (!fo) {
      fclose(in);
      ok = false;
      break;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, fo);
    fclose(in);
    fclose(fo);
    fsSetMode(out, fsGetMode(files.items[i]));
    /* Pertahankan mtime seperti `cp -a`: archive/state satria membandingkan
       mtime input, jadi salinan prime tanpa mtime asli memicu rebuild palsu. */
    struct stat st;
    if (stat(files.items[i], &st) == 0) {
      struct timespec ts[2];
      ts[0] = st.st_atim;
      ts[1] = st.st_mtim;
      utimensat(AT_FDCWD, out, ts, 0);
    }
  }
  for (int i = 0; i < dirs.count && ok; i++) {
    const char *rel = dirs.items[i] + strlen(src);
    if (*rel == '/') rel++;
    char out[BENCH_PATH_LEN];
    snprintf(out, sizeof(out), "%s/%s", dst, rel);
    ok = benchCopyDir(dirs.items[i], out);
    struct stat st;
    if (ok && stat(dirs.items[i], &st) == 0) {
      struct timespec ts[2];
      ts[0] = st.st_atim;
      ts[1] = st.st_mtim;
      utimensat(AT_FDCWD, out, ts, 0);
    }
  }
  listFree(&dirs);
  listFree(&files);
  return ok;
}

bool benchCopyTree(const char *src, const char *dst) {
  fsRemoveTree(dst);
  if (!benchCopyDir(src, dst)) return false;
  fsSetMode(dst, fsGetMode(src));
  return true;
}

/* ---------- snapshot artefak ---------- */

static bool benchIsArtifact(const char *path, int *cat) {
  size_t n = strlen(path);
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  if (n > 2 && strcmp(path + n - 2, ".o") == 0) {
    *cat = 0;
    return true;
  }
  if (strcmp(base, "rupa_modules.tar") == 0) {
    *cat = 1;
    return true;
  }
  if (strncmp(base, "lib", 3) == 0 && (n > 2 && (strstr(base, ".a") || strstr(base, ".so")))) {
    *cat = 2;
    return true;
  }
  if (strcmp(base, "rupa") == 0 || strcmp(base, "bade") == 0 || strcmp(base, "app") == 0) {
    *cat = 3;
    return true;
  }
  return false;
}

static void benchSnapAdd(BenchSnap *s, const char *root, const char *path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return;
  const char *rel = path + strlen(root);
  if (*rel == '/') rel++;
  if (s->n == s->cap) {
    int nc = s->cap ? s->cap * 2 : 128;
    BenchSnapEnt *nv = realloc(s->v, (size_t)nc * sizeof(*nv));
    if (!nv) return;
    s->v = nv;
    s->cap = nc;
  }
  BenchSnapEnt *e = &s->v[s->n++];
  copyStr(e->path, sizeof(e->path), rel);
  e->size = (long long)st.st_size;
  e->mtimeNs = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
  e->ctimeNs = (int64_t)st.st_ctim.tv_sec * 1000000000LL + st.st_ctim.tv_nsec;
}

static void benchSnapWalk(const char *root, const char *dir, BenchSnap *s, int depth) {
  if (depth > 24) return;
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  for (int i = 0; i < files.count; i++) {
    int cat;
    if (benchIsArtifact(files.items[i], &cat)) benchSnapAdd(s, root, files.items[i]);
  }
  for (int i = 0; i < dirs.count; i++) benchSnapWalk(root, dirs.items[i], s, depth + 1);
  listFree(&dirs);
  listFree(&files);
}

bool benchSnapTake(const char *root, BenchSnap *out) {
  memset(out, 0, sizeof(*out));
  benchSnapWalk(root, root, out, 0);
  return true;
}

void benchSnapFree(BenchSnap *s) {
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}

static int benchSnapCmp(const void *a, const void *b) {
  return strcmp(((const BenchSnapEnt *)a)->path, ((const BenchSnapEnt *)b)->path);
}

void benchDelta(const BenchSnap *before, const BenchSnap *after, BenchDelta *d) {
  memset(d, 0, sizeof(*d));
  BenchSnap a = *after;
  qsort(a.v, (size_t)a.n, sizeof(*a.v), benchSnapCmp);
  BenchSnap b = *before;
  qsort(b.v, (size_t)b.n, sizeof(*b.v), benchSnapCmp);
  for (int i = 0; i < a.n; i++) {
    const BenchSnapEnt *e = &a.v[i];
    const BenchSnapEnt *old = NULL;
    for (int j = 0; j < b.n; j++) {
      int c = strcmp(b.v[j].path, e->path);
      if (c == 0) {
        old = &b.v[j];
        break;
      }
      if (c > 0) break;
    }
    bool changed = !old || old->size != e->size || old->mtimeNs != e->mtimeNs ||
                   old->ctimeNs != e->ctimeNs;
    if (!changed) continue;
    int cat = 0;
    if (benchIsArtifact(e->path, &cat)) {
      if (cat == 0) d->obj++;
      else if (cat == 1) d->arc++;
      else if (cat == 2) d->lib++;
      else d->bin++;
    }
  }
}

#endif /* !_WIN32 */

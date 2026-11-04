# bench/compare/gen.sh  generator project sintetis dan graph ninja.
# Dimuat oleh bench/compare.sh (cmp_source). Bukan skrip mandiri.

# ---------------------------- generator project ----------------------------
# Rebuild Ninja graph from the real source tree. Cold measures this work.
# No precomputed source/object list is reused between cold runs.
gen_ninja_graph() {
  local src obj
  local -a sources=() objects=()
  # Discover actual files instead of using NSRC as a precomputed manifest.
  mapfile -d '' -t sources < <(cd "$PROJ" && find src -type f -name '*.c' -print0 | sort -z)
  [ "${#sources[@]}" -gt 0 ] || return 1
  {
    echo 'cflags = -O2 -std=gnu11 -Iinclude'
    echo
    echo 'rule cc'
    echo '  command = gcc $cflags -MMD -MP -MF $out.d -c $in -o $out'
    echo '  depfile = $out.d'
    echo '  deps = gcc'
    echo 'rule link'
    echo '  command = gcc $in -o $out'
    echo
    for src in "${sources[@]}"; do
      obj="build/${src#src/}"
      obj="${obj%.c}.o"
      objects+=("$obj")
      printf 'build %s: cc %s\n' "$obj" "$src"
    done
    printf 'build bin/app: link'
    printf ' %s' "${objects[@]}"
    printf '\n\ndefault bin/app\n'
  } >"$PROJ/build.ninja"
}

gen_project() {
  rm -rf "$PROJ"
  mkdir -p "$PROJ/src" "$PROJ/include" "$PROJ/bin" "$PROJ/build"

  # Header berantai h1 -> h2 -> ... -> hH
  local h
  for ((h = 1; h <= NHEAD; h++)); do
    {
      echo "#pragma once"
      [ "$h" -gt 1 ] && echo "#include \"h$((h - 1)).h\""
      echo "static const int v$h = $h;"
    } >"$PROJ/include/h$h.h"
  done

  # Source files
  local i
  for ((i = 1; i <= NSRC; i++)); do
    {
      echo "#include \"h$((i % NHEAD + 1)).h\""
      echo "int t$i(void) { return v$((i % NHEAD + 1)) + $i; }"
    } >"$PROJ/src/t$i.c"
  done

  # main.c
  {
    echo "#include \"h1.h\""
    echo "int t1(void);"
    echo "int main(void) { return t1() - 1; }"
  } >"$PROJ/src/main.c"
  cp "$PROJ/src/t1.c" "$PROJ/src/t1.c.baseline"

  gen_ninja_graph

  # ---- Makefile ----
  {
    echo "CC      := gcc"
    echo "CFLAGS  := -O2 -std=gnu11 -Iinclude"
    echo -n "OBJS    := build/main.o "
    for ((i = 1; i <= NSRC; i++)); do printf 'build/t%d.o ' "$i"; done
    echo
    echo
    echo "bin/app: \$(OBJS)"
    echo -e "\t\$(CC) \$^ -o \$@"
    echo
    echo "build/%.o: src/%.c"
    echo -e "\t@mkdir -p build"
    echo -e "\t\$(CC) \$(CFLAGS) -MMD -MP -c \$< -o \$@"
    echo
    echo "-include \$(OBJS:.o=.d)"
  } >"$PROJ/Makefile"

  # ---- Buildfile (satria) ----
  {
    echo "use alias"
    echo
    echo "clean as c"
    echo "output as o"
    echo
    echo "root = ."
    echo
    echo "sources = src"
    echo "headers = include"
    echo "flags = O2, MMD, MP"
    echo "std = gnu11"
    echo "compiler = gcc"
    echo "c.compdb = false"
    echo "foreground = false"
    echo
    echo "o.binaryName = app"
    echo "o.binaryDir = bin"
    echo "o.buildDir = build"
    echo "o.compileCommands = false"
  } >"$PROJ/Buildfile"
  ( cd "$PROJ" && "$SATRIA" init >/dev/null 2>&1 )

  # ---- CMakeLists.txt (untuk CMake + Ninja) ----
  {
    echo "cmake_minimum_required(VERSION 3.12)"
    echo "project(app C)"
    echo
    echo "set(CMAKE_C_STANDARD 11)"
    echo "set(CMAKE_C_EXTENSIONS ON)"
    echo "set(CMAKE_C_STANDARD_REQUIRED ON)"
    echo "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)"
    echo "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \${CMAKE_BINARY_DIR}/bin)"
    echo
    echo "file(GLOB SOURCES CONFIGURE_DEPENDS \"\${CMAKE_CURRENT_SOURCE_DIR}/src/*.c\")"
    echo "list(SORT SOURCES)"
    echo "add_executable(app \${SOURCES})"
    echo "target_include_directories(app PRIVATE include)"
    echo "target_compile_options(app PRIVATE -O2 -MMD -MP)"
  } >"$PROJ/CMakeLists.txt"

  # ---- meson.build (untuk Meson + Ninja) ----
  {
    echo "project('app', 'c',"
    echo "        version : '1.0',"
    echo "        default_options : ['c_std=gnu11', 'warning_level=0', 'optimization=2'])"
    echo
    echo "inc = include_directories('include')"
    echo "src = files("
    echo -n "  'src/main.c'"
    for ((i = 1; i <= NSRC; i++)); do
      echo ","
      echo -n "  'src/t$i.c'"
    done
    echo
    echo ")"
    echo
    echo "executable('app', src,"
    echo "           include_directories : inc,"
    echo "           install : false)"
  } >"$PROJ/meson.build"

  # ---- xmake.lua (untuk XMake) ----
  {
    echo "set_project(\"app\")"
    echo "set_version(\"1.0.0\")"
    echo
    echo "set_languages(\"gnu11\")"
    echo "set_warnings(\"none\")"
    echo "set_optimize(\"fast\")"
    echo
    echo "add_includedirs(\"include\")"
    echo
    echo "target(\"app\")"
    echo "    set_kind(\"binary\")"
    echo "    add_files(\"src/*.c\")"
  } >"$PROJ/xmake.lua"

  # ---- Tupfile (untuk Tup) ----
  {
    echo "CFLAGS = -O2 -std=gnu11 -Iinclude -MMD -MP -MF \$(@).d"
    echo
    echo "foreach src in src/*.c"
    echo "  obj = \$(src:t=o)"
    echo "  : foreach src |> gcc \$(CFLAGS) -c %f -o %o |> build/\$(obj)"
    echo "end"
    echo
    echo -n ": foreach build/*.o |> gcc %f -o %o |> bin/app"
    echo
  } >"$PROJ/Tupfile"
  # Tup butuh inisialisasi database
  if [ "$HAVE_TUP" = 1 ]; then
    ( cd "$PROJ" && tup init >/dev/null 2>&1 || true )
  fi
}

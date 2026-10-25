# Generator workspace (source C, Buildfile.ws, CMakeLists.txt).
# ---------------------------- generator workspace --------------------------
gen_c() { # gen_c <proyek> <prefix>
  local proj=$1 pre=$2 h i
  mkdir -p "$WS/$proj/src"
  for ((h = 1; h <= NHEAD; h++)); do
    {
      echo "#pragma once"
      [ "$h" -gt 1 ] && echo "#include \"h$((h / 2)).h\""
      echo "static const int v$h = $h;"
    } > "$WS/$proj/src/h$h.h"
  done
  for ((i = 1; i <= NSRC; i++)); do
    {
      echo "#include \"h$((i % NHEAD + 1)).h\""
      echo "int ${pre}$i(void) { return v$((i % NHEAD + 1)) + $i; }"
    } > "$WS/$proj/src/${pre}$i.c"
  done
  {
    echo "#include \"h1.h\""
    echo "int ${pre}1(void);"
    echo "int main(void) { return ${pre}1() - 1; }"
  } > "$WS/$proj/src/main.c"
}

# Mode embed: Satria kini HANYA memakai codegen (embedded.c dikompilasi sebagai
# source biasa — jalur `ld -r -b binary` dihapus, design/embedded.md). Agar
# pekerjaan kedua tool benar-benar setara, CMake di bench juga memakai array C
# lewat embed_c.sh (codegen + 1 compile), BUKAN ld -r -b binary yang tidak lagi
# mewakili pekerjaan satria. BENCH_EMBED=ld memaksa mode ld lama untuk
# eksperimen (akan menandai kerja berbeda antar tool).
embed_mode() {
  [ -n "${BENCH_EMBED:-}" ] && {
    printf '%s' "$BENCH_EMBED"
    return
  }
  printf carray
}
EMBED_MODE=$(embed_mode)
# Kompilasi tambahan per embed (dihitung shim gcc) bila memakai array C.
embed_extra() { [ "$EMBED_MODE" = carray ] && echo 1 || echo 0; }

gen_ws() {
  # Tool workspace sudah dipilih lewat select_ws; hanya buat konfigurasi miliknya.
  local ws_tool=${WS##*/}
  case "$ws_tool" in satria | cmake) ;; *)
    echo "gen_ws: unknown tool workspace: $WS" >&2
    return 1
    ;;
  esac
  rm -rf "$WS"
  mkdir -p "$WS/rupamod"
  local i
  gen_c core r
  gen_c bade k
  # Header publik milik workspace: kedua proyek memakai pusat library/.
  mkdir -p "$WS/include" "$WS/modules"
  printf '#pragma once\n#define WS_SHARED_VALUE 7\n' > "$WS/include/a.h"
  printf '#pragma once\n#include "a.h"\n#define WS_BADE_VALUE WS_SHARED_VALUE\n' > "$WS/include/b.h"
  # Header bersama: library/ dipantau lewat headers workspace;
  # ext.h tetap di-include secara relatif tanpa symlink.
  printf '#pragma once\n#define WS_EXT_VALUE 3\n' > "$WS/include/ext.h"
  for ((i = 1; i <= NMOD; i++)); do
    printf 'module m%s\nexport f%s\n' "$i" "$i" > "$WS/rupamod/m$i.rp"
  done

  # Simbol benar-benar circular: core_entry -> bade_entry -> core_helper.
  cat > "$WS/core/src/r1.c" << 'EOF'
#include "a.h"
int bade_entry(void);
int core_entry(void) { return bade_entry(); }
EOF
  cat > "$WS/core/src/r2.c" << 'EOF'
#include "a.h"
#include "ext.h"
int ws_ext_value = WS_EXT_VALUE;
int core_helper(void) { return WS_SHARED_VALUE + ws_ext_value - WS_EXT_VALUE; }
EOF
  cat > "$WS/bade/src/k1.c" << 'EOF'
#include "b.h"
int core_helper(void);
int bade_entry(void) { return core_helper() + (WS_BADE_VALUE - WS_SHARED_VALUE); }
EOF
  # Jalur balik independen: bade -> core -> bade.
  cat > "$WS/bade/src/k2.c" << 'EOF'
#include "b.h"
#include "ext.h"
int core_entry(void);
int bade_ext_value = WS_EXT_VALUE;
int bade_reverse(void) { return core_entry() + (WS_BADE_VALUE - WS_SHARED_VALUE) + bade_ext_value - WS_EXT_VALUE; }
EOF
  # Runtime invariant: nilai dari header pusat (library/a.h) mengalir ke
  #kedua main, sehingga object basi setelah edit header pusat membuat
  #binary gagal (exit != 0) dan terdeteksi sebagai kegagalan build.
  cat > "$WS/core/src/main.c" << 'EOF'
#include "a.h"
int core_entry(void);
int main(void) { return core_entry() == WS_SHARED_VALUE ? 0 : 1; }
EOF
  cat > "$WS/bade/src/main.c" << 'EOF'
#include "b.h"
int bade_reverse(void);
int main(void) { return bade_reverse() == WS_BADE_VALUE ? 0 : 1; }
EOF

  # -n 1 tetap memiliki dua translation unit untuk menguji siklus simbol.
  # Source tambahan khusus ini tidak mengubah arti -n (jumlah source beban).
  # ---- Buildfile.ws (Sesuai desain terbaru) ----
  if [ "$ws_tool" = satria ]; then
    {
      extract_tmp="${TMPDIR:-${PREFIX:+$PREFIX/tmp}}"
      extract_tmp="${extract_tmp:-/tmp}"
      echo "use workspace"
      echo "projects.rupamod as mod"
      echo "projects.core as core"
      echo "projects = bade, core, rupamod"
      echo "compiler = $COMPILERS"
      echo "flags = Wall, Wextra, MMD, MP, fPIC"
      echo "headers.each = auto"
      echo "std = gnu11"
      echo "mod.output.binary = false"
      echo "mod.archive.src = ."
      echo "mod.archive.name = rupa_modules"
      echo "mod.archive.with = tar"
      echo "mod.archive.dir = modules"
      echo "mod.archive.exclude = build, dist, LICENSE, README.md, self_archive"
      echo "core.sources = src"
      echo "core.exclude = main.c"
      echo "core.output.binaryName = rupa"
      echo "core.output.libraryName = core"
      echo "core.output.libraryShared = true"
      echo "bade.sources = manager, src"
      echo "bade.exclude = manager/main.c"
      echo "bade.output.binaryName = bade"
      echo "bade.output.libraryName = bade"
      echo "bade.output.libraryShared = true"
      echo "bade.depends_on = rupamod"
      echo "bade.embedded.modules.file = modules/rupa_modules.tar"
      echo "bade.embedded.modules.variable = MODULES"
      echo "bade.embedded.modules.extract = ${extract_tmp}/rupa-system"
      echo "bade.embedded.modules.pattern = .rp"
      echo "bade.library.cdeps = core"
      echo "core.library.cdeps = bade"
    } > "$WS/Buildfile.ws"
  fi

  # CMake idiomatis, mengikuti tests/miniws/CMakeLists.txt:
  # CMake sendiri melakukan discovery, compile, archive dan link.
  if [ "$ws_tool" = cmake ]; then
    cat > "$WS/CMakeLists.txt" << 'CMAKE_EOF'
cmake_minimum_required(VERSION 3.20)
project(wsbench C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-Wall -Wextra -MMD -MP -fPIC)
include_directories("${CMAKE_SOURCE_DIR}/include")
file(GLOB_RECURSE CORE_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/core/src/*.c")
file(GLOB_RECURSE BADE_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/bade/src/*.c")
list(REMOVE_ITEM CORE_SRC "${CMAKE_SOURCE_DIR}/core/src/main.c")
list(REMOVE_ITEM BADE_SRC "${CMAKE_SOURCE_DIR}/bade/src/main.c")
file(GLOB_RECURSE MOD_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/rupamod/*.rp")
set(ARCHIVE "${CMAKE_SOURCE_DIR}/modules/rupa_modules.tar")
add_custom_command(OUTPUT "${ARCHIVE}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/modules"
  COMMAND tar -cf "${ARCHIVE}" -C "${CMAKE_SOURCE_DIR}/rupamod"
    --exclude=build --exclude=dist --exclude=LICENSE
    --exclude=README.md --exclude=self_archive .
  DEPENDS ${MOD_SRC} VERBATIM)
set(EMBED "${CMAKE_CURRENT_BINARY_DIR}/modules/rupa_modules.o")
if("@EMBED_MODE@" STREQUAL "carray")
  add_custom_command(OUTPUT "${EMBED}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/modules"
    COMMAND sh "@WS_LIB@/embed_c.sh" "${ARCHIVE}" "${EMBED}.c" _binary_modules_rupa_modules_tar
    COMMAND gcc -c -fPIC -o "${EMBED}" "${EMBED}.c"
    DEPENDS "${ARCHIVE}" VERBATIM)
else()
  add_custom_command(OUTPUT "${EMBED}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/modules"
    COMMAND ld -r -b binary -o "${EMBED}" modules/rupa_modules.tar
    DEPENDS "${ARCHIVE}" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" VERBATIM)
endif()
set_source_files_properties("${EMBED}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(core_obj OBJECT ${CORE_SRC})
add_library(bade_obj OBJECT ${BADE_SRC})
set_target_properties(core_obj bade_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(core_static STATIC $<TARGET_OBJECTS:core_obj>)
add_library(bade_static STATIC $<TARGET_OBJECTS:bade_obj> "${EMBED}")
add_library(core_shared SHARED $<TARGET_OBJECTS:core_obj>)
add_library(bade_shared SHARED $<TARGET_OBJECTS:bade_obj> "${EMBED}")
set_target_properties(core_static PROPERTIES OUTPUT_NAME core
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/core/build")
set_target_properties(bade_static PROPERTIES OUTPUT_NAME bade
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bade/build")
set_target_properties(core_shared PROPERTIES OUTPUT_NAME core
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/core/lib")
set_target_properties(bade_shared PROPERTIES OUTPUT_NAME bade
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bade/lib")
# Simbol sirkular nyata: kedua executable harus menggunakan archive group.
add_executable(rupa core/src/main.c)
add_executable(bade bade/src/main.c)
set_target_properties(rupa PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/core/bin")
set_target_properties(bade PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bade/bin")
foreach(t IN ITEMS rupa bade)
  target_link_options(${t} PRIVATE "LINKER:--undefined=_binary_modules_rupa_modules_tar_start")
  target_link_libraries(${t} PRIVATE -Wl,--start-group core_static bade_static -Wl,--end-group)
endforeach()
CMAKE_EOF
    sed -i "s|@EMBED_MODE@|$EMBED_MODE|g; s|@WS_LIB@|$WS_LIB|g" "$WS/CMakeLists.txt"
  fi

}

# Perintah per tool (satria/cmake) dan mode selektif/release.
# ---------------------------- perintah per tool ----------------------------
tool_cmd() {
  case $1 in
    satria) "$SATRIA" $SATRIA_ARGS -j"$NPROC" -w ;;
    cmake) cmake --build .cmake-build --parallel "$NPROC" ;;
  esac
}
# Build selektif: cakupan output DISAMAKAN dengan satria -w <proyek>
# (problems/ws.md butir 5 — "pekerjaan wajib setara").
#   -w core -> object/library core saja, tanpa relink binary
#            -> CMake: --target core_static core_shared
#   -w bade -> arsip + codegen embed + library bade + binary bade
#            -> CMake: --target bade_static bade_shared bade
# CMake memakai NAMA TARGET, bukan path keluaran. Pemetaan ke CMakeLists
# hasil gen.sh: rupa -> core/bin/rupa, bade -> bade/bin/bade,
# core_static -> core/build/libcore.a, core_shared -> core/lib/libcore.so,
# bade_shared -> bade/lib/libbade.so.
# --target diulang satu per nama: cmake 3.31 + generator Ninja MENOLAK daftar
# berpemisah koma/spasi ("unknown target 'rupa,bade,...'"), jadi list dipisah
# spasi lalu dipecah jadi flag terpisah. Tanpa --target, cmake --build memakai
# default target "all" sehingga cakupan selektif tidak pernah dinyatakan —
# beda dengan pembanding CMake
# (audit workspace-cmake,satria-n1000-r5).
CMAKE_SEL_TARGETS="core_static core_shared"
CMAKE_DEP_TARGETS="bade_static bade_shared bade"
cmake_targets_cmd() { # cmake_targets_cmd <daftar nama target> -> build target itu saja
  local -a names=() targs=()
  read -ra names <<< "$1"
  local n
  for n in "${names[@]}"; do targs+=(--target "$n"); done
  cmake --build .cmake-build --parallel "$NPROC" "${targs[@]}"
}
sel_cmd() { # build selektif cakupan core
  case $1 in
    satria) "$SATRIA" $SATRIA_ARGS -j"$NPROC" -w core ;;
    cmake) cmake_targets_cmd "$CMAKE_SEL_TARGETS" ;;
  esac
}
dep_cmd() { # build selektif cakupan bade
  case $1 in
    satria) "$SATRIA" $SATRIA_ARGS -j"$NPROC" -w bade ;;
    cmake) cmake_targets_cmd "$CMAKE_DEP_TARGETS" ;;
  esac
}
rel_cmd() { # release terpisah: tar + checksum (docs/guide/releases.md)
  # Validasi packaging ikut menentukan rc run: artefak + checksum harus nyata
  # dan cocok (sha256sum -c), bukan sekadar exit 0 dari release.
  local vf=$1 tarf
  "$SATRIA" $SATRIA_ARGS -j"$NPROC" -w release -- "format=tar,version=$vf,checksum=sha256" || return 1
  tarf="$WS/dist/satria-v$vf.tar.gz"
  [ -s "$tarf" ] && [ -s "$tarf.sha256" ] || {
    echo "satria/rel: artefak atau checksum hilang: $tarf" >&2
    return 1
  }
  (cd "$WS/dist" && sha256sum -c "satria-v$vf.tar.gz.sha256") > /dev/null 2>&1 || {
    echo "satria/rel: checksum tidak cocok: $tarf.sha256" >&2
    return 1
  }
}
build_full() {
  case $1 in
    satria) "$SATRIA" $SATRIA_ARGS -j"$NPROC" -w ;;
    cmake) cmake -S . -B .cmake-build -G Ninja -DCMAKE_C_COMPILER="$WRAPDIR/gcc" >/dev/null && tool_cmd cmake ;;
  esac
}


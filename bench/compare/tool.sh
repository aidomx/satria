# bench/compare/tool.sh  perintah build per tool.
# Dimuat oleh bench/compare.sh (cmp_source). Bukan skrip mandiri.

# ---------------------------- perintah per tool ----------------------------
# ARGV diisi dengan argv build inkremental "biasa" tiap tool (dipakai juga
# oleh strace, yang tidak bisa mengeksekusi fungsi bash).
ARGV=()
tool_argv() {
  case $1 in
    ninja) ARGV=(ninja -j"$NPROC") ;;
    make)  ARGV=(make -j"$NPROC") ;;
    bear)  ARGV=(bear -- make -j"$NPROC") ;;
    satria)  ARGV=("$SATRIA" -j"$NPROC") ;;
    cmake) ARGV=(ninja -C build-cmake -j"$NPROC") ;;
    meson) ARGV=(ninja -C build-meson -j"$NPROC") ;;
    xmake) ARGV=(xmake -y -j"$NPROC") ;;
    tup)   ARGV=(tup) ;;
  esac
}
tool_cmd() { tool_argv "$1"; "${ARGV[@]}"; }

# Cold includes Ninja graph generation and CMake/Meson configure (also used for prime).
build_full() {
  case $1 in
    satria)  "$SATRIA" -j"$NPROC" ;;
    ninja)  gen_ninja_graph && ninja -j"$NPROC" ;;
    cmake) CC=gcc cmake -G Ninja -B build-cmake -DCMAKE_BUILD_TYPE= -DCMAKE_C_FLAGS="-O2 -std=gnu11" && ninja -C build-cmake -j"$NPROC" ;;
    meson) CC=gcc meson setup build-meson --buildtype=plain -Dc_args="-O2 -std=gnu11" && ninja -C build-meson -j"$NPROC" ;;
    xmake) xmake -y -j"$NPROC" ;;
    *)     tool_cmd "$1" ;;
  esac
}

# Harapan (except) per skenario + state verdict. Dimuat dari ws.sh.
EXPECT_OBJ=$((2 * (NSRC + 1) + 1 + (NSRC == 1 ? 2 : 0))) # core + bade + embedded object
EXPECT_CC=$((2 * (NSRC + 1) + 2 * (NSRC == 1 ? 1 : 0))) # kompilasi C pada cold
EXPECT_DEPS=6 # dependen library/a.h: r1,r2,main (core) + k1,k2,main (bade)

# Cache prime per tool: build penuh + gate hanya sekali per tool; skenario
# berikutnya memulihkan salinan hasil prime (cp -a) ke path WS yang sama.
declare -gA PRIME_OK=() PRIME_FAIL=() # -g: global walau dimuat lewat ws_source()
MISMATCH_RUNS=0 # run gagal (rc != 0) atau prime/gate gagal
NOT_ACC=0 # run (semua tool) yang kerjanya tidak sesuai harapan -> Not in accordance
EXPECT_MISS=""
exp_chk() { [ "$3" -lt 0 ] || [ "$2" -eq "$3" ] || EXPECT_MISS+=" $1=$2(harap $3)"; }
# Validasi berbasis kewajiban kerja, bukan memaksakan jumlah relink yang
# identik pada tool dengan strategi cache/link berbeda. EXP_*=-1 berarti
# metrik tetap dilaporkan tetapi bukan syarat kelulusan. Runtime check di
# measure.sh tetap wajib dan kompilasi sumber yang relevan tetap diperiksa.
# Khusus mod, Satria harus hanya memperbarui binary konsumen embed (bade).
expect_check() { # <tool> <scen> <obj> <arc> <lib> <bin> <cc>
  local k=$1 s=$2 o=$3 a=$4 l=$5 b=$6 c=$7
  EXPECT_MISS=""
  EXP_OBJ=-1 EXP_ARC=-1 EXP_LIB=-1 EXP_BIN=-1 EXP_CC=-1
  case $s in
    cold)
      # CMake melakukan compiler identification/probe saat configure cold.
      # Probe itu pekerjaan nyata, bukan kompilasi source workspace.
      local cmake_probe=0
      [ "$k" = cmake ] && cmake_probe=1
      EXP_OBJ=$EXPECT_OBJ EXP_ARC=1 EXP_LIB=4 EXP_BIN=2 EXP_CC=$((EXPECT_CC + $(embed_extra) + cmake_probe))
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    noop)
      EXP_OBJ=0 EXP_ARC=0 EXP_LIB=0 EXP_BIN=0 EXP_CC=0
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    leaf | bade)
      # Satu TU berubah. CMake+Ninja relink kedua binary karena siklus;
      # Satria hanya memperbarui library, tanpa relink binary pada skenario ini.
      EXP_OBJ=1 EXP_ARC=0 EXP_LIB=2 EXP_CC=1
      if [ "$k" = cmake ]; then EXP_BIN=2; else EXP_BIN=0; fi
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    mod)
      # Archive wajib diperbarui, tetapi hanya konsumen embed yang perlu
      # relink. Jumlah object embed berbeda menurut implementasi (ld -b
      # binary vs C array), maka tidak di-hard-check.
      EXP_OBJ=1 EXP_ARC=1 EXP_CC=$(embed_extra)
      if [ "$k" = satria ]; then EXP_LIB=0 EXP_BIN=1; else EXP_LIB=2 EXP_BIN=2; fi
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    touch)
      # Konten header sama: satria wajib 0 (content-hash); CMake (mtime)
      # wajib membangun ulang seluruh dependen library/a.h.
      if [ "$k" = satria ]; then
        EXP_OBJ=0 EXP_ARC=0 EXP_LIB=0 EXP_BIN=0 EXP_CC=0
        exp_chk obj "$o" "$EXP_OBJ"
        exp_chk arc "$a" "$EXP_ARC"
        exp_chk lib "$l" "$EXP_LIB"
        exp_chk bin "$b" "$EXP_BIN"
        exp_chk cc "$c" "$EXP_CC"
      else
        EXP_OBJ=$EXPECT_DEPS EXP_ARC=0 EXP_LIB=4 EXP_BIN=2 EXP_CC=$EXPECT_DEPS
        exp_chk obj "$o" "$EXP_OBJ"
        exp_chk arc "$a" "$EXP_ARC"
        exp_chk lib "$l" "$EXP_LIB"
        exp_chk bin "$b" "$EXP_BIN"
        exp_chk cc "$c" "$EXP_CC"
      fi
      ;;
    hedit)
      # Edit isi header pusat: kedua tool wajib mengompilasi ulang persis
      # dependen library/a.h (r1,r2,main core; k1,k2,main bade), memperbarui
      # keempat library dan kedua binary. Runtime check memastikan tidak ada
      # object basi yang lolos: nilai WS_SHARED_VALUE mengalir ke kedua main.
      EXP_OBJ=$EXPECT_DEPS EXP_ARC=0 EXP_LIB=4 EXP_BIN=2 EXP_CC=$EXPECT_DEPS
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    sel)
      # Perilaku berbeda secara sengaja: satria -w core membangun object/library core tanpa relink binary,
      # sedangkan CMake meminta target dari kedua proyek untuk menangani siklus.
      # Karena cakupan kerja berbeda, waktu tidak boleh dibandingkan sebagai 1:1.
      EXP_ARC=0
      if [ "$k" = satria ]; then
        EXP_OBJ=1 EXP_LIB=2 EXP_BIN=0 EXP_CC=1
      else
        EXP_OBJ=2 EXP_LIB=4 EXP_BIN=2 EXP_CC=2
      fi
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    dep)
      # .rp + core source, target satria hanya bade: arsip diperbarui dan
      # binary konsumen embed ditaut ulang; library tidak berubah; perubahan core di luar target tidak dikompilasi.
      # CMake tetap membangun target kedua proyek, jadi cc-nya tetap 1+embed.
      EXP_ARC=1
      if [ "$k" = satria ]; then
        EXP_OBJ=1 EXP_LIB=0 EXP_BIN=1 EXP_CC=0
      else
        EXP_OBJ=2 EXP_LIB=4 EXP_BIN=2 EXP_CC=$((1 + $(embed_extra)))
      fi
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    libtouch)
      # Header internal (library/ext.h) dengan konten sama: satria wajib 0
      # (content-hash). CMake (mtime) membangun ulang 2 TU yang meng-include-nya.
      if [ "$k" = satria ]; then
        EXP_OBJ=0 EXP_ARC=0 EXP_LIB=0 EXP_BIN=0 EXP_CC=0
        exp_chk obj "$o" "$EXP_OBJ"
        exp_chk arc "$a" "$EXP_ARC"
        exp_chk lib "$l" "$EXP_LIB"
        exp_chk bin "$b" "$EXP_BIN"
        exp_chk cc "$c" "$EXP_CC"
      else
        EXP_OBJ=2 EXP_CC=2
        exp_chk obj "$o" "$EXP_OBJ"
        exp_chk cc "$c" "$EXP_CC"
      fi
      ;;
    libhdr)
      # Edit isi header internal: r2.c + k2.c (yang meng-include ext.h) dikompilasi
      # ulang, 4 library, 2 binary — sama untuk semua tool.
      EXP_OBJ=2 EXP_ARC=0 EXP_LIB=4 EXP_BIN=2 EXP_CC=2
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    rel)
      # Release diukur terpisah: hanya packaging, tanpa compile ulang.
      EXP_OBJ=0 EXP_CC=0
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk cc "$c" "$EXP_CC"
      ;;
  esac
}


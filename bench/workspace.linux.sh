#!/usr/bin/env bash
# ============================================================================
# workspace.sh — benchmark mode workspace satria (WsBuildfile)
#
# Meniru tests/wstest: 3 proyek dalam satu workspace
#   rupamod : proyek kemasan (archive .rp -> dist/rupa_modules.tar.gz)
#   rupa    : binary C mandiri
#   ruka    : binary C, depends_on rupamod, meng-embed arsip rupamod
# WsBuildfile dibuat persis seperti tests/wstest (alias, archive,
# depends_on, embedded). Sebagai PEMBANDING dibuat juga build.ninja dan
# Makefile untuk graf yang sama (satu graf datar, ruka relink bila arsip
# berubah). Bukan klaim: pembanding tidak punya konsep workspace.
#
# Skenario:
#   cold  : build dari nol semua proyek
#   noop  : build tanpa perubahan
#   leaf  : edit 1 source rupa (proyek mandiri) — ruka TIDAK boleh ikut
#   ruka  : edit 1 source ruka
#   mod   : edit 1 file .rp di rupamod — arsip dibuat ulang lalu ruka ikut
#           (uji propagasi depends_on / embedded)
#   touch : touch 1 header ruka (konten sama) — satria: 0 recompile
#   sel   : edit rupa DAN ruka, lalu build selektif hanya rupa
#           (satria -w rupa; pembanding: ninja/make dengan target rupa/bin/rupa)
#           — ruka tidak boleh ikut dibangun (obj 1, bin 1)
#   dep   : edit .rp rupamod DAN source rupa, lalu build selektif ruka
#           (satria -w ruka membangun rupamod dulu via depends_on; rupa tidak
#           boleh ikut: arc 1, bin 1, obj 1 (.o embed))
#
# Perintah satria: build semua = "satria -w", selektif = "satria -w <nama>".
#
# Kolom hasil: obj = .o dikompilasi ulang, arc = arsip dibuat ulang,
#              bin = binary (rupa/ruka) ditulis ulang. Dipakai untuk memeriksa
#              apakah invalidasi antar-proyek tepat, bukan hanya cepat.
#
# Pemakaian:
#   ./bench/workspace.sh                  # default: 80 source/proyek, 5 run
#   ./bench/workspace.sh -n 30 -r 2       # cepat
#   ./bench/workspace.sh -s mod,leaf      # subset skenario
#   ./bench/workspace.sh -t satria,ninja    # subset tool (default: satria,ninja,make)
#   ./bench/workspace.sh -a "--foo"       # argumen tambahan untuk satria
#   ./bench/workspace.sh -c "gcc, clang"  # compiler di WsBuildfile
#   ./bench/workspace.sh -d 1.1           # jeda detik antara build & edit berikutnya
#                                         # (uji deteksi perubahan berbasis mtime detik)
#   NO_COLOR=1 ./bench/workspace.sh       # tanpa warna / progress bar
#
# Catatan: lokasi state internal satria tidak diketahui skrip ini. Bila baris
# cold satria menampilkan "⚠ obj<N", state lama ikut terpakai (cold tidak bersih).
# ============================================================================
set -u

[ -n "${EPOCHREALTIME:-}" ] || { echo "butuh bash >= 5 (EPOCHREALTIME)" >&2; exit 1; }

# ---------------------------- konfigurasi ----------------------------------
NSRC=80         # -n : jumlah source per proyek (rupa & ruka)
NHEAD=20        # -H : jumlah header berantai per proyek
NMOD=40         # -m : jumlah file .rp di rupamod/stdlib
NRUN=5          # -r : pengulangan per skenario (median)
SATRIA=bin/satria   # -b : binary satria
SATRIA_ARGS=""    # -a : argumen tambahan untuk satria
COMPILERS="gcc" # -c : nilai 'compiler =' di WsBuildfile
KEEP=0          # -k : jangan hapus workspace di akhir
ONLY=""         # -s : subset skenario
TOOLS_OPT="satria,ninja,make"  # -t : subset tool pembanding
OUTDIR=""       # -o : direktori kerja (default ws.tmp di cwd)
NPROC=$(nproc 2>/dev/null || echo 4)
MUT_DELAY=0     # -d : jeda (detik) sebelum tiap edit; 0 = langsung

usage() { sed -n '2,/^# ====.*$/p' "$0"; exit 0; }
while getopts "n:H:m:r:b:a:c:d:ko:s:t:h" opt; do
  case $opt in
    n) NSRC=$OPTARG ;;   H) NHEAD=$OPTARG ;;  m) NMOD=$OPTARG ;;
    r) NRUN=$OPTARG ;;   b) SATRIA=$OPTARG ;;   a) SATRIA_ARGS=$OPTARG ;;
    c) COMPILERS=$OPTARG ;;  k) KEEP=1 ;;     o) OUTDIR=$OPTARG ;;
    s) ONLY=$OPTARG ;;   t) TOOLS_OPT=$OPTARG ;;   d) MUT_DELAY=$OPTARG ;;   *) usage ;;
  esac
done

[ -x "$SATRIA" ] || { echo "satria tidak ditemukan/dieksekusi: $SATRIA" >&2; exit 1; }
SATRIA=$(cd "$(dirname "$SATRIA")" && pwd)/$(basename "$SATRIA")
IFS=, read -r -a TOOLS <<<"$TOOLS_OPT"
for t in "${TOOLS[@]}"; do
  case $t in
    satria|ninja|make) ;;
    *) echo "tool tidak dikenal: $t (pilihan: satria, ninja, make)" >&2; exit 1 ;;
  esac
done
for t in gcc tar $(printf '%s\n' "${TOOLS[@]}" | grep -vx satria); do
  command -v "$t" >/dev/null 2>&1 || { echo "tool wajib tidak ada: $t" >&2; exit 1; }
done

HAVE_STRACE=0
command -v strace >/dev/null 2>&1 && HAVE_STRACE=1
label_of() { echo "$1"; }

# ---------------------------- versi ----------------------------------------
first_ver() { grep -oE '[0-9]+(\.[0-9]+)+' | head -1; }
satria_id() {
    local d c h v_bin
    d=$(dirname "$SATRIA")

    # 1. PRIORITAS: Cek apakah ini persis di Git Tag (Release Resmi)
    c=$(git -C "$d" describe --tags --exact-match 2>/dev/null)
    if [ -n "$c" ]; then
        c="release ${c}"
    else
        # 2. FALLBACK 1: Build Development (ada commit setelah tag, atau dirty)
        c=$(git -C "$d" describe --tags --always --dirty 2>/dev/null)
        if [ -n "$c" ]; then
            c="dev git:${c}"
        else
            # 3. FALLBACK 2: Binary sudah di-install (tidak ada folder .git)
            # Coba ambil versi dari output binary itu sendiri, atau dari strings
            v_bin=$($SATRIA version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
            if [ -z "$v_bin" ]; then
                v_bin=$(strings "$SATRIA" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
            fi
            c="release ${v_bin:-unknown}"
        fi
    fi

    # Hash unik untuk file binary (tetap berguna untuk melacak build yang sama)
    h=$(sha1sum "$SATRIA" 2>/dev/null | cut -c1-6)

    echo "${c} bin:${h:-n/a}"
}
declare -A VER
VER_GCC=$(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion 2>/dev/null)
command -v ninja >/dev/null 2>&1 && VER[ninja]=$(ninja --version 2>&1 | first_ver)
command -v make >/dev/null 2>&1 && VER[make]=$(make --version 2>&1 | first_ver)
VER[satria]=$(satria_id)

# ---------------------------- direktori kerja ------------------------------
[ -n "$OUTDIR" ] || OUTDIR="$PWD/ws.tmp"
mkdir -p "$OUTDIR"; OUTDIR=$(cd "$OUTDIR" && pwd)
WS="$OUTDIR/wstest"
CSV="$OUTDIR/results.csv"
SYSCALL_CSV="$OUTDIR/workspace-syscalls.csv"
MARK="$OUTDIR/.mark"
ERRLOG="$OUTDIR/stderr.log"
rm -f "$CSV" "$ERRLOG"
echo 'label,scenario,ms,rc,obj,arc,bin,version' >"$CSV"
echo 'tool,scenario,run,rank,percent,seconds,usecs_per_call,calls,errors,syscall' >"$SYSCALL_CSV"
EXPECT_OBJ=$(( 2 * (NSRC + 1) ))

# ---------------------------- tampilan -------------------------------------
TTY=0; [ -t 1 ] && TTY=1
if [ "$TTY" = 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_RST=$'\033[0m'; C_BLD=$'\033[1m'; C_DIM=$'\033[2m'
  C_GRN=$'\033[32m'; C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_CYN=$'\033[36m'
else
  C_RST=""; C_BLD=""; C_DIM=""; C_GRN=""; C_RED=""; C_YEL=""; C_CYN=""
fi
COLS=$(tput cols 2>/dev/null || echo 80)
[ "$COLS" -gt 0 ] 2>/dev/null || COLS=80
if (( COLS >= 90 )); then WIDE=1; BW=$(( COLS - 70 )); else WIDE=0; BW=$(( COLS - 38 )); fi
(( BW > 24 )) && BW=24
(( BW < 6 ))  && BW=6
PBW=$(( COLS - 30 )); (( PBW > 30 )) && PBW=30; (( PBW < 8 )) && PBW=8

STEP=0; TOTAL=1; CUR=""; T_START=$SECONDS
fmt_t() { printf '%d:%02d' $(( $1 / 60 )) $(( $1 % 60 )); }
bar() {
  local d=$1 t=$2 w=$3 f i s=""
  (( t > 0 )) || t=1
  f=$(( d * w / t ))
  for ((i = 0; i < w; i++)); do if (( i < f )); then s+="█"; else s+="░"; fi; done
  printf '%s' "$s"
}
clear_line() { [ "$TTY" = 1 ] && printf '\r\033[K'; return 0; }
draw_progress() {
  [ "$TTY" = 1 ] || return 0
  local el=$(( SECONDS - T_START )) eta=0 pct
  pct=$(( STEP * 100 / TOTAL ))
  (( STEP > 0 )) && eta=$(( el * (TOTAL - STEP) / STEP ))
  printf '\r\033[K  %s%s%s %3d%% %s%d/%d ETA %s%s' \
    "$C_CYN" "$(bar "$STEP" "$TOTAL" "$PBW")" "$C_RST" "$pct" \
    "$C_DIM" "$STEP" "$TOTAL" "$(fmt_t "$eta")" "$C_RST"
  if (( COLS >= 90 )); then printf '  %s▸ %s%s' "$C_YEL" "$CUR" "$C_RST"; fi
}

# ---------------------------- generator workspace --------------------------
gen_c() {  # gen_c <proyek> <prefix> : source + header berantai di <proyek>/src
  local proj=$1 pre=$2 h i
  mkdir -p "$WS/$proj/src"
  for ((h = 1; h <= NHEAD; h++)); do
    {
      echo "#pragma once"
      if [ "$h" -gt 1 ]; then echo "#include \"h$((h - 1)).h\""; fi
      echo "static const int v$h = $h;"
    } >"$WS/$proj/src/h$h.h"
  done
  for ((i = 1; i <= NSRC; i++)); do
    {
      echo "#include \"h$((i % NHEAD + 1)).h\""
      echo "int ${pre}$i(void) { return v$((i % NHEAD + 1)) + $i; }"
    } >"$WS/$proj/src/${pre}$i.c"
  done
  {
    echo "#include \"h1.h\""
    echo "int ${pre}1(void);"
    echo "int main(void) { return ${pre}1() - 1; }"
  } >"$WS/$proj/src/main.c"
}

gen_ws() {
  rm -rf "$WS"; mkdir -p "$WS/rupamod/stdlib"
  local i
  gen_c rupa r
  gen_c ruka k
  for ((i = 1; i <= NMOD; i++)); do
    printf 'module m%s\nexport f%s\n' "$i" "$i" >"$WS/rupamod/stdlib/m$i.rp"
  done

  # ---- WsBuildfile (bentuk sama dengan tests/wstest) ----
  {
    echo "use workspace"
    echo
    echo "# alias singkat"
    echo "projects.rupamod as mod"
    echo "projects.rupa as rupa"
    echo
    echo "projects = rupamod, rupa, ruka"
    echo
    echo "compiler = $COMPILERS"
    echo "flags = Wall, Wextra"
    echo "std = gnu11"
    echo
    echo "# B. rupamod — proyek kemasan (archive)"
    echo "mod.output.binary = false"
    echo "mod.archive.src = stdlib"
    echo "mod.archive.pattern = .rp"
    echo "mod.archive.name = rupa_modules"
    echo "mod.archive.with.tar = true"
    echo "mod.archive.with.ext = gz"
    echo "mod.archive.dir = dist"
    echo
    echo "# A. rupa"
    echo "rupa.sources = src"
    echo "rupa.output.binaryName = rupa"
    echo
    echo "# C. ruka — embed arsip jadi dari rupamod"
    echo "ruka.sources = src"
    echo "ruka.output.binaryName = ruka"
    echo "ruka.depends_on = rupamod"
    echo "ruka.embedded.modules.file = rupamod/dist/rupa_modules.tar.gz"
    echo "ruka.embedded.modules.variable = MODULES"
    echo "ruka.headers = build, I."
  } >"$WS/WsBuildfile"

  # ---- pembanding: build.ninja (graf datar yang setara) ----
  {
    echo "cflags = -Wall -Wextra -std=gnu11"
    echo
    echo "rule cc"
    echo "  command = gcc \$cflags -MMD -MF \$out.d -c \$in -o \$out"
    echo "  depfile = \$out.d"
    echo "  deps = gcc"
    echo "rule link"
    echo "  command = gcc \$in -o \$out"
    echo "rule pack"
    echo "  command = tar -czf \$out -C rupamod stdlib"
    echo "rule embed"
    echo "  command = ld -r -b binary -o \$out \$in"
    echo
    local p pre
    for p in rupa:r ruka:k; do
      pre=${p#*:}; p=${p%%:*}
      echo "build $p/build/main.o: cc $p/src/main.c"
      for ((i = 1; i <= NSRC; i++)); do echo "build $p/build/$pre$i.o: cc $p/src/$pre$i.c"; done
    done
    echo -n "build rupamod/dist/rupa_modules.tar.gz: pack"
    for ((i = 1; i <= NMOD; i++)); do printf ' rupamod/stdlib/m%d.rp' "$i"; done
    echo
    echo -n "build rupa/bin/rupa: link rupa/build/main.o"
    for ((i = 1; i <= NSRC; i++)); do printf ' rupa/build/r%d.o' "$i"; done
    echo
    echo -n "build ruka/bin/ruka: link ruka/build/main.o"
    for ((i = 1; i <= NSRC; i++)); do printf ' ruka/build/k%d.o' "$i"; done
    echo " ruka/build/embed.o | rupamod/dist/rupa_modules.tar.gz"
    echo "build ruka/build/embed.o: embed rupamod/dist/rupa_modules.tar.gz"
    echo
    echo "default rupa/bin/rupa ruka/bin/ruka"
  } >"$WS/build.ninja"

  # ---- pembanding: Makefile ----
  {
    echo "CFLAGS := -Wall -Wextra -std=gnu11"
    echo -n "RUPA_OBJS := rupa/build/main.o"
    for ((i = 1; i <= NSRC; i++)); do printf ' rupa/build/r%d.o' "$i"; done
    echo
    echo -n "RUKA_OBJS := ruka/build/main.o"
    for ((i = 1; i <= NSRC; i++)); do printf ' ruka/build/k%d.o' "$i"; done
    echo " ruka/build/embed.o"
    echo -n "MODS :="
    for ((i = 1; i <= NMOD; i++)); do printf ' rupamod/stdlib/m%d.rp' "$i"; done
    echo
    echo
    echo "all: rupa/bin/rupa ruka/bin/ruka"
    echo
    printf 'rupa/bin/rupa: $(RUPA_OBJS)\n\t@mkdir -p $(@D)\n\tgcc $^ -o $@\n\n'
    printf 'ruka/bin/ruka: $(RUKA_OBJS) rupamod/dist/rupa_modules.tar.gz\n\t@mkdir -p $(@D)\n\tgcc $(RUKA_OBJS) -o $@\n\n'
    printf 'rupamod/dist/rupa_modules.tar.gz: $(MODS)\n\t@mkdir -p $(@D)\n\ttar -czf $@ -C rupamod stdlib\n\n'
    printf 'ruka/build/embed.o: rupamod/dist/rupa_modules.tar.gz\n\t@mkdir -p $(@D)\n\tld -r -b binary -o $@ $<\n\n'
    printf 'rupa/build/%%.o: rupa/src/%%.c\n\t@mkdir -p $(@D)\n\tgcc $(CFLAGS) -MMD -MP -c $< -o $@\n\n'
    printf 'ruka/build/%%.o: ruka/src/%%.c\n\t@mkdir -p $(@D)\n\tgcc $(CFLAGS) -MMD -MP -c $< -o $@\n\n'
    echo '-include $(RUPA_OBJS:.o=.d) $(RUKA_OBJS:.o=.d)'
  } >"$WS/Makefile"
}

# ---------------------------- perintah per tool ----------------------------
tool_cmd() {
  case $1 in
    satria)  "$SATRIA" $SATRIA_ARGS -w ;;
    ninja) ninja ;;
    make)  make -j"$NPROC" ;;
  esac
}
sel_cmd() {  # build selektif: hanya proyek rupa
  case $1 in
    satria)  "$SATRIA" $SATRIA_ARGS -w rupa ;;
    ninja) ninja rupa/bin/rupa ;;
    make)  make -j"$NPROC" rupa/bin/rupa ;;
  esac
}
dep_cmd() {  # build selektif ruka (menarik rupamod lewat depends_on)
  case $1 in
    satria)  "$SATRIA" $SATRIA_ARGS -w ruka ;;
    ninja) ninja ruka/bin/ruka ;;
    make)  make -j"$NPROC" ruka/bin/ruka ;;
  esac
}
build_full() {
  case $1 in
    satria) "$SATRIA" $SATRIA_ARGS -j"$NPROC" -w ;;
    *)    tool_cmd "$1" ;;
  esac
}

# ---------------------------- pengukuran -----------------------------------
count_new() { find "$WS" "$@" -newer "$MARK" 2>/dev/null | wc -l | tr -d '[:space:]'; }

# Harapan invalidasi per skenario (diverifikasi terhadap satria & make).
# Semua tool membangun 1 .o embed dari arsip (cold, mod, dep): graf pembanding
# dibuat melakukan pekerjaan yang sama dengan satria.
# touch: header diubah tapi kontennya sama, jadi tidak boleh ada recompile (berlaku untuk semua tool).
RC_MISMATCH=97      # exit code baris yang obj/arc/bin-nya tidak sesuai harapan
MISMATCH_RUNS=0     # jumlah run yang gagal karena tidak sesuai harapan
EXPECT_MISS=""
exp_chk() { [ "$2" -eq "$3" ] || EXPECT_MISS+=" $1=$2(harap $3)"; }
expect_check() {  # expect_check <tool> <skenario> <obj> <arc> <bin>
  local s=$2 o=$3 a=$4 b=$5
  EXPECT_MISS=""
  case $s in
    cold)          exp_chk obj "$o" $(( EXPECT_OBJ + 1 )); exp_chk arc "$a" 1; exp_chk bin "$b" 2 ;;
    noop)          exp_chk obj "$o" 0; exp_chk arc "$a" 0; exp_chk bin "$b" 0 ;;
    leaf|ruka|sel) exp_chk obj "$o" 1; exp_chk arc "$a" 0; exp_chk bin "$b" 1 ;;
    mod|dep)       exp_chk obj "$o" 1; exp_chk arc "$a" 1; exp_chk bin "$b" 1 ;;
    touch)         exp_chk obj "$o" 0; exp_chk arc "$a" 0; exp_chk bin "$b" 0 ;;
  esac
}

run_once() {
  local label=$1 scen=$2; shift 2
  local t0 t1 rc ms nobj narc nbin warn=""
  CUR="$label/$scen"; draw_progress
  touch "$MARK"
  t0=${EPOCHREALTIME/[.,]/}
  ( cd "$WS" && "$@" ) >/dev/null 2>>"$ERRLOG"
  rc=$?
  t1=${EPOCHREALTIME/[.,]/}
  ms=$(( (t1 - t0 + 500) / 1000 ))
  nobj=$(count_new -name '*.o')
  narc=$(count_new -type f -name 'rupa_modules.tar.gz')
  nbin=$(count_new -type f \( -name rupa -o -name ruka \) -perm -u+x)
  expect_check "$label" "$scen" "$nobj" "$narc" "$nbin"
  if [ "$rc" -eq 0 ] && [ -n "$EXPECT_MISS" ]; then
    rc=$RC_MISMATCH
    MISMATCH_RUNS=$(( MISMATCH_RUNS + 1 ))
  fi
  clear_line
  if [ "$rc" -eq "$RC_MISMATCH" ]; then
    printf '  %s✗ %-6s %-6s TIDAK SESUAI HARAPAN:%s%s\n' "$C_RED" "$label" "$scen" "$EXPECT_MISS" "$C_RST"
    ms=-1
  elif [ "$rc" -ne 0 ]; then
    printf '  %s✗ %-6s %-6s GAGAL (exit %s)%s\n' "$C_RED" "$label" "$scen" "$rc" "$C_RST"
    MISMATCH_RUNS=$(( MISMATCH_RUNS + 1 ))
    ms=-1
  else
    printf '  %s✓%s %-6s %-6s %6s ms  %sobj:%s arc:%s bin:%s%s%s\n' \
      "$C_GRN" "$C_RST" "$label" "$scen" "$ms" "$C_DIM" "$nobj" "$narc" "$nbin" "$C_RST" "$warn"
  fi
  local ver=${VER[$label]:-}; ver=${ver//,/ }
  printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$label" "$scen" "$ms" "$rc" "$nobj" "$narc" "$nbin" "$ver" >>"$CSV"
  STEP=$(( STEP + 1 )); draw_progress
}

have_scenario() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  return 1
}

reset_all() {
  # build/bin/dist root = output root-centric satria (design/workspace_v1.md);
  # sisanya layout nested ninja/make.
  ( cd "$WS" && rm -rf rupa/build rupa/bin ruka/build ruka/bin \
      rupamod/build rupamod/dist build bin dist .ninja_log .ninja_deps )
}

ARTIFACT_WARNED=0
check_artifacts() {
  [ "$ARTIFACT_WARNED" = 1 ] && return 0
  local miss=""
  [ -n "$(find "$WS" -type f -name rupa -perm -u+x 2>/dev/null | head -1)" ] || miss+=" rupa"
  [ -n "$(find "$WS" -type f -name ruka -perm -u+x 2>/dev/null | head -1)" ] || miss+=" ruka"
  [ -n "$(find "$WS" -type f -name rupa_modules.tar.gz 2>/dev/null | head -1)" ] || miss+=" rupa_modules.tar.gz"
  if [ -n "$miss" ]; then
    ARTIFACT_WARNED=1
    clear_line
    printf '  %s! %s tidak menghasilkan:%s (cek stderr.log / opsi -a)%s\n' "$C_YEL" "$1" "$miss" "$C_RST"
  fi
}

prime_tool() {
  CUR="prime $1"; draw_progress
  reset_all
  if ! ( cd "$WS" && build_full "$1" && tool_cmd "$1" ) >/dev/null 2>>"$ERRLOG"; then
    clear_line
    printf '  %s! prime gagal: %s (lihat %s)%s\n' "$C_YEL" "$1" "$ERRLOG" "$C_RST"
    tail -n 3 "$ERRLOG" 2>/dev/null | sed 's/^/      /'
  fi
  check_artifacts "$1"
}

# mutator: <run#> <tool>
mut_leaf()  { ( cd "$WS" && printf '\n/* leaf %s-%s */\n' "$2" "$1" >>rupa/src/r1.c ); }
mut_ruka()  { ( cd "$WS" && printf '\n/* ruka %s-%s */\n' "$2" "$1" >>ruka/src/k1.c ); }
mut_mod()   { ( cd "$WS" && printf '\n# edit %s-%s\n' "$2" "$1" >>rupamod/stdlib/m1.rp ); }
mut_sel()   { mut_leaf "$@"; mut_ruka "$@"; }
mut_dep()   { mut_mod "$@"; mut_leaf "$@"; }
mut_touch() { ( cd "$WS" && touch "ruka/src/h$(( NHEAD / 2 + 1 )).h" ); }

s_cold() {
  have_scenario cold || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    for ((r = 1; r <= NRUN; r++)); do
      reset_all
      run_once "$k" cold build_full "$k"
    done
  done
}

s_strace() {
  have_scenario strace || return 0
  if [ "$HAVE_STRACE" != 1 ]; then
    clear_line
    printf '  %s! strace tidak tersedia; skenario strace dilewati%s\n' "$C_YEL" "$C_RST"
    return 0
  fi

  local k r report total_sec total_calls total_err rank
  local pct sec usecs calls errs syscall
  for k in "${TOOLS[@]}"; do
    for ((r = 1; r <= NRUN; r++)); do
      reset_all
      CUR="strace/$k#$r"; draw_progress
      report="$OUTDIR/workspace.strace.${k}.${r}.txt"
      rm -f "$report"

      if [ "$k" = satria ]; then
        tcmd=("$SATRIA" $SATRIA_ARGS -j"$NPROC" -w)
      elif [ "$k" = ninja ]; then
        tcmd=(ninja)
      else
        tcmd=(make -j"$NPROC")
      fi

      ( cd "$WS" && strace -c -f -o "$report" "${tcmd[@]}" ) >/dev/null 2>>"$ERRLOG"

      total_sec=$(awk '$NF == "total" { print $2; exit }' "$report")
      total_calls=$(awk '$NF == "total" { print $4; exit }' "$report")
      total_err=$(awk '$NF == "total" { if (NF >= 6) print $5; else print 0; exit }' "$report")
      [ -n "$total_sec" ] || total_sec=0
      [ -n "$total_calls" ] || total_calls=0
      [ -n "$total_err" ] || total_err=0

      rank=0
      while IFS=$'\t' read -r pct sec usecs calls errs syscall; do
        [ -n "$syscall" ] || continue
        rank=$((rank + 1))
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
          "$k" strace "$r" "$rank" "$pct" "$sec" "$usecs" "$calls" "$errs" "$syscall" >>"$SYSCALL_CSV"
        [ "$rank" -ge 10 ] && break
      done < <(awk '
        NR == 1 { next }
        /^[- ]*$/ { next }
        $NF == "total" { next }
        $NF ~ /^[[:alnum:]_?]+$/ {
          if (NF >= 6)
            printf "%s\t%s\t%s\t%s\t%s\t%s\n", $1, $2, $3, $4, $5, $6
          else if (NF == 5)
            printf "%s\t%s\t%s\t%s\t0\t%s\n", $1, $2, $3, $4, $5
        }
      ' "$report" | sort -t$'\t' -k2,2nr)

      clear_line
      printf '  %s✓%s %-6s strace %-4s total=%ss calls=%s err=%s\n' \
        "$C_GRN" "$C_RST" "$k" "$r" "$total_sec" "$total_calls" "$total_err"
      awk '
        NR == 1 { next }
        /^[- ]*$/ { next }
        $NF == "total" { next }
        $NF ~ /^[[:alnum:]_?]+$/ {
          if (NF >= 6)
            printf "%.2f\t%s\t%s\t%s\t%s\t%s\n", $1, $2, $3, $4, $5, $6
          else if (NF == 5)
            printf "%.2f\t%s\t%s\t%s\t0\t%s\n", $1, $2, $3, $4, $5
        }
      ' "$report" | sort -t$'\t' -k2,2nr | head -5 | \
        awk -F '\t' '{ printf "      %5s%% %9ss %8s us/call %7s calls %6s err  %s\n", $1, $2, $3, $4, $5, $6, $7 }'

      STEP=$(( STEP + 1 )); draw_progress
    done
  done
}

run_scenario() {  # run_scenario <skenario> <mutator> [fungsi-perintah]
  have_scenario "$1" || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    for ((r = 1; r <= NRUN; r++)); do
      if [ "$MUT_DELAY" != 0 ]; then sleep "$MUT_DELAY"; fi
      "$2" "$r" "$k"
      run_once "$k" "$1" "${3:-tool_cmd}" "$k"
    done
  done
}
mut_none() { :; }

# ---------------------------- laporan --------------------------------------
report() {
  local sorted="$OUTDIR/.sorted.csv"
  awk -F, 'NR > 1' "$CSV" | sort -t, -k2,2 -k1,1 -k3,3n >"$sorted"
  awk -F, -v BW="$BW" -v WIDE="$WIDE" -v B="$C_BLD" -v D="$C_DIM" \
      -v G="$C_GRN" -v Y="$C_YEL" -v R="$C_RED" -v Z="$C_RST" '
    {
      g = $2 SUBSEP $1
      if (!(g in seen)) { seen[g] = 1; ng++; gs[ng] = g; gscen[ng] = $2; glab[ng] = $1 }
      if ($4 == 0) { n[g]++; v[g, n[g]] = $3; ro[g] += $5; ra[g] += $6; rn[g] += $7 } else bad[g]++
    }
    END {
      nsc = split("cold noop leaf ruka mod touch sel dep", SC, " ")
      desc["cold"]  = "build dari nol, 3 proyek"
      desc["noop"]  = "tanpa perubahan"
      desc["leaf"]  = "edit 1 source rupa (ruka tak boleh ikut)"
      desc["ruka"]  = "edit 1 source ruka"
      desc["mod"]   = "edit 1 .rp rupamod -> arsip -> ruka"
      desc["touch"] = "touch 1 header ruka, konten sama"
      desc["sel"]   = "edit rupa+ruka, build selektif -w rupa"
      desc["dep"]   = "edit .rp+rupa, build selektif -w ruka"
      for (si = 1; si <= nsc; si++) {
        s = SC[si]; k = 0; nf = 0
        for (i = 1; i <= ng; i++) {
          if (gscen[i] != s) continue
          g = gs[i]
          if (n[g] > 0) {
            m = n[g]
            med = (m % 2) ? v[g, (m + 1) / 2] : (v[g, m / 2] + v[g, m / 2 + 1]) / 2
            k++; L[k] = glab[i]; M[k] = med; LO[k] = v[g, 1]; HI[k] = v[g, m]
            OB[k] = ro[g] / m; AR[k] = ra[g] / m; BN[k] = rn[g] / m; BD[k] = bad[g] + 0
          } else fl[++nf] = glab[i]
        }
        if (k == 0 && nf == 0) continue
        for (a = 2; a <= k; a++)
          for (b = a; b > 1 && M[b] < M[b - 1]; b--) {
            t = L[b];  L[b]  = L[b - 1];  L[b - 1]  = t
            t = M[b];  M[b]  = M[b - 1];  M[b - 1]  = t
            t = LO[b]; LO[b] = LO[b - 1]; LO[b - 1] = t
            t = HI[b]; HI[b] = HI[b - 1]; HI[b - 1] = t
            t = OB[b]; OB[b] = OB[b - 1]; OB[b - 1] = t
            t = AR[b]; AR[b] = AR[b - 1]; AR[b - 1] = t
            t = BN[b]; BN[b] = BN[b - 1]; BN[b - 1] = t
            t = BD[b]; BD[b] = BD[b - 1]; BD[b - 1] = t
          }
        printf "\n%s%s%s %s%s%s\n", B, toupper(s), Z, D, desc[s], Z
        mx = (k > 0) ? M[k] : 1; if (mx < 1) mx = 1
        best = (k > 0) ? M[1] : 1; if (best < 1) best = 1
        for (i = 1; i <= k; i++) {
          len = int(M[i] / mx * BW + 0.5); if (len < 1) len = 1
          br = ""; for (j = 1; j <= BW; j++) br = br (j <= len ? "█" : "░")
          col = (i == 1) ? G : ""
          mark = (i == 1) ? "★" : " "
          printf "  %s%s %-6s %s %6d ms %5.2f×%s", col, mark, L[i], br, M[i], M[i] / best, Z
          if (WIDE) printf "  %s(%d–%d) obj:%d arc:%d bin:%d%s", D, LO[i], HI[i], OB[i] + 0.5, AR[i] + 0.5, BN[i] + 0.5, Z
          if (BD[i] > 0) printf " %s✗%d%s", R, BD[i], Z
          printf "\n"
        }
        for (i = 1; i <= nf; i++) printf "  %s✗ %-6s semua run gagal atau tidak sesuai harapan%s\n", R, fl[i], Z
      }
    }' "$sorted"
  rm -f "$sorted"
  echo
  echo "${C_DIM}median ms · ★ tercepat · × = rasio vs tercepat${C_RST}"
  echo "${C_DIM}Invalidasi yang diharapkan (pembanding ninja/make):${C_RST}"
  echo "${C_DIM}  leaf : obj 1, bin 1 (rupa saja) · ruka: obj 1, bin 1${C_RST}"
  echo "${C_DIM}  mod  : arc 1, bin 1, obj 1 (.o embed arsip, ruka relink)${C_RST}"
  echo "${C_DIM}  touch: tidak boleh ada recompile (semua tool)${C_RST}"
  echo "${C_DIM}  sel  : obj 1, bin 1 (hanya rupa; ruka dibiarkan kotor)${C_RST}"
  echo "${C_DIM}  dep  : arc 1, bin 1, obj 1 (rupamod+ruka; rupa kotor; .o embed)${C_RST}"
}

# ---------------------------- main -----------------------------------------
trap 'echo; echo "dibatalkan."; exit 130' INT

echo "${C_BLD}${C_CYN}▌ satria workspace benchmark${C_RST} ${C_DIM}· ${#TOOLS[@]} tool · $NRUN run · median${C_RST}"
echo "  workspace: rupamod ($NMOD .rp) · rupa ($NSRC .c) · ruka ($NSRC .c)"
echo "  graf     : ruka depends_on rupamod (embed arsip)"
echo "  perintah : satria -w (semua) · satria -w <nama> (selektif)"
echo "  versi    :"
for k in "${TOOLS[@]}"; do printf '    %-6s %s\n' "$k" "${VER[$k]:-?}"; done
printf '    %-6s %s\n' gcc "$VER_GCC · $(uname -m)"
[ -n "$SATRIA_ARGS" ] && echo "  satria args: $SATRIA_ARGS"
echo "  dir      : $WS"
echo

TOTAL=0
for s in cold noop leaf ruka mod touch sel dep strace; do
  have_scenario "$s" && TOTAL=$(( TOTAL + ${#TOOLS[@]} * NRUN ))
done
[ "$TOTAL" -gt 0 ] || TOTAL=1

CUR="gen"; draw_progress
gen_ws
clear_line
T_START=$SECONDS
draw_progress

s_cold
run_scenario noop  mut_none
run_scenario leaf  mut_leaf
run_scenario ruka  mut_ruka
run_scenario mod   mut_mod
run_scenario touch mut_touch
run_scenario sel   mut_sel sel_cmd
run_scenario dep   mut_dep dep_cmd
s_strace

clear_line
echo
echo "${C_GRN}✔ selesai${C_RST} dalam $(fmt_t $(( SECONDS - T_START )))"
report

echo
echo "hasil lengkap: $CSV"
[ "$HAVE_STRACE" = 1 ] && echo "syscall detail: $SYSCALL_CSV"
[ -s "$ERRLOG" ] && echo "stderr tool  : $ERRLOG"
rm -f "$MARK"
[ "$KEEP" = 1 ] || rm -rf "$WS"
exit 0



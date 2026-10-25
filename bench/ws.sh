#!/usr/bin/env bash
# ============================================================================
# workspace.sh — benchmark mode workspace satria (Buildfile.ws)
#
# Meniru tests/wstest: 3 proyek dalam satu workspace
#   rupamod : proyek kemasan (archive .rp -> modules/rupa_modules.tar)
#   core    : binary C + library (rupa)
#   bade    : binary C + library (bade), depends_on rupamod, embed arsip
#
# PERBEDAAN PENTING:
# Benchmark end-to-end: Satria vs CMake+Ninja, dengan pekerjaan build ekuivalen:
# 1. Membangun static library (libcore.a, libbade.a).
# 2. Menyelesaikan circular dependency (core <-> bade) menggunakan
#    linker group (-Wl,--start-group ... -Wl,--end-group).
# Tidak ada lagi "curang" dengan me-link .o secara langsung.
#
# Skenario: cold, noop, leaf, bade, mod, touch, hedit, sel, dep, libtouch, libhdr, rel, strace.
# Setiap run memakai shim gcc/ld (PATH) yang mencatat setiap kompilasi/link
# ke work.log — bukti pekerjaan aktual, bukan sekadar timestamp artefak.
#   touch : touch include/a.h tanpa ubah konten (perilaku tiap tool diaudit)
#   hedit : ubah ISI library/a.h -> semua tool wajib rebuild dependen header
#   sel   : edit core+bade, build selektif core — cakupan DISAMAKAN antar tool
#            (satria -w core ↔ CMake --target core_static core_shared)
#   dep   : edit .rp, build selektif bade — cakupan DISAMAKAN antar tool
#            (satria -w bade ↔ CMake --target bade_static bade_shared bade)
#   libtouch: touch library/ext.h (header internal via headers.internal), konten sama
#   libhdr: edit ISI library/ext.h (header internal, diakses include relatif dari r2/k2)
#   rel   : release terpisah (tar + sha256, satria-only) sesuai docs/guide/releases.md
#
# Laporan (design/bench_ws_output.md): Legend/Metrics/Correctness di depan
# (setelah versi), stream per run, tabel ringkas [Detailed Validation Log]
# (verdict In/Not in accordance per skenario+tool), chart median, ditutup
# Success Rate + artefak (sumber data: view.csv).
#
# PRINSIP (acuan interpretasi): setiap tool dinilai terhadap "except" (harapan
# kerja konsep real workspace), BUKAN terhadap tool lain. Tool bebas memakai
# strategi apa pun, asal pekerjaannya sesuai harapan. Bila tidak, verdict
# "Not in accordance" dan skenario itu dihitung (failed) di benchmark report,
# untuk tool mana pun termasuk satria. Tidak ada perbandingan silang "vs satria".
#
# KEADILAN PEKERJAAN (problems/ws.md: "pekerjaan dan hasil wajib setara"):
#  - Mekanisme embed disamakan: satria kini hanya codegen (embedded.c sebagai
#    source biasa), jadi CMake juga memakai embed_c.sh (carray) — bukan ld -r.
#  - Cakupan selektif sel/dep disamakan pada output yang sama antar tool.
#  - Sisa beda kerja yang melekat pada strategi tool (mtime vs content-hash:
#    touch/libtouch/leaf/bade) ditandai [!] — bukan perbandingan setara 1:1.
# ============================================================================
set -uo pipefail
[ -n "${EPOCHREALTIME:-}" ] || {
  echo "butuh bash >= 5 (EPOCHREALTIME)" >&2
  exit 1
}

# ---------------------------- konfigurasi ----------------------------------
NSRC=80                       # -n : jumlah source per proyek
NHEAD=20                      # -H : jumlah header berantai per proyek
NMOD=40                       # -m : jumlah file .rp di rupamod
NRUN=5                        # -r : pengulangan per skenario (median)
SATRIA=bin/satria             # -b : binary satria
SATRIA_ARGS=""                # -a : argumen tambahan untuk satria
COMPILERS="gcc"               # -c : nilai 'compiler =' di Buildfile.ws
KEEP=0                        # -k : jangan hapus workspace di akhir
ONLY=""                       # -s : subset skenario
TOOLS_OPT="satria,cmake" # -t : subset tool pembanding
OUTDIR=""                     # -o : direktori kerja
NPROC=$(nproc 2> /dev/null || echo 4)
MUT_DELAY=0       # -d : jeda (detik) sebelum tiap edit
RUN_DELAY=0       # -D : jeda (detik) sebelum tiap pengukuran
AUDIT_ENABLED=true             # -w true/false: preflight audit sebelum benchmark
ARTIFACTS_ENABLED=true         # -artifacts true/false: simpan log dan CSV
STRACE_ENABLED=false           # -strace true/false: syscall profiling (opt-in)
RUN_ORDER=grouped # -O : grouped atau abba (rotasi urutan tool)

usage() {
  sed -n '2,/^# ====.*$/p' "$0"
  exit 0
}
# Long option diproses sebelum getopts agar -artifacts false tidak dibaca sebagai -a.
args=()
while (($#)); do
  case $1 in
    -strace|--strace)
      (($# >= 2)) || { echo "-strace membutuhkan true/false" >&2; exit 2; }
      STRACE_ENABLED=$2; shift 2 ;;
    -artifacts|--artifacts)
      (($# >= 2)) || { echo "-artifacts membutuhkan true/false" >&2; exit 2; }
      ARTIFACTS_ENABLED=$2; shift 2 ;;
    *) args+=("$1"); shift ;;
  esac
done
set -- "${args[@]}"
while getopts "n:H:m:r:b:a:c:d:D:O:ko:s:t:w:h" opt; do
  case $opt in
    n) NSRC=$OPTARG ;; H) NHEAD=$OPTARG ;; m) NMOD=$OPTARG ;;
    r) NRUN=$OPTARG ;; b) SATRIA=$OPTARG ;; a) SATRIA_ARGS=$OPTARG ;;
    c) COMPILERS=$OPTARG ;; k) KEEP=1 ;; o) OUTDIR=$OPTARG ;;
    s) ONLY=$OPTARG ;; t) TOOLS_OPT=$OPTARG ;; d) MUT_DELAY=$OPTARG ;; D) RUN_DELAY=$OPTARG ;; O) RUN_ORDER=$OPTARG ;; w) AUDIT_ENABLED=$OPTARG ;; *) usage ;;
  esac
done

for v in AUDIT_ENABLED ARTIFACTS_ENABLED STRACE_ENABLED; do
  case ${!v} in true|false) ;; *) echo "$v harus true atau false" >&2; exit 2 ;; esac
done
# Validasi argumen SEJAK DINI (problems/ws.md "Noted"): getopts mengambil
# argv berikutnya apa adanya, jadi `-r -t satria,cmake` membuat NRUN="-t" dan
# gagal jauh setelah parse dengan pesan membingungkan. Contoh aman:
#   bench/ws.sh -n 1 -r 1 -t satria,cmake
opt_uint() { # opt_uint <flag> <nilai> <min>
  case $2 in
    '' | *[!0-9]*)
      echo "opsi -$1 butuh angka (nilai '$2') — '-$1 -t ...' salah baca -t sebagai argumen. Contoh aman: $0 -n 1 -r 1 -t satria,cmake" >&2
      exit 2
      ;;
  esac
  [ "$2" -ge "$3" ] || {
    echo "opsi -$1 harus >= $3 (nilai '$2')" >&2
    exit 2
  }
}
opt_uint n "$NSRC" 1
opt_uint H "$NHEAD" 1
opt_uint m "$NMOD" 1
opt_uint r "$NRUN" 1
case ${BENCH_EMBED:-} in "" | ld | carray) ;; *)
  echo "BENCH_EMBED harus ld atau carray" >&2
  exit 2
  ;;
esac
[[ $MUT_DELAY =~ ^[0-9]+([.][0-9]+)?$ ]] || {
  echo "opsi -d butuh angka detik non-negatif (nilai '$MUT_DELAY')" >&2
  exit 2
}

[[ $RUN_DELAY =~ ^[0-9]+([.][0-9]+)?$ ]] || {
  echo "opsi -D butuh detik non-negatif" >&2
  exit 2
}
case "$RUN_ORDER" in grouped | abba) ;; *)
  echo "opsi -O: grouped atau abba" >&2
  exit 2
  ;;
esac

[ -x "$SATRIA" ] || {
  echo "satria tidak ditemukan/dieksekusi: $SATRIA" >&2
  exit 1
}
SATRIA=$(cd "$(dirname "$SATRIA")" && pwd)/$(basename "$SATRIA")
IFS=, read -r -a TOOLS <<< "$TOOLS_OPT"
for t in "${TOOLS[@]}"; do
  case $t in cmake | cninja | satria) ;; *)
    echo "tool tidak dikenal: $t" >&2
    exit 1
    ;;
  esac
done
# Alias publik cninja = CMake + Ninja; internal tetap cmake untuk
# menjaga seluruh generator, skenario dan validator existing.
for i in "${!TOOLS[@]}"; do
  [ "${TOOLS[$i]}" = cninja ] && TOOLS[$i]=cmake
done
[ "${#TOOLS[@]}" -gt 0 ] || { echo "pilih satria dan/atau cmake" >&2; exit 2; }
for t in gcc tar ar; do
  command -v "$t" > /dev/null 2>&1 || {
    echo "tool wajib tidak ada: $t" >&2
    exit 1
  }
done
if [[ " ${TOOLS[*]} " == *" cmake "* ]]; then
  for t in cmake ninja; do
    command -v "$t" > /dev/null 2>&1 || { echo "tool wajib tidak ada: $t" >&2; exit 1; }
  done
fi

# ---------------------------- strace (opsional) ----------------------------
# strace memakai ptrace yang bentrok dengan proot (juga ptrace/seccomp) dan
# pernah membuat proot-distro FORCE CLOSE — otomatis DILEWATI di dalam proot.
# Paksa aktif kembali dengan SATRIA_BENCH_STRACE=1 bila tetap diinginkan.
IN_PROOT=0
if [ -n "${PROOT:-}" ] || [ -n "${PROOT_L2S_DIR:-}" ]; then IN_PROOT=1; fi
case ${container:-} in *proot*) IN_PROOT=1 ;; esac
HAVE_STRACE=0
STRACE_SKIP=""
if [ "$STRACE_ENABLED" = true ] && command -v strace > /dev/null 2>&1; then
  if [ "${SATRIA_BENCH_STRACE:-}" = 1 ]; then
    HAVE_STRACE=1
  elif [ "$IN_PROOT" = 1 ]; then
    STRACE_SKIP="proot terdeteksi — strace (ptrace) bentrok & pernah force close"
  else
    HAVE_STRACE=1
  fi
fi

# ---------------------------- versi ----------------------------------------
first_ver() { grep -oE '[0-9]+(\.[0-9]+)+' | head -1; }
satria_id() {
  local d c h v_bin
  d=$(dirname "$SATRIA")
  c=$(git -C "$d" describe --tags --exact-match 2> /dev/null)
  if [ -n "$c" ]; then
    c="release ${c}"
  else
    c=$(git -C "$d" describe --tags --always --dirty 2> /dev/null)
    if [ -n "$c" ]; then
      c="dev git:${c}"
    else
      v_bin=$($SATRIA version 2> /dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
      [ -z "$v_bin" ] && v_bin=$(strings "$SATRIA" 2> /dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
      c="release ${v_bin:-unknown}"
    fi
  fi
  h=$(sha1sum "$SATRIA" 2> /dev/null | cut -c1-6)
  echo "${c} bin:${h:-n/a}"
}
declare -A VER
VER_GCC=$(gcc -dumpfullversion 2> /dev/null || gcc -dumpversion 2> /dev/null)
CC_NAME=gcc
gcc --version 2> /dev/null | head -1 | grep -qi clang && CC_NAME=clang
command -v cmake > /dev/null 2>&1 && VER[cmake]=$(cmake --version 2>&1 | first_ver)
command -v ninja > /dev/null 2>&1 && VER[ninja]=$(ninja --version 2>&1 | first_ver)
VER[satria]=$(satria_id)

# ---------------------------- direktori kerja ------------------------------
[ -n "$OUTDIR" ] || OUTDIR="$PWD/ws.tmp"
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)
WSROOT="$OUTDIR"
WS=""
select_ws() {
  case "$1" in
    satria|cmake) WS="$WSROOT/$1" ;;
    *) echo "tool workspace tidak dikenal: $1" >&2; return 1 ;;
  esac
}
CSV="$OUTDIR/results.csv"
SYSCALL_CSV="$OUTDIR/workspace-syscalls.csv"
VIEW_CSV="$OUTDIR/view.csv" # aktual + harapan per run (laporan view ws.md)
MARK="$OUTDIR/.mark"
ERRLOG="$OUTDIR/stderr.log"
rm -f "$CSV" "$ERRLOG" "$VIEW_CSV"
echo 'label,scenario,ms,rc,obj,arc,lib,bin,cc,lnk,version' > "$CSV"
echo 'label,scenario,rc,ms,obj,arc,lib,bin,cc,eobj,earc,elib,ebin,ecc,miss' > "$VIEW_CSV"
echo 'tool,scenario,run,rank,percent,seconds,usecs_per_call,calls,errors,syscall' > "$SYSCALL_CSV"

ws_source() { # ws_source <modul>: muat bench/ws/<modul> di shell ini
  [ -r "$WS_LIB/$1" ] || {
    echo "modul tidak ditemukan: $WS_LIB/$1" >&2
    exit 1
  }
  # shellcheck source=/dev/null
  source "$WS_LIB/$1"
}
WS_LIB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ws"

# ---------------------------- modul (bench/ws/*.sh) ------------------------
# Dimuat di posisi yang sama dengan kode aslinya, sehingga urutan eksekusi
# top-level tidak berubah.
ws_source shim.sh
ws_source expect.sh
ws_source ui.sh
ws_source gen.sh
ws_source tool.sh
ws_source measure.sh
ws_source prime.sh
ws_source scen.sh
ws_source report.sh

# ---------------------------- main -----------------------------------------
trap 'echo; echo "dibatalkan."; exit 130' INT
echo "${C_BLD}${C_CYN}▌ satria workspace benchmark${C_RST} ${C_DIM}· $(plural "${#TOOLS[@]}" tool) · $(plural "$NRUN" run) · median${C_RST}"
rule_line "$(box_width)"
echo "Workspace : rupamod ($NMOD .rp) · core ($NSRC .c) · bade ($NSRC .c)"
echo "Graph     : bade depends_on rupamod (embed), core <-> bade (circular)"
echo "Directory : $WSROOT/{${TOOLS_OPT//,/ ,}}"
echo
echo "Versions  :"
for k in "${TOOLS[@]}"; do printf '  %-6s : %s\n' "$(tool_display "$k")" "${VER[$k]:-?}"; done
printf '  %-5s : %s\n' "$CC_NAME" "$VER_GCC · $(uname -m)"
[ -n "$SATRIA_ARGS" ] && echo "satria args : $SATRIA_ARGS"
echo
rules_header
echo
if [ "$AUDIT_ENABLED" = true ]; then
  echo "${C_BLD}[Actual Work Audit · untimed]${C_RST}"
  audit_failed=0
  AUDIT_QUIET_PROGRESS=1
  for k in "${TOOLS[@]}"; do
    select_ws "$k"
    : > "$RB_CC_LOG"
    if prime_tool "$k"; then
      read -r audit_cc audit_link < <(awk -F'|' '
        $1=="C" {c++} $1=="L" {l++} END {print c+0, l+0}' "$RB_CC_LOG")
      # Dua main + dua kumpulan source; dua executable wajib di-link.
      if ((audit_cc >= 2 * NSRC + 2 && audit_link >= 2)); then
        printf '  ✓ %-8s compile=%s binary-links=%s · gate OK\n' "$(tool_display "$k")" "$audit_cc" "$audit_link"
      else
        printf '  ✗ %-8s compile=%s binary-links=%s (expected >=%s / >=2)\n' \
          "$(tool_display "$k")" "$audit_cc" "$audit_link" "$((2 * NSRC + 2))"
        audit_failed=1
      fi
    else
      printf '  ✗ %-8s prime/gate failed\n' "$(tool_display "$k")"
      audit_failed=1
    fi
  done
  unset AUDIT_QUIET_PROGRESS
  if ((audit_failed)); then
    echo "Audit gagal; benchmark dibatalkan. Lihat $ERRLOG" >&2
    exit 1
  fi
  echo
fi
echo "${C_BLD}[Execution Stream]${C_RST}"

TOTAL=0
for s in cold noop leaf bade mod touch hedit sel dep libtouch libhdr strace; do
  [ "$s" = strace ] && [ "$HAVE_STRACE" != 1 ] && continue # tidak dihitung bila dilewati
  have_scenario "$s" && {
    if [ "$s" = strace ]; then
      TOTAL=$((TOTAL + ${#TOOLS[@]}))
    else
      TOTAL=$((TOTAL + ${#TOOLS[@]} * NRUN))
    fi
  }
done
have_scenario rel && case " ${TOOLS[*]} " in *" satria "*) TOTAL=$((TOTAL + NRUN)) ;; esac
[ "$TOTAL" -gt 0 ] || TOTAL=1

CUR="gen"
draw_progress
for k in "${TOOLS[@]}"; do
  select_ws "$k"
  gen_ws
done
clear_line
T_START=$SECONDS
draw_progress

s_cold
run_scenario noop mut_none
run_scenario leaf mut_leaf
run_scenario bade mut_bade
run_scenario mod mut_mod
run_scenario touch mut_touch
run_scenario hedit mut_hedit
run_scenario sel mut_sel sel_cmd
run_scenario dep mut_dep dep_cmd
run_scenario libtouch mut_libtouch
run_scenario libhdr mut_libhdr
s_release
clear_line # tutup baris progres terakhir; tanpa ini blok view menempel di baris bar
view_report
s_strace

clear_line
echo
echo "${C_GRN}✔ selesai${C_RST} dalam $(fmt_t $((SECONDS - T_START)))"
echo
echo "${C_BLD}BENCHMARK REPORT (Median)${C_RST}"
rule_line "$(box_width)"
report
success_footer
if [ "$NOT_ACC" -gt 0 ] || [ "$MISMATCH_RUNS" -gt 0 ]; then
  printf '%s✗ %d run Not in accordance · %d run gagal — lihat %s%s\n' \
    "$C_RED" "$NOT_ACC" "$MISMATCH_RUNS" "$ERRLOG" "$C_RST"
fi
echo
if [ "$ARTIFACTS_ENABLED" = true ]; then
echo "${C_BLD}Artifacts :${C_RST}"
echo "  hasil lengkap : $CSV"
[ -s "$VIEW_CSV" ] && echo "  view data     : $VIEW_CSV"
[ -s "$WORK_LOG" ] && echo "  bukti kerja   : $WORK_LOG"
[ "$HAVE_STRACE" = 1 ] && echo "  syscall detail: $SYSCALL_CSV"
[ -s "$ERRLOG" ] && echo "  stderr tool   : $ERRLOG"
else
  rm -f "$CSV" "$VIEW_CSV" "$WORK_LOG" "$SYSCALL_CSV" "$ERRLOG"
  rm -f "$OUTDIR"/workspace.strace.*.txt
fi
rm -f "$OUTDIR/.artifacts.before" "$OUTDIR/.artifacts.after" "$OUTDIR/.gate.list" "$OUTDIR/.nm.list"
if [ "$KEEP" != 1 ]; then
  for k in "${TOOLS[@]}"; do select_ws "$k"; rm -rf "$WS"; done
  rm -rf "$OUTDIR/.prime"
fi

{ [ "$MISMATCH_RUNS" -eq 0 ] && [ "$NOT_ACC" -eq 0 ]; } || exit 1

#!/usr/bin/env bash
# ============================================================================
# compare.sh — benchmark build tool: ninja vs make vs bear+make vs satria
#                vs cmake-ninja vs meson-ninja vs xmake vs tup
#
# Membuat project sintetis (N source berantai header H, gaya /tmp/bench),
# menuliskan build.ninja + Makefile + Buildfile + CMakeLists.txt +
# meson.build + xmake.lua + Tupfile untuk project yang SAMA,
# lalu mengukur tiap tool pada skenario yang setara.
#
# Skenario:
#   cold   : build dari nol (cmake/meson TERMASUK langkah configure)
#   noop   : build tanpa perubahan apa pun (metrik utama overhead)
#   touch  : touch 1 header (konten sama) — satria: 0 recompile (content-based)
#   edit   : edit isi 1 source — tepat 1 recompile + relink
#            + baris "gcc-floor" (gcc -c 1 file + link langsung, tanpa build
#              tool) sebagai lantai; selisih tool vs lantai = overhead tool
#   compdb : generate compile_commands.json (bear: butuh build bersih)
#   strace : hitung syscall no-op (opsional: -s strace; jika tersedia)
#
# Metodologi (v2):
#   - Tiap tool diukur berurutan (tool-major) dan DIPRIME dulu: build bersih
#     + 1 run no-op yang tidak dihitung. Ini mencegah tool saling mengotori
#     state (.d, .ninja_log, objek) pada direktori build bersama.
#   - Tabel akhir memakai MEDIAN + min-max, bukan rerata.
#   - Kolom "obj" = jumlah .o yang benar-benar dikompilasi ulang (sanity check).
#   - results.csv memuat versi tiap tool (kolom "version"; satria = git/hash biner).
#   - syscalls.csv memuat rincian syscall per tool (tool,syscall,calls,errors).
#
# Pemakaian:
#   ./bench/compare.sh                # default: 150 source, 30 header, 5 run
#   ./bench/compare.sh -n 40 -H 10    # project kecil, cepat
#   ./bench/compare.sh -s edit,touch  # subset skenario
#   ./bench/compare.sh -s strace      # syscall profiling (opt-in)
#   NO_COLOR=1 ./bench/compare.sh     # tanpa warna / progress bar
#
# Catatan kejujuran: semua tool menghasilkan artefak yang setara, tapi
# fiturnya beda. Angka ini untuk melacak REGRESI satria — bukan klaim
# bahwa satu tool "lebih baik" secara umum.
# ============================================================================
set -u

[ -n "${EPOCHREALTIME:-}" ] || { echo "butuh bash >= 5 (EPOCHREALTIME)" >&2; exit 1; }


# ---------------------------- modul (bench/compare/*.sh) -------------------
# Dimuat di level top-level (bukan lewat fungsi) supaya $@ skrip tetap utuh
# untuk getopts di config.sh. Urutan eksekusi sama dengan kode aslinya.
CMP_LIB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/compare"
CMP_MODULES="config.sh tools.sh workdir.sh ui.sh gen.sh tool.sh measure.sh scen.sh audit.sh"
# Periksa semua modul dulu, baru muat satu per satu.
for _cmp_mod in $CMP_MODULES; do
  [ -r "$CMP_LIB/$_cmp_mod" ] || {
    echo "modul tidak ditemukan: $CMP_LIB/$_cmp_mod" >&2
    exit 1
  }
done
for _cmp_mod in $CMP_MODULES; do
  # shellcheck source=/dev/null
  source "$CMP_LIB/$_cmp_mod"
done
unset _cmp_mod CMP_MODULES

# ---------------------------- main -----------------------------------------
trap 'echo; echo "dibatalkan."; exit 130' INT

SKIPPED=""
[ "$HAVE_BEAR"  = 0 ] && SKIPPED+=" bear"
[ "$HAVE_CMAKE" = 0 ] && SKIPPED+=" cmake"
[ "$HAVE_MESON" = 0 ] && SKIPPED+=" meson"
[ "$HAVE_XMAKE" = 0 ] && SKIPPED+=" xmake"
[ "$HAVE_TUP"   = 0 ] && SKIPPED+=" tup"

echo "${C_BLD}${C_CYN}▌ satria benchmark${C_RST} ${C_DIM}· ${#TOOLS[@]} tool · $NRUN run · median${C_RST}"
rule_line "$(box_width)"
echo "Project   : $NSRC source · $NHEAD header"
echo "Workers   : $NPROC (all tools with configurable -j)"
echo "Compiler  : gcc · -O2 · gnu11 (generator-dependent extra flags may remain)"
echo "Caveat    : cold includes Ninja graph generation and CMake/Meson configure; Bear compiles for compdb"
echo "Directory : $PROJ"
echo
echo 'Versions  :'
for k in "${TOOLS[@]}"; do
  printf '  %-12s : %s\n' "$(label_of "$k")" "${VER[$(label_of "$k")]:-?}"
done
printf '  %-12s : %s\n' gcc "$VER_GCC · $(uname -m)"
[ -n "$SKIPPED" ] && echo "${C_DIM}Skipped   :$SKIPPED${C_RST}"
echo
rules_header
echo
# total langkah untuk progress bar
TOTAL=0
for s in cold noop touch edit; do
  have_scenario "$s" && TOTAL=$(( TOTAL + ${#TOOLS[@]} * NRUN ))
done
have_scenario edit && TOTAL=$(( TOTAL + NRUN ))   # baris gcc-floor
have_scenario compdb && TOTAL=$(( TOTAL + ${#CDB_TOOLS[@]} * NRUN ))
if [ -n "$ONLY" ] && have_scenario strace && [ "$HAVE_STRACE" = 1 ]; then TOTAL=$(( TOTAL + ${#TOOLS[@]} )); fi
[ "$TOTAL" -gt 0 ] || TOTAL=1

gen_project
if ! audit_work; then
  exit 1
fi
echo
echo "${C_BLD}[Execution Stream]${C_RST}"
T_START=$SECONDS
CUR="gen"; draw_progress

s_cold
s_noop
s_touch
s_edit
s_compdb
s_strace

clear_line
echo
echo "${C_GRN}✔ selesai${C_RST} dalam $(fmt_t $(( SECONDS - T_START )))"
validation_report
echo
echo "${C_BLD}BENCHMARK REPORT (Median)${C_RST}"
rule_line "$(box_width)"
report
echo
echo "${C_BLD}Artifacts :${C_RST}"
echo "  hasil lengkap : $CSV"
[ -f "$AUDITCSV" ] && echo "  work audit    : $AUDITCSV"
[ -s "$AUDITLOG" ] && echo "  audit log     : $AUDITLOG"
[ -f "$SYSCSV" ] && echo "  syscall detail: $SYSCSV"
[ -s "$ERRLOG" ] && echo "  stderr tool   : $ERRLOG"

rm -f "$MARK"
[ "$KEEP" = 1 ] || { rm -rf "$PROJ" "$OUTDIR"/strace.*.txt; }
exit 0


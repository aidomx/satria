#!/bin/sh
. "$(dirname "$0")/../lib/layout.sh"
# Uji pilot router plan (P4, design/router_centric.md): induk menyusun rencana
# per fase dan memverifikasi bahwa executor menjalankan tepat rencana itu.
# Pemakaian: sh tests/unit/common/router_plan.sh <path-ke-satria> [path-ke-miniws]
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
SRC=$(abspath "${2:-$(dirname "$0")/../../miniws}")
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/router_plan"; rm -rf "${W}"; mkdir -p "${W}" || exit 1
cp -r "$SRC/." "$W/" || exit 2
printf "flags = MMD, MP\n" >> "$W/WsBuildfile"
cd "$W" || exit 2
export NO_COLOR=1
wait_tick # hindari racy terhadap build pertama
pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }
run() { "$SAT" -j${UNIT_JOBS:-2} -w >"$W/out.txt" 2>&1; return $?; }
verified() { grep -q "router-plan: fase library: .* 0 menyimpang" "$W/out.txt" &&
             grep -q "router-plan: fase binary: .* 0 menyimpang" "$W/out.txt"; }

# Cold build dengan plan aktif: kedua fase harus terverifikasi penuh.
SATRIA_ROUTER_PLAN=1 run; check "cold build dengan router plan berhasil" $?
verified; check "fase library+binary terverifikasi tanpa menyimpang" $?
! grep -q "di luar rencana" "$W/out.txt"; check "tidak ada eksekusi di luar rencana" $?
[ -z "$(ls .satria/ws-plan-* 2>/dev/null)" ]; check "penanda rencana dibersihkan" $?

# Jalur paralel (fork): penanda ditulis anak, diverifikasi induk.
BSRC=$(unit_project_source "$W" bade)
[ -n "$BSRC" ] || { echo "FAIL bade: tidak ada source untuk proyek bade (layout)" >&2; exit 1; }
wait_tick; echo "/* p */" >> "$BSRC"
SATRIA_ROUTER_PLAN=1 SATRIA_WS_PAR=1 run; check "rebuild paralel dengan router plan berhasil" $?
verified; check "jalur fork: rencana terverifikasi tanpa menyimpang" $?
[ -z "$(ls .satria/ws-plan-* 2>/dev/null)" ]; check "jalur fork: penanda dibersihkan" $?

# Tanpa env: pilot tidak boleh aktif (nol overhead & nol output).
wait_tick; echo "/* q */" >> "$BSRC"
run; check "rebuild tanpa router plan berhasil" $?
! grep -q "router-plan" "$W/out.txt"; check "tanpa env: tidak ada output router-plan" $?

echo "---"; echo "lulus: $pass  gagal: $fail  (folder uji: $W)"
[ "$fail" -eq 0 ]

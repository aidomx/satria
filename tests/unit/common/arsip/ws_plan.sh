#!/bin/sh
. "$(dirname "$0")/../lib/layout.sh"
# Uji pilot scheduler plan (P4, design/router_centric.md): induk menyusun rencana
# per fase dan memverifikasi bahwa executor menjalankan tepat rencana itu.
# Pemakaian: sh tests/unit/common/ws_plan.sh <path-ke-satria> [path-ke-miniws]
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
SRC=$(abspath "${2:-$(dirname "$0")/../../miniws}")
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/ws_plan"; rm -rf "${W}"; mkdir -p "${W}" || exit 1
cp -r "$SRC/." "$W/" || exit 2
printf "flags = MMD, MP\n" >> "$W/WsBuildfile"
cd "$W" || exit 2
export NO_COLOR=1
wait_tick # hindari racy terhadap build pertama
pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }
run() { "$SAT" -j${UNIT_JOBS:-2} -w >"$W/out.txt" 2>&1; return $?; }
verified() { ! grep -q "ws-plan:" "$W/out.txt"; }
stage_clean() { [ -z "$(find .satria/router-stage -type f 2>/dev/null)" ]; }

# Cold build dengan plan aktif: kedua fase harus terverifikasi penuh.
SATRIA_WS_PLAN=1 run; check "cold build dengan scheduler plan berhasil" $?
verified; check "fase library+binary terverifikasi tanpa menyimpang" $?
! grep -q "ws-plan: link batch" "$W/out.txt"; check "fase binary satu batch, tanpa link batch terpisah" $?
! grep -q "di luar rencana" "$W/out.txt"; check "tidak ada eksekusi di luar rencana" $?
[ -z "$(ls .satria/ws-plan-* 2>/dev/null)" ]; check "penanda rencana dibersihkan" $?
stage_clean; check "cold: stage router dibersihkan setelah merge" $?

# Jalur paralel (fork): penanda ditulis anak, diverifikasi induk.
BSRC=$(unit_project_source "$W" bade)
[ -n "$BSRC" ] || { echo "FAIL bade: tidak ada source untuk proyek bade (layout)" >&2; exit 1; }
wait_tick; BSUM=$(cksum < bin/bade); echo "int ws_plan_link_probe = 1;" >> "$BSRC"
SATRIA_WS_PLAN=1 SATRIA_WS_PAR=1 run; check "rebuild paralel dengan scheduler plan berhasil" $?
verified; check "jalur fork: rencana terverifikasi tanpa menyimpang" $?
[ "$(cksum < bin/bade)" != "$BSUM" ]; check "batch binary menulis ulang binary setelah compile" $?
[ -z "$(ls .satria/ws-plan-* 2>/dev/null)" ]; check "jalur fork: penanda dibersihkan" $?
stage_clean; check "jalur fork: stage router dibersihkan setelah merge" $?

# Fase 1: tanpa workspace gate, scheduler tetap membawa verdict fast-state dan
# executor melewati validasi ulang saat semua proyek up-to-date.
SATRIA_CACHE_NOOP=0 SATRIA_WS_PLAN=1 run; check "run tanpa perubahan dengan rencana fast-state berhasil" $?
verified; check "rencana fast-state terverifikasi tanpa menyimpang" $?
! grep -q "ws-plan:" "$W/out.txt"; check "no-op plan senyap tanpa peringatan" $?
stage_clean; check "fast-state: stage router dibersihkan" $?

# Tanpa env: pilot tidak boleh aktif (nol overhead & nol output).
wait_tick; echo "/* q */" >> "$BSRC"
run; check "rebuild tanpa scheduler plan berhasil" $?
! grep -q "ws-plan" "$W/out.txt"; check "tanpa env: tidak ada output ws-plan" $?

# Single-project: transaksi stage yang sama dipakai tanpa workspace.
S="$UNIT_TMP/ws_plan-single"; rm -rf "$S"; mkdir -p "$S/src" || exit 1
printf 'use project\n\nsources = src\n' > "$S/Buildfile"
printf 'int main(void){return 0;}\n' > "$S/src/main.c"
cd "$S" || exit 2
srun() { "$SAT" -j${UNIT_JOBS:-2} >"$S/single.txt" 2>&1; return $?; }
wait_tick
SATRIA_WS_PLAN=1 srun; check "single: build dengan transaksi router berhasil" $?
stage_clean; check "single: stage router dibersihkan setelah merge" $?
grep -q "fingerprint" .satria/opts.cache; check "single: intent route diterapkan induk" $?
SATRIA_WS_PLAN=1 srun; check "single: no-op dengan transaksi router berhasil" $?
grep -q "Nothing to build" "$S/single.txt"; check "single: no-op memakai route hasil merge" $?
stage_clean; check "single: no-op tidak meninggalkan stage" $?
wait_tick; printf 'int main(void){return 1;}\n' > "$S/src/main.c"
SATRIA_WS_PLAN=1 srun; check "single: rebuild setelah perubahan berhasil" $?
grep -q "1 compiled" "$S/single.txt"; check "single: rebuild hanya mengompilasi source yang berubah" $?
stage_clean; check "single: rebuild menggabungkan stage dan membersihkannya" $?

cd "$W" || exit 2
echo "---"; echo "lulus: $pass  gagal: $fail  (folder uji: $W | $S)"
[ "$fail" -eq 0 ]

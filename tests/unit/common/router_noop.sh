#!/bin/sh
. "$(dirname "$0")/../lib/layout.sh"
# Uji router opts.cache untuk jalur cache no-op (tanpa gerbang workspace.state).
# Pemakaian: sh tests/unit/common/router_noop.sh <path-ke-satria> [path-ke-miniws]
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
SRC=$(abspath "${2:-$(dirname "$0")/../../miniws}")
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/router_noop"; rm -rf "${W}"; mkdir -p "${W}" || exit 1
cp -r "$SRC/." "$W/" || exit 2
# Cache butuh file .d (-MMD) untuk dependensi header; tanpa itu cache tidak ditulis.
printf "flags = MMD, MP\n" >> "$W/Buildfile.ws"
cd "$W" || exit 2
export NO_COLOR=1
wait_tick # hindari racy terhadap build pertama
pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }
run() { "$SAT" -j${UNIT_JOBS:-2} -w >"$W/out.txt" 2>&1; return $?; }
valid() { grep -q "Nothing to build" "$W/out.txt" && grep -q "Cache valid *:" "$W/out.txt"; }

run; check "cold build berhasil" $?
grep -q " fingerprint$" .satria/opts.cache; check "router tercatat di opts.cache setelah build" $?
run; valid; check "no-op diambil saat router ada" $?

# Gerbang legacy dihapus: no-op harus tetap diambil (router + cache, bukan workspace.state)
rm -f .satria/workspace.state; run; valid; check "no-op tanpa workspace.state tetap diambil" $?
run; check "build berhasil tanpa workspace.state" $?

# Route hilang -> no-op tidak diambil, lalu router dipulihkan
grep -v " fingerprint$" .satria/opts.cache > .satria/opts.tmp && mv .satria/opts.tmp .satria/opts.cache
run; ! valid; check "route hilang: no-op tidak diambil" $?
grep -q " fingerprint$" .satria/opts.cache; check "route dipulihkan setelah build" $?
run; valid; check "setelah route pulih: no-op lagi" $?

# opts.cache rusak -> jalur normal, tidak crash
printf 'garbage\n' > .satria/opts.cache
run; ! valid; check "opts.cache rusak: no-op tidak diambil" $?
run; check "build berhasil dengan opts.cache rusak" $?
run; valid; check "setelah opts.cache pulih: no-op lagi" $?

# Edit sumber tetap terdeteksi
BSRC=$(unit_project_source "$W" bade)
[ -n "$BSRC" ] || { echo "FAIL bade: tidak ada source untuk proyek bade (layout)" >&2; exit 1; }
wait_tick; echo "/* e */" >> "$BSRC"; run; ! valid; check "edit sumber: tidak no-op" $?

echo "---"; echo "lulus: $pass  gagal: $fail  (folder uji: $W)"
[ "$fail" -eq 0 ]

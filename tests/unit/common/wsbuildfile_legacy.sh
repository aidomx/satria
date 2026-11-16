#!/bin/sh
# Nama file workspace: WsBuildfile resmi; Buildfile.ws (dan Buildfile.workspace)
# masih diterima dengan peringatan deprecasi. Dipakai: <satria> [fixture].
# Pemakaian: sh tests/unit/common/wsbuildfile_legacy.sh <path-ke-satria> [path-ke-miniws]
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
SRC=$(abspath "${2:-$(dirname "$0")/../../miniws}")
[ -d "$SRC" ] || { echo "fixture tidak ditemukan: $SRC"; exit 2; }
export NO_COLOR=1

pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"

W="$UNIT_TMP/wsbuildfile"; rm -rf "$W"; mkdir -p "$W" || exit 1
cp -r "$SRC/." "$W/" || exit 2
cd "$W" || exit 2

# 1. Nama resmi: init -w menulis WsBuildfile, bukan Buildfile.ws.
I="$UNIT_TMP/wsbuildfile-init"; rm -rf "$I"; mkdir -p "$I" || exit 1
(cd "$I" && "$SAT" init -w > init.txt 2>&1)
check "init -w sukses" $?
[ -f "$I/WsBuildfile" ]; check "init -w menulis WsBuildfile" $?
[ ! -e "$I/Buildfile.ws" ]; check "init -w tidak menulis Buildfile.ws" $?

# 2. Nama lama masih dibaca, dengan peringatan deprecasi.
mv WsBuildfile Buildfile.ws || exit 2
"$SAT" -j${UNIT_JOBS:-2} -w > legacy.txt 2>&1
check "Buildfile.ws masih membangun workspace" $?
grep -q "deprecated" legacy.txt; check "Buildfile.ws memicu peringatan deprecasi" $?
grep -q "WsBuildfile" legacy.txt; check "peringatan menyarankan WsBuildfile" $?

# 3. Lebih dari satu kandidat: WsBuildfile menang dan nama lama tidak dipakai,
#    sehingga tidak ada peringatan deprecasi.
cp Buildfile.ws WsBuildfile || exit 2
"$SAT" -j${UNIT_JOBS:-2} -w > both.txt 2>&1
check "WsBuildfile + Buildfile.ws tetap sukses" $?
! grep -q "deprecated" both.txt; check "WsBuildfile menang tanpa peringatan" $?

# 4. init -w tidak boleh membuat WsBuildfile di samping Buildfile.ws lama.
L="$UNIT_TMP/wsbuildfile-init-legacy"; rm -rf "$L"; mkdir -p "$L" || exit 1
printf 'use workspace\nprojects = app\n' > "$L/Buildfile.ws"
(cd "$L" && "$SAT" init -w > init.txt 2>&1)
[ $? -ne 0 ]; check "init -w menolak bila nama lama masih ada" $?
grep -q "rename" "$L/init.txt"; check "init -w menyarankan rename" $?
[ ! -e "$L/WsBuildfile" ]; check "init -w tidak menimpa/menyembunyikan workspace lama" $?

echo "---"
echo "lulus: $pass  gagal: $fail  (folder uji: $W | $I | $L)"
[ "$fail" -eq 0 ]

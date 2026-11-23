#!/bin/sh
# release.files multi-baris: nilai daftar boleh dilanjutkan dengan enter+tab
# untuk kerapian. Baris lanjutan harus menjadi entri (bukan dibuang), dan
# spasi/tab/koma pemisah tidak boleh ikut menjadi nama di dist.
# Pemakaian: sh tests/unit/common/release_files_multiline.sh <satria>
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
export NO_COLOR=1

pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"

# ---- A. Buildfile proyek tunggal: daftar berlanjut (binary + aset) --------
P="$UNIT_TMP/release_files_multiline-p"; rm -rf "$P"; mkdir -p "$P/src" || exit 1
printf '#include <stdio.h>\nint main(void){puts("demo");return 0;}\n' > "$P/src/main.c"
printf 'catatan rilis\n' > "$P/notes.txt"
cat > "$P/Buildfile" <<'BF'
use project

sources = src
output.binaryName = demo

release.name = demo
release.version = 1.0.0
release.output = dist/demo-v{version}.tar.gz
release.format = tar
release.compress = gzip
# Baris lanjutan (spasi + trailing comma) harus ikut menjadi entri.
release.files = bin/demo:bin/demo,
                notes.txt:share/demo/notes.txt
BF
cd "$P" || exit 2
"$SAT" -j${UNIT_JOBS:-2} > build.txt 2>&1; check "single: build+release sukses" $?
[ -s dist/demo-v1.0.0.tar.gz ]; check "single: tarball terbentuk" $?
entries=$(tar tzf dist/demo-v1.0.0.tar.gz 2>/dev/null)
printf '%s\n' "$entries" | grep -qx "bin/demo"; check "single: entri baris pertama masuk" $?
printf '%s\n' "$entries" | grep -qx "share/demo/notes.txt"; check "single: entri baris lanjutan masuk" $?
[ "$(printf '%s\n' "$entries" | grep -c .)" -eq 2 ]; check "single: hanya entri terdaftar (2 berkas)" $?
! printf '%s\n' "$entries" | grep -q '[[:space:]]'; check "single: tidak ada nama berisi spasi/tab" $?

# ---- B. WsBuildfile: daftar berlanjut, hanya entri terdaftar --------------
W="$UNIT_TMP/release_files_multiline-w"; rm -rf "$W"; mkdir -p "$W" || exit 1
for n in a c d; do
  mkdir -p "$W/$n/src"
  printf '#include <stdio.h>\nint main(void){puts("%s");return 0;}\n' "$n" > "$W/$n/src/main.c"
done
cat > "$W/WsBuildfile" <<'WS'
use workspace
projects = a, c, d

release.name = demo
release.version = 1.0.0
release.output = dist/demo-v{version}.tar.gz
release.format = tar
release.compress = gzip
release.files =
	bin/a:bin/a
WS
cd "$W" || exit 2
"$SAT" -j${UNIT_JOBS:-2} -w > build.txt 2>&1; check "ws: build+release sukses" $?
[ -s dist/demo-v1.0.0.tar.gz ]; check "ws: tarball terbentuk" $?
entries=$(tar tzf dist/demo-v1.0.0.tar.gz 2>/dev/null)
printf '%s\n' "$entries" | grep -qx "bin/a"; check "ws: entri terdaftar masuk" $?
! printf '%s\n' "$entries" | grep -q "bin/c\|bin/d"
check "ws: proyek tak terdaftar tidak ikut (tanpa fallback default)" $?
[ "$(printf '%s\n' "$entries" | grep -c .)" -eq 1 ]; check "ws: hanya 1 berkas di dist" $?
! printf '%s\n' "$entries" | grep -q '[[:space:]]'; check "ws: tidak ada nama berisi spasi/tab" $?

echo "---"
echo "lulus: $pass  gagal: $fail  (folder uji: $P | $W)"
[ "$fail" -eq 0 ]

#!/bin/sh
# Usage: sh tests/unit/embed_stamp_skip.sh /path/to/satria
# Codegen embedded dilewati bila arsip tidak berubah (stamp), tapi perubahan
# isi arsip tetap memicu regenerasi embedded.c.
set -eu
SAT="$1"
case "$SAT" in /*) ;; *) SAT="$(pwd)/$SAT" ;; esac
SRC_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$(mktemp -d /tmp/satria-embstamp-XXXXXX)
trap 'rm -rf "$W"' EXIT HUP INT TERM
cp -a "$SRC_ROOT/tests/miniws/." "$W/"
cd "$W"
rm -rf build bin lib modules .satria
"$SAT" -w clean >/dev/null 2>&1 || true
"$SAT" -w > /dev/null 2>&1
pass=0; fail=0
check() {
  if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "PASS: $1"; else fail=$((fail+1)); echo "FAIL: $1 (dapat '$2', harap '$3')"; fi
}
EMB=build/bade/embedded.c
cp "$EMB" "$W/emb.orig"
check "stamp terbentuk setelah codegen" "$( [ -f build/bade/embedded.stamp ] && echo 1 || echo 0 )" "1"
# 1) arsip tidak berubah: codegen dilewati, embedded.c tidak ditulis ulang
m0=$(stat -c %Y "$EMB" 2>/dev/null || stat -f %m "$EMB")
sleep 1
"$SAT" -w > /dev/null 2>&1 || true
m1=$(stat -c %Y "$EMB" 2>/dev/null || stat -f %m "$EMB")
check "arsip tak berubah: embedded.c tidak ditulis ulang" "$( [ "$m0" = "$m1" ] && echo 1 || echo 0 )" "1"
# 2) isi arsip berubah (ukuran sama): embedded.c harus berubah
cp modules/rupa_modules.tar "$W/mod.orig"
size=$(wc -c < modules/rupa_modules.tar | tr -d ' ')
head -c "$size" /dev/urandom > modules/rupa_modules.tar
"$SAT" -w > /dev/null 2>&1 || true
check "isi arsip berubah: embedded.c diregenerasi" "$(cmp -s "$EMB" "$W/emb.orig" && echo 0 || echo 1)" "1"
# 3) arsip dikembalikan: embedded.c kembali identik dengan awal
cp "$W/mod.orig" modules/rupa_modules.tar
"$SAT" -w > /dev/null 2>&1 || true
check "arsip dipulihkan: embedded.c kembali identik" "$(cmp -s "$EMB" "$W/emb.orig" && echo 1 || echo 0)" "1"
echo "lulus: $pass  gagal: $fail"
[ "$fail" -eq 0 ]

#!/bin/sh
# Usage: sh tests/unit/trace_summary.sh /path/to/satria
# `satria trace` harus setara pipeline manual:
#   LD_PRELOAD=<tracer> satria ... ; wc -l log ; sort log | uniq -c | sort -rn | head
# Total akses dan hitungan teratas harus cocok dengan pipeline itu.
set -eu
SAT="$1"
case "$SAT" in /*) ;; *) SAT="$(pwd)/$SAT" ;; esac
SRC_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$(mktemp -d /tmp/satria-trace-XXXXXX)
trap 'rm -rf "$W"' EXIT HUP INT TERM
cp -a "$SRC_ROOT/tests/miniws/." "$W/"
cd "$W"
rm -rf build bin lib modules .satria
"$SAT" -w clean >/dev/null 2>&1 || true
"$SAT" -w >/dev/null 2>&1
pass=0; fail=0
check() {
  if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "PASS: $1"; else fail=$((fail+1)); echo "FAIL: $1 (dapat '$2', harap '$3')"; fi
}
# 1) satria trace berjalan, rc child diteruskan (0 pada no-op)
"$SAT" trace --out "$W/t.log" -- -w core > "$W/trace.txt" 2>&1
check "satria trace rc 0 (no-op)" "$?" "0"
check "ringkasan tercetak" "$(grep -c '^> trace   : [0-9]* akses' "$W/trace.txt" || true)" "1"
check "log terbentuk" "$( [ -s "$W/t.log" ] && echo 1 || echo 0 )" "1"
# 2) setara pipeline manual pada log yang sama
total_log=$(wc -l < "$W/t.log" | tr -d ' ')
total_sat=$(grep '^> trace   : [0-9]* akses' "$W/trace.txt" | sed 's/^> trace   : \([0-9]*\) akses.*/\1/')
check "total akses = wc -l log" "$total_sat" "$total_log"
top_manual=$(sort "$W/t.log" | uniq -c | sort -rn | head -1 | awk '{print $1}')
top_sat=$(grep -A1 '^  top ' "$W/trace.txt" | tail -1 | awk '{print $1}')
check "hitungan teratas = sort|uniq -c" "$top_sat" "$top_manual"
# 3) tracer disimpan di .satria/trace (dibangun otomatis bila belum ada)
check "tracer ada di .satria/trace" "$( [ -f .satria/trace/openat_trace.so ] && echo 1 || echo 0 )" "1"
echo "lulus: $pass  gagal: $fail"
[ "$fail" -eq 0 ]

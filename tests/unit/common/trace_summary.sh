#!/bin/sh
# Usage: sh tests/unit/common/trace_summary.sh /path/to/satria
# `satria trace` harus setara pipeline manual pada log gaya-strace:
#   LD_PRELOAD=<tracer> satria ... ; wc -l log ;
#   ekstrak key op+path dari tiap baris, lalu sort | uniq -c | sort -rn | head
# Total akses dan hitungan teratas harus cocok dengan pipeline itu.
set -eu
SAT="$1"
case "$SAT" in /*) ;; *) SAT="$(pwd)/$SAT" ;; esac
SRC_ROOT="${UNIT_REPO:?UNIT_REPO tidak di-set: jalankan lewat scripts/unit_v1.sh}"
SRC="${2:-$SRC_ROOT/tests/miniws}"
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/trace_summary"; rm -rf "${W}"; mkdir -p "${W}" || exit 1
trap 'rm -rf "$W"' EXIT HUP INT TERM
cp -a "$SRC/." "$W/"
cd "$W"
rm -rf build bin lib modules .satria
"$SAT" -j${UNIT_JOBS:-2} -w clean >/dev/null 2>&1 || true
"$SAT" -j${UNIT_JOBS:-2} -w >/dev/null 2>&1
pass=0; fail=0
check() {
  if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "PASS: $1"; else fail=$((fail+1)); echo "FAIL: $1 (dapat '$2', harap '$3')"; fi
}
# 1) satria trace berjalan, rc child diteruskan (0 pada no-op)
"$SAT" -j${UNIT_JOBS:-2} trace --out "$W/t.log" -- -w core > "$W/trace.txt" 2>&1
check "satria trace rc 0 (no-op)" "$?" "0"
check "ringkasan tercetak" "$(grep -c '^> trace   : [0-9]* akses' "$W/trace.txt" || true)" "1"
check "log terbentuk" "$( [ -s "$W/t.log" ] && echo 1 || echo 0 )" "1"
# 2) format gaya-strace: line diproteksi <op>(...) = <hasil>
check "baris gaya-strace" "$(grep -cE '^[a-z_0-9]+\(.+\) = ' "$W/t.log" || true)" "$(wc -l < "$W/t.log" | tr -d ' ')"
# 3) setara pipeline manual pada log yang sama (key = op + path)
total_log=$(wc -l < "$W/t.log" | tr -d ' ')
total_sat=$(grep '^> trace   : [0-9]* akses' "$W/trace.txt" | sed 's/^> trace   : \([0-9]*\) akses.*/\1/')
check "total akses = wc -l log" "$total_sat" "$total_log"
top_manual=$(awk '
{
  i = index($0, "("); if (i <= 1) next
  op = substr($0, 1, i - 1)
  rest = substr($0, i + 1)
  q = index(rest, "\"")
  p = "-"
  if (q > 0) {
    rest2 = substr(rest, q + 1)
    r = index(rest2, "\"")
    if (r > 1) p = substr(rest2, 1, r - 1)
  }
  print op "\t" p
}' "$W/t.log" | sort | uniq -c | sort -rn | head -1 | awk '{print $1}')
top_sat=$(grep -A1 '^  top ' "$W/trace.txt" | tail -1 | awk '{print $1}')
check "hitungan teratas = key op+path sort|uniq -c" "$top_sat" "$top_manual"
# 4) --raw memutar isi log setelah ringkasan
"$SAT" -j${UNIT_JOBS:-2} trace --raw --out "$W/t2.log" -- -w core > "$W/trace2.txt" 2>&1
raw_lines=$(awk '/--- isi log/{f=1; next} f { if ($0 ~ /^[a-z_0-9]+\(/) c++ } END { print c+0 }' "$W/trace2.txt")
check "raw: stream setara isi log" "$raw_lines" "$(wc -l < "$W/t2.log" | tr -d ' ')"
# 5) tracer disimpan di .satria/trace (dibangun otomatis bila belum ada)
check "tracer ada di .satria/trace" "$( [ -f .satria/trace/openat_trace.so ] && echo 1 || echo 0 )" "1"
echo "lulus: $pass  gagal: $fail"
[ "$fail" -eq 0 ]

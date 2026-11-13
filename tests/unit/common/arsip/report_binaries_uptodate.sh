#!/bin/sh
# Usage: sh tests/unit/common/report_binaries_uptodate.sh /path/to/satria
# Binary yang sudah up-to-date harus tercatat di ringkasan (bukan 0 linked, 0 up-to-date):
#   build dingin -> "Binaries : 1 linked, 0 up-to-date"
#   run no-op    -> "Binaries : 0 linked, 1 up-to-date"
set -eu
SAT="$1"
case "$SAT" in /*) ;; *) SAT="$(pwd)/$SAT" ;; esac
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/report_binaries_uptodate"; rm -rf "${W}"; mkdir -p "${W}" || exit 1
trap 'rm -rf "$W"' EXIT HUP INT TERM
mkdir -p "$W/src"
cd "$W"
printf 'use project\n' > Buildfile
printf 'int main(void){return 0;}\n' > src/main.c
pass=0; fail=0
check() {
  if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "PASS: $1"; else fail=$((fail+1)); echo "FAIL: $1 (dapat '$2', harap '$3')"; fi
}
# 1) build dingin: binary di-link di run ini
"$SAT" -j${UNIT_JOBS:-2} > "$W/cold.txt" 2>&1
"$SAT" -j${UNIT_JOBS:-2} > "$W/noop1.txt" 2>&1
check "setelah build dingin: 1 linked, 0 up-to-date" "$(grep -c 'Binaries   : 1 linked, 0 up-to-date' "$W/noop1.txt" || true)" "1"
[ "$(grep -c 'Binaries   : 1 linked, 0 up-to-date' "$W/noop1.txt" || true)" = "1" ] || {
  echo "--- isi noop1.txt (diagnostik) ---"; tail -n 25 "$W/noop1.txt"; echo "--- isi cold.txt (diagnostik) ---"; tail -n 25 "$W/cold.txt"
}
# 2) source dikompilasi ulang tetapi object identik -> link dilewati (up-to-date)
printf '/* komentar */\n' >> src/main.c
"$SAT" -j${UNIT_JOBS:-2} > "$W/touch.txt" 2>&1
check "build komentar: link up-to-date" "$(grep -c '(up-to-date)' "$W/touch.txt" || true)" "1"
[ "$(grep -c '(up-to-date)' "$W/touch.txt" || true)" = "1" ] || {
  echo "--- isi touch.txt (diagnostik) ---"; cat "$W/touch.txt"
  echo "--- isi cold.txt (diagnostik) ---"; cat "$W/cold.txt"
}
"$SAT" -j${UNIT_JOBS:-2} > "$W/noop2.txt" 2>&1
check "run no-op setelah up-to-date: 0 linked, 1 up-to-date" "$(grep -c 'Binaries   : 0 linked, 1 up-to-date' "$W/noop2.txt" || true)" "1"
[ "$(grep -c 'Binaries   : 0 linked, 1 up-to-date' "$W/noop2.txt" || true)" = "1" ] || {
  echo "--- isi noop2.txt (diagnostik) ---"; cat "$W/noop2.txt"
}
echo "lulus: $pass  gagal: $fail"
[ "$fail" -eq 0 ]

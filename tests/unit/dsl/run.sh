#!/bin/bash
# Konforman evaluator ekspresi M1: tiap expr_*.test membawa komentar
#   # expect: lulus=<n> gagal=<m> rc=<r> pre=<0|1>
# pre=1 → output memuat baris PRECONDITION dan rc 0/3 diterima.
set -u
cd "$(dirname "$0")"
ROOT=$(cd ../../.. && pwd)
export SATRIA=${SATRIA:-$ROOT/bin/satria}
export UNIT_TMP=${UNIT_TMP:-/tmp/satria-dsl-kit}
export UNIT_REPO=$ROOT

N=0; G=0
for f in *.test; do
  [ -e "$f" ] || { echo "FAIL dsl-kit: tidak ada *.test"; exit 1; }
  N=$((N + 1))
  exp=$(grep -m1 '^# expect:' "$f"); exp=${exp#\# expect: }
  out=$(mktemp /tmp/opencode/kit-XXXXXX)
  bash "$ROOT/tests/unit/lib/dsl.sh" "$f" > "$out" 2>&1
  rc=$?
  gl=$(awk '/^lulus:/{print $2}' "$out"); gl=${gl:-0}
  gg=$(awk '/^lulus:/{print $4}' "$out"); gg=${gg:-0}
  wl=$(sed -n 's/.*lulus=\([0-9]*\).*/\1/p' <<<"$exp")
  wg=$(sed -n 's/.*gagal=\([0-9]*\).*/\1/p' <<<"$exp")
  wrc=$(sed -n 's/.*rc=\([0-9]*\).*/\1/p' <<<"$exp")
  wpre=$(sed -n 's/.*pre=\([0-9]*\).*/\1/p' <<<"$exp")
  err=''
  [ "${wl:-0}" = "$gl" ] || err+=" lulus=$gl(harap $wl)"
  [ "${wg:-0}" = "$gg" ] || err+=" gagal=$gg(harap $wg)"
  if [ "${wpre:-0}" = 1 ]; then
    grep -q '^PRECONDITION' "$out" || err+=" PRECONDITION hilang"
    { [ "$rc" -eq 0 ] || [ "$rc" -eq 3 ]; } || err+=" rc=$rc(harap 0/3)"
  elif [ -n "$wrc" ] && [ "$rc" != "$wrc" ]; then
    err+=" rc=$rc(harap $wrc)"
  fi
  if [ -z "$err" ]; then
    echo "PASS  $f"
  else
    echo "FAIL  $f :$err"
    G=$((G + 1))
    [ -n "${VERBOSE:-}" ] && sed 's/^/    /' "$out"
  fi
  rm -f "$out"
done
echo "Ringkasan dsl-kit: lulus: $((N - G))  gagal: $G"
[ "$G" -eq 0 ]

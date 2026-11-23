#!/bin/sh
# Paritas eksekutor DSL C vs eksekutor bash (M4b, design/unit_test_dsl.md).
# Tiap tests/unit/dsl/*.test dijalankan dua kali — `tests/unit/lib/dsl.sh`
# (acuan bash) dan `satria test <file>` — lalu dibandingkan: jumlah lulus/gagal,
# baris verdict PASS/FAIL, indikator PRECONDITION, serta rc (pre=1 menerima 0/3).
# Pemakaian: bash tests/unit/dsl/parity.sh
# Prasyarat: bin/satria sudah dibangun.
set -u
cd "$(dirname "$0")" || exit 2
ROOT=$(cd ../../.. && pwd)
BIN=${SATRIA_BIN:-$ROOT/bin/satria}
if [ ! -x "$BIN" ]; then
  printf 'parity: %s belum dibangun (jalankan ./bin/satria build)\n' "$BIN" >&2
  exit 2
fi
TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); printf 'PASS  %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n' "$1"; }

for f in *.test; do
  [ -e "$f" ] || { bad "tidak ada *.test"; continue; }
  exp=$(grep -m1 '^# expect:' "$f")
  exp=${exp#\# expect: }
  wl=$(printf '%s\n' "$exp" | sed -n 's/.*lulus=\([0-9]*\).*/\1/p')
  wg=$(printf '%s\n' "$exp" | sed -n 's/.*gagal=\([0-9]*\).*/\1/p')
  wra=$(printf '%s\n' "$exp" | sed -n 's/.*rc=\([0-9]*\).*/\1/p')
  wpre=$(printf '%s\n' "$exp" | sed -n 's/.*pre=\([0-9]*\).*/\1/p')
  : "${wl:=0}" "${wg:=0}" "${wra:=}" "${wpre:=0}"

  rm -rf "$TMP/b" "$TMP/c"
  mkdir -p "$TMP/b" "$TMP/c"

  ( UNIT_TMP="$TMP/b" UNIT_REPO="$ROOT" SATRIA="$BIN" \
      bash "$ROOT/tests/unit/lib/dsl.sh" "$f" ) >"$TMP/b.out" 2>&1
  brc=$?
  ( UNIT_TMP="$TMP/c" UNIT_REPO="$ROOT" SATRIA="$BIN" \
      "$BIN" test "$f" ) >"$TMP/c.out" 2>&1
  crc=$?

  bl=$(awk '/^lulus:/{print $2}' "$TMP/b.out"); bl=${bl:-0}
  bg=$(awk '/^lulus:/{print $4}' "$TMP/b.out"); bg=${bg:-0}
  cl=$(awk '/^lulus:/{print $2}' "$TMP/c.out"); cl=${cl:-0}
  cg=$(awk '/^lulus:/{print $4}' "$TMP/c.out"); cg=${cg:-0}
  bp=$(grep -c '^PRECONDITION' "$TMP/b.out"); bp=${bp:-0}
  cp=$(grep -c '^PRECONDITION' "$TMP/c.out"); cp=${cp:-0}

  err=''
  [ "$bl" = "$wl" ] || err="$err bash-lulus=$bl(harap $wl)"
  [ "$bg" = "$wg" ] || err="$err bash-gagal=$bg(harap $wg)"
  [ "$cl" = "$wl" ] || err="$err c-lulus=$cl(harap $wl)"
  [ "$cg" = "$wg" ] || err="$err c-gagal=$cg(harap $wg)"
  if [ "$wpre" = 1 ]; then
    [ "$bp" -ge 1 ] || err="$err bash-PRECONDITION hilang"
    [ "$cp" -ge 1 ] || err="$err c-PRECONDITION hilang"
    { [ "$brc" -eq 0 ] || [ "$brc" -eq 3 ]; } || err="$err bash-rc=$brc(harap 0/3)"
    { [ "$crc" -eq 0 ] || [ "$crc" -eq 3 ]; } || err="$err c-rc=$crc(harap 0/3)"
  else
    [ -z "$wra" ] || [ "$brc" = "$wra" ] || err="$err bash-rc=$brc(harap $wra)"
    [ -z "$wra" ] || [ "$crc" = "$wra" ] || err="$err c-rc=$crc(harap $wra)"
  fi

  grep -E '^(PASS|FAIL):' "$TMP/b.out" | LC_ALL=C sort >"$TMP/b.verdict"
  grep -E '^(PASS|FAIL):' "$TMP/c.out" | LC_ALL=C sort >"$TMP/c.verdict"
  cmp -s "$TMP/b.verdict" "$TMP/c.verdict" || err="$err verdict beda"

  if [ -z "$err" ]; then
    ok "$f"
  else
    bad "$f :$err"
    diff -u "$TMP/b.verdict" "$TMP/c.verdict" | head -n 12 | sed 's/^/      /'
  fi
done

printf 'Ringkasan dsl-parity: lulus=%d gagal=%d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

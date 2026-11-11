#!/bin/sh
# Uji konforman scripts/lib/parser.sh — berdiri sendiri, TERISOLASI dari
# suite unit (tests/unit/parser tidak dibaca unit_dev.sh; runner hanya
# memindai tests/unit/common/*.sh + classes.conf).
# Pemakaian: bash tests/unit/parser/run.sh
set -u
cd "$(dirname "$0")/../../.." || exit 2
LIB=scripts/lib/parser.sh
TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'PASS  %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"; }

# --- positif: wajib parse bersih --------------------------------------------
for f in tests/unit/parser/*.test; do
  [ -e "$f" ] || continue
  if bash "$LIB" --check "$f" 2>"$TMP/err"; then
    ok "$(basename "$f")"
  else
    bad "$(basename "$f")"; sed 's/^/      /' "$TMP/err"
  fi
done

# --- negatif: wajib gagal dengan pesan yang diharapkan (# expect:) -----------
for f in tests/unit/parser/negative/*.test; do
  [ -e "$f" ] || continue
  name=$(basename "$f")
  exp=$(sed -n 's/^# expect:[[:space:]]*//p' "$f" | head -n 1)
  if bash "$LIB" --check "$f" 2>"$TMP/err"; then
    bad "$name (seharusnya gagal parse)"
  elif [ -n "$exp" ] && grep -qF -- "$exp" "$TMP/err"; then
    ok "$name"
  else
    bad "$name (pesan tak cocok; harap: '$exp')"
    sed 's/^/      /' "$TMP/err"
  fi
done

printf 'Ringkasan parser: lulus: %d gagal: %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

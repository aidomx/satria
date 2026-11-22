#!/bin/sh
# Paritas parser C vs parser bash (M4a, design/unit_test_dsl.md).
# Membandingkan stdout, stderr, dan exit code `satria test --parse` dengan
# `scripts/lib/parser.sh` untuk seluruh berkas DSL di repo + mode --check,
# --suite, dan berkas yang tidak ada.
# Pemakaian: bash tests/unit/parser/parity.sh
# Prasyarat: bin/satria sudah dibangun (mis. ./bin/satria build).
set -u
cd "$(dirname "$0")/../../.." || exit 2
BIN=${SATRIA_BIN:-bin/satria}
LIB=scripts/lib/parser.sh
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

# cmp_run <label> <arg...> — jalankan parser bash lalu parser C dengan argumen
# yang sama (C memakai awalan `test --parse`), bandingkan ketiga kanal.
cmp_run() {
  label=$1
  shift
  bash "$LIB" "$@" >"$TMP/b.out" 2>"$TMP/b.err"
  brc=$?
  "$BIN" test --parse "$@" >"$TMP/c.out" 2>"$TMP/c.err"
  crc=$?
  if [ "$brc" -ne "$crc" ]; then
    bad "$label (rc bash=$brc c=$crc)"
    return
  fi
  if ! cmp -s "$TMP/b.out" "$TMP/c.out"; then
    bad "$label (stdout beda)"
    diff -u "$TMP/b.out" "$TMP/c.out" | head -n 20 | sed 's/^/      /'
    return
  fi
  if ! cmp -s "$TMP/b.err" "$TMP/c.err"; then
    bad "$label (stderr beda)"
    diff -u "$TMP/b.err" "$TMP/c.err" | head -n 20 | sed 's/^/      /'
    return
  fi
  ok "$label"
}

for f in \
  tests/unit/parser/*.test \
  tests/unit/parser/negative/*.test \
  tests/unit/common/*.test \
  tests/unit/common/*.t \
  tests/unit/dsl/*.test \
  tests/unit/fragmen/*.test; do
  [ -e "$f" ] || continue
  cmp_run "trace $f" "$f"
  cmp_run "check $f" --check "$f"
done

cmp_run "suite parser" --suite tests/unit/parser
cmp_run "suite dsl" --suite tests/unit/dsl
cmp_run "suite tidak ada" --suite tests/unit/tidak-ada
cmp_run "berkas tidak ada" tests/unit/parser/tidak-ada.test
cmp_run "check berkas tidak ada" --check tests/unit/parser/tidak-ada.test

printf 'Ringkasan paritas: lulus: %d gagal: %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

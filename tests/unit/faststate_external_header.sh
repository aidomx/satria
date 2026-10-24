#!/bin/sh
# Regression: single-project fast-state must include actual header dependencies
# outside sources/includeDirs, e.g. #include "../shared.h".
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
export NO_COLOR=1
W=$(mktemp -d /tmp/faststate-header-XXXX) || exit 2
pass=0
fail=0
check() {
  if [ "$2" -eq 0 ]; then pass=$((pass + 1)); echo "PASS  $1";
  else fail=$((fail + 1)); echo "FAIL  $1"; fi
}
cleanup() { rm -rf "$W"; }
trap cleanup EXIT HUP INT TERM
mkdir -p "$W/src" "$W/include"
cat > "$W/Buildfile" <<'BUILD'
use project
sources = src
headers = include
compiler = gcc
flags = Wall, Wextra, MMD, MP
std = gnu11
BUILD
cat > "$W/shared.h" <<'HEADER'
#define EXTERNAL_VALUE 1
HEADER
cat > "$W/src/main.c" <<'SOURCE'
#include "../shared.h"
int main(void) { return EXTERNAL_VALUE == 1 ? 0 : 1; }
SOURCE
cd "$W" || exit 2
"$SAT" > cold.log 2>&1
check "cold build compiles source" "$(grep -q 'src/main.c' cold.log; echo $?)"
"$SAT" > noop.log 2>&1
check "steady state is no-op" "$(grep -q 'Nothing to build' noop.log; echo $?)"
# Same-length content edit: must invalidate fast-state and recompile consumer.
cat > shared.h <<'HEADER'
#define EXTERNAL_VALUE 2
HEADER
"$SAT" > edit.log 2>&1
check "external header edit rejects fast-state" "$( ! grep -q 'Nothing to build' edit.log; echo $?)"
check "external header edit recompiles consumer" "$(grep -q 'src/main.c' edit.log; echo $?)"
# Touch only: same content should be proven by hash and remain a no-op.
sleep 1
touch shared.h
"$SAT" > touch.log 2>&1
check "touch without content change stays no-op" "$(grep -q 'Nothing to build' touch.log; echo $?)"
echo "lulus: $pass  gagal: $fail"
[ "$fail" -eq 0 ]

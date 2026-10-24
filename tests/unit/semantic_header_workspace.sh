#!/bin/sh
# Semantic header invalidation for workspace builds.
# Unused prototype changes must not compile; consumed prototype changes must.
# Usage: sh tests/unit/semantic_header_workspace.sh <path-to-satria> [miniws-fixture]
set -eu
case "${1:-}" in
  /*) SAT=$1 ;;
  *) SAT="$(pwd)/${1:-bin/satria}" ;;
esac
SRC=${2:-"$(dirname "$0")/../miniws"}
[ -x "$SAT" ] || { echo "satria tidak ditemukan: $SAT" >&2; exit 2; }
TMP=$(mktemp -d /tmp/semantic_header_ws_XXXXXX)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

prepare() {
  dst=$1
  mkdir -p "$dst"
  cp -a "$SRC/." "$dst/"
  rm -rf "$dst/.satria" "$dst/build" "$dst/bin" "$dst/lib" "$dst/modules" "$dst/compile_commands.json"
}
run_build() {
  log=$1
  (cd "$2" && SATRIA_CACHE_NOOP=0 "$SAT" -j2 -w) > "$log" 2>&1 || {
    cat "$log" >&2
    return 1
  }
}

# Case 1: add a declaration that no translation unit consumes.
UNUSED="$TMP/unused"
prepare "$UNUSED"
run_build "$TMP/unused-cold.log" "$UNUSED"
printf '\nint miniws_unused_api(int value);\n' >> "$UNUSED/core/include/rupa.h"
run_build "$TMP/unused-edit.log" "$UNUSED"
if grep -Eq 'OK[[:space:]].*\.(c|cc|cpp|cxx)$' "$TMP/unused-edit.log"; then
  echo "FAIL unused header declaration triggered compilation" >&2
  cat "$TMP/unused-edit.log" >&2
  exit 1
fi
# Jumlah source yang melihat header mengikuti fixture (kedua TU core
# meng-include rupa.h -> 2), bukan angka tetap; yang diuji jalur
# semantik-terima (skipped), bukan angkanya.
grep -q 'Headers : [0-9][0-9]* source(s) skipped (touched, content unchanged)' "$TMP/unused-edit.log" || {
  echo "FAIL unused declaration was not semantically accepted" >&2
  cat "$TMP/unused-edit.log" >&2
  exit 1
}
echo "PASS unused declaration: no source compilation"

# Case 2: touch the header without changing its contents.
touch "$UNUSED/core/include/rupa.h"
run_build "$TMP/touch.log" "$UNUSED"
if grep -Eq 'OK[[:space:]].*\.(c|cc|cpp|cxx)$' "$TMP/touch.log"; then
  echo "FAIL touching unchanged header triggered compilation" >&2
  cat "$TMP/touch.log" >&2
  exit 1
fi
echo "PASS unchanged header touch: no source compilation"

# Case 3: change a declaration actually consumed by core/src/main.c.
USED="$TMP/used"
prepare "$USED"
run_build "$TMP/used-cold.log" "$USED"
sed -i 's/^extern void thisBade();$/extern void thisBade(void);/' "$USED/include/ext.h"
run_build "$TMP/used-edit.log" "$USED"
if ! grep -q 'OK .*core/src/main.c' "$TMP/used-edit.log"; then
  echo "FAIL consumed declaration did not recompile core/src/main.c" >&2
  cat "$TMP/used-edit.log" >&2
  exit 1
fi
echo "PASS consumed declaration: affected source recompiled"

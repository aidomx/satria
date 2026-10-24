#!/bin/sh
# A selective workspace target must not build projects linked through cdeps.
# Usage: sh tests/unit/workspace_selective_target.sh <path-to-satria> [miniws-fixture]
set -eu
case "${1:-}" in
  /*) SAT=$1 ;;
  *) SAT="$(pwd)/${1:-bin/satria}" ;;
esac
SRC=${2:-"$(dirname "$0")/../miniws"}
[ -x "$SAT" ] || { echo "satria tidak ditemukan: $SAT" >&2; exit 2; }
TMP=$(mktemp -d /tmp/ws_selective_target_XXXXXX)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cp -a "$SRC/." "$TMP/"
rm -rf "$TMP/.satria" "$TMP/build" "$TMP/bin" "$TMP/lib" "$TMP/modules" "$TMP/compile_commands.json"
(cd "$TMP" && "$SAT" -j2 -w) > "$TMP/cold.log" 2>&1 || {
  cat "$TMP/cold.log" >&2
  exit 1
}
printf '\n/* selected core edit */\n' >> "$TMP/core/src/core.c"
printf '\n/* unselected bade edit */\n' >> "$TMP/bade/src/bade.c"
(cd "$TMP" && "$SAT" -j2 -w core) > "$TMP/selective.log" 2>&1 || {
  cat "$TMP/selective.log" >&2
  exit 1
}
grep -q "warning: selective target 'core' has changed inputs in selected project(s) \[core\] and unselected project(s) \[bade\]\." "$TMP/selective.log" || {
  echo "FAIL: missing divergence warning for changed core and bade inputs" >&2
  cat "$TMP/selective.log" >&2
  exit 1
}
if grep -Eq '^> Project : bade$|bade/src/.*OK|^> Linking : bin/bade|^> Library : lib/libbade' "$TMP/selective.log"; then
  echo "FAIL: -w core processed unselected project bade" >&2
  cat "$TMP/selective.log" >&2
  exit 1
fi
grep -q 'OK .*core/src/core.c' "$TMP/selective.log" || {
  echo "FAIL: -w core did not rebuild changed core source" >&2
  cat "$TMP/selective.log" >&2
  exit 1
}
echo "PASS: -w core rebuilt core only; changed bade source was not processed"
#!/bin/sh
# A selective workspace target must not build projects linked through cdeps.
# Usage: sh tests/unit/workspace_selective_target.sh <path-to-satria> [miniws-fixture]
set -eu
case "${1:-}" in
  /*) SAT=$1 ;;
  *) SAT="$(pwd)/${1:-bin/satria}" ;;
esac
SRC=${2:-"$(dirname "$0")/../miniws"}
[ -x "$SAT" ] || { echo "satria tidak ditemukan: $SAT" >&2; exit 2; }
TMP=$(mktemp -d /tmp/ws_selective_target_XXXXXX)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cp -a "$SRC/." "$TMP/"
rm -rf "$TMP/.satria" "$TMP/build" "$TMP/bin" "$TMP/lib" "$TMP/modules" "$TMP/compile_commands.json"
(cd "$TMP" && "$SAT" -j2 -w) > "$TMP/cold.log" 2>&1 || {
  cat "$TMP/cold.log" >&2
  exit 1
}
printf '\n/* selected core edit */\n' >> "$TMP/core/src/core.c"
printf '\n/* unselected bade edit */\n' >> "$TMP/bade/src/bade.c"
(cd "$TMP" && "$SAT" -j2 -w core) > "$TMP/selective.log" 2>&1 || {
  cat "$TMP/selective.log" >&2
  exit 1
}
if grep -Eq '^> Project : bade$|bade/src/.*OK|^> Linking : bin/bade|^> Library : lib/libbade' "$TMP/selective.log"; then
  echo "FAIL: -w core processed unselected project bade" >&2
  cat "$TMP/selective.log" >&2
  exit 1
fi
grep -q 'OK .*core/src/core.c' "$TMP/selective.log" || {
  echo "FAIL: -w core did not rebuild changed core source" >&2
  cat "$TMP/selective.log" >&2
  exit 1
}
echo "PASS: -w core rebuilt core only; changed bade source was not processed"

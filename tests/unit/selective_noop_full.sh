#!/bin/sh
# O1 (design/build_report.md): setelah build selektif menyimpan merge fingerprint
# per proyek, no-op penuh PERTAMA harus HIT (blok STATUS "Nothing to build")
# tanpa run penuh tambahan — dan perubahan proyek LUAR closure tetap terdeteksi.
# Usage: sh tests/unit/selective_noop_full.sh <path-to-satria> [miniws-fixture]
set -eu
case "${1:-}" in
  /*) SAT=$1 ;;
  *) SAT="$(pwd)/${1:-bin/satria}" ;;
esac
SRC=${2:-"$(dirname "$0")/../miniws"}
[ -x "$SAT" ] || { echo "satria tidak ditemukan: $SAT" >&2; exit 2; }
TMP=$(mktemp -d /tmp/ws_selective_noop_XXXXXX)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cp -a "$SRC/." "$TMP/"
rm -rf "$TMP/.satria" "$TMP/build" "$TMP/bin" "$TMP/lib" "$TMP/modules" "$TMP/compile_commands.json"

# 1. Build penuh dingin.
(cd "$TMP" && "$SAT" -j2 -w) > "$TMP/cold.log" 2>&1 || { cat "$TMP/cold.log" >&2; exit 1; }

# 2. Ubah sumber proyek closure -> build selektif.
printf '\n/* core edit for O1 */\n' >> "$TMP/core/src/core.c"
(cd "$TMP" && "$SAT" -j2 -w core) > "$TMP/selective.log" 2>&1 || {
  cat "$TMP/selective.log" >&2; exit 1
}
grep -q 'OK .*core/src/core.c' "$TMP/selective.log" || {
  echo "FAIL: -w core did not rebuild changed core source" >&2
  cat "$TMP/selective.log" >&2; exit 1
}

# 3. Run penuh PERTAMA: harus HIT — bukan jalur penuh (O1).
(cd "$TMP" && "$SAT" -j2 -w) > "$TMP/full1.log" 2>&1 || { cat "$TMP/full1.log" >&2; exit 1; }
grep -q "Nothing to build" "$TMP/full1.log" || {
  echo "FAIL: full run after selective build did not hit the no-op gate (O1)" >&2
  cat "$TMP/full1.log" >&2; exit 1
}
if grep -Eq 'OK .*(core|bade)/src/' "$TMP/full1.log"; then
  echo "FAIL: full run after selective build recompiled something" >&2
  cat "$TMP/full1.log" >&2; exit 1
fi

# 4. Ubah proyek LUAR closure: run penuh berikutnya harus MISS & rebuild bade.
printf '\n/* bade edit after merge */\n' >> "$TMP/bade/src/bade.c"
(cd "$TMP" && "$SAT" -j2 -w) > "$TMP/full2.log" 2>&1 || { cat "$TMP/full2.log" >&2; exit 1; }
grep -q 'OK .*bade/src/bade.c' "$TMP/full2.log" || {
  echo "FAIL: changed unselected project not rebuilt (merge lost its fingerprint)" >&2
  cat "$TMP/full2.log" >&2; exit 1
}

# 5. Run penuh ketiga: kembali HIT.
(cd "$TMP" && "$SAT" -j2 -w) > "$TMP/full3.log" 2>&1 || { cat "$TMP/full3.log" >&2; exit 1; }
grep -q "Nothing to build" "$TMP/full3.log" || {
  echo "FAIL: run after rebuilding bade did not hit the no-op gate" >&2
  cat "$TMP/full3.log" >&2; exit 1
}

echo "PASS: selective save merges per-project fingerprints (full no-op hits directly; unselected edits still detected)"

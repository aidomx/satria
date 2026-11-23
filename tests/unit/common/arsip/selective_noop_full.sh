#!/bin/sh
# requires: cold  -- regresi fingerprint: riwayat state cold lalu build selektif (bug O1)
# O1 (design/build_report.md): setelah build selektif menyimpan merge fingerprint
# per proyek, no-op penuh PERTAMA harus HIT (blok STATUS "Nothing to build")
# tanpa run penuh tambahan — dan perubahan proyek LUAR closure tetap terdeteksi.
# Usage: sh tests/unit/common/selective_noop_full.sh <path-to-satria> [miniws-fixture]
set -eu
case "${1:-}" in
  /*) SAT=$1 ;;
  *) SAT="$(pwd)/${1:-bin/satria}" ;;
esac
SRC=${2:-"$(dirname "$0")/../../miniws"}
[ -x "$SAT" ] || { echo "satria tidak ditemukan: $SAT" >&2; exit 2; }
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
TMP="$UNIT_TMP/selective_noop_full"; rm -rf "${TMP}"; mkdir -p "${TMP}" || exit 1
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cp -a "$SRC/." "$TMP/"
rm -rf "$TMP/.satria" "$TMP/build" "$TMP/bin" "$TMP/lib" "$TMP/modules" "$TMP/compile_commands.json"
. "$(dirname "$0")/../lib/layout.sh"
CORE_SRC=$(unit_project_source "$TMP" core)
[ -n "$CORE_SRC" ] || unit_precondition "fixture tidak punya sumber .c proyek core"

# 1. Build penuh dingin.
(cd "$TMP" && "$SAT" -j2 -w) > "$TMP/cold.log" 2>&1 || { cat "$TMP/cold.log" >&2; exit 1; }

# 2. Ubah sumber proyek closure -> build selektif.
printf '\n/* core edit for O1 */\n' >> "$TMP/$CORE_SRC"
(cd "$TMP" && "$SAT" -j2 -w core) > "$TMP/selective.log" 2>&1 || {
  cat "$TMP/selective.log" >&2; exit 1
}
grep -q "OK .*$CORE_SRC" "$TMP/selective.log" || {
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
BSRC=$(unit_project_source "$TMP" bade)
[ -n "$BSRC" ] || { echo "FAIL bade: tidak ada source untuk proyek bade (layout)" >&2; exit 1; }
printf '\n/* bade edit after merge */\n' >> "$TMP/$BSRC"
(cd "$TMP" && "$SAT" -j2 -w) > "$TMP/full2.log" 2>&1 || { cat "$TMP/full2.log" >&2; exit 1; }
grep -q "OK .*$BSRC" "$TMP/full2.log" || {
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

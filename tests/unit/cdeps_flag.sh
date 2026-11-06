#!/bin/sh
# --with-cdeps (opt-in): selective closure pulls cdeps providers; without the
# flag the old contract holds (cdeps never expands the build) plus a pre-flight
# warning naming the missing provider and the fix.
# Usage: sh tests/unit/cdeps_flag.sh <path-to-satria> [miniws-fixture]
set -eu
case "${1:-}" in
  /*) SAT=$1 ;;
  *) SAT="$(pwd)/${1:-bin/satria}" ;;
esac
SRC=${2:-"$(dirname "$0")/../miniws"}
[ -x "$SAT" ] || { echo "satria tidak ditemukan: $SAT" >&2; exit 2; }
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
TMP="$UNIT_TMP/cdeps_flag"; rm -rf "${TMP}"; mkdir -p "${TMP}" || exit 1
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cp -a "$SRC/." "$TMP/"
rm -rf "$TMP/.satria" "$TMP/build" "$TMP/bin" "$TMP/lib" "$TMP/modules" "$TMP/compile_commands.json"

fail() { echo "FAIL: $1" >&2; [ -f "$TMP/last.log" ] && cat "$TMP/last.log" >&2; exit 1; }

# 1) Flag tanpa -w harus ditolak (rc 2).
if (cd "$TMP" && "$SAT" --with-cdeps) > "$TMP/last.log" 2>&1; then
  fail "--with-cdeps without -w must fail"
fi
grep -q 'requires -w' "$TMP/last.log" || fail "missing --with-cdeps/-w validation message"

# 2) Dingin, -w bade TANPA flag: provider core tidak dibangun (kontrak lama),
#    tetapi peringatan D menyebut provider + opsi --with-cdeps.
(cd "$TMP" && "$SAT" -j2 -w bade) > "$TMP/last.log" 2>&1 && \
  fail "-w bade without flag must fail when core lib is missing"
grep -q "links against 'core' through cdeps" "$TMP/last.log" || \
  fail "missing pre-flight warning naming provider 'core'"
grep -q -- '--with-cdeps' "$TMP/last.log" || fail "warning must suggest --with-cdeps"
[ ! -e "$TMP/lib/librupa.a" ] || fail "core must NOT be built without --with-cdeps"

# 3) Dingin, -w bade --with-cdeps: core ditarik dan dibangun; link sukses.
rm -rf "$TMP/bin" "$TMP/build" "$TMP/lib" "$TMP/modules" "$TMP/.satria"
(cd "$TMP" && "$SAT" -j2 -w bade --with-cdeps) > "$TMP/last.log" 2>&1 || \
  fail "-w bade --with-cdeps failed from cold"
grep -q '^> core$' "$TMP/last.log" || fail "closure did not include provider 'core'"
[ -e "$TMP/lib/librupa.a" ] || fail "provider library lib/librupa.a not built"
[ -x "$TMP/bin/bade" ] || fail "bin/bade not linked"

# 4) Run kedua dengan flag = no-op (tidak membangun ulang).
(cd "$TMP" && "$SAT" -j2 -w bade --with-cdeps) > "$TMP/last.log" 2>&1 || \
  fail "second --with-cdeps run failed"
grep -q 'cached' "$TMP/last.log" || fail "second --with-cdeps run should be a no-op replay"

# 5) Provider prebuilt: full build lalu -w core TANPA flag sukses dan TIDAK
#    memberi peringatan provider hilang, dan tidak memproses bade.
rm -rf "$TMP/bin" "$TMP/build" "$TMP/lib" "$TMP/modules" "$TMP/.satria"
(cd "$TMP" && "$SAT" -j2 -w) > "$TMP/last.log" 2>&1 || fail "full build failed"
printf '\n/* selected core edit */\n' >> "$TMP/core/src/core.c"
(cd "$TMP" && "$SAT" -j2 -w core) > "$TMP/last.log" 2>&1 || \
  fail "-w core without flag failed although providers are prebuilt"
grep -q "links against 'bade' through cdeps" "$TMP/last.log" && \
  fail "pre-flight warning must not fire when provider library exists"
grep -q 'OK .*core/src/core.c' "$TMP/last.log" || fail "-w core did not rebuild core"
if grep -Eq '^> bade$' "$TMP/last.log"; then
  fail "-w core without flag processed provider bade"
fi

echo "PASS: --with-cdeps pulls cdeps providers; default contract and pre-flight warning hold"

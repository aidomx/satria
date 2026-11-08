#!/bin/sh
# requires: cold  -- asersi 'tidak ada build.log/relay' harus diuji pada build cold; snapshot membuat asersi ini vakum
# source helper sebelum cd: path relatif skrip hanya valid di direktori pemanggil
. "$(dirname "$0")/../lib/layout.sh"
# Usage: sh tests/unit/common/build_log.sh /absolute/path/to/satria [miniws-dir]
set -eu
SAT="$1"; SRC="${2:-${UNIT_REPO:?UNIT_REPO tidak di-set}/tests/miniws}"
case "$SAT" in /*) ;; *) SAT="$(pwd)/$SAT";; esac
case "$SRC" in /*) ;; *) SRC="$(pwd)/$SRC";; esac
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/build_log"; rm -rf "${W}"; mkdir -p "${W}" || exit 1
trap 'rm -rf "$W"' EXIT HUP INT TERM
cp -R "$SRC/." "$W/"
cd "$W"
# State fixture dibagikan antar-test (satu run in-place dengan log aktif
# meninggalkan .satria/build.log di repo) — buang dulu supaya asersi
# "cold build tanpa log" di bawah deterministik.
rm -rf .satria
# Source proyek core diambil dari Buildfile.ws (lihat lib/layout.sh): nama
# berkas berbeda antar target (miniws: core/src/core.c, rupaws: core/src/main.c).
ENG=$(unit_project_source .)
[ -n "$ENG" ] || unit_precondition "fixture tidak punya sumber .c untuk dimutasi"

# Default must not create a log or a relay, even on a cold build.
"$SAT" -j${UNIT_JOBS:-2} -w >out 2>err
[ ! -f .satria/build.log ]
printf '\nflags = Wall, Wextra, MMD, MP\nlog = true\n' >> Buildfile.ws
printf '\n#warning BUILDLOG_UNIT_WARNING\n' >> "$ENG"
"$SAT" -j${UNIT_JOBS:-2} -w >out 2>err
[ -f .satria/build.log ]
grep -q BUILDLOG_UNIT_WARNING .satria/build.log
"$SAT" -j${UNIT_JOBS:-2} --log > shown
grep -q BUILDLOG_UNIT_WARNING shown
before=$(wc -c < .satria/build.log)
"$SAT" -j${UNIT_JOBS:-2} -w >out 2>err
after=$(wc -c < .satria/build.log)
[ "$before" -eq "$after" ]
printf '\ninvalid buildlog syntax !!!\n' >> "$ENG"
if "$SAT" -j${UNIT_JOBS:-2} -w >out 2>err; then echo 'FAIL: invalid source compiled'; exit 1; fi
grep -q 'error:' .satria/build.log
grep -q 'build failed' .satria/build.log
"$SAT" -j${UNIT_JOBS:-2} log > shown
grep -q 'build failed' shown
# Configuration changes must invalidate startup.state without cleaning.
sed -i '/invalid buildlog syntax !!!/d' "$ENG"
sed -i 's/log = true/log = false/' Buildfile.ws
printf '\n#warning BUILDLOG_DISABLED_WARNING\n' >> "$ENG"
before=$(wc -c < .satria/build.log)
"$SAT" -j${UNIT_JOBS:-2} -w >out 2>err
[ "$(wc -c < .satria/build.log)" -eq "$before" ]
grep -q 'C log = false' .satria/startup.state
sed -i 's/log = false/log = true/' Buildfile.ws
printf '\n#warning BUILDLOG_REENABLED_WARNING\n' >> "$ENG"
"$SAT" -j${UNIT_JOBS:-2} -w >out 2>err
grep -q BUILDLOG_REENABLED_WARNING .satria/build.log
grep -q 'C log = true' .satria/startup.state
echo 'PASS: build.log default-off/startup.state/toggle/warning/error/failure/CLI/no-op' 

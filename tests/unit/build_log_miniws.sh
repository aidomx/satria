#!/bin/sh
# Usage: sh tests/unit/build_log_miniws.sh /absolute/path/to/satria [miniws-dir]
set -eu
SAT="$1"; SRC="${2:-tests/miniws}"
case "$SAT" in /*) ;; *) SAT="$(pwd)/$SAT";; esac
case "$SRC" in /*) ;; *) SRC="$(pwd)/$SRC";; esac
W=$(mktemp -d /tmp/satria-buildlog-test-XXXXXX)
trap 'rm -rf "$W"' EXIT HUP INT TERM
cp -R "$SRC/." "$W/"
cd "$W"
# Tata letak miniws: source proyek core berada di core/src.
ENG=core/src/core.c

# Default must not create a log or a relay, even on a cold build.
"$SAT" -w >out 2>err
[ ! -f .satria/build.log ]
printf '\nflags = Wall, Wextra, MMD, MP\nlog = true\n' >> Buildfile.ws
printf '\n#warning BUILDLOG_UNIT_WARNING\n' >> "$ENG"
"$SAT" -w >out 2>err
[ -f .satria/build.log ]
grep -q BUILDLOG_UNIT_WARNING .satria/build.log
"$SAT" --log > shown
grep -q BUILDLOG_UNIT_WARNING shown
before=$(wc -c < .satria/build.log)
"$SAT" -w >out 2>err
after=$(wc -c < .satria/build.log)
[ "$before" -eq "$after" ]
printf '\ninvalid buildlog syntax !!!\n' >> "$ENG"
if "$SAT" -w >out 2>err; then echo 'FAIL: invalid source compiled'; exit 1; fi
grep -q 'error:' .satria/build.log
grep -q 'build failed' .satria/build.log
"$SAT" log > shown
grep -q 'build failed' shown
# Configuration changes must invalidate startup.state without cleaning.
sed -i '/invalid buildlog syntax !!!/d' "$ENG"
sed -i 's/log = true/log = false/' Buildfile.ws
printf '\n#warning BUILDLOG_DISABLED_WARNING\n' >> "$ENG"
before=$(wc -c < .satria/build.log)
"$SAT" -w >out 2>err
[ "$(wc -c < .satria/build.log)" -eq "$before" ]
grep -q 'C log = false' .satria/startup.state
sed -i 's/log = false/log = true/' Buildfile.ws
printf '\n#warning BUILDLOG_REENABLED_WARNING\n' >> "$ENG"
"$SAT" -w >out 2>err
grep -q BUILDLOG_REENABLED_WARNING .satria/build.log
grep -q 'C log = true' .satria/startup.state
echo 'PASS: build.log default-off/startup.state/toggle/warning/error/failure/CLI/no-op' 

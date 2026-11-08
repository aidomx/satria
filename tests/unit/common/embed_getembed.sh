#!/bin/sh
# Usage: sh tests/unit/common/embed_getembed.sh /absolute/path/to/satria [miniws-dir]
#
# Khusus fixture dengan tabel embed (miniws: contoh `bade`, bade/src/embed_use.c,
# path ekstraksi /tmp/satria-miniws-system dari Buildfile.ws). Fixture lain
# (proyek nyata) tidak punya itu — berhenti dengan PRECONDITION, bukan FAIL.
#
# Verifikasi design/embedded.md (codegen sebagai satu-satunya mekanisme):
#   1. codegen     : build/bade/embedded.c + embedded.h (struct Embedded,
#                    getEmbed, tabel) + object biasa embedded.o
#   2. makro kompat: EMBED_MODULES_* tetap diterbitkan (fase 1 migrasi)
#   3. runtime     : bin/bade berjalan; getEmbed("modules") mengekstrak
#                    arsip ke field .extract dengan ukuran cocok
#   4. no-op       : run ulang + touch arsip (isi sama) = tanpa rebuild,
#                    mtime embedded.c/.o/binary tidak berubah
#   5. konten      : ubah isi arsip -> embedded.c diregenerasi -> hanya
#                    embedded.c yang recompile lalu relink
set -eu
. "$(dirname "$0")/../lib/layout.sh"
SAT="$1"; SRC="${2:-$(cd "$(dirname "$0")/../../.." && pwd)/tests/miniws}"
case "$SAT" in /*) ;; *) SAT="$(pwd)/$SAT";; esac
case "$SRC" in /*) ;; *) SRC="$(pwd)/$SRC";; esac
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/embed_getembed"; rm -rf "${W}"; mkdir -p "${W}" || exit 1
# .extract fixture Buildfile.ws = /tmp/satria-miniws-system (dipakai embed_use.c)
EXTRACT=/tmp/satria-miniws-system
trap 'rm -rf "$W"; rm -rf "$EXTRACT"' EXIT HUP INT TERM
cp -R "$SRC/." "$W/"
cd "$W"
rm -rf "$EXTRACT"

# Kebutuhan embed (bademod.extract + project bade): hanya miniws.
# Tanpa itu: PRECONDITION (design unit_test.md §5.3) — bukan kegagalan satria.
if ! grep -q 'extract' Buildfile.ws || [ -z "$(unit_project_source . bade)" ]; then
  unit_precondition "fixture tidak punya bade + bademod.extract (spek embed khusus miniws)"
fi

fail() { echo "FAIL: $*"; [ -f err ] && tail -20 err; exit 1; }

# ---------- 1. cold build + codegen ----------
"$SAT" -j${UNIT_JOBS:-2} -w >out 2>err || fail "cold build"
[ -f build/bade/embedded.c ]    || fail "embedded.c tidak digenerate"
[ -f build/bade/embedded.h ]    || fail "embedded.h tidak digenerate"
[ -f build/bade/embedded.o ]    || fail "embedded.o tidak dikompilasi (bukan source biasa?)"
grep -q 'getEmbed' build/bade/embedded.h   || fail "API getEmbed tidak ada di embedded.h"
grep -q 'typedef struct' build/bade/embedded.h || fail "struct Embedded tidak ada"
grep -q 'embed_table' build/bade/embedded.c || fail "tabel getEmbed tidak ada di embedded.c"
grep -q '"modules"' build/bade/embedded.c  || fail "key default = nama entri tidak ada di tabel"
grep -q 'EMBED_MODULES_SYMBOL_LEN' build/bade/embedded.h || fail "makro kompat EMBED_* hilang"

# ---------- 2. runtime getEmbed ----------
./bin/bade >run1 2>&1 || fail "bin/bade gagal dijalankan"
grep -q 'bade=7' run1 || fail "output bin/bade berubah"
# Ekstraksi memakai path POSIX dari fixture (`bademod.extract = /tmp/...`) dan
# `system()` shell POSIX di bade/src/embed_use.c; bin/bade adalah proses native
# Windows, jadi langkah runtime ini tidak bisa diuji dari shell MSYS. Codegen
# dan no-op di bawah tetap diuji di semua platform.
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*)
    echo "SKIP: ekstraksi runtime getEmbed (path .extract + system() POSIX di fixture)"
    ;;
  *)
    [ -f "$EXTRACT/rupa_modules.tar" ] || fail "ekstraksi via getEmbed tidak terjadi"
    a=$(wc -c < modules/rupa_modules.tar); b=$(wc -c < "$EXTRACT/rupa_modules.tar")
    [ "$a" -eq "$b" ] || fail "ukuran ekstraksi ($b) != arsip ($a)"
    ;;
esac

# ---------- 3. no-op run ulang ----------
"$SAT" -j${UNIT_JOBS:-2} -w >out 2>err || fail "run no-op"
grep -q '^\[' out && fail "no-op masih mengkompilasi: $(grep '^\[' out | head -1)"
S_EMB=$(stat -c %y build/bade/embedded.c); S_OBJ=$(stat -c %y build/bade/embedded.o)
S_BIN=$(stat -c %y bin/bade)

# ---------- 4. touch arsip, isi sama -> tetap no-op ----------
sleep 1.1
touch modules/rupa_modules.tar
"$SAT" -j${UNIT_JOBS:-2} -w >out 2>err || fail "run setelah touch arsip"
grep -q '^\[' out && fail "touch (isi sama) memicu kompilasi ulang"
[ "$(stat -c %y build/bade/embedded.c)" = "$S_EMB" ] || fail "embedded.c disentuh ulang (bukan content-based)"
[ "$(stat -c %y build/bade/embedded.o)" = "$S_OBJ" ] || fail "embedded.o direcompile saat touch"
[ "$(stat -c %y bin/bade)" = "$S_BIN" ] || fail "bin/bade direlink saat touch"

# ---------- 5. ubah isi arsip -> regenerasi + recompile + relink ----------
sleep 1.1
printf 'X' >> modules/rupa_modules.tar
"$SAT" -j${UNIT_JOBS:-2} -w >out 2>err || fail "run setelah isi arsip berubah"
grep -q 'build/bade/embedded.c' out || fail "embedded.c tidak masuk antrean kompilasi"
[ "$(stat -c %y build/bade/embedded.c)" != "$S_EMB" ] || fail "embedded.c tidak diregenerasi"
[ "$(stat -c %y bin/bade)" != "$S_BIN" ] || fail "bin/bade tidak direlink"
./bin/bade >run2 2>&1 || fail "bin/bade gagal setelah rebuild"
grep -q 'bade=7' run2 || fail "output bin/bade berubah setelah rebuild"

echo 'PASS: embed codegen (embedded.c/h, getEmbed runtime, touch-proof, content rebuild)'

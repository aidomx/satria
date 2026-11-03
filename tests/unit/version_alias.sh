#!/bin/sh
# Unit test: --version diperlakukan sama dengan `version` di semua posisi.
#   - satria version == satria --version (argumen pertama)
#   - satria -w --version: dispatch bootstrap, bukan nama proyek
#   - satria --version -w: sama
#   - baris help menyebut alias --version
# Pemakaian: sh tests/unit/version_alias.sh <path-ke-satria> [path-ke-miniws]

S="$1"
MW="${2:-$(dirname "$0")/../miniws}"

fail() { echo "FAIL version_alias: $1"; exit 1; }

[ -x "$S" ] || fail "binary tidak ditemukan: $S"

v1=$("$S" version 2>&1) || fail "'version' rc!=0"
v2=$("$S" --version 2>&1) || fail "'--version' rc!=0"
[ "$v1" = "$v2" ] || fail "'version' ($v1) != '--version' ($v2)"

tmp=$(mktemp -d) || fail "mktemp gagal"
trap 'rm -rf "$tmp"' EXIT
cp -a "$MW/." "$tmp/" || fail "salin miniws gagal"
cd "$tmp" || fail "cd gagal"

w1=$("$S" -w --version 2>&1) || fail "'-w --version' rc!=0"
[ "$w1" = "$v1" ] || fail "'-w --version' salah keluaran: $w1"
w2=$("$S" --version -w 2>&1) || fail "'--version -w' rc!=0"
[ "$w2" = "$v1" ] || fail "'--version -w' salah keluaran: $w2"

"$S" help 2>&1 | grep -q -- '--version' || fail "help tidak menyebut --version"

echo "PASS version_alias"

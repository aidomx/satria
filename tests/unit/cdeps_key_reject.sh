#!/bin/sh
# Unit test: cdeps.<kunci> (mis. cdeps.preshared) ditolak eksplisit, bukan
# diterima diam-diam sebagai proyek implisit bernama `cdeps`.
# Pemakaian: sh tests/unit/cdeps_key_reject.sh <path-ke-satria> [path-ke-miniws]

S="$1"
MW="${2:-$(dirname "$0")/../miniws}"

fail() { echo "FAIL cdeps_key_reject: $1"; exit 1; }

[ -x "$S" ] || fail "binary tidak ditemukan: $S"
[ -d "$MW" ] || fail "fixture tidak ditemukan: $MW"

: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
tmp="$UNIT_TMP/cdeps_key_reject"; rm -rf "${tmp}"; mkdir -p "${tmp}" || exit 1
trap 'rm -rf "$tmp"' EXIT
cp -a "$MW/." "$tmp/" || fail "salin miniws gagal"
cd "$tmp" || fail "cd gagal"

# 1. cdeps.preshared harus ditolak dengan nomor baris dan nama kunci
printf 'cdeps.preshared = windows\n' >> Buildfile.ws
out=$("$S" -w -g compdb 2>&1)
rc=$?
[ $rc -ne 0 ] || fail "cdeps.preshared seharusnya ditolak (rc=0)"
printf '%s' "$out" | grep -q "Buildfile.ws:" || fail "pesan tanpa nomor baris: $out"
printf '%s' "$out" | grep -q "cdeps.preshared" || fail "pesan tidak menyebut kunci: $out"
printf '%s' "$out" | grep -q "belum didukung" || fail "pesan tidak menjelaskan penolakan: $out"

# 2. tidak boleh ada proyek implisit bernama cdeps di state
rm -rf .satria
[ ! -e .satria/startup.state ] || fail "state terbentuk walau input ditolak"

echo "PASS cdeps_key_reject"

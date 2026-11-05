#!/bin/sh
# Unit test: `satria help` tetap tampil walau cmd.txt ber-terminasi CRLF.
# Di Windows file di-checkout dengan "\r\n" (git autocrlf), sehingga baris
# marker terbaca `---help\r` dan section tidak pernah cocok — help kosong.
#   - `satria help` tidak kosong dan memuat baris usage
#   - `satria help <topic>` menemukan section
#   - keluaran tidak membawa sisa '\r'
# Pemakaian: sh tests/unit/cmdhelp_crlf.sh <path-ke-satria>

S="$1"

fail() { echo "FAIL cmdhelp_crlf: $1"; exit 1; }

[ -x "$S" ] || fail "binary tidak ditemukan: $S"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/src/cmd.txt"
[ -f "$SRC" ] || fail "cmd.txt tidak ditemukan: $SRC"

tmp=$(mktemp -d) || fail "mktemp gagal"
trap 'rm -rf "$tmp"' EXIT

# Salinan CRLF, seperti hasil checkout Windows.
awk '{ printf "%s\r\n", $0 }' "$SRC" > "$tmp/cmd.txt" || fail "gagal menulis cmd.txt CRLF"
CR=$(printf '\r')
[ "$(tr -cd '\r' < "$tmp/cmd.txt" | wc -c | tr -d ' ')" -gt 0 ] || { od -c "$tmp/cmd.txt" | head -n 8 >&2; fail "salinan uji bukan CRLF"; }

out=$(SATRIA_CMD="$tmp/cmd.txt" "$S" help 2>&1) || fail "'help' rc!=0 pada cmd.txt CRLF"
[ -n "$out" ] || fail "'help' kosong pada cmd.txt CRLF"
printf '%s' "$out" | grep -q 'usage: satria' || fail "'help' tidak memuat baris usage"

topic=$(SATRIA_CMD="$tmp/cmd.txt" "$S" help workspace 2>&1) ||
  fail "'help workspace' rc!=0 pada cmd.txt CRLF"
[ -n "$topic" ] || fail "'help workspace' kosong pada cmd.txt CRLF"
printf '%s' "$topic" | grep -q 'workspace' || fail "'help workspace' bukan isi topik"

# Di Windows, stdout mode teks menerjemahkan '\n' -> '\r\n', jadi CR ada di
# keluaran SETIAP perintah — itu terjemahan platform, bukan sisa '\r' dari
# cmd.txt. Yang diuji: help tetap utuh dan tidak ada CR tambahan dari sumber,
# sehingga di sana keluaran dinormalkan dulu sebelum dibandingkan.
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) out=$(printf '%s' "$out" | tr -d '\r'); topic=$(printf '%s' "$topic" | tr -d '\r') ;;
esac

[ "$(printf '%s%s' "$out" "$topic" | tr -cd '\r' | wc -c | tr -d ' ')" -eq 0 ] || fail "keluaran masih memuat CR"

outlf=$(SATRIA_CMD="$SRC" "$S" help 2>&1) || fail "'help' rc!=0 pada cmd.txt LF"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) outlf=$(printf '%s' "$outlf" | tr -d '\r') ;;
esac
[ "$outlf" = "$out" ] || fail "keluaran CRLF berbeda dari LF"

echo "PASS cmdhelp_crlf"

#!/bin/sh
# Unit test: grup cdeps level atas (design/cdeps_group.md).
#   - cdeps = {a,b} setara dengan a.cdeps = b dan b.cdeps = a (compdb identik)
#   - grup yang saling tumpang tindih digabung tanpa duplikat
#   - kesalahan: grup satu anggota, proyek tidak dikenal, bentuk lama tanpa
#     kurung, kurung tidak ditutup, nama kosong; pesan menunjuk ke baris Buildfile.ws
# Pemakaian: sh tests/unit/cdeps_group.sh <path-ke-satria> [path-ke-miniws]

S="$1"
MW="${2:-$(dirname "$0")/../miniws}"

fail() { echo "FAIL cdeps_group: $1"; exit 1; }

# Path absolut: compdb() melakukan cd ke direktori tmp sebelum memanggil "$S",
# jadi path relatif akan lupa asalnya dan binary tidak ditemukan.
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
S=$(abspath "$S")
MW=$(abspath "$MW")

[ -x "$S" ] || fail "binary tidak ditemukan: $S"

tmp=$(mktemp -d) || fail "mktemp gagal"
trap 'rm -rf "$tmp"' EXIT

# Buildfile.ws = fixture tanpa baris cdeps, ditambah blok yang diuji
base() { grep -v 'cdeps' "$MW/Buildfile.ws"; }

mkws() { # $1 = nama direktori, $2 = isi tambahan Buildfile.ws
  rm -rf "$tmp/$1"; mkdir -p "$tmp/$1"
  cp -a "$MW/." "$tmp/$1/" || fail "salin miniws gagal"
  { base; printf '%s\n' "$2"; } > "$tmp/$1/Buildfile.ws"
}

compdb() { # $1 = direktori, hasil ke $1/compile_commands.json
  (cd "$tmp/$1" && rm -rf .satria bin build lib modules dist compile_commands.json \
     && "$S" -w -g compdb > "$tmp/$1.log" 2>&1) || fail "-g compdb gagal di $1: $(tail -3 "$tmp/$1.log")"
  # Normalisasi: path di compile_commands.json bisa memakai "/" atau "\\" (Windows,
  # di-escape JSON). Nama direktori uji (grup/manual) diganti token netral,
  # tanpa bergantung pada bentuk path.
  sed -e "s#$tmp/$1#/WS#g" -e "s#$1#WS#g" "$tmp/$1/compile_commands.json" > "$tmp/$1.norm"
}

# 1. kesetaraan: grup vs per proyek
mkws grup 'cdeps = {bade,core}'
mkws manual "$(printf 'bade.cdeps = core\ncore.cdeps = bade')"
compdb grup; compdb manual
cmp -s "$tmp/grup.norm" "$tmp/manual.norm" || fail "grup tidak setara dengan penulisan per proyek"

# 2. tumpang tindih: {bade,core}, {core,bade} = satu relasi, tidak duplikat
mkws tumpang 'cdeps = {bade,core}, {core,bade}'
compdb tumpang
cmp -s "$tmp/tumpang.norm" "$tmp/manual.norm" || fail "grup tumpang tindih tidak sama dengan satu relasi"

# 3. error: pesan harus menunjuk ke baris Buildfile.ws
expect_err() { # $1 = nama, $2 = isi, $3 = potongan pesan yang diharapkan
  mkws "$1" "$2"
  out=$(cd "$tmp/$1" && "$S" -w -g compdb 2>&1)
  rc=$?
  [ $rc -ne 0 ] || fail "$1: seharusnya gagal"
  printf '%s' "$out" | grep -q "Buildfile.ws:" || fail "$1: pesan tanpa lokasi baris"
  printf '%s' "$out" | grep -q -- "$3" || fail "$1: pesan tidak memuat '$3' -> $out"
}
expect_err satu 'cdeps = {bade}'            'minimal dua proyek berbeda'
expect_err nope 'cdeps = {bade,nope}'       "tidak dikenal"
expect_err lama 'cdeps = bade, core'        'harus berbentuk {a,b}'
expect_err tutup 'cdeps = {bade,core'       "tidak ditemukan"
expect_err kosong 'cdeps = {bade,,core}'    'nama proyek kosong'

echo "PASS cdeps_group"

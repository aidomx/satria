#!/bin/sh
# Helper tata letak proyek untuk tes workspace.
#
# Sumber truth = discovery dari RUNNER (design unit_test.md §6): runner
# mengeksekusi `-w -g compdb` (mekanisme satria sendiri, query-only, tanpa
# parser kedua) dan menulis
#
#   "$UNIT_DISCOVERY/sources.tsv" = proyek<TAB>sumber<TAB>objek
#
# dengan `sumber` path rel terhadap akar fixture. Tes di tests/unit/common
# dijalankan terhadap target apa pun (miniws / complexws / rupaws), jadi nama
# berkas TIDAK boleh di-hardcode — selalu lewat fungsi di bawah.
#
# Ketika TSV tidak tersedia (tes dijalankan manual tanpa runner), fungsi
# mengembalikan string kosong; pemanggil memutus PRECONDITION:
#
#   src="$(unit_project_source "$W" core)"
#   [ -z "$src" ] && unit_precondition "layout.sh: butuh UNIT_DISCOVERY"
#
# Surface dipertahankan: semua fungsi menerima direktori akar fixture sebagai
# $1 (default: direktori kerja) dan mencetak path relatif terhadap akar itu.

# unit_project_source <root> [proyek]
#   Satu berkas .c milik <proyek> (default: core) dari sources.tsv.
#   - baris hasil codegen (`embedded.c`) di-skip: berkasnya ditulis ulang tiap
#     build, bukan input yang dapat disentuh.
#   - nama direktori `src` diprioritaskan.
#   - bila <proyek> tidak ada di TSV, pakai proyek pertama di TSV (supaya tes
#     "sentuh satu source" tetap bermakna pada fixture 1-proyek).
unit_project_source() {
  _root=${1:-.}
  _want=${2:-core}
  _tsv="${UNIT_DISCOVERY:-}/sources.tsv"
  [ -s "$_tsv" ] || return 0
  # proyek yang diminta ada di TSV? (tanpa baris embedded.c)
  _have=$(awk -F'\t' -v w="$_want" '$1==w { found=1 } END { print found+0 }' "$_tsv")
  [ "$_have" = "1" ] || _want=$(awk -F'\t' '{ print $1 }' "$_tsv" | head -1)
  [ -n "$_want" ] || return 0
  awk -F'\t' -v w="$_want" '
    $1 == w && $2 !~ /(^|\/)embedded\.c$/ {
      print ( ($2 ~ /^src\// || $2 ~ /\/src\//) ? 0 : 1 ), $2
    }
  ' "$_tsv" | sort | cut -d' ' -f2- | head -1 | {
    _src2=$(cat)
    [ -n "$_src2" ] || return 0
    printf '%s/%s\n' "$_want" "$_src2"
  }
  return 0
}

# unit_project_binary <root> [proyek]
#   Path binary hasil proyek, dari `X.output as A` + `A.binaryName = N` -> bin/N
#   INTERIM: micro-read Buildfile.ws sampai ada query targets native satria
#   (design §6); hanya membaca DUA key sederhana, bukan parser.
unit_project_binary() {
  _root=${1:-.}
  _want=${2:-core}
  _alias=$(sed -n "s/^$_want\.output as *//p" "$_root/Buildfile.ws" 2> /dev/null | head -1 | tr -d ' \t')
  _name=""
  [ -n "$_alias" ] && _name=$(sed -n "s/^$_alias\.binaryName *= *//p" "$_root/Buildfile.ws" 2> /dev/null | head -1 | tr -d ' \t')
  [ -n "$_name" ] || _name=$_want
  printf 'bin/%s\n' "$_name"
}

# unit_archive_project <root>
#   Nama proyek yang memaket direktori (`<proyek>.archive.src`), mis. rupamod.
#   Kunci di Buildfile bisa berupa alias (`projects.rupamod as mod`), sedangkan
#   direktori proyek memakai nama proyek — alias dipetakan balik ke nama proyek.
#   INTERIM: micro-read Buildfile.ws sampai ada query targets native satria.
unit_archive_project() {
  _root=${1:-.}
  _key=$(sed -n 's/^\([A-Za-z0-9_]*\)\.archive\.src *= *\. *$/\1/p' "$_root/Buildfile.ws" 2> /dev/null | head -1)
  [ -n "$_key" ] || return 0
  _proj=$(sed -n "s/^projects\.\([A-Za-z0-9_]*\) as *$_key *$/\1/p" "$_root/Buildfile.ws" 2> /dev/null | head -1)
  printf '%s\n' "${_proj:-$_key}"
}

# unit_precondition <pesan>
#   Verdict prasyarat (design unit_test.md §5.3 + §8): fixture tidak menyediakan
#   kebutuhan tes. Bukan kegagalan Satria — cetak verdict sendiri lalu akhiri tes
#   dengan rc 0. Runner menghitungnya sebagai kategori PRE, bukan FAIL.
unit_precondition() {
  echo "PRECONDITION $*"
  exit 0
}

# wait_tick [dir]: tunggu sampai mtime filesystem berubah, pengganti "sleep 1".
# Racy guard Satria hanya butuh satu tick timestamp berlalu, bukan satu detik penuh.
# Menulis berkas probe di dir sampai mtime-nya berbeda; batas sekitar 2 detik.
wait_tick() {
  # probe di UNIT_TMP (di luar proyek): tidak mengubah mtime direktori proyek
  local d="${1:-${UNIT_TMP:-${TMPDIR:-/tmp}}}" p n=0 a b
  p="$d/.wait_tick_probe"
  : > "$p" || return 0
  a="$(stat -c %y "$p" 2> /dev/null)"
  while [ "$n" -lt 200 ]; do
    printf x >> "$p"   # harus menulis byte: ": >>" tidak mengubah mtime
    b="$(stat -c %y "$p" 2> /dev/null)"
    [ "$a" != "$b" ] && break
    n=$((n + 1))
    sleep 0.01
  done
  rm -f "$p"
}

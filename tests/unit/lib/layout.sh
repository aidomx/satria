#!/bin/sh
# Helper tata letak proyek untuk tes workspace.
#
# Tes di tests/unit/common dijalankan terhadap target apa pun (tests/miniws untuk
# eksplorasi cepat, atau proyek nyata seperti rupaws). Karena itu nama berkas
# TIDAK boleh di-hardcode: ambil dari Buildfile.ws lewat fungsi di bawah.
# Semua fungsi menerima direktori proyek sebagai $1 (default: direktori kerja)
# dan mencetak path relatif terhadap direktori itu (kosong bila tidak ada).

# unit_project_source <root> [proyek]
#   Satu berkas .c milik <proyek> (default: core). `<proyek>.sources` di
#   Buildfile.ws berisi direktori RELATIF terhadap direktori proyek, jadi dicari
#   di `<proyek>/<dir>`. Dipakai tes yang perlu "sentuh/edit satu source" tanpa
#   mengasumsikan nama berkas fixture.
unit_project_source() {
  _root=${1:-.}
  _want=${2:-core}
  _dirs=$(sed -n "s/^$_want\.sources *= *//p" "$_root/Buildfile.ws" 2> /dev/null | head -1)
  [ -n "$_dirs" ] || _dirs=$(sed -n 's/^[A-Za-z0-9_]*\.sources *= *//p' "$_root/Buildfile.ws" 2> /dev/null | head -1)
  # Direktori `src` diprioritaskan: direktori lain bisa berisi berkas hasil
  # codegen (mis. `bade/manager/embedded.c`) yang ditulis ulang tiap build.
  printf '%s\n' "$_dirs" | tr ',' '\n' | tr -d ' \t' |
    awk '{ print ($0 == "src" || $0 ~ /\/src$/ ? 0 : 1), $0 }' | sort | cut -d' ' -f2- |
    while IFS= read -r _d; do
    [ -n "$_d" ] || continue
    [ -d "$_root/$_want/$_d" ] || continue
    _f=$(find "$_root/$_want/$_d" -maxdepth 3 -name '*.c' 2> /dev/null | sort | head -1)
    [ -n "$_f" ] || continue
    printf '%s\n' "${_f#"$_root"/}"
    break
  done
}

# unit_project_binary <root> [proyek]
#   Path binary hasil proyek, dari `X.output as A` + `A.binaryName = N` -> bin/N
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
unit_archive_project() {
  _root=${1:-.}
  _key=$(sed -n 's/^\([A-Za-z0-9_]*\)\.archive\.src *= *\. *$/\1/p' "$_root/Buildfile.ws" 2> /dev/null | head -1)
  [ -n "$_key" ] || return 0
  _proj=$(sed -n "s/^projects\.\([A-Za-z0-9_]*\) as *$_key *$/\1/p" "$_root/Buildfile.ws" 2> /dev/null | head -1)
  printf '%s\n' "${_proj:-$_key}"
}

# unit_precondition <pesan>
#   Verdict prasyarat (design unit_test.md §5.3 + §9): fixture tidak menyediakan
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

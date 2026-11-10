#!/usr/bin/env bash
# scripts/unit.sh - runner unit test stabil (konsumen unit_dev.sh; design/unit_test.md)
#
# unit_dev.sh dan scripts/lib/unit_*.sh adalah sumber kebenaran. Bash membaca
# skrip secara lazy dan `source` juga membaca berkas saat dijalankan, jadi edit
# di tengah run bisa merusak run yang sedang berjalan. Karena itu unit.sh
# menyalin SELURUH loader beserta lib/ ke folder snapshot per proses:
#
#   scripts/.unit.<pid>.d/unit_dev.sh
#   scripts/.unit.<pid>.d/lib/unit_{common,runner,watcher}.sh
#
# lalu menjalankan snapshot itu di foreground. Akibatnya:
#   - scripts/unit_dev.sh dan scripts/lib/* bebas diedit kapan saja, termasuk
#     saat ada run atau watcher berjalan. Proses itu memegang snapshot-nya.
#   - Root repo diteruskan lewat UNIT_ROOT (snapshot tidak tinggal di root).
#   - Snapshot dihapus oleh proses yang memakainya (drop_copy di unit_dev.sh).
#     Proses -b memakai snapshot sampai selesai; pembungkus tidak menghapusnya.
#
# Foreground: Ctrl-C (satu process group), stdin, dan kode keluar sama seperti
# menjalankan unit_dev.sh langsung. cwd tidak diubah: argumen relatif (bin/satria,
# target) diresolusi dari direktori pemanggil.
set -u
_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)" || exit 2
_root="$(CDPATH= cd -- "$_dir/.." && pwd)" || exit 2

# Bersihkan snapshot yatim dari run yang dihentikan keras (SIGKILL tidak bisa
# ditangkap trap). Snapshot milik proses yang masih hidup (mis. -b, watcher)
# tidak disentuh.
for _old in "$_dir"/.unit.*.sh "$_dir"/.unit.*.d; do
  [ -e "$_old" ] || continue
  _p="${_old##*.unit.}"; _p="${_p%.sh}"; _p="${_p%.d}"
  case "$_p" in ''|*[!0-9]*) continue ;; esac
  kill -0 "$_p" 2>/dev/null && continue
  rm -rf "$_old"
done

SNAP="$_dir/.unit.$$.d"
mkdir -p "$SNAP/lib" || exit 2
cp "$_dir/unit_dev.sh" "$SNAP/unit_dev.sh" || exit 2
cp "$_dir/lib/"*.sh "$SNAP/lib/" || exit 2
export UNIT_ROOT="$_root"
# exec: wrapper MENJADI snapshot runner (satu proses, satu pid). Tanpa exec,
# kill ke pid unit.sh membiarkan watcher/runner body olarak yatim dan
# sinyal pembersihan tidak pernah terlihat oleh unit_dev.sh.
exec bash "$SNAP/unit_dev.sh" "$@"

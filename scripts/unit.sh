#!/usr/bin/env bash
# scripts/unit.sh - runner unit test stabil (konsumen unit_dev.sh; design/unit_test.md)
#
# unit_dev.sh adalah sumber kebenaran tunggal. unit.sh menyalinnya ke berkas
# sementara di scripts/ lalu menjalankan salinan itu — bukan unit_dev.sh langsung.
# Akibatnya:
#   - unit_dev.sh bebas diedit kapan saja, termasuk saat ada run berjalan
#     (bash mem-parse skrip secara lazy; tanpa salinan, edit di tengah run
#     menggeser offset byte dan bisa membuat parsing run aktif error). Run itu
#     memegang snapshot-nya sendiri.
#   - unit.sh tidak pernah ditimpa lagi: tugasnya tidak berubah, jadi tidak
#     ada lagi sinkronisasi salinan identik dua file.
# Salinan dijalankan di foreground, bukan sebagai job latar: dengan begitu
# Ctrl-C (satu process group), stdin, dan kode keluar sama persis seperti
# menjalankan unit_dev.sh langsung.
# Salinan menghapus dirinya sendiri (unit_dev.sh: drop_copy) — bukan unit.sh:
# dengan `-b` proses yang memakai salinan hidup lebih lama dari pembungkus.
# cwd tidak diubah: argumen relatif (bin/satria, target) diresolusi dari
# direktori pemanggil, seperti saat menjalankan unit_dev.sh langsung.
set -u
_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)" || exit 2
# Salinan yatim dari run yang dihentikan keras (SIGKILL tidak bisa ditangkap trap
# drop_copy): hapus .unit.<pid>.sh yang PID-nya sudah tidak hidup. Salinan milik
# run yang masih hidup (mis. -b) tidak disentuh, karena bash membaca skrip secara
# lazy dan menghapusnya saat berjalan akan merusak parsing.
for _old in "$_dir"/.unit.*.sh; do
  [ -e "$_old" ] || continue
  _p="${_old##*.unit.}"; _p="${_p%.sh}"
  case "$_p" in ''|*[!0-9]*) continue ;; esac
  kill -0 "$_p" 2>/dev/null || rm -f "$_old"
done
TMP="$_dir/.unit.$$.sh"
cp "$_dir/unit_dev.sh" "$TMP" || exit 2
bash "$TMP" "$@"
exit $?

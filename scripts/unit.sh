#!/usr/bin/env bash
# scripts/unit.sh - runner unit test stabil (konsumen unit_dev.sh; design/unit_test.md)
#
# unit_dev.sh adalah sumber kebenaran tunggal. unit.sh menyalinnya ke berkas
# sementara di scripts/ lalu menjalankan salinan itu — bukan unit_dev.sh langsung.
# Akibatnya:
#   - unit_dev.sh bebas diedit kapan saja, termasuk saat ada run berjalan
#     (bash mem-parse skrip secara lazy; tanpa salinan, edit di tengah run
#     menggeser offset byte dan bisa membuat parsing run aktif error).
#   - unit.sh tidak pernah ditimpa lagi: tugasnya tidak berubah, jadi tidak
#     ada lagi sinkronisasi salinan identik dua file.
# cwd tidak diubah: argumen relatif (bin/satria, target) diresolusi dari
# direktori pemanggil, seperti saat menjalankan unit_dev.sh langsung.
set -u
_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)" || exit 2
TMP="$_dir/.unit.$$.sh"
trap 'rm -f "$TMP"' EXIT
cp "$_dir/unit_dev.sh" "$TMP" || exit 2
bash "$TMP" "$@"
exit $?

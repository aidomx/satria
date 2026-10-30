#!/bin/sh
# gen_tracer_src.sh  Buat src/trace/tracer_src.h dari src/trace/tracer.src.
# Tracer disimpan sebagai string C agar binary satria bisa menulis dan
# membangunnya sendiri (tanpa berkas luar). Jalankan ulang setiap kali
# tracer.src berubah; tests/unit/trace_src_sync.sh memastikan tidak basi.
#
# Usage: sh scripts/gen_tracer_src.sh [in.src] [out.h]
set -eu
in="${1:-src/trace/tracer.src}"
out="${2:-src/trace/tracer_src.h}"
[ -f "$in" ] || { echo "gen_tracer_src: '$in' tidak ada" >&2; exit 2; }
{
  echo "/* GENERATED dari src/trace/tracer.src oleh scripts/gen_tracer_src.sh."
  echo "   Jangan edit manual; ubah tracer.src lalu jalankan ulang skrip ini. */"
  echo "static const char kTracerSrc[] ="
  sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/  "/' -e 's/$/\\n"/' "$in"
  echo "  ;"
} > "$out.tmp"
mv "$out.tmp" "$out"

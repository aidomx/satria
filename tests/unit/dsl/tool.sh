#!/bin/sh
# Alat uji run.tool: menulis sisa argumen (satu baris) ke berkas keluaran.
out=$1
shift
printf '%s\n' "$*" > "$out"

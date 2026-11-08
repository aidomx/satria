#!/bin/sh
# Usage: sh tests/unit/common/trace_src_sync.sh   (dari root repo)
# src/trace/tracer_src.h harus hasil generate dari src/trace/tracer.src.
# Gagal bila tracer.src diubah tanpa menjalankan scripts/gen_tracer_src.sh.
set -eu
ROOT="${UNIT_REPO:?UNIT_REPO tidak di-set: jalankan lewat scripts/unit_v1.sh}"
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/trace_src_sync"
rm -rf "${W}"
mkdir -p "${W}" || exit 1
trap 'rm -rf "$W"' EXIT HUP INT TERM

cd "$ROOT"
sh scripts/gen_tracer_src.sh src/trace/tracer.src "$W/tracer_src.h"
if cmp -s "$W/tracer_src.h" src/trace/tracer_src.h; then
  echo "PASS: tracer_src.h sinkron dengan tracer.src"
  echo "lulus: 1  gagal: 0"
else
  echo "FAIL: tracer_src.h basi; jalankan: sh scripts/gen_tracer_src.sh"
  echo "lulus: 0  gagal: 1"
  exit 1
fi

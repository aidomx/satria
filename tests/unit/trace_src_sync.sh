#!/bin/sh
# Usage: sh tests/unit/trace_src_sync.sh   (dari root repo)
# src/trace/tracer_src.h harus hasil generate dari src/trace/tracer.src.
# Gagal bila tracer.src diubah tanpa menjalankan scripts/gen_tracer_src.sh.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$(mktemp -d /tmp/satria-tracersync-XXXXXX)
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

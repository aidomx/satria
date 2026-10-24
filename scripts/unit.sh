#!/usr/bin/env sh
dir=$(pwd)
units=$(find "$dir/tests/unit" -name "*.sh")
bin="$dir/bin/satria"
target="$dir/tests/miniws"

for t in $units; do
  echo "> $t"
  sh "$t" "$bin" "$target"
done

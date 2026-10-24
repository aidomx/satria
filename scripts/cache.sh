#!/usr/bin/env sh
echo "dir: $(pwd)"
for c in .satria/**/*.cache; do
  echo "file: $c"
  printf "  %s" "$(grep '^P ' "$c")"
  echo
done

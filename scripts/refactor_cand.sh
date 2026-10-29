#!/usr/bin/env bash
main() {
  dir=$(pwd)
  cand=0

  echo "# Daftar kandidat > 600 baris:"

  while IFS=$'\n' read -r src; do
    cand=$(cat < "$src" | wc -l)

    if [ "$cand" -gt 600 ]; then
      echo "> $src:$cand"
    fi
  done < <(find "$dir/src" -name "*.c")
}

main "$@"

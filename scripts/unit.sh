#!/usr/bin/env bash
set -uo pipefail

# ---------------------------- konfigurasi ----------------------------------
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SATRIA="$ROOT_DIR/bin/satria"

TMP_DIR="$(mktemp -d /tmp/tmp_XXX)"

# Sortir agar urutan eksekusi test konsisten
units=$(find "$ROOT_DIR/tests/unit" -name "*.sh" | sort)
target="${1:-$ROOT_DIR/tests/miniws}"

clean_cands=("bin" "build" "dist" "lib" "modules" ".satria")
cands=()
for c in "${clean_cands[@]}"; do
  cands+=("$TMP_DIR/$(basename "$target")/$c")
done

clean() {
  for c in "${cands[@]}"; do
    [ -e "$c" ] && rm -rf "$c" # -e lebih aman untuk cek file/direktori
  done
}

create_tmp() {
  [ -d "$TMP_DIR" ] || mkdir -p "$TMP_DIR"
}

copy_target() {
  if [ ! -d "$target" ]; then
    echo "Error: $target is not found!"
    exit 1
  fi
  cp -r "$target" "$TMP_DIR/"
}

create_tmp
copy_target

cd "$TMP_DIR/$(basename "$target")" || exit 1
clean

run() {
  echo "========================================="
  echo "Running tests for: $(basename "$target")"
  echo "========================================="

  for u in $units; do
    printf "> %s\n" "Running $(basename "$u")..."

    sh "$u" "$SATRIA" "$target"
  done
}

run

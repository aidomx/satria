#!/usr/bin/env bash
set -uo pipefail

# =============================================================================
# scripts/unit.sh — Test runner untuk Satria
# =============================================================================
#
# Contoh pemakaian:
#   bash scripts/unit.sh                                    # test default (miniws)
#   bash scripts/unit.sh tests/miniws                       # target spesifik
#   bash scripts/unit.sh -o /tmp/unit.log                   # output ke file log
#   bash scripts/unit.sh -b                                 # jalankan di background
#   bash scripts/unit.sh -b -o /tmp/unit.log                # background + log file
#   bash scripts/unit.sh -o /tmp/unit.log tests/miniws      # kombinasi dengan target
#   bash scripts/unit.sh -j4                                # batasi satria ke 4 job
#   CLEAN_VERBOSE=1 bash scripts/unit.sh                    # tampilkan detail clean()
#
# Opsi:
#   -o <file>   Simpan semua output ke file log (stdout + stderr) via tee
#   -b          Jalankan di background (detached). PID ditulis ke file .pid
#   -j <N>      Batasi job satria per test (default 2). -j 0 = tanpa batas (core).
#               Banyak test memanggil satria tanpa -j (jatuh ke cpuCount());
#               wrapper menyisipkan -jN agar ringan, terutama saat -b di proot.
#
# Environment:
#   CLEAN_VERBOSE=1   Tampilkan detail setiap item yang dihapus oleh clean()
#
# =============================================================================

# ---------------------------- konfigurasi ----------------------------------
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SATRIA="$ROOT_DIR/bin/satria"

TMP_DIR="$(mktemp -d /tmp/tmp_XXX)"

# Sortir agar urutan eksekusi test konsisten
units=$(find "$ROOT_DIR/tests/unit" -name "*.sh" | sort)

# ---------------------------- parsing argumen ------------------------------
LOG_FILE=""
BACKGROUND=0
JOBS=2                     # default pembatas: ringan di proot; -j 0 = tanpa batas
target=""

while [ $# -gt 0 ]; do
  case "$1" in
    -o)
      if [ $# -lt 2 ]; then
        echo "Error: -o memerlukan argumen file path" >&2
        exit 1
      fi
      LOG_FILE="$2"
      shift 2
      ;;
    -b)
      BACKGROUND=1
      shift
      ;;
    -j)
      if [ $# -lt 2 ]; then
        echo "Error: -j memerlukan argumen angka" >&2
        exit 1
      fi
      JOBS="$2"
      shift 2
      ;;
    -*)
      echo "Opsi tidak dikenal: $1" >&2
      echo "Pemakaian: $0 [-o <file>] [-b] [-j <N>] [target]" >&2
      exit 1
      ;;
    *)
      target="$1"
      shift
      ;;
  esac
done

# Default target jika tidak diberikan
target="${target:-$ROOT_DIR/tests/miniws}"

# Validasi -j: harus bilangan bulat >= 0.
case $JOBS in
  '' | *[!0-9]*)
    echo "Error: -j harus bilangan bulat >= 0 (dapat '$JOBS')" >&2
    exit 1
    ;;
esac

# ---------------------------- pembatas jobs (-j) ---------------------------
# Banyak test memanggil "$SAT" TANPA -j, sehingga satria jatuh ke cpuCount()
# (paralel sebanyak core) — ini yang membuat -b berat di proot. Wrapper
# menyisipkan -jN di depan SETIAP pemanggilan satria. Parse satria mengambil
# -j TERAKHIR, jadi -j1/-j2 yang eksplisit di sebagian test tetap menang.
# -j 0 = tanpa wrapper (perilaku lama: paralel sebanyak core).
if [ "$JOBS" -ge 1 ]; then
  SATRIA_WRAP="$TMP_DIR/satria-j"
  printf '#!/bin/sh\nexec "%s" -j%d "$@"\n' "$SATRIA" "$JOBS" > "$SATRIA_WRAP"
  chmod +x "$SATRIA_WRAP"
  SATRIA="$SATRIA_WRAP"
fi

# ---------------------------- setup environment ----------------------------
# Daftar artifact yang harus dibersihkan sebelum test
clean_cands=("bin" "build" "dist" "lib" "modules" ".satria" "compile_commands.json")
cands=()
for c in "${clean_cands[@]}"; do
  cands+=("$TMP_DIR/$(basename "$target")/$c")
done

# =============================================================================
# clean() — versi robust: menangani semua edge case
# =============================================================================
# Edge case yang ditangani:
#   1. File/direktori tidak ada              → skip tanpa error
#   2. Broken symlink                        → tetap dihapus (via -L)
#   3. File read-only                        → chmod +rm (fallback)
#   4. Permission denied                     → error message jelas
#   5. File "busy" (open oleh proses lain)   → retry 1x setelah delay singkat
#   6. Verbose mode (via env CLEAN_VERBOSE)  → tampilkan detail
# =============================================================================
clean() {
  local removed=0
  local skipped=0
  local failed=0

  for c in "${cands[@]}"; do
    # Cek 1: tidak ada sama sekali (bukan symlink rusak) → skip
    if [ ! -e "$c" ] && [ ! -L "$c" ]; then
      skipped=$((skipped + 1))
      [ "${CLEAN_VERBOSE:-0}" = "1" ] && printf '  [skip]   %s (not found)\n' "$c"
      continue
    fi

    # Cek 2: broken symlink → hapus langsung (rm -rf kadang gagal di broken symlink)
    if [ -L "$c" ] && [ ! -e "$c" ]; then
      if rm -f "$c" 2> /dev/null; then
        removed=$((removed + 1))
        [ "${CLEAN_VERBOSE:-0}" = "1" ] && printf '  [rm-sym] %s (broken symlink)\n' "$c"
      else
        failed=$((failed + 1))
        printf '  [fail]   %s (broken symlink, cannot remove)\n' "$c" >&2
      fi
      continue
    fi

    # Cek 3: coba hapus normal dulu
    if rm -rf "$c" 2> /dev/null; then
      removed=$((removed + 1))
      [ "${CLEAN_VERBOSE:-0}" = "1" ] && printf '  [rm]     %s\n' "$c"
      continue
    fi

    # Cek 4: gagal — kemungkinan file read-only atau permission issue
    # Coba chmod recursive + rm lagi (fallback untuk file read-only)
    if [ -d "$c" ]; then
      chmod -R u+w "$c" 2> /dev/null || true
    else
      chmod u+w "$c" 2> /dev/null || true
    fi

    if rm -rf "$c" 2> /dev/null; then
      removed=$((removed + 1))
      [ "${CLEAN_VERBOSE:-0}" = "1" ] && printf '  [rm+fix] %s (after chmod)\n' "$c"
      continue
    fi

    # Cek 5: masih gagal — kemungkinan file "busy" (open by process)
    # Retry sekali setelah delay singkat
    sleep 0.1
    if rm -rf "$c" 2> /dev/null; then
      removed=$((removed + 1))
      [ "${CLEAN_VERBOSE:-0}" = "1" ] && printf '  [rm+rt]  %s (after retry)\n' "$c"
      continue
    fi

    # Gagal total
    failed=$((failed + 1))
    printf '  [fail]   %s (cannot remove — check permissions or running process)\n' "$c" >&2
  done

  # Ringkasan (hanya di verbose mode atau jika ada kegagalan)
  if [ "${CLEAN_VERBOSE:-0}" = "1" ] || [ "$failed" -gt 0 ]; then
    printf '  clean summary: removed=%d skipped=%d failed=%d\n' \
      "$removed" "$skipped" "$failed"
  fi

  # Return 1 jika ada yang gagal, agar caller bisa handle
  [ "$failed" -eq 0 ]
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

# ---------------------------- eksekusi setup -------------------------------
create_tmp
copy_target

cd "$TMP_DIR/$(basename "$target")" || exit 1

# ---------------------------- fungsi utama ---------------------------------
run() {
  echo "========================================="
  echo "Running tests for: $(basename "$target")"
  echo "========================================="

  for u in $units; do
    printf "> %s\n" "Running $(basename "$u")..."
    sh "$u" "$SATRIA" "$target"
  done
}

# ---------------------------- mode background ------------------------------
if [ "$BACKGROUND" -eq 1 ]; then
  # Tentukan file log default jika -o tidak diberikan
  if [ -z "$LOG_FILE" ]; then
    LOG_FILE="/tmp/unit_$(basename "$target")_$$.log"
  fi

  PID_FILE="${LOG_FILE%.log}.pid"

  # Pastikan direktori log ada
  mkdir -p "$(dirname "$LOG_FILE")"

  # Export variabel dan fungsi yang dibutuhkan subshell
  export TMP_DIR SATRIA target units LOG_FILE
  export -f clean create_tmp copy_target run 2> /dev/null || true

  # Jalankan fungsi run di subshell background dengan nohup
  nohup bash -c '
    cd "$TMP_DIR/$(basename "$target")" || exit 1
    clean > /dev/null 2>&1
    run
  ' > "$LOG_FILE" 2>&1 &

  BG_PID=$!
  echo "$BG_PID" > "$PID_FILE"

  echo "========================================="
  echo "Running in background"
  echo "  PID  : $BG_PID"
  echo "  Log  : $LOG_FILE"
  echo "  PID  : $PID_FILE"
  echo "  Stop : kill $BG_PID"
  echo "========================================="
  exit 0
fi

# ---------------------------- mode foreground ------------------------------
# Jika -o diberikan, redirect semua output ke file + tampilkan di terminal
if [ -n "$LOG_FILE" ]; then
  # Pastikan direktori log ada
  mkdir -p "$(dirname "$LOG_FILE")"
  # Redirect stdout+stderr ke file, sekaligus tampilkan di terminal (tee)
  exec > >(tee "$LOG_FILE") 2>&1
  echo "> Logging to : $LOG_FILE"
fi

# Jalankan clean, tunggu selesai, baru jalankan run
clean > /dev/null 2>&1 &
wait $! && run

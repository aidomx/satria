#!/usr/bin/env bash
set -uo pipefail

# =============================================================================
# scripts/unit.sh  Test runner Satria (satu base direktori)
# =============================================================================
#
# Pemakaian:
#   bash scripts/unit.sh [binary] [target] [opsi]
#
#   binary  path satria yang diuji       (default: <root>/bin/satria)
#   target  direktori fixture            (default: <root>/tests/miniws)
#
#   Catatan: satu argumen posisi dianggap TARGET (kompatibel dengan pemanggilan
#   CI lama: `scripts/unit.sh -o log -j N <suite>`).
#
# Opsi:
#   -o <file>   Simpan output ringkasan ke file (default: $BASE/log/unit.log)
#   -j <N>      Batasi job satria per tes (default 2). -j 0 = tanpa batas.
#   -b          Jalankan di background (PID di $BASE/log/unit.pid)
#   -h          Tampilkan bantuan ini
#
# Base direktori (satu-satunya tempat artefak runner):
#   UNIT_BASE   default /tmp/miniws. Di dalamnya:
#     fixture/  salinan target (dibersihkan & disalin ulang tiap run)
#     t/        folder temp per tes (menggantikan mktemp; dihapus tiap tes)
#     bin/      wrapper satria-j (pembatas -j)
#     log/      log per tes (<nama>.log), ringkasan (unit.log), pid
#
# Environment:
#   CLEAN_VERBOSE=1   tampilkan detail clean()
#   UNIT_BASE         ganti base direktori
#
# Tes di tests/unit/*.sh menerima:  $1 = binary, $2 = target
# dan memakai $UNIT_TMP/<nama> (di-set runner) sebagai folder kerjanya.
# =============================================================================

usage() {
  # Cetak blok komentar header (dari baris kedua sampai sebelum `set`)
  awk 'NR==1{next} /^usage\(\)/{exit} /^set /{next} /^# =+$/{next} {sub(/^# ?/,""); print}' "$0"
}

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_BIN="$ROOT_DIR/bin/satria"
DEFAULT_TARGET="$ROOT_DIR/tests/miniws"

BASE="${UNIT_BASE:-/tmp/miniws}"
FIX="$BASE/fixture"
TDIR="$BASE/t"
WRAPDIR="$BASE/bin"
LOGDIR="$BASE/log"

# ---------------------------- parsing argumen ------------------------------
LOG_FILE=""
BACKGROUND=0
JOBS=2
positional=()

while [ $# -gt 0 ]; do
  case "$1" in
    -o)
      [ $# -ge 2 ] || { echo "Error: -o memerlukan argumen file" >&2; exit 1; }
      LOG_FILE="$2"; shift 2 ;;
    -b)
      BACKGROUND=1; shift ;;
    -j)
      [ $# -ge 2 ] || { echo "Error: -j memerlukan argumen angka" >&2; exit 1; }
      JOBS="$2"; shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    -*)
      echo "Opsi tidak dikenal: $1" >&2
      echo "Pemakaian: $0 [binary] [target] [-o <file>] [-j <N>] [-b]" >&2
      exit 1 ;;
    *)
      positional+=("$1"); shift ;;
  esac
done

case ${#positional[@]} in
  0) SAT_IN="$DEFAULT_BIN"; TARGET_IN="$DEFAULT_TARGET" ;;
  1)
    if [ -d "${positional[0]}" ]; then
      SAT_IN="$DEFAULT_BIN"; TARGET_IN="${positional[0]}"
    else
      echo "Error: argumen tunggal harus direktori target: ${positional[0]}" >&2
      echo "       untuk binary + target: $0 <binary> <target>" >&2
      exit 1
    fi ;;
  2) SAT_IN="${positional[0]}"; TARGET_IN="${positional[1]}" ;;
  *) echo "Error: terlalu banyak argumen posisi (maks: binary target)" >&2; exit 1 ;;
esac

# Binary: harus file yang dapat dieksekusi; dijadikan path absolut
if [ ! -x "$SAT_IN" ] || [ -d "$SAT_IN" ]; then
  echo "Error: binary tidak ditemukan/dapat dieksekusi: $SAT_IN" >&2
  exit 1
fi
SATRIA="$(cd "$(dirname "$SAT_IN")" && pwd)/$(basename "$SAT_IN")"

# Target: direktori; path absolut; '/' ditolak (tes menyalin target)
if [ ! -d "$TARGET_IN" ]; then
  echo "Error: target tidak ditemukan: $TARGET_IN" >&2
  exit 1
fi
TARGET="$(cd "$TARGET_IN" && pwd)"
if [ "$TARGET" = "/" ]; then
  echo "Error: target tidak valid: / (berikan direktori fixture, mis. tests/miniws)" >&2
  exit 1
fi

case "$JOBS" in
  '' | *[!0-9]*)
    echo "Error: -j harus bilangan bulat >= 0 (dapat '$JOBS')" >&2
    exit 1 ;;
esac

# Proses anak (mode background): kill <PID> harus menghentikan seluruh grup,
# termasuk tes dan satria yang dijalankan tes.
if [ -n "${UNIT_CHILD:-}" ]; then
  # Hanya grup proses sendiri (dibuat oleh job control di pemanggil); JANGAN kill 0.
  trap 'kill -- -$$ 2> /dev/null' TERM INT
fi

# ---------------------------- background ----------------------------------
if [ "$BACKGROUND" -eq 1 ] && [ -z "${UNIT_CHILD:-}" ]; then
  mkdir -p "$LOGDIR"
  [ -n "$LOG_FILE" ] || LOG_FILE="$LOGDIR/unit.log"
  PID_FILE="$LOGDIR/unit.pid"
  # set -m: proses anak dapat grup proses sendiri (kill <PID> hanya mengenainya)
  set -m
  UNIT_CHILD=1 nohup bash "$0" "$SATRIA" "$TARGET" -o "$LOG_FILE" -j "$JOBS" \
    > /dev/null 2>&1 &
  set +m
  BG_PID=$!
  echo "$BG_PID" > "$PID_FILE"
  echo "=========================================="
  echo "Running in background"
  echo "  PID  : $BG_PID"
  echo "  Log  : $LOG_FILE"
  echo "  PID  : $PID_FILE"
  echo "  Stop : kill $BG_PID"
  echo "=========================================="
  exit 0
fi

# ---------------------------- setup base -----------------------------------
mkdir -p "$TDIR" "$LOGDIR" "$WRAPDIR"
[ -n "$LOG_FILE" ] || LOG_FILE="$LOGDIR/unit.log"

# Pembatas -j: wrapper di base/bin. Parse satria mengambil -j TERAKHIR,
# jadi -j eksplisit di tes tetap menang.
if [ "$JOBS" -ge 1 ]; then
  SATRIA_WRAP="$WRAPDIR/satria-j"
  printf '#!/bin/sh\nexec "%s" -j%d "$@"\n' "$SATRIA" "$JOBS" > "$SATRIA_WRAP"
  chmod +x "$SATRIA_WRAP"
  SATRIA_ARG="$SATRIA_WRAP"
else
  SATRIA_ARG="$SATRIA"
fi

# Fixture: salinan target di base/fixture (dibersihkan & disalin ulang)
rm -rf "$FIX" 2> /dev/null
cp -a "$TARGET" "$FIX" || { echo "Error: gagal menyalin target ke $FIX" >&2; exit 1; }

export UNIT_BASE="$BASE"
export UNIT_TMP="$TDIR"

# Daftar artefak yang dibersihkan dari fixture sebelum tes
clean_cands=("bin" "build" "dist" "lib" "modules" ".satria" "compile_commands.json")
cands=()
for c in "${clean_cands[@]}"; do
  cands+=("$FIX/$c")
done

# =============================================================================
# clean()  dari versi sebelumnya (tidak berubah perilakunya)
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

# ---------------------------- fungsi utama ---------------------------------
units=$(find "$ROOT_DIR/tests/unit" -name "*.sh" | sort)

run() {
  echo "=========================================="
  echo "Running tests for: $(basename "$TARGET")"
  echo "  binary : $SATRIA"
  echo "  base   : $BASE"
  echo "=========================================="
  local u name
  for u in $units; do
    name="$(basename "$u" .sh)"
    printf "> Running %s...\n" "$name.sh"
    # Satu folder temp per tes: dikosongkan sebelum & sesudah tes
    rm -rf "$TDIR" 2> /dev/null
    mkdir -p "$TDIR"
    sh "$u" "$SATRIA_ARG" "$TARGET" 2>&1 | tee "$LOGDIR/$name.log"
    rm -rf "$TDIR" 2> /dev/null
  done
  echo "=========================================="
  echo "Ringkasan: PASS=$(grep -h '^PASS' "$LOGDIR"/*.log 2> /dev/null | wc -l | tr -d ' ')" \
       "FAIL=$(grep -h '^FAIL' "$LOGDIR"/*.log 2> /dev/null | wc -l | tr -d ' ')"
  echo "Log per tes: $LOGDIR/<nama>.log"
  echo "=========================================="
}

# ---------------------------- eksekusi -------------------------------------
echo "> Logging to : $LOG_FILE"
mkdir -p "$(dirname "$LOG_FILE")"
exec > >(tee "$LOG_FILE") 2>&1

cd "$FIX" || exit 1

# Jalankan clean, tunggu selesai, baru jalankan run (perilaku sama dengan versi lama)
clean > /dev/null 2>&1 &
wait $! && run

#!/usr/bin/env bash
# bench_check.sh — build (opsional) lalu jalankan skenario bench/ws, cetak
# verdict + jejak stderr dalam SATU langkah.
#
# Pakai:
#   scripts/bench_check.sh                      # skenario mod, build dulu
#   scripts/bench_check.sh -N                   # pakai binary sekarang (tanpa build)
#   scripts/bench_check.sh -s dep -s mod        # beberapa skenario
#   scripts/bench_check.sh -t 'DECIDE|MEMO'     # tambahan pola jejak stderr
#   scripts/bench_check.sh -o /tmp/x -k         # outdir sendiri, simpan fixture
#   scripts/bench_check.sh -b                   # jalankan di background
#   scripts/bench_check.sh -b -l /tmp/bench.log # background + log ke file spesifik
#
# Exit: 0 bila semua skenario "in accordance", 1 bila melenceng, 2 bila build gagal.

set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/bin/satria"
OUT=/tmp/bench_check
BUILD=1
KEEP=1
NSRC=100 NHEAD=20 NMOD=20 NRUN=1
SCEN=()
TRACE='Not in accordance|✓|✗|GAGAL|compile='
BACKGROUND=0
MAIN_LOG=""

usage() {
  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

# ---------------------------------------------------------------------------
# Parsing argumen
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
  case $1 in
    -s)
      SCEN+=("$2")
      shift 2
      ;;
    -o)
      OUT="$2"
      shift 2
      ;;
    -l)
      MAIN_LOG="$2"
      shift 2
      ;;
    -n)
      NSRC="$2"
      shift 2
      ;;
    -H)
      NHEAD="$2"
      shift 2
      ;;
    -m)
      NMOD="$2"
      shift 2
      ;;
    -r)
      NRUN="$2"
      shift 2
      ;;
    -t)
      TRACE="$TRACE|$2"
      shift 2
      ;;
    -N)
      BUILD=0
      shift
      ;;
    -k)
      KEEP=1
      shift
      ;;
    -K)
      KEEP=0
      shift
      ;;
    -b)
      BACKGROUND=1
      shift
      ;;
    -h | --help) usage ;;
    *)
      echo "argumen tak dikenal: $1" >&2
      usage
      ;;
  esac
done

[ ${#SCEN[@]} -eq 0 ] && SCEN=(mod)

# ---------------------------------------------------------------------------
# Mode Background — SELF-REINVOKE (paling robust)
# ---------------------------------------------------------------------------
# Daripada export variabel + declare -f yang rapuh (array tidak bisa di-export),
# script memanggil dirinya sendiri tanpa flag -b. Ini menjamin semua parsing
# argumen berjalan normal dan semua variabel ter-set dengan benar.
# ---------------------------------------------------------------------------
if [ "$BACKGROUND" = 1 ]; then
  if [ -z "$MAIN_LOG" ]; then
    MAIN_LOG="/tmp/bench_check_$$.log"
  fi

  PID_FILE="${MAIN_LOG%.log}.pid"
  mkdir -p "$(dirname "$MAIN_LOG")"

  # Rekonstruksi argumen TANPA -b dan -l (karena sudah ditangani di luar)
  REMAINING_ARGS=()
  [ "$BUILD" = 0 ] && REMAINING_ARGS+=("-N")
  [ "$KEEP" = 1 ] && REMAINING_ARGS+=("-k")
  [ "$KEEP" = 0 ] && REMAINING_ARGS+=("-K")
  [ "$NSRC" != 100 ] && REMAINING_ARGS+=("-n" "$NSRC")
  [ "$NHEAD" != 20 ] && REMAINING_ARGS+=("-H" "$NHEAD")
  [ "$NMOD" != 20 ] && REMAINING_ARGS+=("-m" "$NMOD")
  [ "$NRUN" != 1 ] && REMAINING_ARGS+=("-r" "$NRUN")
  [ "$OUT" != "/tmp/bench_check" ] && REMAINING_ARGS+=("-o" "$OUT")
  for s in "${SCEN[@]}"; do
    REMAINING_ARGS+=("-s" "$s")
  done
  [ "$TRACE" != 'Not in accordance|✓|✗|GAGAL|compile=' ] && REMAINING_ARGS+=("-t" "$TRACE")

  # Jalankan diri sendiri di background dengan nohup + disown
  nohup "$0" "${REMAINING_ARGS[@]}" > "$MAIN_LOG" 2>&1 &
  BG_PID=$!
  disown "$BG_PID" 2> /dev/null || true

  echo "$BG_PID" > "$PID_FILE"

  echo "========================================="
  echo "Running bench_check in background"
  echo "  PID      : $BG_PID"
  echo "  Log      : $MAIN_LOG"
  echo "  PID File : $PID_FILE"
  echo "  Monitor  : tail -f $MAIN_LOG"
  echo "  Stop     : kill $BG_PID"
  echo "========================================="
  exit 0
fi

# ---------------------------------------------------------------------------
# Mode Foreground — Eksekusi Utama
# ---------------------------------------------------------------------------
# Jika -l diberikan, redirect semua output ke file + tampilkan di terminal (tee)
if [ -n "$MAIN_LOG" ]; then
  mkdir -p "$(dirname "$MAIN_LOG")"
  exec > >(tee "$MAIN_LOG") 2>&1
  echo "> Logging to : $MAIN_LOG"
fi

if [ "$BUILD" = 1 ]; then
  echo "▸ build: ./install.sh --dev -o $OUT.build.log"
  (cd "$ROOT" && ./install.sh --dev -o "$OUT.build.log") || {
    echo "✗ build gagal — lihat $OUT.build.log" >&2
    exit 2
  }
fi

kflag=""
[ "$KEEP" = 1 ] && kflag="-k"

rm -rf "$OUT"
mkdir -p "$OUT"

for s in "${SCEN[@]}"; do
  echo "▸ skenario: $s"
  taskset -c 0-5 bash "$ROOT/bench/ws.sh" -b "$BIN" -n "$NSRC" -H "$NHEAD" \
    -m "$NMOD" -r "$NRUN" -t satria -s "$s" -o "$OUT.$s" $kflag \
    > "$OUT.$s.log" 2>&1
done

rc=0
for s in "${SCEN[@]}"; do
  log="$OUT.$s.log"
  echo "── $s ──────────────────────────────────────────────"
  grep -E "$TRACE" "$log" | sed 's/^/  /'

  if [ -f "$OUT.$s/stderr.log" ] && [ -s "$OUT.$s/stderr.log" ]; then
    echo "  stderr:"
    grep -E "$TRACE|^\[" "$OUT.$s/stderr.log" | tail -20 | sed 's/^/    /'
  fi

  grep -q "Not in accordance\|GAGAL" "$log" && rc=1
done

echo "────────────────────────────────────────────────────"
if [ $rc -eq 0 ]; then
  echo "✓ semua skenario in accordance"
else
  echo "✗ ada skenario melenceng (log: $OUT.<skenario>.log)"
fi

exit $rc

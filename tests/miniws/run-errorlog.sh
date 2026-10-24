#!/usr/bin/env bash
# ============================================================================
# run-errorlog.sh — jalankan build workspace tests/rupaws, simpan output
# LENGKAP (stdout + stderr) ke error.log tanpa terpotong.
#
# error.log lama hanya berisi stdout (baris status [N/M] OK) — diagnostik
# compiler di stderr (termasuk pesan "recompile with -fPIC") tidak terekam.
# Script ini menyimpan keduanya, plus exit code per langkah.
#
# Pemakaian:
#   ./run-errorlog.sh                  # build semua proyek (rbot -w)
#   ./run-errorlog.sh -j2              # batasi paralel (hemat memori/CPU)
#   ./run-errorlog.sh -j1              # serial — urutan error paling terbaca
#   ./run-errorlog.sh --clean          # rbot -w clean dulu, lalu build penuh
#   ./run-errorlog.sh --only rupa      # hanya proyek itu (+ dependency-nya)
#   RBOT=/path/ke/rbot ./run-errorlog.sh
#
# Hasil di tests/rupaws/:
#   error.log         — gabungan stdout+stderr + exit code per langkah
#   error.stdout.log  — stdout mentah (urutan asli)
#   error.stderr.log  — stderr mentah (diagnostik compiler)
# ============================================================================

set -u
cd "$(dirname "$0")"

RBOT=${RBOT:-../../bin/rbot}
JOBS=""
CLEAN=0
ONLY=""

while [ $# -gt 0 ]; do
  case $1 in
    -j) JOBS=$2; shift 2 ;;
    -j*) JOBS=${1#-j}; shift ;;
    --clean) CLEAN=1; shift ;;
    --only) ONLY=$2; shift 2 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "argumen tidak dikenal: $1" >&2; exit 2 ;;
  esac
done

[ -x "$RBOT" ] || {
  echo "rbot tidak ditemukan/dieksekusi: $RBOT" >&2
  echo "build dulu di root repo (rbot -jN) atau set RBOT=/path/rbot" >&2
  exit 1
}
RBOT=$(cd "$(dirname "$RBOT")" && pwd)/$(basename "$RBOT") # absolutkan

OUT=error.log
SO=error.stdout.log
SE=error.stderr.log
NCPU=$(nproc 2>/dev/null || echo 4)

# stdbuf (bila ada) memaksa line-buffering agar stdout & stderr tidak
# saling menimpa urutan; di Termux stdbuf ada di coreutils.
run() { # run <label> <arg rbot...>
  local label=$1; shift
  echo "==================================================================="
  echo ">>> $label : rbot $*"
  if command -v stdbuf >/dev/null 2>&1; then
    stdbuf -oL -eL "$RBOT" "$@" >"$SO" 2>"$SE"
  else
    "$RBOT" "$@" >"$SO" 2>"$SE"
  fi
  local rc=$?
  echo "--- stdout ($label) ---"
  cat "$SO"
  echo "--- stderr ($label) ---"
  cat "$SE"
  echo "--- exit($label) = $rc ---"
  echo
  return $rc
}

{
  echo "=== rbot error capture — $(date '+%Y-%m-%d %H:%M:%S') ==="
  echo "rbot : $RBOT ($("$RBOT" version 2>&1 | head -1))"
  echo "cpu  : $NCPU"
  echo "args : ${JOBS:+-j$JOBS }${CLEAN:+--clean }${ONLY:+-w $ONLY}"
} >"$OUT"

rc=0
if [ "$CLEAN" = 1 ]; then
  run "clean" -w clean >>"$OUT" 2>&1 || rc=1
fi
if [ -n "$ONLY" ]; then
  run "build $ONLY" -j"${JOBS:-$NCPU}" -w "$ONLY" >>"$OUT" 2>&1 || rc=1
else
  run "build" -j"${JOBS:-$NCPU}" -w >>"$OUT" 2>&1 || rc=1
fi

echo "=== selesai: rc=$rc ==="
echo "log lengkap : $PWD/$OUT ($(wc -l <"$OUT") baris, $(wc -c <"$OUT") byte)"
echo "stdout mentah: $PWD/$SO"
echo "stderr mentah: $PWD/$SE"

# Ringkasan cepat bagian yang menarik (tanpa menelan seluruh log)
HITS=$(grep -nE 'error:|warning:.*fPIC|recompile with|undefined referen|FAIL|failed|Error|cannot' "$OUT" | head -40)
if [ -n "$HITS" ]; then
  echo
  echo "--- potongan yang mungkin error (maks 40) ---"
  printf '%s\n' "$HITS"
fi

exit $rc

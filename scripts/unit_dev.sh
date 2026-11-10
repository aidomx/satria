#!/usr/bin/env bash
# scripts/unit_dev.sh - runner unit test dev (design/unit_test.md)
#
#   bash scripts/unit_dev.sh <binary> <target> [-b] [-j N] [-o <log>] [-t <tes>]
#   bash scripts/unit_dev.sh start <binary> <target> [...]   # sama dengan atas
#   bash scripts/unit_dev.sh stop <target>                   # kill run aktif
#   bash scripts/unit_dev.sh pause <target>                  # berhenti setelah tes berjalan selesai
#   bash scripts/unit_dev.sh resume <target> [-b] [-j N] [-o <log>]  # lanjut sisa run
#   bash scripts/unit_dev.sh -w [target]
#
# Tahap: discovery -> staging (common/, lib/, classes.conf, test.state)
#        -> validasi klasifikasi -> fixture -> snap (bila ada tes [snap])
#        -> fase cold lalu fase snap, setiap tes di folder work.
#
# -w  watcher (design §9): memantau KEMAJUAN run di area staging
#     ($UNIT_STAGE/targets/<nama>/log), bukan sumber repo. Tanpa argumen:
#     target satu-satunya run aktif (bila ada beberapa run, sebutkan target);
#     dengan <target>: target itu. Read-only; berhenti dengan Ctrl-C.
#     Interval poll: UNIT_WATCH_INTERVAL (detik, default 1).
set -uo pipefail

# Paths
# UNIT_ROOT diteruskan unit.sh (loader berjalan dari snapshot di scripts/.unit.<pid>.d).
ROOT="${UNIT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
TESTS_DIR="${UNIT_TESTS_DIR:-$ROOT/tests/unit}"
STAGE="${UNIT_STAGE:-/tmp/satria-unit}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Snapshot handler: folder scripts/.unit.<pid>.d dibuat unit.sh; dihapus saat proses
# yang memakainya berakhir (lihat unit.sh).
SNAP_DIR=""
case "$SCRIPT_DIR" in */.unit.*.d) SNAP_DIR="$SCRIPT_DIR" ;; esac
drop_copy() { [ -z "$SNAP_DIR" ] || rm -rf "$SNAP_DIR"; }
trap drop_copy EXIT

die() {
  echo "Error: $*" >&2
  exit 1
}

say() { echo "$*"; }

usage() { sed -n '2,9p' "$0"; }

# Load modules
source "$SCRIPT_DIR/lib/unit_common.sh"
source "$SCRIPT_DIR/lib/unit_watcher.sh"
source "$SCRIPT_DIR/lib/unit_runner.sh"

# Global state
RUN_T0="$(now_ms)"
export UNIT_REPO="$ROOT"
export SATRIA_CMD="$ROOT/src/cmd.txt"
[ -f "$SATRIA_CMD" ] || die "src/cmd.txt tidak ada di $ROOT"

# Parse arguments
MODE="run" # run (default) | stop | resume — subcommand kata pertama, sisanya opsi
BG=0
JOBS=2
LOG=""
CLEAN=0
WATCH=0
POS=()
SEL=()

case "${1-}" in
  start)
    MODE="run"
    shift
    ;;
  stop)
    MODE="stop"
    shift
    ;;
  pause)
    MODE="pause"
    shift
    ;;
  resume)
    MODE="resume"
    shift
    ;;
esac

while [ $# -gt 0 ]; do
  case "$1" in
    -b)
      BG=1
      shift
      ;;
    -w | --watch)
      WATCH=1
      shift
      ;;
    -j)
      [ $# -ge 2 ] || die "-j memerlukan angka"
      JOBS="$2"
      shift 2
      ;;
    -o)
      [ $# -ge 2 ] || die "-o memerlukan path log"
      LOG="$2"
      shift 2
      ;;
    -t)
      [ $# -ge 2 ] || die "-t memerlukan nama tes"
      SEL+=("$2")
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    -*) die "opsi tidak dikenal: $1" ;;
    *)
      POS+=("$1")
      shift
      ;;
  esac
done

case "$JOBS" in '' | *[!0-9]*) die "-j harus bilangan bulat >= 0" ;; esac

# Dispatch
if [ "$MODE" = "stop" ]; then
  [ "$WATCH" -eq 0 ] || die "stop tidak bisa digabung dengan -w"
  [ "$BG" -eq 0 ] || die "stop tidak bisa digabung dengan -b"
  [ "$CLEAN" -eq 0 ] || die "stop tidak bisa digabung dengan --clean"
  [ "${#SEL[@]}" -eq 0 ] || die "stop tidak menerima -t"
  stop_run
elif [ "$MODE" = "pause" ]; then
  [ "$WATCH" -eq 0 ] || die "pause tidak bisa digabung dengan -w"
  [ "$BG" -eq 0 ] || die "pause tidak bisa digabung dengan -b"
  [ "$CLEAN" -eq 0 ] || die "pause tidak bisa digabung dengan --clean"
  [ "${#SEL[@]}" -eq 0 ] || die "pause tidak menerima -t"
  pause_run
elif [ "$MODE" = "resume" ]; then
  [ "$WATCH" -eq 0 ] || die "resume tidak bisa digabung dengan -w"
  [ "$CLEAN" -eq 0 ] || die "resume tidak bisa digabung dengan --clean"
  [ "${#SEL[@]}" -eq 0 ] || die "resume menolak -t — seleksi otomatis dari sisa plan"
  rm -f "$STAGE/targets/${POS[0]}/log/pause.flag" 2> /dev/null
  resume_run
elif [ "$WATCH" -eq 1 ]; then
  watcher_init
  watcher_run
else
  runner_run
fi

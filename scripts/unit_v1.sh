#!/usr/bin/env bash
# scripts/unit_v1.sh - runner unit test v1 (design/unit_test.md, Ruang lingkup v1)
#
#   bash scripts/unit_v1.sh <binary> <target> [-b] [-j N] [-o <log>] [-t <tes>]
#
# Tahap: discovery -> staging (common/, lib/, classes.conf, test.state)
#        -> validasi klasifikasi -> fixture -> snap (bila ada tes [snap])
#        -> fase cold lalu fase snap, setiap tes di folder work.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TESTS_DIR="${UNIT_TESTS_DIR:-$ROOT/tests/unit}"
STAGE="${UNIT_STAGE:-/tmp/satria-unit}"

die() { echo "Error: $*" >&2; exit 1; }
# Waktu dalam milidetik. Pakai %N bila didukung (GNU date), selain itu detik*1000.
now_ms() {
  local t
  t="$(date +%s%3N 2>/dev/null)"
  case "$t" in *N*|'') t="$(( $(date +%s) * 1000 ))" ;; esac
  echo "$t"
}
RUN_T0="$(now_ms)"
# Lingkungan yang disediakan runner (tes tidak menebak lokasi dari $0):
#   UNIT_REPO   root repo Satria (sumber cmd.txt, scripts/, src/)
#   SATRIA_CMD  sumber bantuan binary (src/cmdhelp.h, urutan pertama)
export UNIT_REPO="$ROOT"
export SATRIA_CMD="$ROOT/src/cmd.txt"
[ -f "$SATRIA_CMD" ] || die "src/cmd.txt tidak ada di $ROOT"
say() { echo "$*"; }

usage() { sed -n '2,6p' "$0"; }

# ---------------------------------------------------------------- argumen
BG=0; JOBS=2; LOG=""; CLEAN=0; POS=(); SEL=()
while [ $# -gt 0 ]; do
  case "$1" in
    -b) BG=1; shift ;;
    -j) [ $# -ge 2 ] || die "-j memerlukan angka"; JOBS="$2"; shift 2 ;;
    -o) [ $# -ge 2 ] || die "-o memerlukan path log"; LOG="$2"; shift 2 ;;
    -t) [ $# -ge 2 ] || die "-t memerlukan nama tes"; SEL+=("$2"); shift 2 ;;
    --clean) CLEAN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) die "opsi tidak dikenal: $1" ;;
    *) POS+=("$1"); shift ;;
  esac
done
[ "${#POS[@]}" -eq 2 ] || { usage >&2; die "perlu tepat dua argumen: <binary> <target>"; }
case "$JOBS" in ''|*[!0-9]*) die "-j harus bilangan bulat >= 0";; esac

# ---------------------------------------------------------------- discovery
BIN_IN="${POS[0]}"; TNAME="${POS[1]}"
[ -f "$BIN_IN" ] && [ -x "$BIN_IN" ] || die "binary tidak ditemukan atau tidak dapat dieksekusi: $BIN_IN"
BIN_ABS="$(cd "$(dirname "$BIN_IN")" && pwd)/$(basename "$BIN_IN")"
TGT_SRC="$ROOT/tests/$TNAME"
[ -d "$TGT_SRC" ] || die "target tidak ditemukan: tests/$TNAME"
TGT_SRC="$(cd "$TGT_SRC" && pwd)"
case "$TNAME" in ''|*/*|.*) die "nama target tidak valid: $TNAME" ;; esac
[ -d "$TESTS_DIR/common" ] || die "tests/unit/common tidak ada: $TESTS_DIR"
[ -f "$TESTS_DIR/classes.conf" ] || die "classes.conf tidak ada: $TESTS_DIR/classes.conf"

# ---------------------------------------------------------------- clean
# Design 2.5: --clean hanya menghapus targets/<nama>/, tidak menyentuh
# target lain, common/, lib/, classes.conf, dan test.state.
if [ "$CLEAN" -eq 1 ]; then
  T="$STAGE/targets/$TNAME"
  rm -rf "$T"
  echo "dihapus: $T"
  exit 0
fi

BIN_SHA="$(sha256sum "$BIN_ABS" | cut -c1-64)"
BIN_SHA8="${BIN_SHA:0:8}"

T="$STAGE/targets/$TNAME"
LOGDIR="$T/log"
mkdir -p "$LOGDIR" || die "gagal membuat $LOGDIR"
[ -n "$LOG" ] || LOG="$LOGDIR/unit.log"

# lock per target: run lain pada target yang sama ditolak
if [ -f "$LOGDIR/run.pid" ]; then
  OLDPID="$(cat "$LOGDIR/run.pid" 2>/dev/null)"
  if [ -n "$OLDPID" ] && [ "$OLDPID" != "$$" ] && kill -0 "$OLDPID" 2>/dev/null; then
    die "run lain masih aktif pada target $TNAME (PID $OLDPID). Hentikan dulu: kill $OLDPID"
  fi
fi

# ---------------------------------------------------------------- background
if [ "$BG" -eq 1 ] && [ -z "${UNIT_V1_CHILD:-}" ]; then
  set -m
  UNIT_V1_CHILD=1 nohup bash "$0" "$BIN_IN" "$TNAME" -j "$JOBS" -o "$LOG" \
    > /dev/null 2>&1 &
  CPID=$!
  set +m
  echo "$CPID" > "$LOGDIR/bg.pid"
  say "Berjalan di background"
  say "  PID : $CPID"
  say "  Log : $LOG"
  say "  Stop: kill $CPID"
  exit 0
fi

kill_children() {
  local kid
  for kid in $(ps -eo pid=,ppid= 2>/dev/null | awk -v P="$1" '$2==P {print $1}'); do
    kill_children "$kid"; kill -9 "$kid" 2>/dev/null
  done
}
if [ -n "${UNIT_V1_CHILD:-}" ]; then
  trap 'kill_children $$; rm -f "$LOGDIR/run.pid"; exit 130' TERM INT HUP
fi

# dari sini semua keluaran masuk ke log
mkdir -p "$(dirname "$LOG")"
: > "$LOG"
exec > >(tee -a "$LOG") 2>&1

echo "$$" > "$LOGDIR/run.pid"
trap 'rm -f "$LOGDIR/run.pid"' EXIT

# ---------------------------------------------------------------- staging
[ -n "$STAGE" ] && [ "$STAGE" != "/" ] || die "STAGE tidak valid"
mkdir -p "$STAGE/bin" || die "gagal membuat $STAGE"

src_fingerprint() {
  (cd "$TESTS_DIR" && find common lib -type f | sort | xargs sha256sum; sha256sum classes.conf) 2>/dev/null
}
FP_NOW="$(src_fingerprint | sha256sum | cut -c1-64)"
STATE="$STAGE/test.state"
state_valid() {
  [ -f "$STATE" ] || return 1
  grep -q "^SRC_FINGERPRINT=$FP_NOW\$" "$STATE" || return 1
  local f sha
  while read -r f sha; do
    [ -f "$STAGE/$f" ] || return 1
    [ "$(cd "$STAGE" && sha256sum "$f" | cut -c1-64)" = "$sha" ] || return 1
  done < <(grep -E '^  ' "$STATE")
  return 0
}
if state_valid; then
  say "> Staging : sama dengan test.state (tidak disalin ulang)"
else
  say "> Staging : sinkronkan common/, lib/, classes.conf"
  rm -rf "$STAGE/common" "$STAGE/lib"
  cp -a "$TESTS_DIR/common" "$TESTS_DIR/lib" "$STAGE/" || die "gagal menyalin tests/unit"
  cp "$TESTS_DIR/classes.conf" "$STAGE/classes.conf" || die "gagal menyalin classes.conf"
  {
    echo "TEST_STATE_VERSION=1"
    echo "SRC_FINGERPRINT=$FP_NOW"
    echo "FILES:"
    (cd "$STAGE" && find common lib -type f | sort | xargs sha256sum; sha256sum classes.conf) \
      | awk '{print "  " $2 " " substr($1,1,64)}'
  } > "$STATE.tmp" && mv -f "$STATE.tmp" "$STATE"
fi

# shim sleep: mencatat durasi yang diminta per tes, lalu meneruskan ke sleep asli
REAL_SLEEP="$(command -v sleep)" || die "sleep tidak ditemukan"
export UNIT_REAL_SLEEP="$REAL_SLEEP"
export UNIT_JOBS="$JOBS" # nilai -j milik runner; tes memakai -j${UNIT_JOBS:-2}
mkdir -p "$STAGE/shim"
cat > "$STAGE/shim/sleep" <<'SHIM'
#!/bin/sh
[ -n "$UNIT_SLEEP_LOG" ] && echo "${UNIT_TEST_NAME:-?} $1" >> "$UNIT_SLEEP_LOG"
exec "$UNIT_REAL_SLEEP" "$@"
SHIM
chmod +x "$STAGE/shim/sleep"
export PATH="$STAGE/shim:$PATH"
SLEEP_LOG="$LOGDIR/sleep.log"
export UNIT_SLEEP_LOG="$SLEEP_LOG"

# binary staging: versi immutable, dipin per run
PIN="$STAGE/bin/satria-$BIN_SHA8"
if [ ! -x "$PIN" ]; then
  cp "$BIN_ABS" "$PIN.tmp" && chmod +x "$PIN.tmp" && mv -f "$PIN.tmp" "$PIN" \
    || die "gagal menyalin binary ke $PIN"
fi

# ---------------------------------------------------------------- validasi klasifikasi
COLD=(); SNAP=()
SEC=""
while IFS= read -r line || [ -n "$line" ]; do
  line="${line%%#*}"; line="$(echo "$line" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
  [ -z "$line" ] && continue
  case "$line" in
    "[cold]") SEC=cold; continue ;;
    "[snap]") SEC=snap; continue ;;
    \[*) die "classes.conf: bagian tidak dikenal: $line" ;;
  esac
  [ -n "$SEC" ] || die "classes.conf: entri '$line' di luar bagian"
  [ -f "$STAGE/common/$line.sh" ] || die "classes.conf: '$line' tidak punya berkas common/$line.sh"
  if [ "$SEC" = cold ]; then COLD+=("$line"); else SNAP+=("$line"); fi
done < "$STAGE/classes.conf"

ALL_TESTS=()
for f in "$STAGE"/common/*.sh; do ALL_TESTS+=("$(basename "$f" .sh)"); done
for t in "${ALL_TESTS[@]}"; do
  n=0
  for c in "${COLD[@]}" "${SNAP[@]}"; do [ "$c" = "$t" ] && n=$((n+1)); done
  [ "$n" -eq 1 ] || die "tes '$t' belum diklasifikasikan (atau terdaftar ganda) di classes.conf"
done

for t in "${SNAP[@]}"; do
  if grep -qE 'rm -rf[^|;&]*(\.satria|/build|/bin|/lib)([^A-Za-z_]|$)' "$STAGE/common/$t.sh"; then
    die "tes [snap] '$t' menghapus state build. Pindahkan ke [cold] atau tulis ulang."
  fi
done

say "> Klasifikasi : ${#COLD[@]} cold, ${#SNAP[@]} snap (dari ${#ALL_TESTS[@]} tes)"

# Seleksi: -t <tes> (boleh berulang) mempersempit run ke tes terpilih saja.
# Validasi tetap penuh: nama tak dikenal atau tak terdaftar digagalkan.
# Snap hanya dibangun bila ada tes [snap] yang terpilih.
if [ "${#SEL[@]}" -gt 0 ]; then
  NC=(); NS=()
  for s in "${SEL[@]}"; do
    ok=0
    for t in "${COLD[@]}"; do [ "$t" = "$s" ] && ok=1 && break; done
    for t in "${SNAP[@]}"; do [ "$t" = "$s" ] && ok=1 && break; done
    [ "$ok" -eq 1 ] || die "tes '-t $s' tidak ada di tests/unit/common atau tidak terdaftar di classes.conf"
    for t in "${COLD[@]}"; do [ "$t" = "$s" ] && { NC+=("$t"); break; }; done
    for t in "${SNAP[@]}"; do [ "$t" = "$s" ] && { NS+=("$t"); break; }; done
  done
  say "> Seleksi : ${#SEL[@]} tes (-t: ${SEL[*]}), cold $((${#SEL[@]} - ${#NS[@]})), snap ${#NS[@]}"
  if [ "${#NC[@]}" -gt 0 ]; then COLD=("${NC[@]}"); else COLD=(); fi
  if [ "${#NS[@]}" -gt 0 ]; then SNAP=("${NS[@]}"); else SNAP=(); fi
fi

# ---------------------------------------------------------------- fixture & snap
FIX="$T/fixture"; SNAPD="$T/snap"; W="$T/work"; TMPD="$T/t"
ARTIFACTS=(bin lib build modules dist .satria compile_commands.json)

rm -rf "$FIX"
cp -a "$TGT_SRC" "$FIX" || die "gagal menyalin fixture"
for a in "${ARTIFACTS[@]}"; do rm -rf "$FIX/$a"; done

WRAP="$T/bin/satria-j"
mkdir -p "$T/bin"
printf '#!/bin/sh\nexec "%s" -j%s "$@"\n' "$PIN" "$JOBS" > "$WRAP"
chmod +x "$WRAP"

FIX_SHA="$( (cd "$FIX" && find . -type f | sort | xargs sha256sum) | sha256sum | cut -c1-64)"
SNAP_ID="$(printf '%s|%s|%s' "$BIN_SHA8" "$TNAME" "$FIX_SHA" | sha256sum | cut -c1-64)"

if [ "${#SNAP[@]}" -gt 0 ]; then
  if [ -f "$SNAPD/snap.env" ] && grep -q "^SNAP_ID=$SNAP_ID\$" "$SNAPD/snap.env"; then
    say "> Snap : identitas cocok, tidak dibangun ulang"
  else
    say "> Snap : build cold sekali"
    rm -rf "$SNAPD"; cp -a "$FIX" "$SNAPD" || die "gagal menyalin fixture ke snap"
    T0=$(date +%s)
    ( cd "$SNAPD" && "$WRAP" -w ) > "$LOGDIR/snap-build.log" 2>&1
    RC=$?
    [ "$RC" -eq 0 ] || die "build snap gagal (rc=$RC), lihat $LOGDIR/snap-build.log"
    printf 'SNAP_ID=%s\nSNAP_BUILT_S=%s\n' "$SNAP_ID" "$(( $(date +%s) - T0 ))" > "$SNAPD/snap.env"
    say "> Snap : selesai dalam $(( $(date +%s) - T0 )) s"
  fi
fi

# timing per operasi (ms): snap-build dan setiap tes
TIMING="$LOGDIR/timing.tsv"
: > "$TIMING"
printf 'operasi\tfase\tms\tpass\tfail\tpre\tsleep_ms\twork_ms\n' >> "$TIMING"
[ -f "$SNAPD/snap.env" ] && printf 'snap-build\tsnap\t%s\t-\t-\t-\t-\t-\n' \
  "$(( $(grep '^SNAP_BUILT_S=' "$SNAPD/snap.env" | cut -d= -f2) * 1000 ))" >> "$TIMING"

# run.env (manifest, K5)
{
  echo "RUN_STARTED=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "RUN_BINARY=$PIN"
  echo "RUN_ORIGINAL=$BIN_ABS"
  echo "RUN_BINARY_SHA=$BIN_SHA"
  echo "RUN_PINNED=$PIN"
  echo "RUN_TARGET=$TNAME"
  echo "RUN_TARGET_SRC=$TGT_SRC"
  echo "RUN_JOBS=$JOBS"
  echo "RUN_BACKGROUND=$BG"
  echo "RUN_SNAP_ID=$SNAP_ID"
  echo "RUN_TESTS_COLD=${#COLD[@]}"
  echo "RUN_TESTS_SNAP=${#SNAP[@]}"
} > "$LOGDIR/run.env"

# ---------------------------------------------------------------- eksekusi
reset_work() {
  local mode="$1"
  rm -rf "$W"; cp -a "$FIX" "$W" || die "gagal reset work"
  if [ "$mode" = snap ]; then
    for a in .satria bin lib build modules dist compile_commands.json; do
      [ -e "$SNAPD/$a" ] && cp -a "$SNAPD/$a" "$W/"
    done
  fi
}

PASS=0; FAILN=0; PREN=0; TOT=0
RESULTS=()
run_phase() {
  local mode="$1"; shift
  local name
  for name in "$@"; do
    TOT=$((TOT+1))
    printf '> Running %s.sh [%s]...\n' "$name" "$mode"
    local t0 t1 ms
    t0="$(now_ms)"
    reset_work "$mode"
    rm -rf "$TMPD"; mkdir -p "$TMPD"
    : > "$SLEEP_LOG"
    ( cd "$W" && UNIT_TEST_NAME="$name" UNIT_TMP="$TMPD" UNIT_BASE="$T" \
        sh "$STAGE/common/$name.sh" "$WRAP" "$TGT_SRC" ) \
      2>&1 | tee "$LOGDIR/$name.log"
    t1="$(now_ms)"; ms=$((t1 - t0))
    sl="$(awk '{s+=$2} END{printf "%d", s*1000}' "$SLEEP_LOG")"; sl=${sl:-0}
    wk=$((ms - sl)); [ "$wk" -lt 0 ] && wk=0
    p=$(grep -c '^PASS' "$LOGDIR/$name.log"); f=$(grep -c '^FAIL' "$LOGDIR/$name.log")
    pc=$(grep -cE '^PRECONDITION' "$LOGDIR/$name.log"); pc=${pc:-0}
    PASS=$((PASS+p)); FAILN=$((FAILN+f)); PREN=$((PREN+pc))
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$mode" "$ms" "$p" "$f" "$pc" "$sl" "$wk" >> "$TIMING"
    printf '> Selesai %s [%s] %s.%03ds\n' "$name" "$mode" "$((ms/1000))" "$((ms%1000))"
    # Design §9: precondition BUKAN kegagalan Satria — kategorinya sendiri.
    if [ "$f" -gt 0 ]; then RESULTS+=("FAIL $name ($f)")
    elif [ "$pc" -gt 0 ]; then RESULTS+=("PRE $name ($pc)")
    else RESULTS+=("OK   $name ($p)"); fi
  done
}

run_phase cold "${COLD[@]}"
[ "${#SNAP[@]}" -gt 0 ] && run_phase snap "${SNAP[@]}"

RUN_ELAPSED=$(( $(now_ms) - RUN_T0 ))
printf 'RUN_ENDED=%s\nRUN_ELAPSED_MS=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ELAPSED" >> "$LOGDIR/run.env"
echo "=========================================="
echo "Ringkasan: PASS=$PASS FAIL=$FAILN PRE=$PREN tes=$TOT target=$TNAME"
for r in "${RESULTS[@]}"; do echo "  $r"; done
echo "Waktu total : $((RUN_ELAPSED/1000)).$(printf '%03d' $((RUN_ELAPSED%1000))) s"
echo "5 tes terlama (kerja tanpa tunggu):"
tail -n +2 "$TIMING" | sort -t"$(printf '\t')" -k8,8nr | head -n 5 | \
  awk -F'\t' '{printf "  %-30s %-5s kerja %7.3f s  tunggu %6.3f s  total %7.3f s\n", $1, $2, $8/1000, $7/1000, $3/1000}'
echo "Timing : $TIMING"
echo "Log    : $LOG"
[ "$FAILN" -eq 0 ] || exit 1
exit 0

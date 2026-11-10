#!/usr/bin/env bash
# Runner mode - menjalankan unit test

runner_init() {
  if [ "${#POS[@]}" -ne 2 ]; then
    usage >&2
    die "perlu tepat dua argumen: <binary> <target> (atau -w [target]) — lihat -h"
  fi

  BIN_IN="${POS[0]}"
  TNAME="${POS[1]}"
  [[ -f "$BIN_IN" && -x "$BIN_IN" ]] || die "binary tidak ditemukan atau tidak dapat dieksekusi: $BIN_IN"
  BIN_ABS="$(cd "$(dirname "$BIN_IN")" && pwd)/$(basename "$BIN_IN")"
  TGT_SRC="$ROOT/tests/$TNAME"
  [ -d "$TGT_SRC" ] || die "target tidak ditemukan: tests/$TNAME"
  TGT_SRC="$(cd "$TGT_SRC" && pwd)"
  case "$TNAME" in '' | */* | .*) die "nama target tidak valid: $TNAME" ;; esac
  [ -d "$TESTS_DIR/common" ] || die "tests/unit/common tidak ada: $TESTS_DIR"
  [ -f "$TESTS_DIR/classes.conf" ] || die "classes.conf tidak ada: $TESTS_DIR/classes.conf"

  # Clean mode
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

  # Lock
  if [ -f "$LOGDIR/run.pid" ]; then
    OLDPID="$(cat "$LOGDIR/run.pid" 2> /dev/null)"
    if [ -n "$OLDPID" ] && [ "$OLDPID" != "$$" ] && kill -0 "$OLDPID" 2> /dev/null; then
      die "run lain masih aktif pada target $TNAME (PID $OLDPID). Hentikan dulu: kill $OLDPID"
    fi
  fi

  # run.pid sejak detik nol (design §9.1): dengan run.pid ada SEBELUM staging,
  # run.env milik run sebelumnya tak pernah sempat diterima watcher sebagai
  # cerita — cek "env -ot run.pid" langsung menahan jalannya jalur lama.
  # Parent pembungkus -b tidak menulis: yang menulis adalah child-nya sendiri,
  # karena run.pid = identitas proses runner, bukan proses wrapper.
  if [ "$BG" -eq 0 ] || [ -n "${UNIT_V1_CHILD:-}" ]; then
    echo "$$" > "$LOGDIR/run.pid"
  fi
}

runner_background() {
  if [ "$BG" -eq 1 ] && [ -z "${UNIT_V1_CHILD:-}" ]; then
    set -m
    SELARGS=()
    for s in "${SEL[@]}"; do SELARGS+=(-t "$s"); done
    UNIT_V1_CHILD=1 nohup bash "$0" "$BIN_IN" "$TNAME" -j "$JOBS" -o "$LOG" \
      "${SELARGS[@]}" > /dev/null 2>&1 &
    CPID=$!
    set +m
    echo "$CPID" > "$LOGDIR/bg.pid"
    say "Berjalan di background"
    say "  PID : $CPID"
    say "  Log : $LOG"
    say "  Stop: kill $CPID"
    trap - EXIT
    exit 0
  fi

  if [ -n "${UNIT_V1_CHILD:-}" ]; then
    trap 'kill_children $$; rm -f "$LOGDIR/run.pid"; exit 130' TERM INT HUP
  fi

  mkdir -p "$(dirname "$LOG")"
  : > "$LOG"
  exec > >(tee -a "$LOG") 2>&1
  # run.pid sudah ditulis runner_init; di sini hanya trap kebersihan keluar.
  trap 'rm -f "$LOGDIR/run.pid"; drop_copy' EXIT
}

runner_staging() {
  [[ -n "$STAGE" && "$STAGE" != "/" ]] || die "STAGE tidak valid"
  mkdir -p "$STAGE/bin" || die "gagal membuat $STAGE"

  FP_NOW="$(src_fingerprint | sha256sum | cut -c1-64)"
  STATE="$STAGE/test.state"

  if state_valid; then
    say "> Staging : sama dengan test.state (tidak disalin ulang)"
  else
    say "> Staging : sinkronkan common/, lib/, classes.conf"
    rm -rf "$STAGE/common" "${STAGE:-/tmp/satria-unit}/lib"
    cp -a "$TESTS_DIR/common" "$TESTS_DIR/lib" "$STAGE/" || die "gagal menyalin tests/unit"
    cp "$TESTS_DIR/classes.conf" "$STAGE/classes.conf" || die "gagal menyalin classes.conf"
    {
      echo "TEST_STATE_VERSION=1"
      echo "SRC_FINGERPRINT=$FP_NOW"
      echo "FILES:"
      (
        cd "$STAGE" && find common lib -type f | sort | xargs sha256sum
        sha256sum classes.conf
      ) \
        | awk '{print "  " $2 " " substr($1,1,64)}'
    } > "$STATE.tmp" && mv -f "$STATE.tmp" "$STATE"
  fi

  # Shim sleep
  REAL_SLEEP="$(command -v sleep)" || die "sleep tidak ditemukan"
  export UNIT_REAL_SLEEP="$REAL_SLEEP"
  export UNIT_JOBS="$JOBS"
  mkdir -p "$STAGE/shim"
  cat > "$STAGE/shim/sleep" << 'SHIM'
#!/bin/sh
[ -n "$UNIT_SLEEP_LOG" ] && echo "${UNIT_TEST_NAME:-?} $1" >> "$UNIT_SLEEP_LOG"
exec "$UNIT_REAL_SLEEP" "$@"
SHIM
  chmod +x "$STAGE/shim/sleep"
  export PATH="$STAGE/shim:$PATH"
  SLEEP_LOG="$LOGDIR/sleep.log"
  export UNIT_SLEEP_LOG="$SLEEP_LOG"

  # Binary staging
  PIN="$STAGE/bin/satria-$BIN_SHA8"
  if [ ! -x "$PIN" ]; then
    cp "$BIN_ABS" "$PIN.tmp" && chmod +x "$PIN.tmp"
    mv -f "$PIN.tmp" "$PIN" || die "gagal menyalin binary ke $PIN"
  fi
}

runner_validate() {
  COLD=()
  SNAP=()
  SEC=""
  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%%#*}"
    line="$(echo "$line" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    [ -z "$line" ] && continue
    case "$line" in
      "[cold]")
        SEC=cold
        continue
        ;;
      "[snap]")
        SEC=snap
        continue
        ;;
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
    for c in "${COLD[@]}" "${SNAP[@]}"; do [ "$c" = "$t" ] && n=$((n + 1)); done
    [ "$n" -eq 1 ] || die "tes '$t' belum diklasifikasikan (atau terdaftar ganda) di classes.conf"
  done

  for t in "${SNAP[@]}"; do
    if grep -qE 'rm -rf[^|;&]*(\.satria|/build|/bin|/lib)([^A-Za-z_]|$)' "$STAGE/common/$t.sh"; then
      die "tes [snap] '$t' menghapus state build. Pindahkan ke [cold] atau tulis ulang."
    fi
  done

  say "> Klasifikasi : ${#COLD[@]} cold, ${#SNAP[@]} snap (dari ${#ALL_TESTS[@]} tes)"

  # Selection
  if [ "${#SEL[@]}" -gt 0 ]; then
    NC=()
    NS=()
    for s in "${SEL[@]}"; do
      ok=0
      for t in "${COLD[@]}"; do [ "$t" = "$s" ] && ok=1 && break; done
      for t in "${SNAP[@]}"; do [ "$t" = "$s" ] && ok=1 && break; done
      [ "$ok" -eq 1 ] || die "tes '-t $s' tidak ada di tests/unit/common atau tidak terdaftar di classes.conf"
      for t in "${COLD[@]}"; do [ "$t" = "$s" ] && {
        NC+=("$t")
        break
      }; done
      for t in "${SNAP[@]}"; do [ "$t" = "$s" ] && {
        NS+=("$t")
        break
      }; done
    done
    say "> Seleksi : ${#SEL[@]} tes (-t: ${SEL[*]}), cold $((${#SEL[@]} - ${#NS[@]})), snap ${#NS[@]}"
    if [ "${#NC[@]}" -gt 0 ]; then COLD=("${NC[@]}"); else COLD=(); fi
    if [ "${#NS[@]}" -gt 0 ]; then SNAP=("${NS[@]}"); else SNAP=(); fi
  fi
}

runner_fixture() {
  FIX="$T/fixture"
  SNAPD="$T/snap"
  W="$T/work"
  TMPD="$T/t"
  ARTIFACTS=(bin lib build modules dist .satria compile_commands.json)

  rm -rf "$FIX"
  cp -a "$TGT_SRC" "$FIX" || die "gagal menyalin fixture"
  for a in "${ARTIFACTS[@]}"; do rm -rf "${FIX:-$T/fixture}/$a"; done

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
      # Registri build (design §9.1 prepare): satu file per build aktif di
      # $STAGE/snap/<pid>.pid — pembaca luar (watcher) mengukur kemajuan build
      # realtime dari sini. Kolom: target, mode, denominasi persen = total byte
      # artefak (build,bin,lib,dist,modules,.satria) build sebelumnya.
      # Ditulis runner (bukan child build) supaya pembersihan di bawah kendali
      # runner; bila runner mati keras, file basi diacak oleh sweep watcher.
      mkdir -p "$STAGE/snap"
      _prev_bytes=0
      if [ -d "$SNAPD" ]; then
        _b=0
        for _a in build bin lib dist modules .satria; do
          [ -e "$SNAPD/$_a" ] || continue
          _s="$(du -sb "$SNAPD/$_a" 2> /dev/null | cut -f1)"
          case "$_s" in
            '' | *[!0-9]*) continue ;;
          esac
          _b=$((_b + _s))
        done
        _prev_bytes=$_b
      fi
      printf '%s cold %s\n' "$TNAME" "$_prev_bytes" > "$STAGE/snap/$$.pid"

      rm -rf "$SNAPD"
      cp -a "$FIX" "$SNAPD" || die "gagal menyalin fixture ke snap"
      T0=$(date +%s)
      (cd "$SNAPD" && "$WRAP" -w) > "$LOGDIR/snap-build.log" 2>&1
      RC=$?
      rm -f "$STAGE/snap/$$.pid"
      [ "$RC" -eq 0 ] || die "build snap gagal (rc=$RC), lihat $LOGDIR/snap-build.log"
      printf 'SNAP_ID=%s\nSNAP_BUILT_S=%s\n' "$SNAP_ID" "$(($(date +%s) - T0))" > "$SNAPD/snap.env"
      say "> Snap : selesai dalam $(($(date +%s) - T0)) s"
    fi
  fi
}

runner_discovery() {
  export UNIT_DISCOVERY="$T/discovery"
  mkdir -p "$UNIT_DISCOVERY"
  rm -rf "$T/discw"
  cp -a "$FIX" "$T/discw" || die "gagal menyiapkan salinan discovery"

  if (cd "$T/discw" && "$WRAP" -w -g compdb) > "$LOGDIR/discovery.log" 2>&1; then
    awk '
function cl(p) { gsub(/\\/, "/", p); sub(/^ *"[^:]+": *"/, "", p); sub(/",?$/, "", p); return p }
/"file":/   { f = cl($0) }
/"output":/ {
o = cl($0)
obj = o; sub(/^.*\/build\//, "", obj)
proj = obj; sub(/\/.*$/, "", proj)
src = f; sub("^.*/" proj "/", "", src)
if (src == f) { src = f; sub(/^.+\//, "", src) }
print proj "\t" src "\t" obj
}' "$T/discw/compile_commands.json" > "$UNIT_DISCOVERY/sources.tsv"
    [ -s "$UNIT_DISCOVERY/sources.tsv" ] \
      || die "discovery: compdb tanpa entri (lihat $LOGDIR/discovery.log)"
    NP=$(awk -F'\t' '{print $1}' "$UNIT_DISCOVERY/sources.tsv" | sort -u | wc -l)
    say "> Discovery : $NP proyek, $(wc -l < "$UNIT_DISCOVERY/sources.tsv") unit kompilasi (-w -g compdb)"
  else
    die "discovery compdb gagal, lihat $LOGDIR/discovery.log"
  fi
  rm -rf "$T/discw"

  # Plan & timing
  PLAN="$LOGDIR/plan.tsv"
  : > "$PLAN"
  _pi=0
  for _t in "${COLD[@]}"; do
    _pi=$((_pi + 1))
    printf '%s\t%s\tcold\n' "$_pi" "$_t" >> "$PLAN"
  done
  for _t in "${SNAP[@]}"; do
    _pi=$((_pi + 1))
    printf '%s\t%s\tsnap\n' "$_pi" "$_t" >> "$PLAN"
  done
  # timing.prev = pengetahuan durasi yang terakumulasi: gabungan prev lama +
  # timing run terakhir (run terakhir menang bila nama sama — dibaca kedua,
  # mengubah nilai). Gabungan, bukan salinan: run -t yang memilih subset
  # tidak lagi menyempitkan cakupan percen watcher (problem: persen hanya
  # tampil untuk tes yang kebetulan ada di run terakhir).
  if [ -s "$LOGDIR/timing.tsv" ]; then
    {
      printf 'operasi\tfase\tms\tpass\tfail\tpre\tsleep_ms\twork_ms\n'
      awk -F'\t' '
        FNR > 1 && $1 != "" { row[$1] = $0 }
        END { for (k in row) print row[k] }
      ' "$LOGDIR/timing.prev.tsv" "$LOGDIR/timing.tsv" 2> /dev/null
    } > "$LOGDIR/timing.prev.tmp" \
      && mv "$LOGDIR/timing.prev.tmp" "$LOGDIR/timing.prev.tsv" \
      || rm -f "$LOGDIR/timing.prev.tmp"
  fi

  TIMING="$LOGDIR/timing.tsv"
  : > "$TIMING"
  printf 'operasi\tfase\tms\tpass\tfail\tpre\tsleep_ms\twork_ms\n' >> "$TIMING"
  [ -f "$SNAPD/snap.env" ] && printf 'snap-build\tsnap\t%s\t-\t-\t-\t-\t-\n' \
    "$(($(grep '^SNAP_BUILT_S=' "$SNAPD/snap.env" | cut -d= -f2) * 1000))" >> "$TIMING"

  # run.env
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
    echo "RUN_LOG=$LOG"
  } > "$LOGDIR/run.env"
}

runner_exec() {
  reset_work() {
    local mode="$1"
    rm -rf "$W"
    cp -a "$FIX" "$W" || die "gagal reset work"
    if [ "$mode" = snap ]; then
      for a in .satria bin lib build modules dist compile_commands.json; do
        [ -e "$SNAPD/$a" ] && cp -a "$SNAPD/$a" "$W/"
      done
    fi
  }

  PASS=0
  FAILN=0
  PREN=0
  TOT=0
  PAUSED=0
  RESULTS=()

  run_phase() {
    local mode="$1"
    shift
    local name
    for name in "$@"; do
      # Pause antar tes (graceful): flag ditulis `unit_dev.sh pause <target>`;
      # tes yang sedang berjalan dibiarkan selesai, berikutnya tidak dimulai.
      if [ -f "$LOGDIR/pause.flag" ]; then
        rm -f "$LOGDIR/pause.flag"
        PAUSED=1
        break
      fi
      TOT=$((TOT + 1))
      _rt="$(date +%s%3N 2> /dev/null)"
      case "$_rt" in *N* | '') _rt=$(($(date +%s) * 1000)) ;; esac
      printf '> Running %s.sh [%s]... @%s\n' "$name" "$mode" "$_rt"

      local t0 t1 ms
      t0="$(now_ms)"

      reset_work "$mode"
      rm -rf "$TMPD"
      mkdir -p "$TMPD"
      : > "$SLEEP_LOG"

      (cd "$W" && UNIT_TEST_NAME="$name" UNIT_TMP="$TMPD" UNIT_BASE="$T" \
        sh "$STAGE/common/$name.sh" "$WRAP" "$FIX") \
        2>&1 | tee "$LOGDIR/$name.log"

      t1="$(now_ms)"
      ms=$((t1 - t0))
      sl="$(awk '{s+=$2} END{printf "%d", s*1000}' "$SLEEP_LOG")"
      sl=${sl:-0}
      wk=$((ms - sl))
      [ "$wk" -lt 0 ] && wk=0

      p=$(grep -c '^PASS' "$LOGDIR/$name.log")
      f=$(grep -c '^FAIL' "$LOGDIR/$name.log")
      pc=$(grep -cE '^PRECONDITION' "$LOGDIR/$name.log")
      pc=${pc:-0}

      PASS=$((PASS + p))
      FAILN=$((FAILN + f))
      PREN=$((PREN + pc))

      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$mode" "$ms" "$p" "$f" "$pc" "$sl" "$wk" >> "$TIMING"
      printf '> Selesai %s [%s] %s.%03ds\n' "$name" "$mode" "$((ms / 1000))" "$((ms % 1000))"

      if [ "$f" -gt 0 ]; then
        RESULTS+=("FAIL $name ($f)")
      elif [ "$pc" -gt 0 ]; then
        RESULTS+=("PRE $name ($pc)")
      else RESULTS+=("OK   $name ($p)"); fi
    done
  }

  run_phase cold "${COLD[@]}"
  [ "$PAUSED" -eq 0 ] && [ "${#SNAP[@]}" -gt 0 ] && run_phase snap "${SNAP[@]}"

  # Pause: tandai run.env, cetak status sisa, keluar bersih (trap menangani
  # run.pid + snapshot). Tanpa RUN_ENDED — run belum selesai, watcher tahu.
  if [ "$PAUSED" -eq 1 ]; then
    printf 'RUN_PAUSED=1\n' >> "$LOGDIR/run.env"
    nsisa="$(remaining_count)"
    echo "=========================================="
    echo "Paused : $TOT tes selesai, sisa $nsisa — lanjutkan dengan: unit_dev.sh resume $TNAME"
    echo "Timing : $TIMING"
    exit 0
  fi

  RUN_ELAPSED=$(($(now_ms) - RUN_T0))
  printf 'RUN_ENDED=%s\nRUN_ELAPSED_MS=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ELAPSED" >> "$LOGDIR/run.env"

  echo "=========================================="
  echo "Ringkasan: PASS=$PASS FAIL=$FAILN PRE=$PREN tes=$TOT target=$TNAME"
  for r in "${RESULTS[@]}"; do echo "  $r"; done
  echo "Waktu total : $((RUN_ELAPSED / 1000)).$(printf '%03d' $((RUN_ELAPSED % 1000))) s"
  echo "5 tes terlama (kerja tanpa tunggu):"
  tail -n +2 "$TIMING" | sort -t"$(printf '\t')" -k8,8nr | head -n 5 \
    | awk -F'\t' '{printf "  %-30s %-5s kerja %7.3f s  tunggu %6.3f s  total %7.3f s\n", $1, $2, $8/1000, $7/1000, $3/1000}'
  echo "Timing : $TIMING"
  echo "Log    : $LOG"

  [ "$FAILN" -eq 0 ] || exit 1
  exit 0
}

# Main runner entry
runner_run() {
  runner_init
  runner_background
  runner_staging
  runner_validate
  runner_fixture
  runner_discovery
  runner_exec
}

# Validasi nama target untuk stop/resume (satu kata, tanpa jalur).
control_target() {
  [ "${#POS[@]}" -ge 1 ] || {
    usage >&2
    die "$1 perlu <target>"
  }
  [ "${#POS[@]}" -le 1 ] || {
    usage >&2
    die "$1 hanya menerima <target>"
  }
  TNAME="${POS[0]}"
  case "$TNAME" in '' | */* | .*) die "nama target tidak valid: $TNAME" ;; esac
  T="$STAGE/targets/$TNAME"
  [ -d "$T" ] || die "target belum pernah dijalankan: $STAGE/targets/$TNAME"
  LOGDIR="$T/log"
  [ -d "$LOGDIR" ] || die "tidak ada log run di $LOGDIR"
}

# Jumlah tes sisa di target (plan.tsv yang belum ada di timing.tsv).
# plan.tsv: nomor, nama, fase — TANPA header, nama di kolom 2;
# timing.tsv: nama di kolom 1 (dengan header; snap-build tak dihitung).
remaining_count() {
  awk -F'\t' '
    NR == FNR {
      if (FNR > 1 && $1 != "snap-build") done[$1] = 1
      next
    }
    !($2 in done) { n++ }
    END { print n + 0 }
  ' "$LOGDIR/timing.tsv" "$LOGDIR/plan.tsv" 2> /dev/null
}

# Daftar nama tes sisa (urut plan) — satu per baris.
remaining_names() {
  awk -F'\t' '
    NR == FNR {
      if (FNR > 1 && $1 != "snap-build") done[$1] = 1
      next
    }
    !($2 in done) { print $2 }
  ' "$LOGDIR/timing.tsv" "$LOGDIR/plan.tsv" 2> /dev/null
}

# unit_dev.sh pause <target> — tandai run aktif agar berhenti setelah tes
# yang sedang berjalan selesai (graceful, antara tes). Runner menuliskan
# RUN_PAUSED=1 ke run.env; `resume <target>` melanjutkan sisa plan−timing.
pause_run() {
  control_target "pause"
  local rpid
  rpid="$(cat "$LOGDIR/run.pid" 2> /dev/null)"
  if [ -z "$rpid" ] || ! kill -0 "$rpid" 2> /dev/null; then
    die "tidak ada run aktif di target $TNAME — pause hanya untuk run yang sedang berjalan"
  fi
  : > "$LOGDIR/pause.flag"
  say "> Pause : ditandai (pid $rpid) — berhenti setelah tes berjalan selesai"
  say "> Pause : lanjutkan dengan: unit_dev.sh resume $TNAME"
}

# unit_dev.sh stop <target> — hentikan run aktif (kill tree), bersihkan
# jejaknya (run.pid, bg.pid, registri snap); timing & plan dibiarkan agar
# `resume <target>` bisa melanjutkan sisa tes.
stop_run() {
  control_target "stop"
  local rpid nsisa
  rpid="$(cat "$LOGDIR/run.pid" 2> /dev/null)"
  if [ -z "$rpid" ] || ! kill -0 "$rpid" 2> /dev/null; then
    rpid="$(cat "$LOGDIR/bg.pid" 2> /dev/null)"
    if [ -z "$rpid" ] || ! kill -0 "$rpid" 2> /dev/null; then
      die "tidak ada run aktif di target $TNAME"
    fi
  fi
  nsisa="$(remaining_count)"
  kill_children "$rpid"
  kill -TERM "$rpid" 2> /dev/null
  sleep 0.2
  kill -0 "$rpid" 2> /dev/null && kill -KILL "$rpid" 2> /dev/null
  rm -f "$LOGDIR/run.pid" "$LOGDIR/bg.pid" "$STAGE/snap/$rpid.pid"
  say "> Stop : run $TNAME (pid $rpid) dihentikan"
  if [ "$nsisa" -gt 0 ]; then
    say "> Stop : sisa $nsisa tes — lanjutkan dengan: unit_dev.sh resume $TNAME"
  fi
}

# unit_dev.sh resume <target> — lanjutkan run yang terputus: sisa plan
# (plan.tsv minus timing.tsv) dijalankan dengan binary yang tercatat di
# run.env. Snap & staging dianggap milik run itu — identitas menentukan.
resume_run() {
  control_target "resume"
  local rpid binin
  rpid="$(cat "$LOGDIR/run.pid" 2> /dev/null)"
  if [ -n "$rpid" ] && kill -0 "$rpid" 2> /dev/null; then
    die "run masih aktif di target $TNAME (pid $rpid) — hentikan dulu: unit_dev.sh stop $TNAME"
  fi
  [ -f "$LOGDIR/plan.tsv" ] || die "belum ada plan untuk di-resume: jalankan run biasa dulu"
  [ -f "$LOGDIR/run.env" ] || die "belum ada run.env untuk di-resume: jalankan run biasa dulu"

  SEL=()
  while IFS= read -r _n; do
    [ -n "$_n" ] && SEL+=("$_n")
  done < <(remaining_names)
  if [ "${#SEL[@]}" -eq 0 ]; then
    say "> Resume : semua tes sudah selesai di target $TNAME — tidak ada yang dijalankan"
    return 0
  fi

  # Binary dari run: path original dulu, pinned sebagai fallback.
  binin="$(sed -n 's/^RUN_ORIGINAL=//p' "$LOGDIR/run.env" | head -1)"
  [ -n "$binin" ] && [ -x "$binin" ] ||
    binin="$(sed -n 's/^RUN_BINARY=//p' "$LOGDIR/run.env" | head -1)"
  [ -n "$binin" ] && [ -x "$binin" ] ||
    die "binary run sebelumnya tidak ditemukan (${binin:-?}) — jalankan run biasa"
  POS=("$binin" "$TNAME")
  say "> Resume : sisa ${#SEL[@]} tes di $TNAME (${SEL[*]})"
  runner_run
}

#!/usr/bin/env bash
# Watcher mode - memantau kemajuan run

# Setup watcher
watcher_init() {
  [ "${#POS[@]}" -le 1 ] || {
    usage >&2
    die "watcher: argumen hanya <target> opsional, tanpa <binary>"
  }
  [ "$BG" -eq 0 ] || die "-w hanya menampilkan status; jangan digabung dengan -b"
  [ "$CLEAN" -eq 0 ] || die "-w dan --clean tidak bisa digabung"
  [ "${#SEL[@]}" -eq 0 ] || die "-w tidak menerima -t (watcher tidak memilih tes)"

  WTARGET="${POS[0]-}"
  case "$WTARGET" in */* | .*) die "nama target tidak valid: $WTARGET" ;; esac
  [ -d "$STAGE/targets" ] || die "belum ada run di staging: $STAGE/targets (jalankan run dulu)"

  WINTERVAL="${UNIT_WATCH_INTERVAL:-1}"
  case "$WINTERVAL" in '' | *[!0-9]*) die "UNIT_WATCH_INTERVAL harus bilangan bulat" ;; esac

  # Auto-detect target aktif
  if [ -z "$WTARGET" ]; then
    WACTIVE=""
    for d in "$STAGE"/targets/*/; do
      [ -f "$d/log/run.pid" ] || continue
      _rp="$(cat "$d/log/run.pid" 2> /dev/null)"
      [ -n "${_rp:-}" ] && kill -0 "$_rp" 2> /dev/null \
        && WACTIVE="$WACTIVE $(basename "$d")"
    done
    WACTIVE="${WACTIVE# }"
    case "$WACTIVE" in
      *' '*)
        die "ada beberapa run aktif ($(printf '%s' "$WACTIVE" | tr ' ' ',')): sebutkan yang mau dipantau, mis. -w ${WACTIVE%% *}"
        ;;
      '')
        _known=""
        for d in "$STAGE"/targets/*/; do [ -f "$d/log/run.env" ] && _known="$_known $(basename "$d")"; done
        die "tidak ada run aktif: sebutkan target yang mau dipantau${_known:+ ($(printf '%s' "${_known# }" | tr ' ' ','))}"
        ;;
    esac
    WTARGET="$WACTIVE"
  fi

  # Registri watcher (satu file per PID) — bukan lock: multiwatch bebas.
  WATCH_DIR="$STAGE/watch"
  mkdir -p "$WATCH_DIR"

  # Bersihkan file watcher yang sudah mati (stale)
  for f in "$WATCH_DIR"/*; do
    [ -f "$f" ] || continue
    _opid="$(basename "$f")"
    case "$_opid" in '' | *[!0-9]*)
      rm -f "$f"
      continue
      ;;
    esac
    [ "$_opid" = "$$" ] && continue
    kill -0 "$_opid" 2> /dev/null || rm -f "$f"
  done

  # Daftar watcher aktif (selain diri sendiri)
  OTHER_WATCHERS=()
  for f in "$WATCH_DIR"/*; do
    [ -f "$f" ] || continue
    _opid="$(basename "$f")"
    [ "$_opid" = "$$" ] && continue
    case "$_opid" in '' | *[!0-9]*) continue ;; esac
    kill -0 "$_opid" 2> /dev/null && OTHER_WATCHERS+=("$_opid")
  done

  # Daftar target yang sedang dipantau watcher lain (untuk info)
  OTHER_TARGETS=()
  for f in "$WATCH_DIR"/*; do
    [ -f "$f" ] || continue
    _opid="$(basename "$f")"
    [ "$_opid" = "$$" ] && continue
    case "$_opid" in '' | *[!0-9]*) continue ;; esac
    if kill -0 "$_opid" 2> /dev/null; then
      _ot="$(cat "$f" 2> /dev/null)"
      [ -n "$_ot" ] && OTHER_TARGETS+=("$_opid:$_ot")
    fi
  done

  # Daftarkan diri sendiri (file berisi target yang dipantau)
  echo "${WTARGET:-}" > "$WATCH_DIR/$$"

  # Info watcher lain (opsional, tidak memblokir)
  if [ "${#OTHER_WATCHERS[@]}" -gt 0 ]; then
    say "* Watcher lain aktif: ${#OTHER_WATCHERS[@]} (${OTHER_WATCHERS[*]})"
  fi

  # Registri build (dibaca juga, dibersihkan bila basi): $STAGE/snap/<pid>.pid
  for f in "$STAGE"/snap/*.pid; do
    [ -f "$f" ] || continue
    _bpid="$(basename "$f" .pid)"
    case "$_bpid" in
      '' | *[!0-9]*)
        rm -f "$f"
        continue
        ;;
    esac
    [ "$_bpid" = "$$" ] && continue
    kill -0 "$_bpid" 2> /dev/null || rm -f "$f"
  done

  trap 'rm -f "$WATCH_DIR/$$"; exit 130' INT TERM HUP
}

# State watcher
CUR_NAME=""
CUR_START=0
RUN_KEY=""
EMIT_K=0
STREAM_K=""
LIVE=0
SUM_LINES=()
SUM_PRINTED=0
LIVE_NAME=""
LIVE_100=""
LIVE_MS=0 # milidetik terakhir yang sudah ditampilkan pada baris process

# State fase prepare (build snap, design §9.1): sebelum run.env run ini muncul.
PREP_PID=""  # pid runner yang sedang build
PREP_T0=0    # epoch ms awal prepare (mtime run.pid)
PREP_MS=0    # clamp: ms yang pernah ditampilkan
PREP_PREV=0  # denominasi persen (byte build sebelumnya)
PREP_COLD="" # "cold" bila registri build menyatakan cold
PREP_SHOWN=0 # baris prepare pernah ditampilkan
PREP_DONE=0  # 0 belum mulai, 1 berjalan, 2 selesai/aborted
EXEC_SHOWN=0 # "# Execution test" sudah tercetak

# Render entry tes selesai
entry() { # entry <idx> <nama> <pass> <fail> <pre> <ms|->
  local t="$6"
  case "$t" in '' | *[!0-9]*) t="-" ;; *) t="$t ms" ;; esac
  printf '[%d/%d] %s\n* PASS: %s FAIL: %s PRE: %s TIME: %s' "$1" "$N" "$2" "$3" "$4" "$5" "$t"
}

# Render entry tes berjalan
entry_live() {
  printf '[%d/%d] %s\n* process: %s%s' "$1" "$N" "$2" "$3" "$4"
}

# Teks baris process
LIVE_MS=0 # deklarasi di awal, dekat LIVE_100

# Ukuran manusiawi: KB/MB
fmt_bytes() {
  awk "BEGIN{b=$1; if (b >= 1048576) printf \"%.1fMB\", b/1048576; else printf \"%.1fKB\", b/1024}"
}

# Total byte artefak build di folder snap (pembilang persen prepare)
snap_bytes() {
  local d="$1" t=0 s a
  for a in build bin lib dist modules .satria; do
    [ -e "$d/$a" ] || continue
    s="$(du -sb "$d/$a" 2> /dev/null | cut -f1)"
    case "$s" in
      '' | *[!0-9]*) continue ;;
    esac
    t=$((t + s))
  done
  printf '%s' "$t"
}

# Ukuran berkas/folder manusiawi dari path, kosong bila tidak ada
size_of() {
  local s
  s="$(du -sb "$1" 2> /dev/null | cut -f1)"
  case "$s" in
    '' | *[!0-9]*) return 0 ;;
  esac
  printf ' %s' "$(fmt_bytes "$s")"
}

proc_text() { # isi PROC_TXT (di shell utama, supaya clamp LIVE_MS berlaku)
  local tms pctx pm pct
  tms=$(($(now_ms) - CUR_START))
  [ "$tms" -lt 0 ] && tms=0
  pctx=""
  pm="$(awk -F'\t' -v n="$CUR" 'NR > 1 && $1 == n { v = $3 } END { print v }' "$PREV" 2> /dev/null)"
  if [ -n "$pm" ] && [ "$pm" -gt 0 ] 2> /dev/null; then
    pct=$((tms * 100 / pm))
    [ "$pct" -gt 99 ] && pct=99
    pctx=" ${pct}%"
  fi
  [ "$tms" -lt "${LIVE_MS:-0}" ] && tms=$LIVE_MS
  LIVE_MS=$tms
  # Info size: folder work tes aktif (hasilnya tumbuh saat tes bekerja)
  PROC_TXT="* process: ${tms} ms${pctx}$(size_of "$STAGE/targets/$WTARGET/work")"
}

# Hasil tes dari timing.tsv
result_of() {
  awk -F'\t' -v n="$1" 'NR > 1 && $1 == n { p = $4; f = $5; q = $6; t = $3; ok = 1 } END { if (ok) printf "%s %s %s %s", p, f, q, t }' "$TIM" 2> /dev/null
}

# Load state run
load_run() {
  local d ld env rpid rlog cur k
  d="$STAGE/targets/$WTARGET"
  ld="$d/log"
  env="$ld/run.env"
  TIM="$ld/timing.tsv"
  PREV="$ld/timing.prev.tsv"
  PLAN="$ld/plan.tsv"
  ENV="$env"

  ACTIVE=0
  [ -f "$env" ] || return 1
  rpid="$(cat "$ld/run.pid" 2> /dev/null)"
  [ -n "${rpid:-}" ] && kill -0 "$rpid" 2> /dev/null && ACTIVE=1

  if [ "$ACTIVE" -eq 1 ] && [ "$env" -ot "$ld/run.pid" ]; then return 1; fi

  RS="$(sed -n 's/^RUN_STARTED=//p' "$env" | head -1)"
  if [ "$RS" != "$RUN_KEY" ]; then
    RUN_KEY="$RS"
    EMIT_K=0
    CUR_NAME=""
    CUR_START=0
    STREAM_K=""
    LIVE=0
  fi

  NAMES=()
  if [ -s "$PLAN" ]; then
    while IFS=$'\t' read -r _i _n _f; do NAMES+=("$_n"); done < "$PLAN"
  else
    while IFS= read -r _n; do
      [ -n "$_n" ] && NAMES+=("$_n")
    done < <(awk -F'\t' 'NR > 1 && $1 != "snap-build" { print $1 }' "$TIM" 2> /dev/null)
  fi

  N=${#NAMES[@]}
  if [ "$N" -eq 0 ]; then
    N=$(($(sed -n 's/^RUN_TESTS_COLD=//p' "$env" | head -1) + $(sed -n 's/^RUN_TESTS_SNAP=//p' "$env" | head -1)))
  fi

  ENDED="$(sed -n 's/^RUN_ENDED=//p' "$env" | head -1)"
  CI=0
  CUR=""

  if [ "$ACTIVE" -eq 1 ]; then
    rlog="$(sed -n 's/^RUN_LOG=//p' "$env" | head -1)"
    [ -n "$rlog" ] || rlog="$ld/unit.log"

    # Nama tes yang sedang berjalan dan waktu mulainya (@<epoch_ms> pada baris Running).
    read -r cur cur_ms < <(awk '/^> Running /{ c = $3; sub(/\.sh$/, "", c); m = $NF; sub(/^@/, "", m) } /^> Selesai /{ c = ""; m = "" } END { print c, m }' "$rlog" 2> /dev/null)
    if [ -n "$cur" ]; then
      if [ "$cur" != "$CUR_NAME" ]; then
        CUR_NAME="$cur"
        LIVE_MS=0
        case "${cur_ms:-}" in '' | *[!0-9]*) CUR_START="$(now_ms)" ;; *) CUR_START="$cur_ms" ;; esac
      fi
    else
      CUR_NAME=""
    fi

    CUR="$CUR_NAME"
    for k in "${!NAMES[@]}"; do [ "${NAMES[$k]}" = "$CUR" ] && CI=$((k + 1)); done
  fi
  return 0
}

# Main watcher loop
watcher_run() {
  # Fase prepare: run.pid segar tetapi run.env run ini belum ada (atau lebih
  # tua dari run.pid — milik run sebelumnya, design §9.1). Set PREP_* dan
  # return: 0 = prepare dalam jalur, 1 = bukan cerita prepare (biarkan caller).
  prep_check() {
    local ld="$STAGE/targets/$WTARGET/log" rpid pf tn md pv bpid
    rpid="$(cat "$ld/run.pid" 2> /dev/null)"
    # run.pid belum ditulis: sinyal jalan -b bisa lewat bg.pid (lebih baru
    # dari run.env — wrapper baru saja fork; bg.pid basi dari run lama tak
    # counts). Tanpa sinyal hidup, bukan cerita prepare.
    if [ -z "$rpid" ]; then
      bpid="$(cat "$ld/bg.pid" 2> /dev/null)"
      if [ -n "$bpid" ] && kill -0 "$bpid" 2> /dev/null \
        && [ "$ld/bg.pid" -nt "$ld/run.env" ]; then
        rpid="$bpid"
      fi
    fi
    # Run sudah mati sebelum run.env: bila prepare pernah tercetak, akhiri
    # cerita; kalau tidak, diam saja — watcher menutup run mati tanpa env.
    if [ -z "$rpid" ] || ! kill -0 "$rpid" 2> /dev/null; then
      if [ "$PREP_SHOWN" -eq 1 ] && [ "$PREP_DONE" -ne 2 ]; then
        PREP_DONE=2
        [ "$TTY" -eq 1 ] && printf '\r\033[2K'
        printf '# Prepare : terhenti (run berhenti sebelum run.env)\n'
        [ -n "${LOG:-}" ] && printf '# Prepare : terhenti (run berhenti sebelum run.env)\n' >> "$LOG"
        watch_cleanup
        exit 1
      fi
      return 1
    fi
    # Run berganti (pid beda): reset state prepare
    if [ "$rpid" != "$PREP_PID" ]; then
      PREP_PID="$rpid"
      PREP_T0=0
      PREP_MS=0
      PREP_PREV=0
      PREP_COLD=""
      PREP_SHOWN=0
    fi
    # Registri build: $STAGE/snap/<pid>.pid berisi "target mode denominasi"
    pf="$STAGE/snap/$rpid.pid"
    if [ -f "$pf" ]; then
      read -r tn md pv < "$pf"
      case "$pv" in
        '' | *[!0-9]*) pv=0 ;;
      esac
      PREP_PREV="$pv"
      case "$md" in
        cold) PREP_COLD="cold" ;;
        *) PREP_COLD="" ;;
      esac
      [ "$tn" = "$WTARGET" ] || {
        PREP_COLD=""
        PREP_PREV=0
      }
    fi
    # Awal prepare: mtime run.pid sebagai epoch ms
    if [ "$PREP_T0" -eq 0 ]; then
      PREP_T0="$(stat -c %Y "$ld/run.pid" 2> /dev/null)"
      case "$PREP_T0" in
        '' | *[!0-9]*) PREP_T0="$(now_ms)" ;;
        *) PREP_T0=$((PREP_T0 * 1000)) ;;
      esac
    fi
    PREP_DONE=1
    PREP_SHOWN=1
    return 0
  }

  # Render baris prepare aktif (baris ke-2 yang isinya direwrite di tempat,
  # setelah baris process tes). Kalert dicetak: "# prepare : <ms> ms <pct%>"
  # — % dan size hanya saat cold (registri build ada), ms selalu.
  prep_text() {
    local tms cur pct pctx
    tms=$(($(now_ms) - PREP_T0))
    [ "$tms" -lt 0 ] && tms=0
    [ "$tms" -lt "$PREP_MS" ] && tms=$PREP_MS
    PREP_MS=$tms
    pctx=""
    cur="$(snap_bytes "$STAGE/targets/$WTARGET/snap")"
    if [ "$PREP_COLD" = cold ] && [ "$PREP_PREV" -gt 0 ] 2> /dev/null \
      && [ "$cur" -gt 0 ]; then
      pct=$((cur * 100 / PREP_PREV))
      [ "$pct" -gt 99 ] && pct=99
      pctx=" ${pct}%"
    fi
    PREP_TXT="# Prepare : ${tms} ms${pctx}$(size_of "$STAGE/targets/$WTARGET/snap")"
  }

  # Print "# Execution test" sekali sebelum entri pertama — setelah baris
  # prepare terakhir (bila prepare pernah ditampilkan). Non-TTY tetap dapat
  # baris ini (bagian alur teks).
  exec_line() {
    [ "$EXEC_SHOWN" -eq 1 ] && return 0
    if [ "$TTY" -eq 1 ] && [ "$PREP_DONE" -eq 1 ]; then
      # Versi akhir prepare: kunci ms, persen boleh mencapai 100
      local tms cur pct pctx
      tms=$(($(now_ms) - PREP_T0))
      [ "$tms" -lt 0 ] && tms=0
      [ "$tms" -lt "$PREP_MS" ] && tms=$PREP_MS
      PREP_MS=$tms
      pctx=""
      if [ "$PREP_COLD" = cold ]; then
        pctx="$(size_of "$STAGE/targets/$WTARGET/snap")"
        cur="$(snap_bytes "$STAGE/targets/$WTARGET/snap")"
        if [ "$PREP_PREV" -gt 0 ] 2> /dev/null && [ "$cur" -gt 0 ]; then
          pct=$((cur * 100 / PREP_PREV))
          [ "$pct" -gt 100 ] && pct=100
          pctx=" ${pct}%${pctx}"
        fi
      fi
      echo ""
      printf '\r\033[2K# Prepare : %s ms%s\n' "$tms" "$pctx"
    fi
    echo ""
    printf '# Execution test\n'
    [ -n "${LOG:-}" ] && printf '# Execution test\n' >> "$LOG"
    EXEC_SHOWN=1
    PREP_DONE=2
  }

  TTY=0
  [ -t 1 ] && TTY=1
  trap 'watch_cleanup; exit 130' INT TERM HUP

  # Header
  WPIDTXT=""
  [ -n "$WTARGET" ] && WPIDTXT="$(cat "$STAGE/targets/$WTARGET/log/run.pid" 2> /dev/null)"
  body_line() {
    printf '%s\n' "$1"
    [ -n "${LOG:-}" ] && printf '%s\n' "$1" >> "$LOG"
  }
  body_line "# Watch unit test"
  body_line "* PID      : $$"
  body_line "* Staging  : $STAGE/targets"
  body_line "* Interval : ${WINTERVAL}s, Ctrl-C untuk berhenti"
  [ -n "$WTARGET" ] && body_line "* Target   : $WTARGET${WPIDTXT:+ - $WPIDTXT}"
  [ -n "${WACTIVE:-}" ] && [ -z "${POS[0]-}" ] && body_line "* Dipilih otomatis: satu-satunya run aktif"
  if [ "${#OTHER_WATCHERS[@]}" -gt 0 ]; then
    body_line "* Watcher lain: ${#OTHER_WATCHERS[@]} (${OTHER_WATCHERS[*]})"
  fi

  # Loop utama
  WAIT_UNKNOWN=0
  PREP_DRAWN=0 # blank-gap antar header sudah dicetak untuk fase prepare aktif
  while :; do
    # Anti-stale (design §9.1): run.pid tidak ada dan bg.pid tidak lebih baru
    # dari run.env → ditolak terbaca. Jangan memaksakan run.env milik run
    # sebelumnya sebagai cerita — watcher yang di-attach terlalu cepat menahan
    # 2 tick; setelahnya cerita lama (re-attach ke run yang sudah selesai)
    # tetap ditampilkan.
    _ld="$STAGE/targets/$WTARGET/log"
    if [ "$RUN_KEY" = "" ] && [ ! -f "$_ld/run.pid" ] \
      && ! { [ -f "$_ld/bg.pid" ] && [ "$_ld/bg.pid" -nt "$_ld/run.env" ]; }; then
      WAIT_UNKNOWN=$((WAIT_UNKNOWN + 1))
      if [ "$WAIT_UNKNOWN" -le 2 ]; then
        sleep "$WINTERVAL"
        continue
      fi
    fi
    if [ -n "$WTARGET" ] && load_run; then
      DONE_N=0
      ENTR=()
      k=0
      while [ "$k" -lt "$N" ]; do
        name="${NAMES[$k]:-}"
        [ -n "$name" ] || break # daftar tes belum lengkap (plan.tsv belum ada): tunggu tick berikutnya
        res="$(result_of "$name")"
        if [ -n "$res" ]; then
          DONE_N=$((DONE_N + 1))
          read -r _p _f _q _t <<< "$res"
          _p="${_p:-0}"
          _f="${_f:-0}"
          _q="${_q:-0}"
          _t="${_t:-0}"
        elif [ -z "$ENDED" ]; then
          k=$((k + 1))
          continue
        else
          _p="-"
          _f="-"
          _q="-"
          _t="-"
        fi
        ENTR+=("$(entry $((k + 1)) "$name" "$_p" "$_f" "$_q" "$_t")"$'\n')
        k=$((k + 1))
      done

      # TTY rendering
      if [ "$TTY" -eq 1 ]; then
        exec_line
        WANT=""
        if [ -z "$ENDED" ] && [ "$ACTIVE" -eq 1 ] && [ "$CI" -gt 0 ]; then WANT="$CUR"; fi
        HOLD=0
        if [ -n "$LIVE_NAME" ] && [ "${#ENTR[@]}" -gt "$EMIT_K" ] && [ "$LIVE_100" != "$LIVE_NAME" ]; then
          nxt="${ENTR[$EMIT_K]%%$'\n'*}"
          if [ "$nxt" = "[$((EMIT_K + 1))/$N] $LIVE_NAME" ]; then
            fin="$(awk -F'\t' -v n="$LIVE_NAME" 'NR > 1 && $1 == n { v = $3 } END { print v }' "$TIM" 2> /dev/null)"
            _ms=$((${fin:-0}))
            [ "$_ms" -lt "${LIVE_MS:-0}" ] && _ms=$LIVE_MS # clamp: tidak pernah mundur
            printf '\r\033[2K* process: %s ms 100%%%s' "$_ms" "$(size_of "$STAGE/targets/$WTARGET/work")"
            LIVE_100="$LIVE_NAME"
            HOLD=1
          fi
        fi
        if [ "$HOLD" -eq 1 ]; then
          :
        elif [ "${#ENTR[@]}" -gt "$EMIT_K" ] || [ "$LIVE_NAME" != "$WANT" ]; then
          if [ -n "$LIVE_NAME" ]; then
            printf '\r\033[2K\033[1A\r\033[2K'
            LIVE_NAME=""
          fi
          while [ "${#ENTR[@]}" -gt "$EMIT_K" ]; do
            printf '%s' "${ENTR[$EMIT_K]}"
            EMIT_K=$((EMIT_K + 1))
          done
          if [ -n "$WANT" ]; then
            proc_text
            printf '[%d/%d] %s\n%s' "$CI" "$N" "$WANT" "$PROC_TXT"
            LIVE_NAME="$WANT"
          fi
        elif [ -n "$LIVE_NAME" ]; then
          proc_text
          printf '\r\033[2K%s' "$PROC_TXT"
        fi
      else
        # Non-TTY
        exec_line
        done_block=""
        while [ "${#ENTR[@]}" -gt "$EMIT_K" ]; do
          done_block+="${ENTR[$EMIT_K]}"
          EMIT_K=$((EMIT_K + 1))
        done
        [ -n "$done_block" ] && {
          printf '%s' "$done_block"
          [ -n "${LOG:-}" ] && printf '%s' "$done_block" >> "$LOG"
        }
      fi

      # Summary
      if [ -n "$ENDED" ] && [ "$SUM_PRINTED" -eq 0 ]; then
        SUM_PRINTED=1
        ms="$(sed -n 's/^RUN_ELAPSED_MS=//p' "$ENV" | head -1)"
        read -r _sp _sf _sq <<< "$(awk -F'\t' 'NR > 1 && $1 != "snap-build" { p += $4; f += $5; q += $6 } END { printf "%d %d %d", p+0, f+0, q+0 }' "$TIM" 2> /dev/null)"
        _sp="${_sp:-0}"
        _sf="${_sf:-0}"
        _sq="${_sq:-0}"
        SUM_LINES=("" "# Summary" "* $DONE_N/$N selesai, total $(fmt_hms $((${ms:-0} / 1000)))" "* PASS: $_sp FAIL: $_sf PRE: $_sq")
      fi

      if [ "$SUM_PRINTED" -eq 1 ] && [ "${SUM_SHOWN:-}" != 1 ]; then
        SUM_SHOWN=1
        echo ""
        for line in "${SUM_LINES[@]}"; do
          printf '%s\n' "$line"
          [ -n "${LOG:-}" ] && printf '%s\n' "$line" >> "$LOG"
        done

        # Setelah jeda 2 detik keluar otomatis
        sleep 2
        watch_cleanup
        exit 0
      fi
    elif [ "$PREP_DONE" -ne 2 ]; then
      # Bukan cerita run (run.env belum muncul): cek fase prepare — build
      # snap berjalan (registri $STAGE/snap/<pid>.pid) atau tunggu ringkas.
      if prep_check && [ "$TTY" -eq 1 ]; then
        if [ "$PREP_DRAWN" -eq 0 ]; then
          printf '\n' # gap antar header: header → # Prepare (sekali render pertama)
          PREP_DRAWN=1
        fi
        prep_text
        printf '\r\033[2K%s' "$PREP_TXT"
      fi
    fi
    sleep "$WINTERVAL"
  done
}

watch_cleanup() {
  if [ "$TTY" -eq 1 ]; then
    printf '\033[?25h' # kursor tampil lagi
    printf '\n'        # newline agar prompt tidak menimpa baris terakhir
  fi
  rm -f "$WATCH_DIR/$$"
}

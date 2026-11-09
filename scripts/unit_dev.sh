#!/usr/bin/env bash
# scripts/unit_dev.sh - runner unit test dev (design/unit_test.md)
#
#   bash scripts/unit_dev.sh <binary> <target> [-b] [-j N] [-o <log>] [-t <tes>]
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

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TESTS_DIR="${UNIT_TESTS_DIR:-$ROOT/tests/unit}"
STAGE="${UNIT_STAGE:-/tmp/satria-unit}"

# Pustaka tampilan (adaptasi v0.2.1): dipakai watcher; warna TTY+NO_COLOR-aware.

# Dijalankan dari salinan scripts/unit.sh? Salinan itu milik run ini (bukan milik
# pembungkus): ia yang menghapus dirinya, supaya `-b` — yang me-re-exec "$0" di
# proses lain — tidak kehilangan skripnya saat pembungkus keluar lebih dulu.
SELF_COPY=""
case "$0" in */.unit.*.sh) SELF_COPY="$0" ;; esac
drop_copy() { [ -z "$SELF_COPY" ] || rm -f "$SELF_COPY"; }
trap drop_copy EXIT

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

usage() { sed -n '2,14p' "$0"; }

# ---------------------------------------------------------------- argumen
BG=0; JOBS=2; LOG=""; CLEAN=0; WATCH=0; POS=(); SEL=()
while [ $# -gt 0 ]; do
  case "$1" in
    -b) BG=1; shift ;;
    -w|--watch) WATCH=1; shift ;;
    -j) [ $# -ge 2 ] || die "-j memerlukan angka"; JOBS="$2"; shift 2 ;;
    -o) [ $# -ge 2 ] || die "-o memerlukan path log"; LOG="$2"; shift 2 ;;
    -t) [ $# -ge 2 ] || die "-t memerlukan nama tes"; SEL+=("$2"); shift 2 ;;
    --clean) CLEAN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) die "opsi tidak dikenal: $1" ;;
    *) POS+=("$1"); shift ;;
  esac
done
case "$JOBS" in ''|*[!0-9]*) die "-j harus bilangan bulat >= 0";; esac

# ---------------------------------------------------------------- watcher
# design §9: watcher memantau kemajuan run, bukan memicunya. Satu-satunya
# sumber status adalah area staging — log yang ditulis run sendiri — sehingga
# watcher tidak membaca sumber repo dan tidak menulis ke targets/.
if [ "$WATCH" -eq 1 ]; then
  [ "${#POS[@]}" -le 1 ] || { usage >&2; die "watcher: argumen hanya <target> opsional, tanpa <binary>"; }
  [ "$BG" -eq 0 ] || die "-w hanya menampilkan status; jangan digabung dengan -b"
  [ "$CLEAN" -eq 0 ] || die "-w dan --clean tidak bisa digabung"
  [ "${#SEL[@]}" -eq 0 ] || die "-w tidak menerima -t (watcher tidak memilih tes)"
  WTARGET="${POS[0]-}"
  case "$WTARGET" in */*|.*) die "nama target tidak valid: $WTARGET" ;; esac
  [ -d "$STAGE/targets" ] || die "belum ada run di staging: $STAGE/targets (jalankan run dulu)"
  WINTERVAL="${UNIT_WATCH_INTERVAL:-1}"
  case "$WINTERVAL" in ''|*[!0-9]*) die "UNIT_WATCH_INTERVAL harus bilangan bulat";; esac

  # Run aktif = run.pid yang prosesnya masih hidup. Tanpa <target>, watcher
  # hanya boleh memilih sendiri kalau run aktif tepat satu; kalau lebih dari
  # satu, status mana yang dimaksud ambigu, jadi minta target disebut.
  if [ -z "$WTARGET" ]; then
    WACTIVE=""
    for d in "$STAGE"/targets/*/; do
      [ -f "$d/log/run.pid" ] || continue
      _rp="$(cat "$d/log/run.pid" 2>/dev/null)"
      [ -n "${_rp:-}" ] && kill -0 "$_rp" 2>/dev/null &&
        WACTIVE="$WACTIVE $(basename "$d")"
    done
    WACTIVE="${WACTIVE# }"
    case "$WACTIVE" in
      *' '*)
        die "ada beberapa run aktif ($(printf '%s' "$WACTIVE" | tr ' ' ',')): sebutkan yang mau dipantau, mis. -w ${WACTIVE%% *}" ;;
    esac
    WTARGET="$WACTIVE"
  fi

  if [ -f "$STAGE/watch.pid" ]; then
    OPID="$(cat "$STAGE/watch.pid" 2>/dev/null)"
    if [ -n "$OPID" ] && [ "$OPID" != "$$" ] && kill -0 "$OPID" 2>/dev/null; then
      die "watcher lain masih aktif (PID $OPID). Stop: kill $OPID"
    fi
  fi
  echo "$$" > "$STAGE/watch.pid"
  trap 'rm -f "$STAGE/watch.pid"; exit 130' INT TERM HUP

  # "" bukan waktu valid: GNU date memaknai string kosong sebagai awal hari,
  # sehingga durasi run akan terlihat sepanjang sejak tengah malam.
  epoch_of() {
    [ -n "${1:-}" ] || { echo 0; return 0; }
    date -d "$1" +%s 2>/dev/null || echo 0
  }

  # ---------------------------------------------------------------- tampilan
  # Model tampilan (design §9, versi sederhana): SATU dokumen — header + entri.
  # Entri = baris nama + satu baris status: `* process: …` selama tes berjalan,
  # jadi `* PASS: …` saat selesai (baris process hilang). `# Summary` masuk flow
  # saat run berakhir. Kursor aktif = baris entri berjalan (ujung dokumen).
  # Tidak ada section tetap: semuanya teks mengalir; repaint = gambar ulang
  # viewport (potongan terakhir dokumen) dari (1,1), tanpa penulisan absolut.
  TTY=0; [ -t 1 ] && TTY=1
  RESIZED=0; trap 'RESIZED=1' WINCH
  trap 'watch_cleanup; exit 130' INT TERM HUP
  now_ms() { local t; t="$(date +%s%3N 2>/dev/null)"; case "$t" in *N*|'') t=$(( $(date +%s) * 1000 )) ;; esac; echo "$t"; }
  fmt_dot() { printf '%02d.%02d.%02d' $(( $1/3600 )) $(( $1%3600/60 )) $(( $1%60 )); }
  # Ukuran layar dari ioctl terminal (stty size), BUKAN tput: tput memakai LINES/COLUMNS
  # dari lingkungan bila ada, dan nilai lama di sana membuat footer ditulis di luar layar.
  scr_size() {
    local r c
    read -r r c < <(stty size 2>/dev/null)
    case "${r:-}" in ''|*[!0-9]*) r=24 ;; esac
    case "${c:-}" in ''|*[!0-9]*) c=80 ;; esac
    SCR_R=$r; SCR_C=$c
  }
  scr_rows() { scr_size; echo "$SCR_R"; }
  scr_cols() { scr_size; echo "$SCR_C"; }
  any_active() {
    local d n rp
    for d in "$STAGE"/targets/*/; do
      [ -d "$d" ] || continue
      n="$(basename "$d")"
      [ -n "$WTARGET" ] && [ "$n" != "$WTARGET" ] && continue
      rp="$(cat "$d/log/run.pid" 2>/dev/null)"
      [ -n "${rp:-}" ] && kill -0 "$rp" 2>/dev/null && return 0
    done
    return 1
  }
  # Keadaan antar iterasi (shell yang sama, bukan subshell):
  CUR_NAME=""; CUR_START=0; RUN_KEY=""; EMIT_K=0; LASTBUF=""
  LASTH=""; LASTC=""
  HDR_LINES=(); SUM_LINES=(); SUM_PRINTED=0
  watch_cleanup() {
    if [ "$TTY" -eq 1 ]; then
      printf '\033[?25h'                    # kursor tampil lagi
      printf '\033[%d;1H\n' "$(scr_rows)"   # kursor ke bawah
    fi
    rm -f "$STAGE/watch.pid"
  }
  # Entri tes selesai: dua baris, "[i/N] nama" lalu "* PASS: p FAIL: f PRE: q".
  entry() { printf '[%d/%d] %s\n* PASS: %s FAIL: %s PRE: %s' "$1" "$N" "$2" "$3" "$4" "$5"; }
  # Entri tes berjalan: baris status process, berganti jadi PASS saat selesai.
  entry_live() { printf '[%d/%d] %s\n* process: %s%s' "$1" "$N" "$2" "$3" "$4"; }
  # Hasil satu tes dari timing.tsv (baris terakhir untuk nama itu): "p f q" atau kosong.
  result_of() {
    awk -F'\t' -v n="$1" 'NR > 1 && $1 == n { p = $4; f = $5; q = $6; ok = 1 } END { if (ok) printf "%s %s %s", p, f, q }' "$TIM" 2>/dev/null
  }
  # Muat keadaan run target WTARGET. Return 1 bila belum ada run yang bisa ditampilkan.
  load_run() {
    local d ld env rpid rlog cur k
    d="$STAGE/targets/$WTARGET"; ld="$d/log"; env="$ld/run.env"
    TIM="$ld/timing.tsv"; PREV="$ld/timing.prev.tsv"; PLAN="$ld/plan.tsv"; ENV="$env"
    ACTIVE=0; [ -f "$env" ] || return 1
    rpid="$(cat "$ld/run.pid" 2>/dev/null)"
    [ -n "${rpid:-}" ] && kill -0 "$rpid" 2>/dev/null && ACTIVE=1
    # run.env ditulis sesudah run.pid. Bila lebih tua, isinya milik run sebelumnya.
    if [ "$ACTIVE" -eq 1 ] && [ "$env" -ot "$ld/run.pid" ]; then return 1; fi
    RS="$(sed -n 's/^RUN_STARTED=//p' "$env" | head -1)"
    if [ "$RS" != "$RUN_KEY" ]; then RUN_KEY="$RS"; EMIT_K=0; CUR_NAME=""; CUR_START=0; fi
    NAMES=()
    if [ -s "$PLAN" ]; then
      while IFS=$'\t' read -r _i _n _f; do NAMES+=("$_n"); done < "$PLAN"
    else
      while IFS= read -r _n; do [ -n "$_n" ] && NAMES+=("$_n"); done < <(awk -F'\t' 'NR > 1 && $1 != "snap-build" { print $1 }' "$TIM" 2>/dev/null)
    fi
    N=${#NAMES[@]}
    if [ "$N" -eq 0 ]; then
      N=$(( $(sed -n 's/^RUN_TESTS_COLD=//p' "$env" | head -1) + $(sed -n 's/^RUN_TESTS_SNAP=//p' "$env" | head -1) ))
    fi
    ENDED="$(sed -n 's/^RUN_ENDED=//p' "$env" | head -1)"
    CI=0; CUR=""
    if [ "$ACTIVE" -eq 1 ]; then
      rlog="$(sed -n 's/^RUN_LOG=//p' "$env" | head -1)"; [ -n "$rlog" ] || rlog="$ld/unit.log"
      cur="$(awk '/^> Running /{ c = $3; sub(/\.sh$/, "", c) } /^> Selesai /{ c = "" } END { print c }' "$rlog" 2>/dev/null)"
      if [ -n "$cur" ]; then
        [ "$cur" = "$CUR_NAME" ] || { CUR_NAME="$cur"; CUR_START="$(now_ms)"; }
      else
        CUR_NAME=""   # "> Selesai" tercetak: jangan tampilkan duplek sesaat
      fi
      CUR="$CUR_NAME"
      for k in "${!NAMES[@]}"; do [ "${NAMES[$k]}" = "$CUR" ] && CI=$((k+1)); done
    fi
    return 0
  }
  # Model dua-arah commit: SATU dokumen mengalir (header + entri). Repaint =
  # gambar ulang viewport (potongan terakhir sesuai tinggi layar) dari (1,1).
  # Tidak ada tulisan absolut yang menimpa isi; markup percentだけ di baris
  # entri berjalan (cursor aktif).
  # doc_build <live>: rakit seluruh isi dokumen (header + entri + summary +
  # live) ke DBUF.
  doc_build() { # doc_build <entri-berjalan-2-baris|>
    DBUF=""
    local line j
    for line in "${HDR_LINES[@]}"; do DBUF="$DBUF$line"$'\n'; done
    for (( j=0; j<${#ENTR[@]}; j++ )); do DBUF="$DBUF${ENTR[j]}"; done
    for line in "${SUM_LINES[@]}"; do DBUF="$DBUF$line"$'\n'; done
    [ -n "${1:-}" ] && DBUF="$DBUF$1"
  }
  # doc_paint: gambar ulang dokumen dari (1,1). Isi = header + entri selesai +
  # entri berjalan (paling akhir, 2 baris). Bila lebih panjang dari viewport,
  # yang terlihat adalah POTONGAN TERAKHIR sebanyak viewport — dokumen muncul
  # "mendorong" ke atas sebagai satu section, seperti teks yang mengalir normal.
  doc_paint() { # repaint viewport dokumen dari DBUF
    local w b n i start
    scr_size; w=$SCR_C; b=$SCR_R
    mapfile -t LST <<< "$DBUF"
    n=${#LST[@]}
    [ "$n" -gt 0 ] || return 0
    start=0; [ "$n" -gt "$b" ] && start=$(( n - b ))
    printf '\033[?25l\033[1;1H'
    for (( i=start; i<n; i++ )); do
      [ "$i" -gt "$start" ] && printf '\n'
      printf '\033[2K%s\033[K' "${LST[i]:0:$(( w - 1 ))}"   # 2K+K: bersihkan seluruh baris (celahan lama tidak boleh tinggal)
    done
    # Baris kosong tersisa viewport juga dibersihkan (residu lama di bawah).
    local rest rem j
    rest=$(( n - start )); rem=$(( b - rest ))
    for (( j=0; j<rem; j++ )); do printf '\n\033[2K'; done
  }
  # Header (bagian dokumen, surat design §9).
  WPIDTXT=""
  [ -n "$WTARGET" ] && WPIDTXT="$(cat "$STAGE/targets/$WTARGET/log/run.pid" 2>/dev/null)"; :
  body_line() { # header juga disimpan ke dokumen section 1 (untuk repaint doc_paint)
    HDR_LINES+=("$1")
    printf '%s\n' "$1"
    [ -n "${LOG:-}" ] && printf '%s\n' "$1" >> "$LOG"
  }
  body_line "# Watch unit test"
  body_line "* PID      : $$"
  body_line "* Staging  : $STAGE/targets"
  body_line "* Interval : ${WINTERVAL}s, Ctrl-C untuk berhenti"
  [ -n "$WTARGET" ] && body_line "* Target   : $WTARGET${WPIDTXT:+ - $WPIDTXT}"
  [ -n "${WACTIVE:-}" ] && [ -z "${POS[0]-}" ] && body_line "* Dipilih otomatis: satu-satunya run aktif"

  while :; do
    h=$(scr_rows); w=$(scr_cols)
    relayout=0
    if [ "$RESIZED" -eq 1 ] || [ "$h" != "$LASTH" ] || [ "$w" != "$LASTC" ]; then
      RESIZED=0; LASTH="$h"; LASTC="$w"
      [ "$TTY" -eq 1 ] && relayout=1
    fi
    if [ -n "$WTARGET" ] && load_run; then
        # Entri selesai (semua, urut) + entri berjalan (ujung body). Placeholder
      # "- - -" hanya tampil, tidak dihitung DONE_N.
      DONE_N=0
      ENTR=(); k=0
      while [ "$k" -lt "$N" ]; do
        name="${NAMES[$k]}"
        res="$(result_of "$name")"
        if [ -n "$res" ]; then
          DONE_N=$((DONE_N+1))
        elif [ -z "$ENDED" ]; then
          k=$((k+1)); continue     # belum selesai dan run masih jalan: bukan slot terisi
        else
          res="- - -"              # run berakhir tanpa catatan: tampil sebagai "-"
        fi
        set -- $res
        ENTR+=("$(entry $((k+1)) "$name" "$1" "$2" "$3")"$'\n')
        k=$((k+1))
      done
      # Entri tes yang sedang berjalan (ujung body), hasil "-". Saat "> Selesai"
      # tercetak tetapi tes berikut belum terlihat di log (urantai), CUR kosong:
      # entri barusan selesai TIDAK diulang di tail (tak ada duplek sesaat).
      # Entri berjalan: baris process (elapsed + persen bila data prev ada).
      # "> Selesai" tercetak tetapi tes berikut belum terlihat di log (urantai):
      # CUR kosong — entri barusan selesai TIDAK diulang.
      wanted=""
      if [ "$ACTIVE" -eq 1 ] && [ "$CI" -gt 0 ] && [ -z "$ENDED" ]; then
        tms=$(( $(now_ms) - CUR_START ))
        pctx=""
        pm="$(awk -F'\t' -v n="$CUR" 'NR > 1 && $1 == n { v = $3 } END { print v }' "$PREV" 2>/dev/null)"
        if [ -n "$pm" ] && [ "$pm" -gt 0 ] 2>/dev/null; then
          pct=$(( tms * 100 / pm )); [ "$pct" -gt 99 ] && pct=99
          pctx=" ${pct}%"
        fi
        wanted="$(entry_live "$CI" "$CUR" "$(fmt_dot $((tms/1000)))" "$pctx")"
      fi
      if [ -n "$ENDED" ] && [ "$SUM_PRINTED" -eq 0 ]; then
        # Summary dibuat sekali dan dipegang di SUM_LINES: ENTR dibangun ulang
        # tiap tick, jadi baris summary tidak boleh menempel di sini.
        SUM_PRINTED=1
        ms="$(sed -n 's/^RUN_ELAPSED_MS=//p' "$ENV" | head -1)"
        set -- $(awk -F'\t' 'NR > 1 && $1 != "snap-build" { p += $4; f += $5; q += $6 } END { printf "%d %d %d", p+0, f+0, q+0 }' "$TIM" 2>/dev/null)
        SUM_LINES=("" "# Summary" "* $DONE_N/$N selesai, total $(fmt_dot $(( ${ms:-0} / 1000 )))" "* PASS: $1 FAIL: $2 PRE: $3")
      fi
      if [ "$TTY" -eq 1 ]; then
        # Repaint dokumen bila ISI berubah (entri tercommit, live berganti,
        # summary masuk, resize) — deteksi via kesetaraan isi dokumen penuh,
        # bukan penambahan elemen: hasil tes mengganti baris "-" tanpa
        # menambah elemen, dan tetap terdeteksi.
        doc_build "$wanted"
        if [ "$DBUF" != "$LASTBUF" ] || [ "$relayout" -eq 1 ]; then
          doc_paint
          LASTBUF="$DBUF"
        fi
      else
        # Non-TTY: entri baru dicetak sekali (alur teks biasa).
        done_block=""
        while [ "${#ENTR[@]}" -gt "$EMIT_K" ]; do
          done_block+="${ENTR[$EMIT_K]}"
          EMIT_K=$((EMIT_K+1))
        done
        [ -n "$done_block" ] && { printf '%s' "$done_block"; [ -n "${LOG:-}" ] && printf '%s' "$done_block" >> "$LOG"; }
        if [ "$SUM_PRINTED" -eq 1 ] && [ "${SUM_SHOWN:-}" != 1 ]; then
          SUM_SHOWN=1
          for line in "${SUM_LINES[@]}"; do printf '%s\n' "$line"; [ -n "${LOG:-}" ] && printf '%s\n' "$line" >> "$LOG"; done
        fi
      fi
    fi   # load_run
    sleep "$WINTERVAL"
  done
fi

if [ "${#POS[@]}" -ne 2 ]; then
  usage >&2
  die "perlu tepat dua argumen: <binary> <target> (atau -w [target]) — lihat -h"
fi

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
  # Anak background masih membaca salinan ini lewat "$0"; biarkan ia yang
  # menghapusnya saat run selesai.
  trap - EXIT
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
# EXIT tunggal: lock run dilepas dan salinan skrip (bila ada) dihapus.
trap 'rm -f "$LOGDIR/run.pid"; drop_copy' EXIT

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

# -------------------------------------------------------------- discovery
# design §6: konfigurasi workspace dibaca lewat MEKANISME SATRIA sendiri —
# `-w -g compdb` (query-only, tanpa build, tanpa state), bukan parser kedua.
# Salinan throwaway fixture dipakai agar artefak CompDB tidak masuk jalur tes
# (tes cold mengharapkan tidak ada compile_commands.json sejak awal).
# Output: discovery/sources.tsv = proyek<TAB>sumber<TAB>objek (path rel fixture).
# Path rel dihitung dari segmen `/build/` — aman lintas platform (prefix drive
# Windows berbeda tidak ikut tersimpan).
export UNIT_DISCOVERY="$T/discovery"
mkdir -p "$UNIT_DISCOVERY"
rm -rf "$T/discw"
cp -a "$FIX" "$T/discw" || die "gagal menyiapkan salinan discovery"
if ( cd "$T/discw" && "$WRAP" -w -g compdb ) > "$LOGDIR/discovery.log" 2>&1; then
  awk '
    function cl(p) { gsub(/\\/, "/", p); sub(/^ *"[^:]+": *"/, "", p); sub(/",?$/, "", p); return p }
    /"file":/   { f = cl($0) }
    /"output":/ {
      o = cl($0)
      obj = o; sub(/^.*\/build\//, "", obj)
      proj = obj; sub(/\/.*$/, "", proj)
      src = f; sub("^.*/" proj "/", "", src)
      if (src == f) { src = f; sub(/^.+\//, "", src) } # fallback: nama berkas saja
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

# plan.tsv: urutan eksekusi final (cold lalu snap, setelah -t), dibaca watcher
# untuk daftar [n/N]. timing.prev.tsv: timing run sebelumnya, dipakai watcher
# sebagai acuan persen per tes (sebelum timing.tsv ditimpa di bawah).
PLAN="$LOGDIR/plan.tsv"; : > "$PLAN"; _pi=0
for _t in "${COLD[@]}"; do _pi=$((_pi+1)); printf '%s\t%s\tcold\n' "$_pi" "$_t" >> "$PLAN"; done
for _t in "${SNAP[@]}"; do _pi=$((_pi+1)); printf '%s\t%s\tsnap\n' "$_pi" "$_t" >> "$PLAN"; done
[ -f "$LOGDIR/timing.tsv" ] && cp "$LOGDIR/timing.tsv" "$LOGDIR/timing.prev.tsv"

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
  # path log run: dibaca watcher (-w) untuk nama tes yang sedang berjalan
  echo "RUN_LOG=$LOG"
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
        sh "$STAGE/common/$name.sh" "$WRAP" "$FIX" ) \
      2>&1 | tee "$LOGDIR/$name.log"
    t1="$(now_ms)"; ms=$((t1 - t0))
    sl="$(awk '{s+=$2} END{printf "%d", s*1000}' "$SLEEP_LOG")"; sl=${sl:-0}
    wk=$((ms - sl)); [ "$wk" -lt 0 ] && wk=0
    p=$(grep -c '^PASS' "$LOGDIR/$name.log"); f=$(grep -c '^FAIL' "$LOGDIR/$name.log")
    pc=$(grep -cE '^PRECONDITION' "$LOGDIR/$name.log"); pc=${pc:-0}
    PASS=$((PASS+p)); FAILN=$((FAILN+f)); PREN=$((PREN+pc))
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$mode" "$ms" "$p" "$f" "$pc" "$sl" "$wk" >> "$TIMING"
    printf '> Selesai %s [%s] %s.%03ds\n' "$name" "$mode" "$((ms/1000))" "$((ms%1000))"
    # Design §8: precondition BUKAN kegagalan Satria — kategorinya sendiri.
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

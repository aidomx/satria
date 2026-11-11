#!/bin/sh
# Unit test: `-w` memantau kemajuan run di staging (design §9).
#
# Watcher tidak menjalankan tes: ia hanya membaca targets/<nama>/log/. Karena
# itu tes ini tidak butuh run sungguhan — cukup staging sintetis yang berkasnya
# mengikuti kontrak run (run.pid, run.env, plan.tsv, timing.tsv, log run).
#   - header: PID watcher, staging, interval, target + PID run, pilihan otomatis
#   - entri tes selesai: "[i/N] nama" lalu "* PASS: p FAIL: f PRE: q"
#   - saat RUN_ENDED: summary dicetak sekali ("N/N selesai, total durasi")
#   - non-TTY (berkas) tidak mencetak baris "* process:" maupun escape TTY
#   - `-w <target>` hanya menampilkan target itu
#   - `-w` tanpa argumen memilih satu-satunya run aktif; bila run aktif lebih
#     dari satu, ditolak dengan permintaan menyebut target (lalu `-w <target>`
#     tetap jalan); tanpa run aktif sama sekali, ditolak dengan daftar kandidat
#   - multi-watch: beberapa watcher bisa berjalan bersamaan, masing-masing
#     menulis lock sendiri di $UNIT_STAGE/watch/<pid>
#   - lock basi (PID mati) dibersihkan saat watcher baru dimulai
#   - setiap watcher hanya menghapus lock miliknya sendiri saat berhenti
#   - run yang mati tanpa RUN_ENDED tidak mengarang hasil/summary
#   - `-w` menolak <binary>, dua argumen, -b, dan -t
# Pemakaian: sh tests/unit/common/watcher_staging.sh <binary>
fail() {
  echo "FAIL watcher_staging: $1"
  exit 1
}
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
ROOT="${UNIT_REPO:?UNIT_REPO tidak di-set: jalankan lewat scripts/unit_dev.sh}"
RUNNER="$ROOT/scripts/unit.sh"
[ -f "$RUNNER" ] || fail "runner tidak ada: $RUNNER"
tmp="$UNIT_TMP/watcher_staging"
rm -rf "$tmp"
mkdir -p "$tmp" || exit 1
st="$tmp/stage"
dl="$st/targets/demo/log"
mkdir -p "$dl" || fail "gagal membuat staging uji"
ll="$st/targets/lain/log"
mkdir -p "$ll" || fail "gagal membuat staging uji"
WPID=""
WPID2=""
RPID=""
RPID2=""
cleanup() {
  [ -n "$WPID" ] && kill "$WPID" 2> /dev/null
  [ -n "$WPID2" ] && kill "$WPID2" 2> /dev/null
  [ -n "$RPID" ] && kill "$RPID" 2> /dev/null
  [ -n "$RPID2" ] && kill "$RPID2" 2> /dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT INT TERM HUP
# wait_for <berkas> <pola> [detik]: tunggu pola muncul di berkas.
wait_for() {
  _i=0
  while [ "$_i" -lt "${3:-15}" ]; do
    grep -q "$2" "$1" 2> /dev/null && return 0
    _i=$((_i + 1))
    sleep 1
  done
  return 1
}
# stop_watcher [pid]: hentikan watcher tertentu (default: WPID).
# Bersihkan state global (WPID/WPID2) jika yang dihentikan adalah salah satunya.
stop_watcher() {
  _w="${1:-$WPID}"
  [ -n "$_w" ] || return 0
  # Lock file berisi target, bukan PID host; kirim sinyal ke PID watcher langsung.
  kill -TERM "$_w" 2> /dev/null
  _i=0
  while [ -f "$st/watch/$_w" ] && [ "$_i" -lt 50 ]; do
    _i=$((_i + 1))
    sleep 0.1
  done
  wait "$_w" 2> /dev/null
  [ "$_w" = "$WPID" ] && WPID=""
  [ "$_w" = "$WPID2" ] && WPID2=""
}
# --- staging: `demo` sedang berjalan (1 dari 2 tes), `lain` sudah selesai -----
# Urutan penulisan mengikuti run sungguhan: run.pid lebih dulu (lock run),
# manifest run.env menyusul — watcher memperlakukan env yang lebih tua sebagai
# milik run sebelumnya.
sleep 60 &
RPID=$!
printf '%s\n' "$RPID" > "$dl/run.pid"
{
  printf 'RUN_STARTED=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'RUN_TARGET=demo\nRUN_TESTS_COLD=2\nRUN_TESTS_SNAP=0\nRUN_LOG=%s\n' "$dl/run.log"
} > "$dl/run.env"
printf '1\talpha\tcold\n2\tbeta\tcold\n' > "$dl/plan.tsv"
printf 'operasi\tfase\tms\tpass\tfail\tpre\tsleep_ms\twork_ms\n' > "$dl/timing.tsv"
printf 'alpha\tcold\t1000\t3\t0\t0\t0\t1000\n' >> "$dl/timing.tsv"
printf '> Running beta.sh [cold]...\n' > "$dl/run.log"
{
  printf 'RUN_STARTED=%s\nRUN_ENDED=%s\nRUN_ELAPSED_MS=1000\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'RUN_TARGET=lain\nRUN_TESTS_COLD=1\nRUN_TESTS_SNAP=0\nRUN_LOG=%s\n' "$ll/run.log"
} > "$ll/run.env"
printf 'operasi\tfase\tms\tpass\tfail\tpre\tsleep_ms\twork_ms\n' > "$ll/timing.tsv"
printf 'gamma\tcold\t500\t1\t0\t0\t0\t500\n' >> "$ll/timing.tsv"
: > "$ll/run.log"
# --- tanpa argumen: tepat satu run aktif -> watcher memilih target itu -------
out0="$tmp/watch-auto.out"
: > "$out0"
UNIT_STAGE="$st" UNIT_WATCH_INTERVAL=1 bash "$RUNNER" -w > "$out0" 2>&1 &
WPID=$!
wait_for "$out0" '\* Target   : demo - '"$RPID" \
  || {
    cat "$out0" >&2
    fail "-w tanpa argumen tidak memilih satu-satunya run aktif"
  }
echo "PASS watcher_staging: -w tanpa argumen memilih satu-satunya run aktif"
grep -q 'Dipilih otomatis' "$out0" \
  || {
    cat "$out0" >&2
    fail "pilihan otomatis tidak diumumkan di header"
  }
wait_for "$out0" '\[1/2\] alpha' \
  || {
    cat "$out0" >&2
    fail "entri tes selesai tidak tampil"
  }
wait_for "$out0" '^\* PASS: 3 FAIL: 0 PRE: 0' \
  || {
    cat "$out0" >&2
    fail "baris PASS entri selesai tidak sesuai timing.tsv"
  }
echo "PASS watcher_staging: entri selesai tampil sesuai design §9"
grep -q 'lain' "$out0" \
  && {
    cat "$out0" >&2
    fail "-w tanpa argumen menampilkan target yang tidak berjalan"
  }
echo "PASS watcher_staging: pilihan otomatis menyempitkan cakupan"
stop_watcher
# --- `-w <target>`: status hanya untuk target itu ----------------------------
out="$tmp/watch-demo.out"
: > "$out"
UNIT_STAGE="$st" UNIT_WATCH_INTERVAL=1 bash "$RUNNER" -w demo > "$out" 2>&1 &
WPID=$!
wait_for "$out" '\[1/2\] alpha' \
  || {
    cat "$out" >&2
    fail "-w demo tidak menampilkan entri target demo"
  }
echo "PASS watcher_staging: -w <target> menampilkan entri target itu"
grep -q 'lain' "$out" && {
  cat "$out" >&2
  fail "-w demo menampilkan target lain"
}
echo "PASS watcher_staging: -w <target> menyempitkan cakupan"
# Non-TTY: berkas log tidak boleh dipenuhi baris process berulang atau escape.
grep -q '\* process:' "$out" && {
  cat "$out" >&2
  fail "berkas non-TTY memuat baris process"
}
grep -q "$(printf '\033')" "$out" && {
  cat "$out" >&2
  fail "berkas non-TTY memuat escape TTY"
}
echo "PASS watcher_staging: non-TTY hanya mencetak entri selesai"
stop_watcher
# --- multi-watch: beberapa watcher berjalan bersamaan ------------------------
# Dua watcher dipantau target yang sama; keduanya harus jalan tanpa saling menolak.
out_mw1="$tmp/watch-mw1.out"
: > "$out_mw1"
UNIT_STAGE="$st" UNIT_WATCH_INTERVAL=1 bash "$RUNNER" -w demo > "$out_mw1" 2>&1 &
WPID=$!
wait_for "$out_mw1" '\* Target   : demo' \
  || {
    cat "$out_mw1" >&2
    fail "watcher pertama tidak jalan"
  }
out_mw2="$tmp/watch-mw2.out"
: > "$out_mw2"
UNIT_STAGE="$st" UNIT_WATCH_INTERVAL=1 bash "$RUNNER" -w demo > "$out_mw2" 2>&1 &
WPID2=$!
wait_for "$out_mw2" '\* Target   : demo' \
  || {
    cat "$out_mw2" >&2
    fail "watcher kedua tidak jalan (multi-watch ditolak)"
  }
echo "PASS watcher_staging: multi-watch diperbolehkan"
# Setiap watcher menulis lock file-nya sendiri (berisi target yang dipantau).
[ -f "$st/watch/$WPID" ] || fail "lock watcher pertama tidak ada"
[ -f "$st/watch/$WPID2" ] || fail "lock watcher kedua tidak ada"
echo "PASS watcher_staging: setiap watcher punya lock sendiri"
# Kedua watcher tetap hidup setelah yang lain bergabung.
kill -0 "$WPID" 2> /dev/null || fail "watcher pertama mati saat multi-watch"
kill -0 "$WPID2" 2> /dev/null || fail "watcher kedua mati saat multi-watch"
echo "PASS watcher_staging: kedua watcher hidup bersamaan"
# Watcher kedua menampilkan info watcher pertama di header.
grep -q "Watcher lain" "$out_mw2" \
  || {
    cat "$out_mw2" >&2
    fail "watcher kedua tidak mengumumkan watcher lain di header"
  }
echo "PASS watcher_staging: header mengumumkan watcher lain"
stop_watcher "$WPID2"
# Setelah watcher kedua berhenti, lock-nya hilang, tapi watcher pertama tetap jalan.
[ -f "$st/watch/$WPID2" ] && fail "lock watcher kedua tertinggal setelah dihentikan"
kill -0 "$WPID" 2> /dev/null || fail "watcher pertama ikut mati saat watcher kedua berhenti"
echo "PASS watcher_staging: watcher independen (satu mati, yang lain tetap hidup)"
stop_watcher "$WPID"
# --- lock basi (PID mati) dibersihkan saat watcher baru dimulai --------------
sleep 1 &
deadpid=$!
wait "$deadpid" 2> /dev/null
mkdir -p "$st/watch"
printf '%s\n' "demo" > "$st/watch/$deadpid"
out2="$tmp/watch-stale.out"
: > "$out2"
UNIT_STAGE="$st" UNIT_WATCH_INTERVAL=1 bash "$RUNNER" -w > "$out2" 2>&1 &
WPID=$!
wait_for "$out2" '\* Target   : demo' || {
  cat "$out2" >&2
  fail "lock basi tidak dibersihkan"
}
[ -f "$st/watch/$deadpid" ] && fail "lock basi tidak dibersihkan oleh watcher baru"
echo "PASS watcher_staging: lock basi dibersihkan"
stop_watcher
# --- run selesai: entri terakhir + summary dicetak sekali --------------------
kill "$RPID" 2> /dev/null
wait "$RPID" 2> /dev/null
RPID=""
printf 'beta\tcold\t1000\t2\t0\t0\t0\t1000\n' >> "$dl/timing.tsv"
printf '> Selesai beta [cold] 1.000s\n' >> "$dl/run.log"
printf 'RUN_ENDED=%s\nRUN_ELAPSED_MS=75000\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$dl/run.env"
rm -f "$dl/run.pid"
out3="$tmp/watch-done.out"
: > "$out3"
UNIT_STAGE="$st" UNIT_WATCH_INTERVAL=1 bash "$RUNNER" -w demo > "$out3" 2>&1 &
WPID=$!
wait_for "$out3" '\[2/2\] beta' \
  || {
    cat "$out3" >&2
    fail "entri tes terakhir tidak tampil saat run selesai"
  }
wait_for "$out3" '^\* PASS: 2 FAIL: 0 PRE: 0' \
  || {
    cat "$out3" >&2
    fail "baris PASS tes terakhir tidak sesuai timing.tsv"
  }
echo "PASS watcher_staging: entri terakhir tampil saat run selesai"
wait_for "$out3" '^# Summary' || {
  cat "$out3" >&2
  fail "summary tidak dicetak"
}
wait_for "$out3" '^\* 2/2 selesai, total 00:01:15' \
  || {
    cat "$out3" >&2
    fail "summary tidak memakai RUN_ELAPSED_MS (fmt_hms)"
  }
wait_for "$out3" '^\* PASS: 5 FAIL: 0 PRE: 0' \
  || {
    cat "$out3" >&2
    fail "summary tidak menjumlahkan PASS/FAIL/PRE"
  }
echo "PASS watcher_staging: summary sekali di akhir run"
[ "$(grep -c '^# Summary' "$out3")" -eq 1 ] \
  || {
    cat "$out3" >&2
    fail "summary tercetak lebih dari sekali"
  }
echo "PASS watcher_staging: summary tidak berulang tiap tick"
stop_watcher
# Pastikan tidak ada lock yang tertinggal (iterasi semua file di watch/).
for f in "$st/watch"/*; do
  [ -f "$f" ] || continue
  _p="$(basename "$f")"
  case "$_p" in '' | *[!0-9]*) continue ;; esac
  fail "lock tertinggal setelah watcher berhenti: $f"
done
echo "PASS watcher_staging: semua lock dihapus saat watcher berhenti"
# --- run mati tanpa RUN_ENDED: jangan mengarang hasil ------------------------
ml="$st/targets/mati/log"
mkdir -p "$ml" || fail "gagal membuat staging uji"
sleep 1 &
_dead=$!
wait "$_dead" 2> /dev/null
printf '%s\n' "$_dead" > "$ml/run.pid"
{
  printf 'RUN_STARTED=%s\nRUN_TARGET=mati\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'RUN_TESTS_COLD=1\nRUN_TESTS_SNAP=0\nRUN_LOG=%s\n' "$ml/run.log"
} > "$ml/run.env"
printf '1\talpha\tcold\n' > "$ml/plan.tsv"
printf 'operasi\tfase\tms\tpass\tfail\tpre\tsleep_ms\twork_ms\n' > "$ml/timing.tsv"
: > "$ml/run.log"
out4="$tmp/watch-dead.out"
: > "$out4"
UNIT_STAGE="$st" UNIT_WATCH_INTERVAL=1 bash "$RUNNER" -w mati > "$out4" 2>&1 &
WPID=$!
wait_for "$out4" '\* Target   : mati' || {
  cat "$out4" >&2
  fail "header target mati tidak tercetak"
}
sleep 2
grep -q '^# Summary' "$out4" \
  && {
    cat "$out4" >&2
    fail "run mati dilaporkan selesai (summary dicetak)"
  }
echo "PASS watcher_staging: run mati tanpa RUN_ENDED tidak mengarang hasil"
stop_watcher
# --- beberapa run aktif: `-w` tanpa target ditolak, dengan target diterima ---
for t in dua tiga; do
  tl="$st/targets/$t/log"
  mkdir -p "$tl" || fail "gagal membuat staging uji"
  sleep 60 &
  _p=$!
  [ "$t" = dua ] && RPID2="$_p" || RPID="$_p"
  printf '%s\n' "$_p" > "$tl/run.pid"
  {
    printf 'RUN_STARTED=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'RUN_TARGET=%s\nRUN_TESTS_COLD=2\nRUN_TESTS_SNAP=0\nRUN_LOG=%s\n' "$t" "$tl/run.log"
  } > "$tl/run.env"
  printf 'operasi\tfase\tms\tpass\tfail\tpre\tsleep_ms\twork_ms\n' > "$tl/timing.tsv"
  : > "$tl/run.log"
done
err2="$(UNIT_STAGE="$st" bash "$RUNNER" -w 2>&1)"
rc=$?
[ "$rc" -ne 0 ] || fail "-w tanpa target diterima padahal ada dua run aktif"
printf '%s' "$err2" | grep -q 'sebutkan yang mau dipantau' \
  || fail "pesan pemilihan target tidak muncul: $err2"
printf '%s' "$err2" | grep -q 'dua,tiga' \
  || fail "daftar run aktif tidak menyebut kedua target: $err2"
echo "PASS watcher_staging: beberapa run aktif menuntut target disebut"
out5="$tmp/watch-dua.out"
: > "$out5"
UNIT_STAGE="$st" UNIT_WATCH_INTERVAL=1 bash "$RUNNER" -w dua > "$out5" 2>&1 &
WPID=$!
wait_for "$out5" '\* Target   : dua - '"$RPID2" \
  || {
    cat "$out5" >&2
    fail "-w <target> tidak jalan saat ada beberapa run aktif"
  }
grep -q '\* Target   : tiga' "$out5" && {
  cat "$out5" >&2
  fail "-w dua menampilkan target lain"
}
echo "PASS watcher_staging: -w <target> dipakai saat run ambigu"
stop_watcher
kill "$RPID" "$RPID2" 2> /dev/null
wait "$RPID" 2> /dev/null
wait "$RPID2" 2> /dev/null
RPID=""
RPID2=""
rm -rf "$st/targets/dua" "$st/targets/tiga"
# --- tidak ada run aktif: `-w` tanpa target ditolak dengan daftar kandidat ---
err3="$(UNIT_STAGE="$st" bash "$RUNNER" -w 2>&1)"
rc=$?
[ "$rc" -ne 0 ] || fail "-w tanpa target diterima padahal tidak ada run aktif"
printf '%s' "$err3" | grep -q 'tidak ada run aktif' \
  || fail "pesan tidak ada run aktif tidak muncul: $err3"
printf '%s' "$err3" | grep -q 'demo' \
  || fail "pesan tidak menyebut kandidat target: $err3"
echo "PASS watcher_staging: tidak ada run aktif menuntut target disebut"
# --- argumen yang tidak sah ---------------------------------------------------
UNIT_STAGE="$st" bash "$RUNNER" -w demo -b > /dev/null 2>&1 \
  && fail "-w digabung -b diterima"
UNIT_STAGE="$st" bash "$RUNNER" -w demo -t alpha > /dev/null 2>&1 \
  && fail "-w digabung -t diterima"
UNIT_STAGE="$st" bash "$RUNNER" -w demo lain > /dev/null 2>&1 \
  && fail "-w dengan dua argumen diterima"
echo "PASS watcher_staging: argumen tidak sah ditolak"
echo "PASS watcher_staging"

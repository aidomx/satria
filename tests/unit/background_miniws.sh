#!/bin/sh
# Uji background build + watch (design/background.md) pada fixture miniws
# (workspace) dan proyek minimal satu-proyek (Buildfile).
# Perilaku kunci (masalah 26_10_2026, item Startup/Background):
#   - spawn TIDAK mencetak blok startup di terminal; barisnya direkam ke
#     .satria/background.startup (cold) sebagai satu-satunya sumber output
#     startup run background.
#   - `watch` me-replay blok startup, menampilkan progres, dan menutup run
#     dengan summary + status selama build MASIH berjalan.
#   - `watch` yang dibuka SETELAH build selesai DITOLAK: tidak ada pekerjaan
#     yang berjalan di background.
#   - run tanpa startup (warm) membuang rekaman lama.
# Pemakaian: sh tests/unit/background_miniws.sh <path-ke-satria> [path-ke-miniws]
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
SRC=$(abspath "${2:-$(dirname "$0")/../miniws}")
W=$(mktemp -d /tmp/bg_ws_XXXXXX) || exit 2
cp -r "$SRC/." "$W/" || exit 2
printf "flags = MMD, MP\nbackground = true\n" >> "$W/Buildfile.ws"
cd "$W" || exit 2
export NO_COLOR=1
pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }

# Child dihentikan (SIGSTOP) sebelum watch dijalankan supaya watch SELALU
# terpasang saat build masih berjalan — deterministik. Tanpa jeda ini watch
# bisa menyusul build yang sudah selesai, dan kini permintaan itu ditolak.
stop_bg() { kill -s STOP "$(cat "$1" 2>/dev/null)" 2>/dev/null; }
cont_bg() { kill -s CONT "$(cat "$1" 2>/dev/null)" 2>/dev/null; }

echo "== workspace (background = true di Buildfile.ws) =="

# §3A: spawn mengembalikan kontrol segera, tanpa output build di induk.
"$SAT" -w > out.txt 2>&1
check "spawn: rc=0 (berhasil memulai)" $?
! grep -q "Project :" out.txt
check "spawn: induk tidak menjalankan build sendiri" $?
! grep -q "startup :" out.txt
check "spawn: blok startup TIDAK dicetak di terminal" $?
[ -f .satria/workspace.pid ]
check "spawn: PID tertulis (.satria/workspace.pid)" $?
[ -s .satria/background.startup ] && grep -q "discovery" .satria/background.startup
check "spawn: startup direkam (cold)" $?

# §3B: watch terpasang SAAT build berjalan -> replay startup + progres,
#   lalu summary + status saat child menutup run.
stop_bg .satria/workspace.pid
"$SAT" -w watch > watch.txt 2>&1 &
wp=$!
sleep 1
cont_bg .satria/workspace.pid
wait $wp
check "watch: rc=0 setelah build sukses" $?
grep -q "startup :" watch.txt && grep -q "discovery" watch.txt
check "watch: me-replay blok startup" $?
grep -q "> progress :" watch.txt
check "watch: menampilkan progres" $?
grep -q "> status : success" watch.txt
check "watch: mencetak status success" $?
grep -q "Summary" watch.txt
check "watch: mencetak build summary" $?
sleep 1 # jendela teardown child: status ditulis, lalu PID dihapus
[ ! -f .satria/workspace.pid ]
check "teardown: PID dihapus setelah selesai" $?
[ -s .satria/background.log ] && grep -q "Summary" .satria/background.log
check "log: background.log berisi output build" $?

# Watch BARU setelah run final: DITOLAK (tidak ada pekerjaan berjalan).
"$SAT" -w watch > done.txt 2>&1
[ $? -eq 1 ] && grep -q "tidak ada build background" done.txt
check "watch setelah selesai: ditolak (rc=1)" $?
"$SAT" -w watch --full > done_full.txt 2>&1
[ $? -eq 1 ] && grep -q "tidak ada build background" done_full.txt
check "watch --full setelah selesai: ditolak (rc=1)" $?

# §3C: --full men-stream log live SELAMA build berjalan. Run baru dengan
#   sumber berubah = warm (startup.state sudah ada): tanpa blok startup dan
#   rekaman lama wajib sudah dibuang.
printf '\n/* perubahan uji background */\n' >> bade/src/bade.c
"$SAT" -w > warm.txt 2>&1
check "run warm: rc=0 (spawn)" $?
! grep -q "startup :" warm.txt
check "run warm: tetap tanpa blok startup" $?
[ ! -f .satria/background.startup ]
check "run warm: rekaman startup lama dibuang" $?
stop_bg .satria/workspace.pid
"$SAT" -w watch --full > full.txt 2>&1 &
fp=$!
sleep 1
cont_bg .satria/workspace.pid
wait $fp
check "watch --full: rc=0" $?
grep -q "> status : success" full.txt
check "watch --full: mencetak status success" $?
grep -q "Summary" full.txt
check "watch --full: men-stream isi log" $?

# No-op: run berikutnya TIDAK boleh masuk jalur background — dilayani inline
# oleh jalur router (fingerprint.cache), tanpa child dan tanpa output kosong.
"$SAT" -w > noop.out 2>&1
check "no-op ws: rc=0" $?
grep -q "Nothing to build" noop.out && grep -q "Cache valid" noop.out
check "no-op ws: pesan router inline" $?
[ ! -f .satria/workspace.pid ]
check "no-op ws: tidak ada child yang di-spawn" $?

# §4.1: PID hidup -> build berikutnya DITOLAK dengan pesan dari desain.
echo $$ > .satria/workspace.pid
"$SAT" -w > dup.txt 2>&1
[ $? -ne 0 ] && grep -q "Background build is already running" dup.txt
check "deteksi build ganda: ditolak + pesan desain" $?

# §4.4: clean DIBLOKIR selama build background berjalan.
"$SAT" -w clean > clean.txt 2>&1
[ $? -ne 0 ] && grep -q "diblokir" clean.txt
check "clean diblokir saat build background berjalan" $?

# watch tanpa build sama sekali: rc=1 + pesan jelas.
rm -f .satria/workspace.pid .satria/background.status
"$SAT" -w watch > none.txt 2>&1
[ $? -eq 1 ] && grep -q "tidak ada build background" none.txt
check "watch tanpa build: rc=1 + pesan" $?

echo "== satu proyek (background = true di Buildfile) =="
S=$(mktemp -d /tmp/bg_single_XXXXXX) || exit 2
mkdir -p "$S/src"
printf 'use project\n\nbackground = true\n' > "$S/Buildfile"
printf '#include <stdio.h>\nint main(void) { puts("bg"); return 0; }\n' > "$S/src/main.c"
cd "$S" || exit 2

"$SAT" > s_out.txt 2>&1
check "single: spawn rc=0" $?
! grep -q "startup :" s_out.txt
check "single: blok startup TIDAK dicetak" $?
[ -s .satria/background.startup ] && grep -q "loading configuration" .satria/background.startup
check "single: startup direkam (cold)" $?
[ -f .satria/build.pid ]
check "single: PID tertulis (.satria/build.pid)" $?

# §3B: watch terpasang saat build berjalan (lihat catatan SIGSTOP di atas).
stop_bg .satria/build.pid
"$SAT" watch > s_watch.txt 2>&1 &
sw=$!
sleep 1
cont_bg .satria/build.pid
wait $sw
check "single: watch rc=0 setelah sukses" $?
grep -q "startup :" s_watch.txt
check "single: watch me-replay startup" $?
grep -q "> status : success" s_watch.txt
check "single: watch mencetak status success" $?
sleep 1
[ ! -f .satria/build.pid ] && [ -s .satria/background.log ]
check "single: PID dihapus & log terisi" $?

# Setelah selesai: watch baru ditolak.
"$SAT" watch > s_done.txt 2>&1
[ $? -eq 1 ] && grep -q "tidak ada build background" s_done.txt
check "single: watch setelah selesai ditolak" $?

# No-op single: fast-state/router dijawab inline, tanpa spawn.
"$SAT" > s_noop.out 2>&1
check "single no-op: rc=0" $?
grep -q "Nothing to build" s_noop.out
check "single no-op: pesan router inline" $?
[ ! -f .satria/build.pid ]
check "single no-op: tidak ada child yang di-spawn" $?

echo $$ > .satria/build.pid
"$SAT" clean > s_clean.txt 2>&1
[ $? -ne 0 ] && grep -q "diblokir" s_clean.txt
check "single: clean diblokir" $?
rm -f .satria/build.pid

echo "---"
echo "lulus: $pass  gagal: $fail  (folder uji: $W)"
[ "$fail" -eq 0 ]
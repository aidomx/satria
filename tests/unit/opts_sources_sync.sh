#!/bin/sh
# Uji sambungan dua arah opts.cache <-> sources.cache (design/startup.md §4):
#   opts -> sources : route `sources` wajib ada dengan stempel nanodetik+ukuran;
#                     perubahan sources.cache (touch, isi dalam detik sama,
#                     hapus) menolak no-op sampai run sukses menyegarkan route.
#   sources -> opts : run sukses (merge P maupun re-walk) menyegarkan SEMUA route
#                     di opts.cache sehingga no-op kembali diambil run berikutnya.
# Pemakaian: sh tests/unit/opts_sources_sync.sh <path-ke-satria> [path-ke-miniws]
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
SRC=$(abspath "${2:-$(dirname "$0")/../miniws}")
[ -d "$SRC" ] || { echo "fixture tidak ditemukan: $SRC"; exit 2; }

pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }
note() { echo "SKIP  $1"; }

# route `sources` dengan time > 10^18 = stempel nanodetik (epoch-detik ~1.79e9).
route_ns_ok() {
  awk '$7=="sources" && $5+0 > 1000000000000000000 {found=1} END {exit !found}' "$1"
}
# Isi 1 hex pertama baris entri pertama (ukuran byte TETAP), lalu kembalikan
# mtime ke DETIK YANG SAMA seperti stempel route dengan ns sengaja berbeda.
# Pada route beresolusi detik (bug lama) gate lolos -> no-op salah diambil;
# dengan route ns gate menolak. Deterministik, tanpa balapan waktu (regresi H1).
# 0 = sukses, 6 = tanpa python3, 3 = tak ada entri.
corrupt_keep_sec() {
  command -v python3 >/dev/null 2>&1 || return 6
  python3 - "$1" <<'PYEOF'
import sys, os, re
p = sys.argv[1]
data = open(p, "rb").read()
sec = int(os.stat(p).st_mtime)
lines = data.split(b"\n")
for i, ln in enumerate(lines):
    m = re.match(rb"^([0-9a-f]{16}) (S|D) ", ln)
    if m:
        a = m.group(1)
        lines[i] = (b"0" if a[:1] != b"0" else b"1") + a[1:] + ln[16:]
        break
else:
    sys.exit(3)
out = b"\n".join(lines)
if len(out) != len(data):
    sys.exit(4)
open(p, "wb").write(out)
# detik sama (sama seperti route), ns pasti berbeda (offset tetap 123456789)
os.utime(p, ns=(sec * 10**9 + 123456789, sec * 10**9 + 123456789))
PYEOF
}

export NO_COLOR=1
BK=$(mktemp /tmp/opts_sync_bak_XXXX)

# ============================= MODE WORKSPACE =============================
W=$(mktemp -d /tmp/opts_sync_ws_XXXX)
cp -r "$SRC/." "$W/" || exit 2
printf "flags = MMD, MP\n" >> "$W/Buildfile.ws"
cd "$W" || exit 2
run() { "$SAT" -w >"$W/out.txt" 2>&1; return $?; }
valid() { grep -q "Nothing to build" "$W/out.txt" && grep -q "Cache valid:" "$W/out.txt"; }

run; check "ws: cold build" $?
sleep 1 # hindari racy terhadap run pertama
route_ns_ok .satria/opts.cache; check "ws: route sources ada dengan stempel ns" $?
run; valid; check "ws: no-op diambil setelah cold" $?

# arah opts -> sources: perubahan stempel file menolak no-op, run sukses resync
touch .satria/sources.cache
run; ! valid; check "ws: touch sources.cache menolak no-op" $?
run; valid; check "ws: setelah resync route, no-op lagi" $?

# sources -> opts: sumber baru -> sources.cache berubah -> run sukses -> no-op
echo 'int z_sync(void){return 1;}' > bade/src/z_sync.c
run; ! valid; check "ws: sumber baru memicu build" $?
grep -q "z_sync" .satria/sources.cache; check "ws: sumber baru masuk sources.cache" $?
run; valid; check "ws: no-op sesudah build sumber baru (route tersinkron)" $?

# regresi H1: isi sources.cache berubah, ukuran & detik mtime sama, ns berbeda
cp .satria/sources.cache "$BK"
corrupt_keep_sec .satria/sources.cache
crc=$?
if [ "$crc" -eq 0 ]; then
  run; ! valid; check "ws: perubahan isi same-second+same-size ditolak" $?
  cmp -s .satria/sources.cache "$BK"; check "ws: re-walk memperbaiki isi yang korup" $?
  run; valid; check "ws: no-op pulih setelah perbaikan" $?
elif [ "$crc" -eq 6 ]; then
  note "ws: python3 tidak ada — regresi same-second tidak diuji"
else
  cp "$BK" .satria/sources.cache
  note "ws: entri tak ditemukan (crc=$crc) — regresi same-second tidak diuji"
fi

# indeks dihapus: run sukses membangun ulang + route konsisten
rm .satria/sources.cache
run; ! valid; check "ws: kehilangan sources.cache menolak no-op" $?
head -1 .satria/sources.cache | grep -q "^SATRIASRC2 "; check "ws: sources.cache dibangun ulang (SATRIASRC2)" $?
run; valid; check "ws: no-op setelah indeks dibangun ulang" $?

# ============================ MODE SINGLE-PROJECT ============================
S=$(mktemp -d /tmp/opts_sync_s_XXXX)
mkdir -p "$S/src"
printf 'use project\n\nsources = src\n' > "$S/Buildfile"
printf 'int main(void){return 0;}\n' > "$S/src/main.c"
cd "$S" || exit 2
run() { "$SAT" >"$S/out.txt" 2>&1; return $?; }
ostamp() { stat -c '%y' .satria/opts.cache; }

run; check "single: cold build" $?
grep -q "^SATRIASRC2 " .satria/sources.cache; check "single: sources.cache format SATRIASRC2" $?
route_ns_ok .satria/opts.cache; check "single: route sources ada dengan stempel ns" $?
sleep 1
a=$(ostamp); run; b=$(ostamp)
[ "$a" = "$b" ]; check "single: no-op tidak menulis opts.cache" $?

# arah opts -> sources
touch .satria/sources.cache
a=$(ostamp); run; b=$(ostamp)
[ "$a" != "$b" ]; check "single: touch sources.cache memaksa jalur penuh" $?
a=$(ostamp); run; b=$(ostamp)
[ "$a" = "$b" ]; check "single: setelah resync route, no-op lagi" $?

# regresi H1
cp .satria/sources.cache "$BK"
corrupt_keep_sec .satria/sources.cache
crc=$?
if [ "$crc" -eq 0 ]; then
  a=$(ostamp); run; b=$(ostamp)
  [ "$a" != "$b" ]; check "single: perubahan isi same-second+same-size ditolak" $?
  cmp -s .satria/sources.cache "$BK"; check "single: re-walk memperbaiki isi yang korup" $?
  a=$(ostamp); run; b=$(ostamp)
  [ "$a" = "$b" ]; check "single: no-op pulih setelah perbaikan" $?
elif [ "$crc" -eq 6 ]; then
  note "single: python3 tidak ada — regresi same-second tidak diuji"
else
  cp "$BK" .satria/sources.cache
  note "single: entri tak ditemukan (crc=$crc) — regresi same-second tidak diuji"
fi

# arah sources -> opts: sumber baru
echo 'int z2(void){return 2;}' > "$S/src/z2.c"
a=$(ostamp); run; b=$(ostamp)
[ "$a" != "$b" ]; check "single: sumber baru memicu build" $?
grep -q "z2.c" .satria/sources.cache; check "single: sumber baru masuk sources.cache" $?
a=$(ostamp); run; b=$(ostamp)
[ "$a" = "$b" ]; check "single: no-op sesudah build sumber baru" $?

rm -f "$BK"
echo "---"
echo "lulus: $pass  gagal: $fail  (folder uji: $W | $S)"
[ "$fail" -eq 0 ]

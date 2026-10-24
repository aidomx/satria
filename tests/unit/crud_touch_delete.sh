#!/bin/sh
# Unit test CRUD per-source (design/startup.md §4; opsi minimal terpilih):
#   update = SENTUH file -> stamp di-PATCH via hash isi, TANPA rebuild
#   delete = sumber dihapus -> entri hilang dari cache, no-op pulih
#   create = sumber ditambah -> tercatat + rebuild (kontrol)
# Kedua mode (workspace & single-project). Router = opts.cache + cache store.
# Pemakaian: sh tests/unit/crud_touch_delete.sh <path-ke-satria> [path-ke-miniws]
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
SRC=$(abspath "${2:-$(dirname "$0")/../miniws}")
[ -d "$SRC" ] || { echo "fixture tidak ditemukan: $SRC"; exit 2; }
export NO_COLOR=1

pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }
hit_ws() { grep -q "Nothing to build" "$1" && grep -q "Cache valid:" "$1"; }
ostamp() { stat -c '%y' .satria/opts.cache; }

# ============================ MODE WORKSPACE ============================
W=$(mktemp -d /tmp/crud_ws_XXXX)
cp -r "$SRC/." "$W/" || exit 2
printf "flags = MMD, MP\n" >> "$W/Buildfile.ws"
cd "$W" || exit 2
# Tata letak miniws: source proyek core berada di core/src.
ENGDIR=core/src

run() { "$SAT" -w >"$W/out.txt" 2>&1; return $?; }

run; check "ws: cold build" $?
run; hit_ws out.txt; check "ws: no-op dasar" $?

# update: sentuh sumber -> PATCH fingerprint, TANPA rebuild
sleep 1.1
touch "$ENGDIR/core.c"
run
hit_ws out.txt; check "ws: sentuh sumber = no-op (tanpa rebuild)" $?

# create: sumber baru -> tercatat + rebuild + no-op pulih
echo 'int crud_tmp(void){return 1;}' > "$ENGDIR/crud_tmp.c"
run
! hit_ws out.txt; check "ws: sumber baru memicu build (create)" $?
grep -q "crud_tmp" .satria/sources.cache; check "ws: sumber baru tercatat di sources.cache" $?
run; hit_ws out.txt; check "ws: no-op setelah create" $?

# delete: hapus sumber -> entri hilang, rebuild, no-op pulih
rm "$ENGDIR/crud_tmp.c"
run
! hit_ws out.txt; check "ws: hapus sumber membatalkan no-op (delete)" $?
! grep -q "crud_tmp" .satria/sources.cache
check "ws: entri sumber terhapus dari sources.cache" $?
run; hit_ws out.txt; check "ws: no-op pulih setelah delete" $?

# ============================ MODE SINGLE ============================
S=$(mktemp -d /tmp/crud_s_XXXX)
mkdir -p "$S/src"
printf 'use project\n\nsources = src\n' > "$S/Buildfile"
printf 'int main(void){return 0;}\n' > "$S/src/main.c"
printf 'int crud2(void){return 2;}\n' > "$S/src/crud2.c"
cd "$S" || exit 2
run() { "$SAT" >"$S/out.txt" 2>&1; return $?; }

run; check "single: cold build" $?
a=$(ostamp); run; b=$(ostamp)
[ "$a" = "$b" ]; check "single: no-op dasar (opts tak tertulis)" $?
grep -q "Cache valid: .satria/caches/fingerprint.cache" "$S/out.txt"
check "single: pesan no-op 'Nothing to build' (paralel workspace)" $?
pt=$(grep -ac '^P ' .satria/opts.cache); pm=$(grep -acE '^P [0-9a-f]{16} [0-9a-f]{16} [0-9]+ [0-9]+$' .satria/opts.cache)
[ "$pt" -gt 0 ] && [ "$pm" -eq "$pt" ]; check "single: P opts.cache bermetadata ($pm/$pt)" $?

# update: sentuh -> fast path TETAP dipakai (opts tak berubah) + build.state di-PATCH
sleep 1.1
touch src/crud2.c
s1=$(stat -c '%y' .satria/caches/fingerprint.cache)
a=$(ostamp); run; b=$(ostamp); s2=$(stat -c '%y' .satria/caches/fingerprint.cache)
[ "$a" = "$b" ]; check "single: sentuh sumber = fast path (tanpa rebuild penuh)" $?
[ "$s1" != "$s2" ]; check "single: sentuh -> build.state di-PATCH (stamp diperbarui)" $?
grep -q "Status     : Success" "$S/out.txt"; check "single: no-op tetap informatif" $?
a=$(ostamp); run; b=$(ostamp)
[ "$a" = "$b" ]; check "single: no-op lagi setelah PATCH" $?

# create: sumber baru -> build (opts tertulis) + no-op pulih
echo 'int crud3(void){return 3;}' > src/crud3.c
a=$(ostamp); run; b=$(ostamp)
[ "$a" != "$b" ]; check "single: sumber baru memicu build (create)" $?
a=$(ostamp); run; b=$(ostamp)
[ "$a" = "$b" ]; check "single: no-op setelah create" $?

# delete: hapus -> build + no-op pulih
rm src/crud2.c
a=$(ostamp); run; b=$(ostamp)
[ "$a" != "$b" ]; check "single: hapus sumber memicu build (delete)" $?
a=$(ostamp); run; b=$(ostamp)
[ "$a" = "$b" ]; check "single: no-op pulih setelah delete" $?

echo "---"
echo "lulus: $pass  gagal: $fail  (folder uji: $W | $S)"
[ "$fail" -eq 0 ]

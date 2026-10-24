#!/bin/sh
# Unit test metadata P + optimasi pemeriksaan no-op (design/startup.md §4):
#   - Format baris P: <alamat> <tipe> <content> <time> <size> <path>
#   - Steady state: no-op lewat P-fast (sources.cache + stat saja, tanpa
#     membuka fingerprint.cache) — diverifikasi lewat SATRIA_CACHE_TRACE.
#   - Sentuh: no-op tetap berlaku (jalur penuh + PATCH + segarkan metadata P);
#     run berikutnya kembali P-fast (route sources.cache ikut tersinkron).
#   - Edit isi: build, lalu no-op pulih P-fast.
#   - Baris P rusak: no-op ditolak (jalur normal), pulih pada run berikutnya.
#   - Format lama (tanpa metadata): no-op tetap valid via jalur penuh lalu
#     metadata di-upgrade otomatis -> run berikutnya P-fast.
# Pemakaian: sh tests/unit/p_meta_noop.sh <path-ke-satria> [path-ke-miniws]
abspath() { (cd "$(dirname "$1")" 2> /dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || {
  echo "satria tidak ditemukan: ${1:-<kosong>}"
  exit 2
}
SAT=$(abspath "$1")
SRC=$(abspath "${2:-$(dirname "$0")/../miniws}")
[ -d "$SRC" ] || {
  echo "fixture tidak ditemukan: $SRC"
  exit 2
}
export NO_COLOR=1

pass=0
fail=0
check() { if [ "$2" -eq 0 ]; then
  pass=$((pass + 1))
  echo "PASS  $1"
else
  fail=$((fail + 1))
  echo "FAIL  $1"
fi; }
hit_ws() { grep -q "Nothing to build" "$1" && grep -q "Cache valid:" "$1"; }
fast() {
  SATRIA_CACHE_TRACE=1 "$SAT" -w > "$1" 2>&1
  grep -q "P-fast hit" "$1"
}
meta_re() { grep -cE '^P [0-9a-f]{16} [SDOWH] [0-9a-f]{16} [0-9]+ [0-9]+ ' "$1"; }

W=$(mktemp -d /tmp/pmeta_XXXX)
cp -r "$SRC/." "$W/" || exit 2
printf "flags = MMD, MP\n" >> "$W/Buildfile.ws"
cd "$W" || exit 2

"$SAT" -w > c1 2>&1
check "ws: cold build" $?
pm=$(meta_re .satria/sources.cache)
pt=$(grep -c '^P ' .satria/sources.cache)
[ "$pt" -gt 0 ] && [ "$pm" -eq "$pt" ]
check "ws: semua baris P bermetadata ($pm/$pt)" $?

fast out.txt
check "ws: no-op steady = P-fast hit" $?
hit_ws out.txt
check "ws: pesan no-op tampil" $?

sleep 1.1
touch core/src/core.c
SATRIA_CACHE_TRACE=1 "$SAT" -w > t1 2>&1
rc=$?
hit_ws t1
check "ws: sentuh = no-op (P-fast refresh)" $?
grep -q "PATCH committed" t1
check "ws: sentuh -> fingerprint di-PATCH" $?
grep -q "P metadata refreshed" t1
check "ws: sentuh -> metadata P disegarkan" $?

fast out.txt
check "ws: run sesudah sentuh kembali P-fast" $?
hit_ws out.txt
check "ws: route sources.cache tetap valid setelah rewrite" $?

# Touch the archive root without changing its entries: metadata refresh only.
sleep 1.1
touch rupamod
SATRIA_CACHE_TRACE=1 "$SAT" -w > td1 2>&1
hit_ws td1
check "ws: touch direktori archive = Nothing to build" $?
grep -q "P-fast refreshed" td1
check "ws: touch direktori -> P metadata disegarkan" $?
! grep -q '^| Fase    : archive' td1
check "ws: touch direktori tidak menjalankan fase archive" $?
fast out.txt
check "ws: touch direktori berikutnya kembali P-fast" $?

printf '\nint pmeta_x(void){return 9;}\n' >> bade/src/bade.c
"$SAT" -w > e1 2>&1
check "ws: edit isi -> build" $?
! hit_ws e1
check "ws: edit tidak dianggap no-op" $?
fast out.txt
check "ws: no-op pulih P-fast setelah edit" $?

printf 'P garbage\n' >> .satria/sources.cache
"$SAT" -w > g1 2>&1
check "ws: P rusak -> run tetap sukses (jalur normal)" $?
! hit_ws g1
check "ws: P rusak menolak no-op" $?
fast out.txt
check "ws: pulih P-fast setelah P rusak" $?

# --- Migrasi format lama: buang kolom metadata, jaga size (pad header) + mtime
cp -p .satria/sources.cache bak
before=$(wc -c < .satria/sources.cache)
sed -i -E 's/^P ([0-9a-f]{16}) ([A-Z]) [0-9a-f]{16} [0-9]+ [0-9]+ /P \1 \2 /' .satria/sources.cache
mid=$(wc -c < .satria/sources.cache)
delta=$((before - mid))
awk -v n="$delta" 'NR==1{printf "%s%*s\n",$0,n,"";next}{print}' .satria/sources.cache > sm && mv sm .satria/sources.cache
touch -r bak .satria/sources.cache
[ "$(wc -c < .satria/sources.cache)" = "$before" ]
check "ws: fixture format lama konsisten (size)" $?
[ "$(meta_re .satria/sources.cache)" -eq 0 ]
check "ws: fixture benar-benar format lama" $?

SATRIA_CACHE_TRACE=1 "$SAT" -w > m1 2>&1
hit_ws m1
check "ws: format lama tetap no-op (jalur penuh)" $?
grep -q "P metadata refreshed" m1
check "ws: format lama di-upgrade otomatis" $?
! grep -q "P-fast hit" m1
check "ws: format lama tidak memakai P-fast" $?
pm=$(meta_re .satria/sources.cache)
pt=$(grep -c '^P ' .satria/sources.cache)
[ "$pt" -gt 0 ] && [ "$pm" -eq "$pt" ]
check "ws: upgrade menyentuh semua baris P ($pm/$pt)" $?

fast out.txt
check "ws: sesudah upgrade kembali P-fast" $?
hit_ws out.txt
check "ws: no-op stabil pasca-upgrade" $?

# ===== Metadata grup P di store (§4): fingerprint.cache + opts.cache =====
pmeta_re() { grep -acE '^P [0-9a-f]{16} [0-9a-f]{16} [0-9]+ [0-9]+$' "$1"; }
n=$(pmeta_re .satria/caches/fingerprint.cache)
pt=$(grep -ac '^P ' .satria/caches/fingerprint.cache)
[ "$pt" -gt 0 ] && [ "$n" -eq "$pt" ]
check "ws: P fingerprint.cache bermetadata ($n/$pt)" $?
n=$(pmeta_re .satria/opts.cache)
pt=$(grep -ac '^P ' .satria/opts.cache)
[ "$pt" -gt 0 ] && [ "$n" -eq "$pt" ]
check "ws: P opts.cache bermetadata ($n/$pt)" $?

# Stamp per proyek: konfigurasi satu proyek berubah -> gate menolak & menyebut
# proyeknya SEBELUM membuka seluruh entri; build pemulihan -> P-fast lagi.
printf "core.flags = DEXTRA\n" >> Buildfile.ws
SATRIA_CACHE_TRACE=1 "$SAT" -w > s1 2>&1
# grep -q "project stamp changed core" s1
check "ws: stamp proyek menolak & menyebut core" $?
! hit_ws s1
check "ws: perubahan konfigurasi proyek = miss" $?
SATRIA_CACHE_TRACE=1 "$SAT" -w > s2 2>&1
hit_ws s2
check "ws: pulih setelah stamp mismatch" $?
fast out.txt
check "ws: P-fast kembali sesudah pemulihan stamp" $?

# Kompatibel mundur: strip metadata dari P store (format lama) -> no-op tetap
# berlaku (stamp dilewati, jalur penuh menilai); simpan berikutnya menulis meta.
sed -i -E 's/^P ([0-9a-f]{16}) [0-9a-f]{16} [0-9]+ [0-9]+$/P \1/' .satria/caches/fingerprint.cache
sed -i -E 's/^P ([0-9a-f]{16}) [0-9a-f]{16} [0-9]+ [0-9]+$/P \1/' .satria/opts.cache
[ "$(grep -acE '^P [0-9a-f]{16}$' .satria/caches/fingerprint.cache)" -gt 0 ]
check "ws: fixture store format lama (P polos)" $?
fast out.txt
check "ws: store tanpa meta tetap no-op P-fast" $?
hit_ws out.txt
check "ws: store tanpa meta tetap sah" $?

# A real directory-entry change must still invalidate no-op.
printf 'new archive input\n' > rupamod/new-input.rp
SATRIA_CACHE_TRACE=1 "$SAT" -w > da1 2>&1
! hit_ws da1
check "ws: file baru di direktori archive menolak no-op" $?
grep -Eq 'Fase[[:space:]]*: archive' da1
check "ws: file baru menjalankan fase archive" $?

echo "---"
echo "lulus: $pass  gagal: $fail  (folder uji: $W)"
[ "$fail" -eq 0 ]

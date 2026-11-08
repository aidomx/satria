#!/bin/sh
# Uji cache no-op pada fixture workspace (default: tests/miniws). Default AKTIF
# (tukar default 2026-10-19); nonaktifkan dengan SATRIA_CACHE_NOOP=0.
# Pemakaian: sh tests/unit/common/cache_noop.sh <path-ke-satria> [path-ke-workspace]
case "${1:-}" in
  /*) SAT="$1" ;;
  *) SAT="$(pwd)/${1:-bin/satria}" ;;
esac
SRC="${2:-$(dirname "$0")/../../miniws}"
. "$(dirname "$0")/../lib/layout.sh"
[ -x "$SAT" ] || {
  echo "satria tidak ditemukan: $SAT"
  exit 2
}
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/cache_noop"; rm -rf "${W}"; mkdir -p "${W}" || exit 1
cp -rf "$SRC/." "$W/" || exit 2
printf "flags = MMD, MP\n" >> "$W/Buildfile.ws" # cache butuh .d (-MMD)
cd "$W" || exit 2
export NO_COLOR=1
# Jeda agar mtime salinan tidak racy terhadap build pertama (lihat workspace.c, wsCacheVisit).
wait_tick
# Sumber yang dimutasi diambil dari Buildfile.ws: nama berkasnya berbeda antar
# target (miniws: core/src/core.c, rupaws: core/src/main.c).
SOURCE=$(unit_project_source . core)
OUTPUT=$(unit_project_binary . core)
if [ -z "$SOURCE" ]; then
  unit_precondition "fixture tidak dikenali: tidak ada source untuk mutasi"
fi
EXTRA="$(dirname "$SOURCE")/cache_noop_extra.c"
pass=0
fail=0
check() { # $1 = nama, $2 = kondisi (0 = benar)
  if [ "$2" -eq 0 ]; then
    pass=$((pass + 1))
    echo "PASS  $1"
  else
    fail=$((fail + 1))
    echo "FAIL  $1"
  fi
}
run() {
  SATRIA_CACHE_NOOP=1 "$SAT" -j${UNIT_JOBS:-2} -w > "$W/out.txt" 2>&1
  return $?
}
valid() { grep -q "Nothing to build" "$W/out.txt" && grep -q "Cache valid *:" "$W/out.txt"; }

run
check "cold build berhasil" $?
[ -s .satria/caches/fingerprint.cache ] && [ -s .satria/sources.cache ]
check "cache tertulis setelah build sukses" $?

run
check "no-op dengan cache: keluar cepat" $?
valid
check "no-op memakai cache (pesan cache valid)" $?

"$SAT" -j${UNIT_JOBS:-2} -w > "$W/out_off.txt" 2>&1
grep -q "Cache valid *:" "$W/out_off.txt"
check "tanpa flag: jalur cache aktif (default on)" $?

SATRIA_CACHE_NOOP=0 "$SAT" -j${UNIT_JOBS:-2} -w > "$W/out_off.txt" 2>&1
! grep -q "Cache valid *:" "$W/out_off.txt"
check "SATRIA_CACHE_NOOP=0: jalur lama, tidak memakai cache" $?

# touch: isi sama -> tetap valid
wait_tick
touch "$SOURCE" || exit 2
run
valid
check "touch (isi sama) tetap no-op" $?

# edit sumber -> tidak valid, build jalan, lalu valid lagi
wait_tick
echo "/* edit */" >> "$SOURCE"
run
! valid
check "edit sumber: tidak no-op" $?
run
valid
check "setelah build ulang: no-op lagi" $?

# hapus output -> tidak valid
rm -f "$OUTPUT"
run
! valid
check "output hilang: tidak no-op" $?
[ -f "$OUTPUT" ]
check "output dibangun ulang" $?
run
valid
check "setelah output pulih: no-op lagi" $?

# Buildfile.ws berubah -> tidak valid
wait_tick
echo "# x" >> Buildfile.ws
run
! valid
check "Buildfile.ws berubah: tidak no-op" $?
run
valid
check "setelah Buildfile disimpan ulang: no-op lagi" $?

# file sumber baru -> direktori berubah -> tidak valid
wait_tick
echo "/* baru */" > "$EXTRA"
run
! valid
check "file sumber baru: tidak no-op" $?
[ -f "$EXTRA" ] || exit 2

# cache rusak -> jalur normal, tidak crash
printf 'P garbage\n' >> .satria/sources.cache
run
! valid
check "sources.cache mapping rusak: jalur normal" $?
run
check "build tetap berhasil setelah cache rusak" $?

# Format baru: header SATRIASRC2 + entri <alamat> <type> <size> <mtime> <path>
head -1 .satria/sources.cache | grep -q '^SATRIASRC2 '
check "sources.cache memakai format SATRIASRC2" $?
grep -qE '^[0-9a-f]{16} S [0-9]+ [0-9]+ ' .satria/sources.cache
check "entri sumber: <alamat> S <size> <mtime> <path>" $?
grep -qE '^[0-9a-f]{16} D 0 -?[0-9]+ ' .satria/sources.cache
check "entri folder: <alamat> D 0 <mtime> <path>" $?

# Alamat salah -> record invalid -> re-walk (paksa jalur walk: hapus output)
sed 's/^[0-9a-f]\{16\} S /0000000000000000 S /' .satria/sources.cache > .satria/sources.cache.bad
mv .satria/sources.cache.bad .satria/sources.cache
rm -f "$OUTPUT"
run
[ -f "$OUTPUT" ] && head -1 .satria/sources.cache | grep -q '^SATRIASRC2 ' && \
  ! grep -q '^0000000000000000 S ' .satria/sources.cache
check "alamat salah: record dihitung ulang, entri diperbaiki" $?
run
valid
check "setelah rekalkulasi alamat: no-op lagi" $?

# File format lama (SATRIASRC1) diabaikan dan ditulis ulang (self-heal)
sed 's/^SATRIASRC2 /SATRIASRC1 /' .satria/sources.cache > .satria/sources.cache.old
mv .satria/sources.cache.old .satria/sources.cache
rm -f "$OUTPUT"
run
[ -f "$OUTPUT" ] && head -1 .satria/sources.cache | grep -q '^SATRIASRC2 '
check "cache SATRIASRC1 self-heal ke SATRIASRC2" $?
run
valid
check "setelah self-heal: no-op lagi" $?

echo "---"
echo "lulus: $pass  gagal: $fail  (folder uji: $W)"
[ "$fail" -eq 0 ]

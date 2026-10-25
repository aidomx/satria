#!/bin/sh
# Unit test design/startup.md §9 (artifact.cache) dan §10 (report.blob):
#   - cold/build sukses -> .satria/report.blob + .satria/artifact.cache terbentuk
#   - no-op ws -> laporan tersimpan ditampilkan ke terminal, pesan no-op tetap terakhir
#   - release aktif: no-op KINI berlaku (gerbang dibuka; validasi via artifact.cache)
#   - artefak/input release berubah -> batal no-op + dibangun ulang; sentuh -> PATCH
#   - single-project: kedua file ikut terbentuk, no-op tetap informatif
# Pemakaian: sh tests/unit/report_artifact.sh <satria> [miniws]
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
MWS=$(abspath "${2:-$(dirname "$0")/../miniws}")
[ -d "$MWS" ] || { echo "fixture tidak ditemukan: $MWS"; exit 2; }
export NO_COLOR=1

pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }
hit_ws() { grep -q "Nothing to build" "$1" && grep -q "Cache valid:" "$1"; }

# ===================== workspace miniws: report.blob =====================
W=$(mktemp -d /tmp/rpt_ws_XXXX)
cp -r "$MWS/." "$W/" || exit 2
printf "flags = MMD, MP\n" >> "$W/Buildfile.ws"
cd "$W" || exit 2
"$SAT" -w > c.txt 2>&1; check "miniws: cold build" $?
[ -s .satria/report.blob ]; check "miniws: report.blob terbentuk dan tidak kosong" $?
grep -q "Status     : Success" .satria/report.blob; check "miniws: report.blob berisi detail laporan" $?
grep -q "^STATUS$" .satria/report.blob && grep -q "Cache valid: .satria/caches/fingerprint.cache" .satria/report.blob
check "miniws: report.blob blok STATUS (format §10)" $?
grep -q "^LAST BUILD SUMMARY$" .satria/report.blob && grep -q "Total time : " .satria/report.blob
grep -q "Libraries  : [0-9]* generated" .satria/report.blob && grep -q "Binaries   : [0-9]* linked" .satria/report.blob
check "miniws: report.blob blok LAST BUILD SUMMARY agregat" $?
[ -f .satria/artifact.cache ]; check "miniws: artifact.cache terbentuk (§9)" $?
"$SAT" -w > n.txt 2>&1; check "miniws: no-op rc=0" $?
grep -q "Nothing to build" n.txt; check "miniws: pesan no-op tetap ada" $?
grep -q "Status     : Success" n.txt; check "miniws: no-op menampilkan detail dari report.blob" $?
# urutan replay (format §10): blok STATUS (pesan no-op) mendahului LAST BUILD SUMMARY
sline=$(grep -n "Status     : Success" n.txt | head -1 | cut -d: -f1)
nline=$(grep -n "Nothing to build" n.txt | head -1 | cut -d: -f1)
[ -n "$sline" ] && [ -n "$nline" ] && [ "$nline" -lt "$sline" ]
check "miniws: urutan output = STATUS dulu, ringkasan sesudah" $?

# ===================== workspace miniws + release: release + artifact =====================
# Salinan miniws ditambahi konfigurasi release (fixture asli tidak diubah).
R=$(mktemp -d /tmp/rpt_rel_XXXX)
cp -r "$MWS/." "$R/" || exit 2
cd "$R" || exit 2
rm -rf build bin lib modules dist .satria
printf 'perintah-test\n' > core/src/cmd.txt
cat >> Buildfile.ws <<'REL'
release.name = rupa
release.version = 0.2.4
release.files = core/src/cmd.txt
release.output = dist/{name}-v{version}.tar.gz
release.format = tar
release.compress = gzip
release.checksum = sha256
REL
"$SAT" -w > rc0.txt 2>&1; check "release: cold build (release)" $?
grep -c "dist/rupa-v" .satria/artifact.cache > /tmp/rpt_cnt.$$
[ "$(cat /tmp/rpt_cnt.$$)" -gt 0 ]; check "release: artefak dist tercatat di artifact.cache" $?
grep -q "cmd.txt" .satria/artifact.cache
check "release: input release (cmd.txt) tercatat di artifact.cache" $?
grep -q "rupa_modules.tar" .satria/artifact.cache
check "release: arsip proyek kemasan tercatat di artifact.cache" $?
"$SAT" -w > rc1.txt 2>&1
hit_ws rc1.txt; check "release: no-op HIT saat release aktif (gerbang terbuka)" $?

# artefak dihapus -> batal no-op + dibangun ulang + hit lagi
rm -f dist/*.tar.gz
"$SAT" -w > rc2.txt 2>&1
! hit_ws rc2.txt; check "release: artefak hilang membatalkan no-op" $?
ls dist/*.tar.gz >/dev/null 2>&1; check "release: artefak dibangun ulang" $?
"$SAT" -w > rc3.txt 2>&1
hit_ws rc3.txt; check "release: no-op pulih setelah artefak terbangun" $?

# input release (di luar fingerprint) berubah isi -> batal no-op + release ulang
echo "perubahan-test" >> core/src/cmd.txt
"$SAT" -w > rc4.txt 2>&1
! hit_ws rc4.txt; check "release: edit input release membatalkan no-op" $?
grep -q "Package" rc4.txt; check "release: fase release berjalan ulang" $?
"$SAT" -w > rc5.txt 2>&1
hit_ws rc5.txt; check "release: no-op pulih setelah input sinkron" $?

# sentuh artefak (stempel beda, isi sama) -> PATCH, no-op tetap diambil
sleep 1.1
touch dist/*.tar.gz
"$SAT" -w > rc6.txt 2>&1
hit_ws rc6.txt; check "release: sentuh artefak tetap no-op (PATCH hash)" $?
rm -f /tmp/rpt_cnt.$$

# ===================== single-project =====================
S=$(mktemp -d /tmp/rpt_s_XXXX)
mkdir -p "$S/src"
printf 'use project\n\nsources = src\n' > "$S/Buildfile"
printf 'int main(void){return 0;}\n' > "$S/src/main.c"
cd "$S" || exit 2
"$SAT" > sc.txt 2>&1; check "single: cold build" $?
[ -s .satria/report.blob ]; check "single: report.blob terbentuk" $?
[ -f .satria/artifact.cache ]; check "single: artifact.cache terbentuk" $?
"$SAT" > sn.txt 2>&1; check "single: no-op rc=0" $?
grep -q "Status     : Success" sn.txt; check "single: no-op informatif dari state" $?

echo "---"
echo "lulus: $pass  gagal: $fail  (folder uji: $W | $R | $S)"
[ "$fail" -eq 0 ]

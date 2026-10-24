#!/bin/sh
# Unit test audit penghapusan state lama (design/startup.md, Daftar Implementasi):
#   - .satria/workspace.state, .satria/workspace/<p>.<leaf> lama, <p>.hdr/,
#     .satria/{build.state,build.lib.state,build.fingerprint}, paths.cache
#     dibuang otomatis pada run (workspace & single) dan saat clean.
#   - .satria/workspace/<p>.Buildfile (CURRENT — cache config) TIDAK disentuh.
#   - No-op tetap berlaku setelah penyapuan (state lama memang tak terbaca).
# Pemakaian: sh tests/unit/legacy_sweep.sh <path-ke-satria> [path-ke-miniws]
abspath() { (cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
[ -n "$1" ] && [ -x "$1" ] || { echo "satria tidak ditemukan: ${1:-<kosong>}"; exit 2; }
SAT=$(abspath "$1")
SRC=$(abspath "${2:-$(dirname "$0")/../miniws}")
[ -d "$SRC" ] || { echo "fixture tidak ditemukan: $SRC"; exit 2; }
export NO_COLOR=1

pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "PASS  $1"; else fail=$((fail+1)); echo "FAIL  $1"; fi; }
hit_ws() { grep -q "Nothing to build" "$1" && grep -q "Cache valid:" "$1"; }

# Tanam sampah legacy + penanda current. Args: $1 = folder .satria workspace-nya
plant() {
  d=$1
  : > "$d/workspace.state"
  mkdir -p "$d/workspace"
  : > "$d/workspace/core.build.fingerprint"
  : > "$d/workspace/core.ws.state"
  : > "$d/workspace/core.ws.lib.state"
  : > "$d/workspace/core.deps.cache"
  : > "$d/workspace/core.src"
  mkdir -p "$d/workspace/core.hdr" && : > "$d/workspace/core.hdr/abc.txt"
  : > "$d/caches/paths.cache"
  : > "$d/build.state"
  : > "$d/build.lib.state"
  : > "$d/build.fingerprint"
  # CURRENT: cache config per proyek (workspace/<nama>.Buildfile) — jangan disentuh
  echo "# current sentinel" > "$d/workspace/core.Buildfile"
}
legacy_left() { # $1 = folder .satria; echo jumlah sisa legacy
  n=0
  for f in workspace.state workspace/core.build.fingerprint workspace/core.ws.state \
           workspace/core.ws.lib.state workspace/core.deps.cache workspace/core.src \
           workspace/core.hdr/abc.txt caches/paths.cache build.state build.lib.state \
           build.fingerprint; do
    [ -e "$1/$f" ] && n=$((n+1))
  done
  echo "$n"
}

# ============================ MODE WORKSPACE ============================
W=$(mktemp -d /tmp/leg_ws_XXXX)
cp -r "$SRC/." "$W/" || exit 2
printf "flags = MMD, MP\n" >> "$W/Buildfile.ws"
cd "$W" || exit 2
"$SAT" -w > c1 2>&1; check "ws: cold build" $?

plant .satria
[ "$(legacy_left .satria)" -eq 11 ]; check "ws: sampah legacy tertanam (11)" $?

"$SAT" -w > c2 2>&1; check "ws: run tetap sukses" $?
hit_ws c2; check "ws: no-op tetap berlaku setelah menyapu" $?
[ "$(legacy_left .satria)" -eq 0 ]; check "ws: semua state lama terbuang (run)" $?
[ -f .satria/workspace/core.Buildfile ]; check "ws: <p>.Buildfile current dipertahankan" $?

plant .satria
"$SAT" -w clean > c3 2>&1; check "ws: clean sukses" $?
[ "$(legacy_left .satria)" -eq 0 ]; check "ws: state lama terbuang saat clean" $?
[ -f .satria/workspace/core.Buildfile ]; check "ws: clean juga mempertahankan <p>.Buildfile" $?

"$SAT" -w > c4 2>&1; check "ws: build setelah clean sukses" $?
"$SAT" -w > c5 2>&1; hit_ws c5; check "ws: no-op pulih pasca-clean" $?

# ============================ MODE SINGLE ============================
S=$(mktemp -d /tmp/leg_s_XXXX)
mkdir -p "$S/src"
printf 'use project\n\nsources = src\nflags = Wall\n' > "$S/Buildfile"
printf 'int main(void){return 0;}\n' > "$S/src/main.c"
cd "$S" || exit 2
"$SAT" > s1 2>&1; check "single: cold build" $?

plant .satria
rm -f .satria/workspace.state   # tidak berlaku di single; tetap tak apa
[ "$(legacy_left .satria)" -eq 10 ]; check "single: sampah legacy tertanam (10)" $?

"$SAT" > s2 2>&1; check "single: run tetap sukses" $?
grep -q "Nothing to build" s2; check "single: no-op tetap berlaku setelah menyapu" $?
[ "$(legacy_left .satria)" -eq 0 ]; check "single: semua state lama terbuang (run)" $?

plant .satria
rm -f .satria/workspace.state
"$SAT" clean > s3 2>&1; check "single: clean sukses" $?
[ "$(legacy_left .satria)" -eq 0 ]; check "single: state lama terbuang saat clean" $?

echo "---"
echo "lulus: $pass  gagal: $fail  (folder uji: $W | $S)"
[ "$fail" -eq 0 ]

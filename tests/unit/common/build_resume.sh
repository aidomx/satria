#!/bin/sh
# Usage: sh tests/unit/common/build_resume.sh /absolute/path/to/satria
# Build gagal melanjutkan dari kemajuan (tidak mengulang dari awal):
#   A. cold build gagal di satu source  -> perbaikan hanya mengompilasi source itu
#   B. 30 source berubah, satu gagal    -> perbaikan hanya mengompilasi source gagal
#   C. semua compile sukses, link gagal -> perbaikan tidak mengompilasi object apa pun
#   D. Ctrl+C di tengah build           -> build ulang melanjutkan, tidak dari awal
set -eu
SAT="$1"
case "$SAT" in /*) ;; *) SAT="$(pwd)/$SAT" ;; esac
pass=0
fail=0
check() { # check <nama> <nilai> <harap>
  if [ "$2" = "$3" ]; then
    pass=$((pass + 1))
    echo "PASS: $1 ($2)"
  else
    fail=$((fail + 1))
    echo "FAIL: $1 (dapat $2, harap $3)"
  fi
}
mkproj() { # mkproj <dir>: 30 source + main, single-project
  rm -rf "$1"
  mkdir -p "$1/src"
  cd "$1"
  printf 'use project\n' > Buildfile
  i=1
  while [ $i -le 30 ]; do
    printf 'int f%d(void){return %d;}\n' $i $i > "src/f$i.c"
    i=$((i + 1))
  done
  printf 'int main(void){return 0;}\n' > src/main.c
}
: "${UNIT_TMP:=${UNIT_BASE:-/tmp/miniws}/t}"
W="$UNIT_TMP/build_resume"; rm -rf "${W}"; mkdir -p "${W}" || exit 1
trap 'rm -rf "$W"' EXIT HUP INT TERM

# A. cold gagal di satu source
mkproj "$W/a"
printf 'int broken = ;\n' > src/f15.c
"$SAT" -j${UNIT_JOBS:-2} > "$W/a1.txt" 2>&1 || true
printf 'int f15(void){return 15;}\n' > src/f15.c
"$SAT" -j${UNIT_JOBS:-2} > "$W/a2.txt" 2>&1
check "A: perbaikan setelah cold gagal hanya kompilasi source gagal" "$(grep -c 'OK ' "$W/a2.txt")" "1"

# B. 30 source berubah, satu gagal
mkproj "$W/b"
"$SAT" -j${UNIT_JOBS:-2} > /dev/null 2>&1
i=1
while [ $i -le 30 ]; do
  printf 'int f%d(void){return %d+0;}\n' $i $i > "src/f$i.c"
  i=$((i + 1))
done
printf 'int broken = ;\n' > src/f15.c
"$SAT" -j${UNIT_JOBS:-2} > "$W/b1.txt" 2>&1 || true
printf 'int f15(void){return 15+0;}\n' > src/f15.c
"$SAT" -j${UNIT_JOBS:-2} > "$W/b2.txt" 2>&1
check "B: perbaikan hanya kompilasi source gagal (29 yang sukses dilewati)" "$(grep -c 'OK ' "$W/b2.txt")" "1"

# C. semua compile sukses, link gagal
mkproj "$W/c"
printf 'int g_missing(void);\nint main(void){return g_missing();}\n' > src/main.c
"$SAT" -j${UNIT_JOBS:-2} > "$W/c1.txt" 2>&1 || true
printf 'int g_missing(void){return 0;}\n' > src/g.c
"$SAT" -j${UNIT_JOBS:-2} > "$W/c2.txt" 2>&1
check "C: perbaikan link tidak mengompilasi ulang object (hanya g.c baru)" "$(grep -c 'OK ' "$W/c2.txt")" "1"
check "C: binary ter-link setelah perbaikan" "$([ -f bin/c ] && echo 1 || echo 0)" "1"

# D. Ctrl+C di tengah build -> progres tersimpan, build ulang tidak mengulang dari awal
# Ctrl+C dikirim dengan kill -INT ke proses latar; di shell MSYS/MINGW/CYGWIN
# sinyal itu tidak sampai ke proses Windows native, sehingga build selesai normal
# (rc 0). Tes ini dilewati di sana dan dicatat jelas (lihat todos/).
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*)
    echo "SKIP: D Ctrl+C (kill -INT tidak sampai ke proses native Windows dari shell MSYS)"
    ;;
  *)
  # Kalibrasi: cari jumlah fungsi per file terkecil sehingga build cold penuh
  # minimal 2 x jeda Ctrl+C (KILL_MS). Dengan begitu Ctrl+C pada detik KILL_S selalu terjadi
  # di tengah build, di mesin cepat maupun lambat. MAX_K hanya batas pengaman.
  KILL_MS=500
  MIN_FULL_MS=$((KILL_MS * 2))
  MAX_K=5000
  ms_now() {
    t="$(date +%s%3N 2>/dev/null)"
    case "$t" in *N*|'') t=$(( $(date +%s) * 1000 )) ;; esac
    echo "$t"
  }
  gen_heavy() { # gen_heavy <dir> <k>: 8 file, masing-masing k fungsi, + main.c
    mkproj "$1"
    cd "$1" || exit 1
    rm -f src/f*.c
    i=1
    while [ $i -le 8 ]; do
      awk -v n=$i -v K="$2" 'BEGIN{for(j=0;j<K;j++) printf "static int h%d_%d(int a){int s=a; for(int k=0;k<%d;k++) s+=k*%d; return s;}\n", n, j, j%7+1, j; print "int f" n "(void){return 0;}"}' > "src/f$i.c"
      i=$((i + 1))
    done
  }
  K=20; FULL=0
  while :; do
    gen_heavy "$W/cal" "$K"
    rm -rf "$W/cal/.satria" "$W/cal/bin" "$W/cal/lib" "$W/cal/build"
    t0="$(ms_now)"
    ( cd "$W/cal" && "$SAT" -j1 > "$W/cal.txt" 2>&1 )
    t1="$(ms_now)"; FULL=$((t1 - t0))
    echo "INFO: D kalibrasi k=$K build cold penuh ${FULL} ms"
    [ "$FULL" -ge "$MIN_FULL_MS" ] && break
    [ "$K" -ge "$MAX_K" ] && break
    K=$((K * 2)); [ "$K" -gt "$MAX_K" ] && K=$MAX_K
  done
  if [ "$FULL" -lt "$MIN_FULL_MS" ]; then
    echo "SKIP: D tidak terkalibrasi (build penuh ${FULL} ms < ${MIN_FULL_MS} ms pada k=$MAX_K): mesin terlalu cepat untuk uji Ctrl+C"
  else
    gen_heavy "$W/d" "$K"
    "$SAT" -j1 > "$W/d1.txt" 2>&1 &
    pid=$!
    sleep "$(awk -v m=$KILL_MS 'BEGIN{printf "%.3f", m/1000}')"
    kill -INT "$pid" 2> /dev/null || true
    wait "$pid" && rc=0 || rc=$?
    check "D: Ctrl+C menghentikan build (rc 130)" "$rc" "130"
    "$SAT" -j1 > "$W/d2.txt" 2>&1 || true
    n2=$(grep -c 'OK ' "$W/d2.txt")
    total=9
    if [ "$n2" -lt "$total" ]; then
      pass=$((pass + 1))
      echo "PASS: D: build ulang tidak mengulang dari awal ($n2 < $total)"
    else
      fail=$((fail + 1))
      echo "FAIL: D: build ulang mengompilasi $n2 dari $total (mengulang dari awal)"
    fi
  fi
  ;;
esac

echo "lulus: $pass  gagal: $fail"
[ "$fail" -eq 0 ]

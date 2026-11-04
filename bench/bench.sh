#!/usr/bin/env bash
# ============================================================================
# bench.sh  pintu masuk benchmark satria: tentang, daftar, dan pemilihan.
#
# TENTANG (dibaca sebelum menjalankan apa pun):
#   Benchmark ini ada untuk MEMBANGUN satria menjadi lebih baik: mengukur
#   overhead, melihat dampak tiap perubahan, dan mendeteksi regresi antar
#   versi. Ini BUKAN klaim bahwa satria atau tool lain "lebih cepat" atau
#   "terbaik". Tool pembanding punya fitur yang berbeda (mis. workspace,
#   compdb, cakupan incremental), jadi angka hanya berarti dalam konteks
#   skenarionya. Cara membaca yang benar:
#     - bandingkan angka dengan versi satria sebelumnya di mesin yang sama;
#     - kolom obj/rebuilt adalah pemeriksaan kewajaran: pastikan pekerjaan
#       yang dihitung memang yang diharapkan;
#     - hasil bergantung pada mesin, beban, cache, dan versi tool. Jangan
#       membandingkan angka dari mesin atau kondisi yang berbeda.
#
# Pemakaian:
#   bench/bench.sh                       menu interaktif (pilih nomor, lalu opsi)
#   bench/bench.sh about                 tentang benchmark
#   bench/bench.sh list                  daftar benchmark
#   bench/bench.sh help <nama>           opsi lengkap sebuah benchmark
#   bench/bench.sh <nama> [opsi...]      jalankan, opsi diteruskan apa adanya
#   bench/bench.sh run <nama> [opsi...]  sama dengan di atas
#
# Contoh:
#   bench/bench.sh compare -t satria,cmake -n 40 -H 10 -r 5
#   bench/bench.sh scale -n 100,500 -e 1,5 -r 3
#   bench/bench.sh ws -n 80 -H 20 -r 3 -s cold,noop,touch
#
# Opsi umum (lihat `help <nama>` untuk daftar lengkap per benchmark):
#   -n  jumlah source / node     -H  jumlah header (hanya compare & ws)
#   -r  jumlah pengulangan       -b  binary satria (default bin/satria)
#   -e  jumlah edge (scale*)     -o  direktori kerja hasil
#   -s  subset skenario          -t  tool yang diukur (compare)
#
# Semua benchmark dijalankan dari root repo (tempat bin/satria berada).
# ============================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# Registri: nama | skrip | deskripsi | tool wajib (dipisah spasi)
BENCHES=(
  "compare|compare.sh|Perbandingan build tool: satria vs ninja, make, cmake-ninja (project sintetis)|ninja make gcc"
  "ws|ws.sh|Mode workspace satria (Buildfile.ws) vs CMake+Ninja, dengan audit pekerjaan|cmake ninja gcc"
  "scale|scale.sh|Skala no-op: waktu parsing graph dan invalidasi untuk banyak node/edge|gcc"
  "scale-build|scale-build.sh|Skala full build untuk banyak node/edge|gcc"
  "scale-graph|scale-graph.sh|Skala graph/invalidasi tanpa recompile|gcc"
)

about() {
  cat <<'TXT'
TENTANG BENCHMARK SATRIA
------------------------
Tujuan benchmark ini adalah MEMBANGUN satria menjadi lebih baik:
mengukur overhead, melihat dampak setiap perubahan, dan mendeteksi regresi
antar versi.

Bukan klaim bahwa satria atau tool lain "lebih cepat" atau "terbaik".
Tool pembanding punya fitur yang berbeda (workspace, compdb, cakupan
incremental), jadi angka hanya berarti dalam konteks skenarionya.

Cara membaca hasil:
  - bandingkan dengan versi satria sebelumnya di mesin yang sama;
  - kolom obj/rebuilt memastikan pekerjaan yang dihitung memang yang
    diharapkan;
  - hasil bergantung pada mesin, beban, cache, dan versi tool. Jangan
    membandingkan angka dari mesin atau kondisi yang berbeda.
TXT
}

list() {
  local i=1 entry name script desc req
  echo "BENCHMARK YANG TERSEDIA"
  echo "-----------------------"
  for entry in "${BENCHES[@]}"; do
    IFS='|' read -r name script desc req <<< "$entry"
    printf '  %d) %-12s %s\n' "$i" "$name" "$desc"
    printf '     tool wajib: %s\n' "$req"
    i=$((i + 1))
  done
}

# lookup <nama> -> mencetak baris registri, gagal bila tidak ada
lookup() {
  local entry name
  for entry in "${BENCHES[@]}"; do
    name=${entry%%|*}
    if [ "$name" = "$1" ]; then
      printf '%s\n' "$entry"
      return 0
    fi
  done
  echo "benchmark tidak dikenal: $1" >&2
  echo "" >&2
  list >&2
  return 1
}

# run_bench <nama> [opsi...]: cek prasyarat lalu jalankan skrip dari root repo
run_bench() {
  local name=$1; shift
  local entry script req t missing=""
  entry=$(lookup "$name") || return 1
  IFS='|' read -r _ script _ req <<< "$entry"

  for t in $req; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
  done
  if [ -n "$missing" ]; then
    echo "tool wajib belum tersedia di PATH:$missing" >&2
    echo "pasang tool tersebut, atau tambahkan lokasinya ke PATH, lalu coba lagi." >&2
    return 1
  fi

  # Tanpa -b, skrip memakai bin/satria di root: beri tahu bila belum ada.
  case " $* " in
    *" -b "*) ;;
    *) [ -x "$ROOT/bin/satria" ] || {
         echo "bin/satria belum ada di $ROOT. Build dulu: ./install.sh --dev" >&2
         return 1
       } ;;
  esac

  echo "> benchmark : $name  ($script)"
  echo "> opsi      : ${*:-<default>}"
  echo ""
  cd "$ROOT" && bash "$HERE/$script" "$@"
}

help_bench() {
  local entry script
  entry=$(lookup "$1") || return 1
  IFS='|' read -r _ script _ _ <<< "$entry"
  bash "$HERE/$script" -h
}

# Menu interaktif: pilih nomor, lalu opsi tambahan (boleh kosong = default)
interactive() {
  about
  echo ""
  list
  echo ""
  local choice entry name opts
  printf 'Pilih nomor benchmark (kosong = keluar): '
  read -r choice || { echo ""; return 0; }
  [ -n "$choice" ] || return 0
  if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt "${#BENCHES[@]}" ]; then
    echo "pilihan tidak valid: $choice" >&2
    return 1
  fi
  entry=${BENCHES[$((choice - 1))]}
  name=${entry%%|*}
  echo ""
  echo "Opsi untuk $name (kosong = default; lihat: bench/bench.sh help $name)"
  printf 'opsi: '
  read -r opts || opts=""
  # Pisahkan opsi menjadi argumen; tidak memakai eval
  local -a args=()
  if [ -n "$opts" ]; then
    read -r -a args <<< "$opts"
  fi
  run_bench "$name" "${args[@]}"
}

main() {
  if [ $# -eq 0 ]; then
    interactive
    return $?
  fi
  case "$1" in
    about)        about ;;
    list)         list ;;
    help)         [ $# -ge 2 ] || { echo "pemakaian: bench/bench.sh help <nama>" >&2; return 1; }
                  help_bench "$2" ;;
    run)          [ $# -ge 2 ] || { echo "pemakaian: bench/bench.sh run <nama> [opsi...]" >&2; return 1; }
                  shift; run_bench "$@" ;;
    -h|--help)    about; echo ""; list ;;
    *)            run_bench "$@" ;;
  esac
}

main "$@"

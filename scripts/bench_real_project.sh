#!/usr/bin/env bash
# ============================================================================
# bench_real_project.sh — ukur dampak kebijakan runner workspace pada
# COLD build proyek nyata: serial (default baru) vs paralel (baseline).
#
# Kebijakan diukur (src/runners/runners.c, runnerWantParallel):
#   - default       : paralel HANYA di child background; foreground = serial
#                     -> diukur apa adanya, tanpa env.
#   - SATRIA_WS_PAR=1: paksa paralel (baseline lama)  -> konfigurasi B
#   - SATRIA_WS_PAR=0: paksa serial                    -> konfigurasi A eksplisit
#
# Metode (ikuti pola bench/ws.sh):
#   - Dua salinan IDENTIK proyek (A & B) di direktori sementara, artefak
#     build (.satria/build/bin/lib/modules/dist/.git) dibuang saat penyalinan.
#   - Tiap run diawali cold reset (artefak build dihapus) sehingga yang
#     diukur selalu build dingin penuh; reset TIDAK ikut dihitung waktu.
#   - Urutan A/B berselang tiap repetisi (efek panas/sistem-berkas seimbang).
#   - Waktu: EPOCHREALTIME sebelum/sesudah `satria -w -jN`; laporan
#     median (bukan mean) seperti benchmark lain di bench/.
#   - Log per run disimpan; kegagalan build ditampilkan dan menghentikan run.
#
# Pemakaian:
#   scripts/bench_real_project.sh [-r N] [-j N] [-s satria] [-o dir] [-k] [proyek]
#     -r N   pengulangan per konfigurasi (default 3) -> median
#     -j N   jobs compiler/runner (default nproc). PERHATIAN: runner paralel
#            tidak mungkin dengan -j1 (runnerWantParallel menolak jobs<=1).
#     -s PATH binary satria (default <repo>/bin/satria)
#     -o DIR  folder log (default direktori sementara)
#     -k      jangan hapus salinan A/B di akhir (inspeksi manual)
#     proyek  folder berisi Buildfile.ws / Buildfile
#             (default: tests/rupaws — workspace nyata di repo ini)
#
# Contoh:
#   scripts/bench_real_project.sh -r 5 tests/rupaws
#   scripts/bench_real_project.sh -r 3 -j 4 "$HOME/rupa"
# Catatan: cold build proyek besar bisa menit per run; total ≈ 2 × -r × waktu.
# ============================================================================
set -uo pipefail
[ -n "${EPOCHREALTIME:-}" ] || {
  echo "butuh bash >= 5 (EPOCHREALTIME)" >&2
  exit 1
}

# ---------------------------- konfigurasi ----------------------------------
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NRUN=3 # -r : pengulangan per konfigurasi (median)
JOBS=$(nproc 2> /dev/null || echo 4)
SATRIA="$ROOT_DIR/bin/satria"    # -s : binary satria
OUTDIR=""                        # -o : folder log (kosong = mktemp)
KEEP=0                           # -k : simpan salinan A/B
PROJECT="$ROOT_DIR/tests/rupaws" # folder proyek (argumen bebas)

usage() {
  sed -n '3,/^# ===/p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while getopts "r:j:s:o:kh" opt; do
  case "$opt" in
    r) NRUN="$OPTARG" ;;
    j) JOBS="$OPTARG" ;;
    s) SATRIA="$OPTARG" ;;
    o) OUTDIR="$OPTARG" ;;
    k) KEEP=1 ;;
    h) usage 0 ;;
    *) usage 2 ;;
  esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] && PROJECT="$1"

case "$SATRIA" in /*) ;; *) SATRIA="$(pwd)/$SATRIA" ;; esac
PROJECT="${PROJECT%/}"
[ -x "$SATRIA" ] || {
  echo "satria tidak ditemukan: $SATRIA" >&2
  exit 2
}
[ -d "$PROJECT" ] || {
  echo "proyek tidak ditemukan: $PROJECT" >&2
  exit 2
}
if [ ! -f "$PROJECT/Buildfile.ws" ] && [ ! -f "$PROJECT/Buildfile" ]; then
  echo "bukan proyek satria (tidak ada Buildfile.ws/Buildfile): $PROJECT" >&2
  exit 2
fi
case "$NRUN" in '' | *[!0-9]* | 0)
  echo "-r harus bilangan bulat > 0" >&2
  exit 2
  ;;
esac
[ "$JOBS" -ge 1 ] 2> /dev/null || {
  echo "-j harus bilangan bulat >= 1" >&2
  exit 2
}

# ---------------------------- util -----------------------------------------

# Artefak yang tidak ikut disalin (hasil build) — juga daftar yang dihapus
# pada cold reset. bin/lib/modules/dist mengikuti konvensi keluaran satria.
ARTIFACTS=".satria build bin lib modules dist compile_commands.json"

prepare_tmp() { # prepare_tmp <dir> — buat folder kerja bila belum ada
  [ -d "$1" ] || mkdir -p "$1"
}

clean() { # clean <dir>
  [ -d "$1" ] && rm -rf "$1"
  return 0
}

copy_project() { # copy_project <src> <dst> — salinan bersih tanpa artefak
  prepare_tmp "$2"
  local ex=()
  # Pola artefak DI-ANCHOR ke root (./build dll.): pola tanpa slash di GNU tar
  # match di level mana pun, jadi --exclude=modules ikut menyingkirkan direktori
  # sumber bernama sama (tests/modules, core/src/.../modules pada rupaws) dan
  # salinan pincang -> build gagal. .git/node_modules tetap tanpa anchor
  # (sampah di level mana pun tetap dibuang).
  ex+=(--exclude=.git --exclude=node_modules --exclude=.satria
    --exclude=./build --exclude=./bin --exclude=./lib --exclude=./modules
    --exclude=./dist --exclude=./compile_commands.json)
  # archive
  tar -C "$1" "${ex[@]}" -cf - . | tar -C "$2" -xf -
}

cold_reset() { # cold_reset <dir> — buang artefak supaya run berikutnya dingin
  local d="$1" a
  for a in $ARTIFACTS; do rm -rf "$d/$a"; done
}

now() { printf '%s' "$EPOCHREALTIME"; }

elapsed() { # elapsed <t0> <t1> -> detik dengan 2 desimal
  awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", b - a }'
}

median() { # median: baca angka per baris dari stdin
  sort -n | awk '
    { v[NR] = $1 }
    END {
      if (NR == 0) { print "n/a"; exit }
      if (NR % 2) printf "%.2f\n", v[(NR + 1) / 2]
      else printf "%.2f\n", (v[NR / 2] + v[NR / 2 + 1]) / 2
    }'
}

stat_line() { # stat_line <nama> <angka...> -> n/min/median/max
  local name="$1"
  shift
  printf '%s\n' "$@" | sort -n | awk -v name="$name" '
    { v[NR] = $1 }
    END {
      if (NR == 0) { printf "  %-22s %3s  %8s  %8s  %8s\n", name, 0, "-", "-", "-"; exit }
      med = (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2
      printf "  %-22s %3d  %8.2f  %8.2f  %8.2f\n", name, NR, v[1], med, v[NR]
    }'
}

# ---------------------------- inti benchmark --------------------------------

# bench_AB <label> <dir> <SATRIA_WS_PAR|-> <log-file>
# Jalankan SAT -w -j JOBS pada <dir> (sudah cold reset) dan cetak durasi
# detik ke stdout. Kegagalan build = hentikan seluruh benchmark.
bench_AB() {
  local label="$1" dir="$2" par="$3" log="$4" t0 t1 rc
  # -j harus menempel (CLI satria menerima -jN; "-j N" dianggap perintah)
  t0=$(now)
  if [ "$par" = "-" ]; then
    (cd "$dir" && "$SATRIA" -w "-j$JOBS") > "$log" 2>&1
  else
    (cd "$dir" && SATRIA_WS_PAR="$par" "$SATRIA" -w "-j$JOBS") > "$log" 2>&1
  fi
  rc=$?
  t1=$(now)
  if [ "$rc" -ne 0 ]; then
    echo "" >&2
    echo "FAIL $label (rc=$rc) — 15 baris terakhir $log:" >&2
    tail -15 "$log" >&2
    exit 1
  fi
  elapsed "$t0" "$t1"
}

# repeat — NRUN x (A berselang dengan B); A = serial, B = paralel
repeat() {
  local r cfg t
  for ((r = 1; r <= NRUN; r++)); do
    # berselang: rep ganjil A dulu, rep genap B dulu (keseimbangan termal)
    if ((r % 2 == 1)); then cfgs="A B"; else cfgs="B A"; fi
    for cfg in $cfgs; do
      if [ "$cfg" = A ]; then
        cold_reset "$A"
        if ! t=$(bench_AB "serial #$r" "$A" 0 "$LOGDIR/serial_$r.log"); then
          echo "benchmark berhenti (konfigurasi serial run $r gagal)" >&2
          exit 1
        fi
        SERIAL+=("$t")
        printf '  run %-2d %-8s %8ss  (log: %s)\n' "$r" "serial" "$t" "$LOGDIR/serial_$r.log"
      else
        cold_reset "$B"
        if ! t=$(bench_AB "paralel #$r" "$B" 1 "$LOGDIR/paralel_$r.log"); then
          echo "benchmark berhenti (konfigurasi paralel run $r gagal)" >&2
          exit 1
        fi
        PARALLEL+=("$t")
        printf '  run %-2d %-8s %8ss  (log: %s)\n' "$r" "paralel" "$t" "$LOGDIR/paralel_$r.log"
      fi
    done
  done
}

main() {
  SERIAL=()
  PARALLEL=()
  LOGDIR="${OUTDIR:-$(mktemp -d /tmp/bench_real_XXXXXX)}"
  prepare_tmp "$LOGDIR"

  A=$(mktemp -d /tmp/bench_A_XXXXXX)
  B=$(mktemp -d /tmp/bench_B_XXXXXX)

  echo "▌ satria cold-build benchmark · runner serial (default) vs paralel (baseline)"
  echo "  proyek : $PROJECT"
  echo "  binary : $SATRIA"
  echo "  jobs   : $JOBS   run: $NRUN per konfigurasi   nproc: $(nproc 2> /dev/null || echo '?')"
  echo "  konfig : A = serial (SATRIA_WS_PAR=0) · B = paralel (SATRIA_WS_PAR=1)"
  echo "  log    : $LOGDIR"
  echo "  menyalin proyek (tanpa artefak build) ..."

  copy_project "$PROJECT" "$A" || {
    echo "gagal menyalin $PROJECT" >&2
    exit 1
  }
  copy_project "$PROJECT" "$B" || {
    echo "gagal menyalin $PROJECT" >&2
    exit 1
  }

  repeat

  echo ""
  echo "  Hasil (detik, median dari $NRUN run, cold build)"
  printf '  %-22s %3s  %8s  %8s  %8s\n' "konfigurasi" "n" "min" "median" "max"
  stat_line "serial (default)" "${SERIAL[@]}"
  stat_line "paralel (baseline)" "${PARALLEL[@]}"
  echo ""

  local ms mp
  ms=$(printf '%s\n' "${SERIAL[@]}" | median)
  mp=$(printf '%s\n' "${PARALLEL[@]}" | median)
  awk -v s="$ms" -v p="$mp" 'BEGIN {
    if (p > 0) printf "  median serial %.2fs · paralel %.2fs · rasio paralel/serial %.2fx\n", s, p, s / p
    else printf "  median serial %.2fs · paralel %.2fs\n", s, p
  }'

  if [ "$KEEP" -eq 1 ]; then
    echo "  salinan dipertahankan: A=$A B=$B"
  else
    clean "$A"
    clean "$B"
    echo "  salinan A/B dibersihkan (log tetap di $LOGDIR)"
  fi
}

main "$@"

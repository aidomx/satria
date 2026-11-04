# bench/compare/ui.sh  tampilan: progress bar, kotak header, dan laporan validasi.
# Dimuat oleh bench/compare.sh (cmp_source). Bukan skrip mandiri.

# ---------------------------- tampilan -------------------------------------
TTY=0; [ -t 1 ] && TTY=1
if [ "$TTY" = 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_RST=$'\033[0m'; C_BLD=$'\033[1m'; C_DIM=$'\033[2m'
  C_GRN=$'\033[32m'; C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_CYN=$'\033[36m'
else
  C_RST=""; C_BLD=""; C_DIM=""; C_GRN=""; C_RED=""; C_YEL=""; C_CYN=""
fi
COLS=$(tput cols 2>/dev/null || echo 80)
[ "$COLS" -gt 0 ] 2>/dev/null || COLS=80
if (( COLS >= 80 )); then WIDE=1; BW=$(( COLS - 60 )); else WIDE=0; BW=$(( COLS - 38 )); fi
(( BW > 24 )) && BW=24
(( BW < 6 ))  && BW=6
PBW=$(( COLS - 30 ))
(( PBW > 30 )) && PBW=30
(( PBW < 8 ))  && PBW=8

STEP=0; TOTAL=1; CUR=""; T_START=$SECONDS

rule_line() {
  local w=$1 s
  printf -v s '%*s' "$w" ''
  printf '%s%s%s\n' "$C_DIM" "${s// /─}" "$C_RST"
}
box_width() {
  local w=$(( COLS > 72 ? 70 : COLS - 2 ))
  (( w < 40 )) && w=40
  printf '%s' "$w"
}
rules_header() {
  echo 'Legend :'
  echo '  ★    : Tercepat pada skenario ini'
  echo '  ×    : Rasio waktu terhadap yang tercepat'
  echo '  ✗    : Eksekusi gagal (exit non-zero)'
  echo
  echo 'Metrics :'
  echo '  obj   : Jumlah .o yang berubah (mtime)'
  echo '  ms    : Durasi eksekusi; laporan memakai median'
  echo
  echo 'Correctness (Validation Rules) :'
  echo '  cold  : build bersih; jumlah .o dicatat'
  echo '  noop  : build tanpa perubahan'
  echo '  touch : header di-touch tanpa mengubah konten'
  echo '  edit  : satu source diubah; gcc-floor diukur terpisah'
  echo '  compdb: compile_commands.json harus dihasilkan'
  echo '  Audit : compile/link diuji terpisah tanpa memengaruhi timed runs'
}
validation_report() {
  echo
  echo "${C_BLD}[Detailed Validation Log]${C_RST}"
  echo
  awk -F, -v TOOLS="${TOOLS[*]}" '
    NR == 1 { next }
    {
      key = $2 SUBSEP $1
      seen[key] = 1
      if ($4 != 0) failed[key]++
      else passed[key]++
    }
    END {
      nsc = split("cold noop touch edit compdb", SC, " ")
      nt = split(TOOLS, TL, " ")
      w = 12
      printf "%-*s | %-9s | %s\n", w, "tool", "execution", "status"
      for (si = 1; si <= nsc; si++) {
        for (ti = 1; ti <= nt; ti++) {
          t = TL[ti]
          name = t == "bear" ? "bear-make" : t == "cmake" ? "cmake-ninja" : t == "meson" ? "meson-ninja" : t
          k = SC[si] SUBSEP name
          if (!(k in seen)) continue
          printf "%-*s | %-9s | %s\n", w, name, SC[si], failed[k] ? "Failed" : "Passed (exit status)"
        }
      }
    }
  ' "$CSV"
}

fmt_t() { printf '%d:%02d' $(( $1 / 60 )) $(( $1 % 60 )); }

bar() {  # bar <selesai> <total> <lebar>
  local d=$1 t=$2 w=$3 f i s=""
  (( t > 0 )) || t=1
  f=$(( d * w / t ))
  for ((i = 0; i < w; i++)); do
    if (( i < f )); then s+="█"; else s+="░"; fi
  done
  printf '%s' "$s"
}

clear_line() { [ "$TTY" = 1 ] && printf '\r\033[K'; return 0; }

draw_progress() {
  [ "$TTY" = 1 ] || return 0
  local el=$(( SECONDS - T_START )) eta=0 pct
  pct=$(( STEP * 100 / TOTAL ))
  (( STEP > 0 )) && eta=$(( el * (TOTAL - STEP) / STEP ))
  printf '\r\033[K  %s%s%s %3d%% %s%d/%d ETA %s%s' \
    "$C_CYN" "$(bar "$STEP" "$TOTAL" "$PBW")" "$C_RST" "$pct" \
    "$C_DIM" "$STEP" "$TOTAL" "$(fmt_t "$eta")" "$C_RST"
  if (( COLS >= 90 )); then printf '  %s▸ %s%s' "$C_YEL" "$CUR" "$C_RST"; fi
}

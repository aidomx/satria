# Laporan (design/bench_ws_output.md): rules_header (Legend/Metrics/Correctness),
# tabel ringkas [Detailed Validation Log], chart median, footer Success Rate.
# Dimuat dari ws.sh.
# ---------------------------- utilitas -------------------------------------
rule_line() {
  local w=$1 s=""
  s=$(printf '%*s' "$w" '')
  printf '%s%s%s\n' "$C_DIM" "${s// /─}" "$C_RST"
}
plural() { [ "$1" = 1 ] && printf '%s %s' "$1" "$2" || printf '%s %ss' "$1" "$2"; }
box_width() {
  local tw=$((COLS > 72 ? 70 : COLS - 2))
  ((tw < 40)) && tw=40
  printf '%s' "$tw"
}

# ---------------------------- chart median ---------------------------------
report() {
  local sorted="$OUTDIR/.sorted.csv"
  awk -F, 'NR > 1' "$CSV" | sort -t, -k2,2 -k1,1 -k3,3n > "$sorted"
  awk -F, -v BW="$BW" -v B="$C_BLD" -v D="$C_DIM" -v G="$C_GRN" -v Y="$C_YEL" -v R="$C_RED" -v Z="$C_RST" '
{
g = $2 SUBSEP $1
if (!(g in seen)) { seen[g] = 1; ng++; gs[ng] = g; gscen[ng] = $2; glab[ng] = $1 }
if ($4 == 0) { n[g]++; v[g, n[g]] = $3; ro[g] += $5; ra[g] += $6; rl[g] += $7; rn[g] += $8; rcc[g] += $9 } else bad[g]++
}
END {
nsc = split("cold noop leaf bade mod touch hedit sel dep libtouch libhdr rel", SC, " ")
desc["cold"]  = "build dari nol, 3 proyek"
desc["noop"]  = "tanpa perubahan"
desc["leaf"]  = "edit 1 source core (bade tak boleh ikut)"
desc["bade"]  = "edit 1 source bade"
desc["mod"]   = "edit 1 .rp rupamod -> arsip -> codegen embed -> bade+rupa"
desc["touch"] = "touch include/a.h pusat, konten sama"
desc["hedit"] = "edit isi include/a.h pusat (konten berbeda)"
desc["sel"]   = "edit core+bade, cakupan selektif core (setara -w core)"
desc["dep"]   = "edit .rp, cakupan selektif bade (setara -w bade)"
desc["libtouch"] = "touch include/ext.h (header internal), konten sama"
desc["libhdr"]   = "edit isi include/ext.h (header internal, include relatif)"
desc["rel"]   = "release tar+sha256 (satria-only, diukur terpisah)"
for (si = 1; si <= nsc; si++) {
s = SC[si]; k = 0; nf = 0
for (i = 1; i <= ng; i++) {
if (gscen[i] != s) continue; g = gs[i]
if (n[g] > 0) {
m = n[g]; med = (m % 2) ? v[g, (m + 1) / 2] : (v[g, m / 2] + v[g, m / 2 + 1]) / 2
k++; L[k] = glab[i]; M[k] = med; LB[k] = rl[g] / m; BN[k] = rn[g] / m; BD[k] = bad[g] + 0
} else fl[++nf] = glab[i]
}
if (k == 0 && nf == 0) continue
for (a = 2; a <= k; a++) for (b = a; b > 1 && M[b] < M[b - 1]; b--) {
t = L[b]; L[b] = L[b - 1]; L[b - 1] = t; t = M[b]; M[b] = M[b - 1]; M[b - 1] = t
t = LB[b]; LB[b] = LB[b - 1]; LB[b - 1] = t; t = BN[b]; BN[b] = BN[b - 1]; BN[b - 1] = t
t = BD[b]; BD[b] = BD[b - 1]; BD[b - 1] = t
}
printf "\n%s%s%s %s%s%s\n", B, toupper(s), Z, D, desc[s], Z
mx = (k > 0) ? M[k] : 1; if (mx < 1) mx = 1; best = (k > 0) ? M[1] : 1; if (best < 1) best = 1
div = (k > 1 && (diverges(LB, k) || diverges(BN, k)))
for (i = 1; i <= k; i++) {
len = int(M[i] / mx * BW + 0.5); if (len < 1) len = 1; br = ""
for (j = 1; j <= BW; j++) br = br (j <= len ? "█" : "░")
col = (i == 1) ? G : ""; mark = (i == 1) ? "★" : " "
printf "  %s%s %-6s %s %6d ms %5.2f×%s", col, mark, (L[i]=="cmake"?"cninja":L[i]), br, M[i], M[i] / best, Z
if (BD[i] > 0) printf " %s✗%d%s", R, BD[i], Z
# Divergensi pekerjaan antar tool: tanda [!] menempel di belakang baris
# (design/bench_ws_output.md) — waktu bukan perbandingan setara 1:1.
if (div) printf " %s[!]%s", Y, Z
printf "\n"
}
for (i = 1; i <= nf; i++) printf "  %s✗ %-6s semua run gagal atau tidak sesuai harapan%s\n", R, (fl[i]=="cmake"?"cninja":fl[i]), Z
}
}
function diverges(arr, cnt,  i, mn, mx) {
mn = arr[1]; mx = arr[1]
for (i = 2; i <= cnt; i++) { if (arr[i] < mn) mn = arr[i]; if (arr[i] > mx) mx = arr[i] }
return mx - mn > 0.5
}
' "$sorted"
  rm -f "$sorted"
}

# ---------------------------- rules (header) -------------------------------
# Legend/Metrics/Build Expect dicetak SEBELUM execution stream (design/
# bench_ws_output.md) supaya arti metrik & harapan kerja terbaca dulu,
# bukan setelah angka-angka.
rules_header() {
  # Baris literal (bukan printf %-Ns): ★/× multibyte, padding printf = byte.
  echo "Legend :"
  echo "  ★    : Tercepat pada skenario ini"
  echo "  ×    : Rasio waktu terhadap yang tercepat"
  echo "  [!]  : Pekerjaan berbeda antar tool (waktu tidak setara 1:1, bandingkan dengan hati-hati)"
  echo "  *    : Tidak di-hard-check pada skenario ini (lihat EXPECTED di blok validasi)"
  echo
  echo "Metrics :"
  printf '  %-5s : %s\n' "obj" ".o berubah"
  printf '  %-5s : %s\n' "cc" "kompilasi C aktual (shim)"
  printf '  %-5s : %s\n' "lib" "lib .a/.so"
  printf '  %-5s : %s\n' "bin" "binary berubah"
  printf '  %-5s : %s\n' "arc" "arsip .rp berubah"
  echo
  echo "Build Expect (aturan validasi per backend) :"
  echo "  cmake + ninja :"
  printf '    %-10s: %s\n' "cold" "obj penuh, arc 1, lib 4, bin 2; cc = penuh + embed + probe CMake"
  printf '    %-10s: %s\n' "noop" "obj/arc/lib/bin/cc semua 0"
  printf '    %-10s: %s\n' "leaf/bade" "obj 1, cc 1, lib 2, bin 2 — compile + relink siklus via shim"
  printf '    %-10s: %s\n' "mod" "obj 1, cc 1, lib 2, bin 2 — arsip berubah -> codegen embedded.c"
  printf '    %-10s: %s\n' "touch" "header pusat hanya berubah mtime: rebuild dependency (obj/cc 6, lib 4, bin 2)"
  printf '    %-10s: %s\n' "hedit" "obj/cc 6, lib 4, bin 2 — konten header pusat berubah"
  printf '    %-10s: %s\n' "sel" "obj/cc 1, lib 2, bin 0 — setara satria -w core"
  printf '    %-10s: %s\n' "dep" "obj 1, cc 1, lib 2, bin 1 — setara satria -w bade (codegen embed)"
  printf '    %-10s: %s\n' "libtouch" "header internal mtime: obj/cc 2"
  printf '    %-10s: %s\n' "libhdr" "obj/cc 2, lib 4, bin 2 — konten header internal berubah"
  echo "  satria :"
  printf '    %-10s: %s\n' "cold" "obj penuh, arc 1, lib 4, bin 2; cc = penuh + codegen embed"
  printf '    %-10s: %s\n' "noop" "obj/arc/lib/bin/cc semua 0"
  printf '    %-10s: %s\n' "leaf/bade" "obj 1, cc 1, lib 2, bin 0 — perubahan source, tanpa relink binary"
  printf '    %-10s: %s\n' "mod" "obj 1, cc 1, lib 2, bin 2 — codegen embed, relink lib + binary pengguna arsip"
  printf '    %-10s: %s\n' "touch" "obj/arc/lib/bin/cc semua 0 — content-hash"
  printf '    %-10s: %s\n' "hedit" "obj/cc 6, lib 4, bin 2 — konten header pusat berubah"
  printf '    %-10s: %s\n' "sel" "obj/cc 1, lib 2, bin 0 — hanya core"
  printf '    %-10s: %s\n' "dep" "obj 1, cc 1, lib 2, bin 1 — arsip + codegen embed, bade saja"
  printf '    %-10s: %s\n' "libtouch" "obj/arc/lib/bin/cc semua 0 — content-hash"
  printf '    %-10s: %s\n' "libhdr" "obj/cc 2, lib 4, bin 2 — konten header internal berubah"
  printf '    %-10s: %s\n' "rel" "tar + SHA-256 diverifikasi dengan sha256sum -c (satria-only)"
}

# ---------------------------- [Detailed Validation Log] --------------------
# Tabel ringkas per skenario+tool (design/bench_ws_output.md): verdict
# agregat dari SEMUA run — In accordance hanya bila semua run rc 0 DAN kerja
# sesuai harapan. Detail miss/gagal tetap tercetak merah saat stream + stderr.log.
view_report() {
  [ -s "$VIEW_CSV" ] || return 0
  echo
  echo "${C_BLD}[Detailed Validation Log]${C_RST}"
  echo
  awk -F, -v TOOLS="${TOOLS[*]}" -v SUCC="$OUTDIR/.success.txt" '
{
  k = $2 SUBSEP $1
  seen[k] = 1
  if ($3 != 0 || $15 != 0) acc[k] = 0
  else if (!(k in acc)) acc[k] = 1
}
END {
  nsc = split("cold noop leaf bade mod touch hedit sel dep libtouch libhdr rel", SC, " ")
  nt = split(TOOLS, TL, " ")
  for (ti = 1; ti <= nt; ti++) if (TL[ti] == "cmake") TL[ti] = "cninja"
  w = 4
  for (ti = 1; ti <= nt; ti++) if (length(TL[ti]) > w) w = length(TL[ti])
  printf "%-*s | %-9s | %s\n", w, "tool", "execution", "status"
  for (si = 1; si <= nsc; si++)
    for (ti = 1; ti <= nt; ti++) {
      t = TL[ti]; k = SC[si] SUBSEP t
      if (!(k in seen)) continue
      printf "%-*s | %-9s | %s\n", w, t, SC[si], (acc[k] ? "In accordance" : "Not in accordance")
      if (acc[k]) okc[t]++; else badc[t]++
    }
  for (ti = 1; ti <= nt; ti++) print TL[ti], okc[TL[ti]] + 0, badc[TL[ti]] + 0 > SUCC
  close(SUCC)
}' "$VIEW_CSV"
}

# ---------------------------- Success Rate (footer) ------------------------
# Setelah chart median; catatan strace dilewati dicetak SEKALI di sini
# (design/bench_ws_output.md — tanpa blok [Strace] terpisah).
success_footer() {
  local succ="$OUTDIR/.success.txt" w=0
  echo
  echo "Success Rate:"
  if [ -s "$succ" ]; then
    w=$(awk '{ if (length($1) > w) w = length($1) } END { print w + 0 }' "$succ")
    awk -v W="$w" '{ printf "  %-*s : %d/%d scenarios passed in workspace concept\n", W, $1, $2, $2 + $3 }' "$succ"
  fi
  [ "$HAVE_STRACE" = 1 ] || echo "  strace : dilewati${STRACE_SKIP:+ ($STRACE_SKIP)}"
  rm -f "$succ"
}

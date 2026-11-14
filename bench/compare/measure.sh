# bench/compare/measure.sh  pengukuran (run_once) dan laporan tabel.
# Dimuat oleh bench/compare.sh (cmp_source). Bukan skrip mandiri.

# ---------------------------- util pengukuran ------------------------------
run_once() {
  local label=$1 scen=$2; shift 2
  local t0 t1 rc ms nobj
  CUR="$label/$scen"; draw_progress
  touch "$MARK"
  t0=${EPOCHREALTIME/[.,]/}
  ( cd "$PROJ" && "$@" ) >/dev/null 2>>"$ERRLOG"
  rc=$?
  t1=${EPOCHREALTIME/[.,]/}
  ms=$(( (t1 - t0 + 500) / 1000 ))
  nobj=$(find "$PROJ" -name '*.o' -newer "$MARK" 2>/dev/null | wc -l)
  nobj=${nobj//[[:space:]]/}
  # Check actual outputs, not merely process exit status.
  if [ "$rc" -eq 0 ] && [ -z "${CHECK_COMPDB:-}" ]; then
    local binary="$PROJ/bin/app"
    case "$label" in
      cmake-ninja) binary="$PROJ/build-cmake/bin/app" ;;
      meson-ninja) binary="$PROJ/build-meson/app" ;;
      xmake) binary="" ;; # xmake output location is tool-dependent
    esac
    if [ -n "$binary" ] && [ ! -x "$binary" ]; then rc=97; fi
    if [ "$rc" -eq 0 ]; then
      case "$scen" in
        cold) [ "$nobj" -eq "$((NSRC+1))" ] || rc=96 ;;
        noop) [ "$nobj" -eq 0 ] || rc=96 ;;
        edit) [ "$nobj" -eq 1 ] || rc=96 ;;
        editrel) [ "$nobj" -eq 1 ] || rc=96
                 [ -z "$binary" ] || [ "$binary" -nt "$MARK" ] || rc=95 ;;
      esac
    fi
  fi
  if [ "$rc" -eq 0 ] && [ -n "${CHECK_COMPDB:-}" ]; then
    find "$PROJ" -name compile_commands.json -newer "$MARK" -size +2c 2>/dev/null \
      | grep -q . || rc=98
  fi
  clear_line
  if [ "$rc" -ne 0 ]; then
    local why="exit $rc"
    [ "$rc" -eq 98 ] && why="compile_commands.json tidak dihasilkan"
    [ "$rc" -eq 97 ] && why="binary tidak dihasilkan"
    [ "$rc" -eq 96 ] && why="jumlah objek tidak sesuai"
    printf '  %s✗ %-12s %-7s GAGAL (%s)%s\n' "$C_RED" "$label" "$scen" "$why" "$C_RST"
    ms=-1
  else
    printf '  %s✓%s %-12s %-7s %6s ms  %s%s obj%s\n' \
      "$C_GRN" "$C_RST" "$label" "$scen" "$ms" "$C_DIM" "$nobj" "$C_RST"
  fi
  local ver=${VER[$label]:-}; ver=${ver//,/ }
  printf '%s,%s,%s,%s,%s,%s\n' "$label" "$scen" "$ms" "$rc" "$nobj" "$ver" >>"$CSV"
  STEP=$(( STEP + 1 )); draw_progress
}

report() {
  local sorted="$OUTDIR/.sorted.csv"
  awk -F, 'NR > 1' "$CSV" | sort -t, -k2,2 -k1,1 -k3,3n >"$sorted"
  awk -F, -v BW="$BW" -v WIDE="$WIDE" -v B="$C_BLD" -v D="$C_DIM" \
      -v G="$C_GRN" -v Y="$C_YEL" -v R="$C_RED" -v Z="$C_RST" '
    {
      g = $2 SUBSEP $1
      if (!(g in seen)) { seen[g] = 1; ng++; gs[ng] = g; gscen[ng] = $2; glab[ng] = $1 }
      if ($4 == 0) { n[g]++; v[g, n[g]] = $3; rb[g] += $5 } else bad[g]++
    }
    END {
      nsc = split("cold noop touch edit editrel compdb", SC, " ")
      desc["cold"]   = "build dari nol (Ninja graph gen, CMake/Meson configure included)"
      desc["noop"]   = "tanpa perubahan"
      desc["touch"]  = "touch 1 header, konten sama"
      desc["edit"]    = "edit 1 source KOMENTAR - satria boleh no-relink; BUKAN pembanding link"
      desc["editrel"] = "edit 1 source + fungsi baru - relink wajib semua tool (pembanding link jujur vs lantai gcc)"
      desc["compdb"] = "compile_commands.json (bear = build penuh)"
      for (si = 1; si <= nsc; si++) {
        s = SC[si]; k = 0; nf = 0; hasfl = 0; nofl = 0
        for (i = 1; i <= ng; i++) {
          if (gscen[i] != s) continue
          g = gs[i]
          if (n[g] > 0) {
            m = n[g]
            med = (m % 2) ? v[g, (m + 1) / 2] : (v[g, m / 2] + v[g, m / 2 + 1]) / 2
            if (glab[i] == "gcc-floor") { if (s != "edit") { FL = med; FLLO = v[g, 1]; FLHI = v[g, m]; hasfl = 1 } else nofl = 1; continue }
            k++; L[k] = glab[i]; M[k] = med; LO[k] = v[g, 1]; HI[k] = v[g, m]
            RB[k] = rb[g] / m; BD[k] = bad[g] + 0
          } else fl[++nf] = glab[i]
        }
        if (k == 0 && nf == 0 && !hasfl) continue
        for (a = 2; a <= k; a++)
          for (b = a; b > 1 && M[b] < M[b - 1]; b--) {
            t = L[b];  L[b]  = L[b - 1];  L[b - 1]  = t
            t = M[b];  M[b]  = M[b - 1];  M[b - 1]  = t
            t = LO[b]; LO[b] = LO[b - 1]; LO[b - 1] = t
            t = HI[b]; HI[b] = HI[b - 1]; HI[b - 1] = t
            t = RB[b]; RB[b] = RB[b - 1]; RB[b - 1] = t
            t = BD[b]; BD[b] = BD[b - 1]; BD[b - 1] = t
          }
        printf "\n%s%s%s %s%s%s\n", B, toupper(s), Z, D, desc[s], Z
        mx = (k > 0) ? M[k] : 1; if (mx < 1) mx = 1
        best = (k > 0) ? M[1] : 1; if (best < 1) best = 1
        # Seri: yang tercepat dan saingannya selisih <= 5% atau <= 1 ms.
        # Pada kondisi seri TIDAK ada yang disebut tercepat (tanpa ★).
        tol = best * 0.05; if (tol < 1) tol = 1
        ntie = 0; for (i = 1; i <= k; i++) if (M[i] - M[1] <= tol) ntie++
        seri = (ntie >= 2)
        for (i = 1; i <= k; i++) {
          len = int(M[i] / mx * BW + 0.5); if (len < 1) len = 1
          br = ""; for (j = 1; j <= BW; j++) br = br (j <= len ? "█" : "░")
          if (seri) { col = (i <= ntie) ? Y : ""; mark = (i <= ntie) ? "≈" : " " }
          else      { col = (i == 1) ? G : ""; mark = (i == 1) ? "★" : " " }
          printf "  %s%s %-12s %s %6d ms %5.2f×%s", col, mark, L[i], br, M[i], M[i] / best, Z
          if (WIDE) printf "  %s(%d–%d) obj:%d%s", D, LO[i], HI[i], RB[i] + 0.5, Z
          if (WIDE && hasfl) printf " %s%+dms%s", D, M[i] - FL, Z
          if (s == "touch" && RB[i] < 0.5 && L[i] != "satria") printf " %s⚠ header?%s", Y, Z
          if (BD[i] > 0) printf " %s✗%d%s", R, BD[i], Z
          printf "\n"
        }
        if (hasfl) printf "  %s┄ %-12s %6d ms  lantai: gcc -c + link (+N = overhead)%s\n", D, "gcc-floor", FL, Z
        if (nofl) printf "  %s lantai gcc tidak dibandingkan di sini (edit komentar, tanpa relink); lihat editrel%s\n", Y, Z
        for (i = 1; i <= nf; i++) printf "  %s✗ %-12s semua run gagal%s\n", R, fl[i], Z
      }
    }' "$sorted"
  rm -f "$sorted"
  echo
}

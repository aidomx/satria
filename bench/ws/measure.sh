# Pengukuran: snapshot artefak, run_once, verdict per run.
# ---------------------------- pengukuran -----------------------------------
# Snapshot artefak sebelum/sesudah command. Jangan gunakan find -newer:
# timestamp marker dapat beresolusi sama dengan artefak, dan file yang
# sekadar tersedia bukan berarti dibangun ulang.
artifact_snapshot() {
  local dest=$1
  (cd "$WS" && find . -type f \( -name '*.o' -o -name 'rupa_modules.tar' \
    -o -name 'lib*.a' -o -name 'lib*.so' \
    -o -name rupa -o -name bade \) -print0 | sort -z | \
    xargs -0 -r stat -c '%n|%s|%y|%z' 2>/dev/null) > "$dest"
}
artifact_delta() {
  local before=$1 after=$2
  awk -F '|' '
    FILENAME==ARGV[1] { old[$1]=$0; next }
    !($1 in old) || old[$1] != $0 {
      p=$1
      if (p ~ /\.o$/) obj++
      else if (p ~ /(^|\/)rupa_modules\.tar$/) arc++
      else if (p ~ /(^|\/)lib(core|bade)\.(a|so)$/) lib++
      else if (p ~ /(^|\/)(rupa|bade)$/) bin++
    }
    END { printf "%d %d %d %d\n", obj+0, arc+0, lib+0, bin+0 }
  ' "$before" "$after"
}

run_once() {
  local label=$1 scen=$2
  shift 2
  local t0 t1 rc ms nobj narc nlib nbin ncc nlnk
  CUR="$label/$scen"
  draw_progress
  artifact_snapshot "$OUTDIR/.artifacts.before"
  : > "$RB_CC_LOG"
  t0=${EPOCHREALTIME/[.,]/}
  (cd "$WS" && "$@") > /dev/null 2>> "$ERRLOG"
  rc=$?
  t1=${EPOCHREALTIME/[.,]/}
  ms=$(((t1 - t0 + 500) / 1000))
  artifact_snapshot "$OUTDIR/.artifacts.after"
  # Tidak cukup sekadar link sukses: kedua arah circular wajib berjalan,
  # dan setelah edit header tidak boleh ada object basi yang lolos (nilai
  # WS_SHARED_VALUE mengalir ke kedua main).
  if [ "$rc" -eq 0 ]; then
    # Path binary: satria root-centric (bin/ di root workspace — design/
    # workspace_v1.md); CMake memakai layout nested per proyek.
    local rupa_bin bade_bin
    case $label in
      satria) rupa_bin="$WS/bin/rupa"; bade_bin="$WS/bin/bade" ;;
      *) rupa_bin="$WS/core/bin/rupa"; bade_bin="$WS/bade/bin/bade" ;;
    esac
    if ! "$rupa_bin" >/dev/null 2>>"$ERRLOG" ||
       ! "$bade_bin" >/dev/null 2>>"$ERRLOG"; then
      rc=96
      echo "$label/$scen: runtime check gagal (circular / object basi)" >> "$ERRLOG"
    fi
  fi
  read -r nobj narc nlib nbin < <(artifact_delta "$OUTDIR/.artifacts.before" "$OUTDIR/.artifacts.after")
  read -r ncc nlnk < <(awk -F '|' \
    '{ c[$1]++ } END { printf "%d %d\n", c["C"] + 0, c["L"] + c["S"] + c["D"] + c["E"] + 0 }' \
    "$RB_CC_LOG" 2> /dev/null) || { ncc=0; nlnk=0; }
  # Jejak bukti kerja aktual per run (audit pasca-run: compile/relink mana
  # yang benar-benar terjadi).
  { printf '## %s/%s rc=%s\n' "$(tool_display "$label")" "$scen" "$rc"
    sed 's/^/   /' "$RB_CC_LOG" 2> /dev/null; } >> "$WORK_LOG"

  expect_check "$label" "$scen" "$nobj" "$narc" "$nlib" "$nbin" "$ncc"
  # Jejak ringkas per run untuk laporan view: aktual + harapan (EXP_* = -1
  # bila tidak di-hard-check) + miss flag (kerja berbeda dari harapan).
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$(tool_display "$label")" "$scen" "$rc" "$ms" "$nobj" "$narc" "$nlib" "$nbin" "$ncc" \
    "$EXP_OBJ" "$EXP_ARC" "$EXP_LIB" "$EXP_BIN" "$EXP_CC" \
    $([ -n "$EXPECT_MISS" ] && echo 1 || echo 0) >> "$VIEW_CSV"
  if [ "$rc" -eq 0 ] && [ -n "$EXPECT_MISS" ]; then
    # Shim/snapshot mengukur pekerjaan nyata. Kerja di luar harapan berarti
    # Not in accordance untuk tool mana pun (termasuk satria).
    echo "$label/$scen: kerja berbeda dari harapan:$EXPECT_MISS" >> "$ERRLOG"
    NOT_ACC=$((NOT_ACC + 1))
  fi
  clear_line
  if [ "$rc" -eq 0 ] && [ -n "$EXPECT_MISS" ]; then
    printf '  %s✗ %-6s %-6s Not in accordance:%s%s\n' "$C_RED" "$(tool_display "$label")" "$scen" "$EXPECT_MISS" "$C_RST"
  elif [ "$rc" -ne 0 ]; then
    printf '  %s✗ %-6s %-6s GAGAL (exit %s)%s\n' "$C_RED" "$(tool_display "$label")" "$scen" "$rc" "$C_RST"
    MISMATCH_RUNS=$((MISMATCH_RUNS + 1))
    ms=-1
  else
    # Per-run: satu baris ringkas, format "> [tool in skenario] N ms obj cc lib bin" (rata kolom, <= 54 kolom). "summary vs except" tidak dicetak di sini
    # (verdict baru pasti setelah semua run selesai, lihat laporan view).
    printf '%s>%s [%-5s %-8s] %5sms | obj:%s cc:%s lib:%s bin:%s\n' \
      "$C_CYN" "$C_RST" "$(tool_display "$label")" "$scen" "$ms" "$nobj" "$ncc" "$nlib" "$nbin"
  fi
  local ver=${VER[$label]:-}
  ver=${ver//,/ }
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$label" "$scen" "$ms" "$rc" \
    "$nobj" "$narc" "$nlib" "$nbin" "$ncc" "$nlnk" "$ver" >> "$CSV"
  STEP=$((STEP + 1))
  draw_progress
}

have_scenario() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  return 1
}


# bench/compare/scen.sh  skenario: cold, noop, touch, edit, compdb, strace.
# Dimuat oleh bench/compare.sh (cmp_source). Bukan skrip mandiri.

# ---------------------------- skenario -------------------------------------
have_scenario() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  return 1
}

reset_build() {
  ( cd "$PROJ" && rm -rf build bin/app )
}

reset_all() {
  # Reset semua artefak termasuk configure cache
  ( cd "$PROJ" && rm -rf build bin/app build-cmake build-meson .xmake .tup .satria .rbot .ninja_log .ninja_deps compile_commands.json )
  if [ "$HAVE_TUP" = 1 ]; then ( cd "$PROJ" && tup init >/dev/null 2>&1 || true ); fi
}

# Siapkan state bersih & konsisten untuk SATU tool: build penuh + 1 no-op
# (tidak dihitung). Dipanggil sebelum noop/touch/edit/compdb/strace.
prime_tool() {
  CUR="prime $(label_of "$1")"; draw_progress
  reset_all
  # Each tool starts from the exact same source content.
  [ ! -f "$PROJ/src/t1.c.baseline" ] || cp "$PROJ/src/t1.c.baseline" "$PROJ/src/t1.c"
  if ! ( cd "$PROJ" && build_full "$1" && tool_cmd "$1" ) >/dev/null 2>>"$ERRLOG"; then
    clear_line
    printf '  %s! prime gagal: %s (lihat %s)%s\n' "$C_YEL" "$(label_of "$1")" "$ERRLOG" "$C_RST"
  fi
}

touch_header() { ( cd "$PROJ" && touch "include/h$(( NHEAD / 2 + 1 )).h" ); }

# ---- COLD: build dari nol -------------------------------------------------
s_cold() {
  have_scenario cold || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    for ((r = 1; r <= NRUN; r++)); do
      reset_all
      run_once "$(label_of "$k")" cold build_full "$k"
    done
  done
}

# ---- NOOP: build tanpa perubahan -----------------------------------------
s_noop() {
  have_scenario noop || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    for ((r = 1; r <= NRUN; r++)); do
      run_once "$(label_of "$k")" noop tool_cmd "$k"
    done
  done
}

# ---- TOUCH: touch 1 header (konten sama) ---------------------------------
s_touch() {
  have_scenario touch || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    for ((r = 1; r <= NRUN; r++)); do
      touch_header
      run_once "$(label_of "$k")" touch tool_cmd "$k"
    done
  done
}

# Edit yang mengubah codegen (fungsi baru per run) — .o pasti berubah byte,
# jadi SEMUA tool wajib relink. Skenario editrel memakai ini supaya perbandingan
# terhadap gcc-floor (yang selalu link) jujur; edit komentar murni pada
# skenario `edit` membiarkan satria lewat tanpa relink (verdict byte-object).
relink_edit() {
  ( cd "$PROJ" && printf '\nint bench_r%s_%s(void) { return %s; }\n' "$1" "$2" "$2" >>src/t1.c )
}

# ---- EDITREL: edit 1 source + relink pasti (vs lantai gcc) ---------------
s_editrel() {
  have_scenario editrel || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    for ((r = 1; r <= NRUN; r++)); do
      relink_edit "$k" "$r"
      run_once "$(label_of "$k")" editrel tool_cmd "$k"
    done
  done
  # lantai (gcc langsung) — objek disiapkan lewat make
  prime_tool make
  for ((r = 1; r <= NRUN; r++)); do
    relink_edit floor "$r"
    run_once gcc-floor editrel floor_run
  done
}

# ---- EDIT: edit 1 source (file & jumlah edit sama untuk semua tool) -------
floor_run() {  # lantai: compile 1 file + link, tanpa build tool
  gcc -O2 -std=gnu11 -Iinclude -MMD -MP -c src/t1.c -o build/t1.o && gcc build/*.o -o bin/app
}
s_edit() {
  have_scenario edit || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    for ((r = 1; r <= NRUN; r++)); do
      ( cd "$PROJ" && printf '\n/* edit %s-%s */\n' "$k" "$r" >>src/t1.c )
      run_once "$(label_of "$k")" edit tool_cmd "$k"
    done
  done
  # lantai (gcc langsung) — objek disiapkan lewat make
  prime_tool make
  for ((r = 1; r <= NRUN; r++)); do
    ( cd "$PROJ" && printf '\n/* edit floor-%s */\n' "$r" >>src/t1.c )
    run_once gcc-floor edit floor_run
  done
}

# ---- COMPDB: generate compile_commands.json ------------------------------
# Hasil divalidasi: file harus benar-benar ada & tidak kosong.
compdb_run() {
  case $1 in
    ninja) ninja -t compdb cc >compile_commands.json ;;
    bear)  bear -- make -j"$NPROC" ;;
    satria)  "$SATRIA" -g compdb ;;
    cmake) cmake -G Ninja -B build-cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ;;
    meson) meson setup --reconfigure build-meson --buildtype=release ;;
    xmake) xmake project -k compile_commands ;;
  esac
}

s_compdb() {
  have_scenario compdb || return 0
  local k r
  for k in "${CDB_TOOLS[@]}"; do
    # bear hanya merekam compile yang benar-benar jalan => butuh build bersih
    [ "$k" = bear ] || prime_tool "$k"
    [ "$k" = satria ] && ( cd "$PROJ" && sed -i 's/^o.compileCommands = false/o.compileCommands = auto/' Buildfile )
    for ((r = 1; r <= NRUN; r++)); do
      ( cd "$PROJ" && rm -f compile_commands.json build-cmake/compile_commands.json build-meson/compile_commands.json )
      [ "$k" = bear ] && { reset_all; cp "$PROJ/src/t1.c.baseline" "$PROJ/src/t1.c"; }
      CHECK_COMPDB=1 run_once "$(label_of "$k")" compdb compdb_run "$k"
    done
    [ "$k" = satria ] && ( cd "$PROJ" && sed -i 's/^o.compileCommands = auto/o.compileCommands = false/' Buildfile )
  done
}

# ---- STRACE: hitung syscall no-op ----------------------------------------
s_strace() {
  # strace hanya berjalan bila diminta eksplisit melalui -s strace.
  [ -n "$ONLY" ] || return 0
  have_scenario strace || return 0
  [ "$HAVE_STRACE" = 1 ] || { echo "  (strace tidak ada — lewati)"; return 0; }
  local k n f lbl top err
  echo
  echo "${C_BLD}SYSCALL${C_RST} ${C_DIM}no-op, strace -c -f${C_RST}"
  echo 'tool,syscall,calls,errors' >"$SYSCSV"
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    lbl=$(label_of "$k")
    CUR="strace $lbl"; draw_progress
    f="$OUTDIR/strace.$k.txt"
    tool_argv "$k"
    ( cd "$PROJ" && strace -c -f "${ARGV[@]}" >/dev/null 2>"$f" )
    n=$(awk '$NF == "total" {print $4}' "$f")
    echo "$lbl,TOTAL,${n:-0},0" >>"$SYSCSV"
    awk -v t="$lbl" '$1 ~ /^[0-9]+\.[0-9]+$/ && $NF != "total" && NF >= 5 {
      print t "," $NF "," $4 "," ((NF == 6) ? $5 : 0) }' "$f" >>"$SYSCSV"
    top=$(awk -F, -v t="$lbl" '$1 == t && $2 != "TOTAL"' "$SYSCSV" | sort -t, -k3,3nr | head -4 \
          | awk -F, '{printf "%s%s=%s", (NR > 1 ? " " : ""), $2, $3}')
    err=$(awk -F, -v t="$lbl" '$1 == t && $2 != "TOTAL" {s += $4} END {print s + 0}' "$SYSCSV")
    clear_line
    printf '  %-12s %10s syscall  %serr=%s%s\n' "$lbl" "${n:-?}" "$C_DIM" "$err" "$C_RST"
    printf '  %s    %s%s\n' "$C_DIM" "$(printf '%s' "$top" | cut -c1-$(( COLS - 8 )))" "$C_RST"
    STEP=$(( STEP + 1 )); draw_progress
  done
}

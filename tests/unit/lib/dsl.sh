#!/bin/bash
# dsl.sh — eksekutor berkas `.test`/`.t` (M0+M1+M2+M3) di atas event dari scripts/lib/parser.sh.
#
# M0: work/fixture/file(write|append|touch|delete|contains)/env/time.tick/
# time.mark/run.build/run.cmd, @response @guard @requires @include @set @get
# @find @keep @return, selektor project(p).source|binary, gate work.clean
# pada [snap], verdict PASS/FAIL/PRECONDITION + ringkasan.
# M1: evaluator ekspresi (predikat/pembanding/and/or/not/ternary), variabel,
# interpolasi + escape \r\n\t, @return badan utama, selektor project(p).sources|
# dir|embedded|stamp|archive + repo()/work()/ws(), run.exec, file.copy lf|crlf.
# M2: @if/@else, @while (guard 1000 iterasi), @for (rentang a..b dua arah,
# koleksi selektor/{var}).
# M3: run.bg/stop/wait (proses latar, sinyal INT, batas tunggu), run.tool
# (alat repo, cwd=UNIT_REPO), stage.set/log (rekaman staging sintetis),
# text.count/aggregate (hitung baris; agregasi sort|uniq -c|sort -rn|head).
#
# Pemakaian: SATRIA=bin/satria bash tests/unit/lib/dsl.sh <file.test>
# Env: SATRIA, UNIT_REPO, UNIT_TMP, UNIT_JOBS, UNIT_CLASS (cold|snap),
#      UNIT_DISCOVERY (dir berisi sources.tsv), UNIT_FIXTURE (dir fixture siap pakai).
# Batas M0: tanpa pemrosesan kutip pada argumen (kutip ganda di nilai
# argumen dilepas; argumen ber-spasi tanpa kutip), tanpa tab di isi heredoc.

set -u
shopt -s nullglob globstar

SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=${UNIT_REPO:-$(cd "$SELF/../../.." && pwd)}
# parser: satu direktori dengannya dulu (staging runner: $STAGE/lib/parser.sh),
# fallback repo asal (pemakaian langsung: tests/unit/lib/dsl.sh).
if [ -f "$SELF/parser.sh" ]; then PARSER="$SELF/parser.sh"; else PARSER="$ROOT/scripts/lib/parser.sh"; fi
SATRIA=${SATRIA:-$ROOT/bin/satria}
UNIT_REPO=${UNIT_REPO:-$ROOT}
UNIT_TMP=${UNIT_TMP:-/tmp/satria-dsl}
UNIT_JOBS=${UNIT_JOBS:-2}
UNIT_CLASS=${UNIT_CLASS:-}
UNIT_DISCOVERY=${UNIT_DISCOVERY:-}

declare -A VAR OUT RC MTIME MTIME_T BGPID
PASSN=0 FAILN=0 KEEP=0 WORK='' MODE='workspace' FIX='' RESP=''
COLLECT=0 CBUF='' PEND=''
COLLECT=0 CBUF='' PEND=''
RET_VALUE='' RET_HIT=0
WORKS=()
mkdir -p "$UNIT_TMP"

die()     { echo "FAIL dsl: $*" >&2; exit 1; }
precond() { echo "PRECONDITION $*" >&2; exit 3; } # rc 3: PRECONDITION (log tetap terbaca runner)
ok()      { PASSN=$((PASSN+1)); echo "PASS: $1"; }
no()      { FAILN=$((FAILN+1)); echo "FAIL: $1 (dapat $2, harap $3)"; }
unq()     { local s=$1; [[ "$s" == \"*\" && ${#s} -ge 2 ]] && s=${s:1:${#s}-2}; printf '%s' "$s"; }
cleanup() { if [ "$KEEP" -eq 0 ] && [ "${#WORKS[@]}" -gt 0 ]; then rm -rf "${WORKS[@]}"; fi; }
trap cleanup EXIT

interp() {
  local s=$1 k
  for k in "${!VAR[@]}"; do s=${s//"{$k}"/"${VAR[$k]}"}; done
  printf '%s' "$s"
}

sel_source() { # $1=proyek → path `<proyek>/<sumber>` relatif WORK
  local tsv="$UNIT_DISCOVERY/sources.tsv" p=$1 want have r
  [ -s "$tsv" ] || return 1
  have=$(awk -F'\t' -v w="$p" '$1==w{f=1}END{print f+0}' "$tsv")
  want=$p
  [ "$have" = "1" ] || want=$(awk -F'\t' '{print $1}' "$tsv" | head -1)
  [ -n "$want" ] || return 1
  r=$(awk -F'\t' -v w="$want" \
      '$1==w && $2!~/(^|\/)embedded\.c$/ {print (($2~/^src\//||$2~/\/src\//)?0:1), $2}' \
      "$tsv" | sort | cut -d' ' -f2- | head -1)
  [ -n "$r" ] || return 1
  printf '%s/%s' "$want" "$r"
}

sel_binary() { # $1=proyek → bin/<binaryName> (micro-read Buildfile.ws di WORK)
  local p=$1 alias name
  alias=$(sed -n "s/^$p\.output as *//p" "$WORK/Buildfile.ws" 2>/dev/null | head -1 | tr -d ' \t')
  name=''
  [ -n "$alias" ] && name=$(sed -n "s/^$alias\.binaryName *= *//p" "$WORK/Buildfile.ws" 2>/dev/null | head -1 | tr -d ' \t')
  [ -n "$name" ] || name=$p
  printf 'bin/%s' "$name"
}

sel_sources() { # semua sumber `<p>/<src>` (skip embedded.c), dipisah spasi
  local tsv="$UNIT_DISCOVERY/sources.tsv" p=$1 want have
  [ -s "$tsv" ] || return 1
  have=$(awk -F'\t' -v w="$p" '$1==w{f=1}END{print f+0}' "$tsv")
  want=$p
  [ "$have" = "1" ] || want=$(awk -F'\t' '{print $1}' "$tsv" | head -1)
  [ -n "$want" ] || return 1
  awk -F'\t' -v w="$want" '$1==w && $2!~/(^|\/)embedded\.c$/ {print w "/" $2}' "$tsv" \
    | tr '\n' ' ' | sed 's/ $//'
}

sel_dir() { # proyek → root dir-nya relatif WORK (fallback proyek-pertama)
  local tsv="$UNIT_DISCOVERY/sources.tsv" p=$1 have want
  [ -s "$tsv" ] || return 1
  have=$(awk -F'\t' -v w="$p" '$1==w{f=1}END{print f+0}' "$tsv")
  want=$p
  [ "$have" = "1" ] || want=$(awk -F'\t' '{print $1}' "$tsv" | head -1)
  [ -n "$want" ] || return 1
  printf '%s' "$want"
}

sel_archive() { # berkas arsip proyek: alias embed (`<p>.embedded.<x> as a` → a.file)
  local p=$1 alias f dir name          # fallback: <p>.archive.dir + <p>.archive.name
  alias=$(sed -n "s/^$p\.embedded\.[A-Za-z0-9_]* as *//p" "$WORK/Buildfile.ws" 2>/dev/null | head -1 | tr -d ' \t')
  if [ -n "$alias" ]; then
    f=$(sed -n "s/^$alias\.file *= *//p" "$WORK/Buildfile.ws" 2>/dev/null | head -1 | tr -d ' \t')
    [ -n "$f" ] && { printf '%s' "$f"; return 0; }
  fi
  dir=$(sed -n "s/^$p\.archive\.dir *= *//p" "$WORK/Buildfile.ws" 2>/dev/null | head -1 | tr -d ' \t')
  name=$(sed -n "s/^$p\.archive\.name *= *//p" "$WORK/Buildfile.ws" 2>/dev/null | head -1 | tr -d ' \t')
  [ -n "$dir" ] && [ -n "$name" ] && { printf '%s/%s.tar' "$dir" "$name"; return 0; }
  return 1
}

resolve_args() { # interpolasi {var} + selektor project(p).* / repo() / work() / ws()
  local s; s=$(interp "$1")
  local re='project\(([A-Za-z0-9_]+)\)\.(source|sources|binary|dir|embedded|stamp|archive)' p at r
  while [[ "$s" =~ $re ]]; do
    p=${BASH_REMATCH[1]} at=${BASH_REMATCH[2]} r=''
    case "$at" in
      source)   r=$(sel_source "$p") || precond "selektor project($p).source tak terpenuhi" ;;
      sources)  r=$(sel_sources "$p") || precond "selektor project($p).sources tak terpenuhi" ;;
      binary)   r=$(sel_binary "$p") || precond "selektor project($p).binary tak terpenuhi" ;;
      dir)      r=$(sel_dir "$p") || precond "selektor project($p).dir tak terpenuhi" ;;
      embedded) r="build/$p/embedded.c" ;;
      stamp)    r="build/$p/embedded.stamp" ;;
      archive)  r=$(sel_archive "$p") || precond "selektor project($p).archive tak terpenuhi" ;;
    esac
    s=${s/"project($p).$at"/$r}
  done
  local re2='repo\(([^)]+)\)' w
  while [[ "$s" =~ $re2 ]]; do
    w=${BASH_REMATCH[1]}
    # tanpa precond: berkas boleh belum ada — exists()/absent yang memutuskan
    s=${s/"repo($w)"/"$UNIT_REPO/$w"}
  done
  local re3='work\(([^)]+)\)'
  while [[ "$s" =~ $re3 ]]; do
    w=${BASH_REMATCH[1]}
    s=${s/"work($w)"/"$WORK/$w"}
  done
  local re4='ws\(([^)]+)\)'
  while [[ "$s" =~ $re4 ]]; do
    w=${BASH_REMATCH[1]}
    r=$(sed -n "s/^$w *= *//p" "$WORK/Buildfile.ws" 2>/dev/null | head -1 | tr -d ' \t')
    [ -n "$r" ] || precond "kunci Buildfile.ws tidak ada: $w"
    s=${s/"ws($w)"/$r}
  done
  printf '%s' "$s"
}

set_work() {
  WORK="$UNIT_TMP/$1"; WORKS+=("$WORK")
  rm -rf "$WORK"; mkdir -p "$WORK"
}

# ===== Ekspresi (L3, M1) =====
unesc() { # escape literal: \r \n \t \\ → karakter aslinya
  local s=$1
  s=${s//\\r/$'\r'}; s=${s//\\n/$'\n'}; s=${s//\\t/$'\t'}; s=${s//\\\\/\\}
  printf '%s' "$s"
}
ev_truthy() { case "$1" in '' | 0 | false) return 1 ;; esac; return 0; }

scan_top() { # $1=teks $2=pemisah-1-char → OUT=kiri REST=kanan (rc0); rc1: tak ketemu
  local s=$1 sep=$2 i c q='' d=0
  OUT=''; REST=''
  for ((i = 0; i < ${#s}; i++)); do
    c=${s:i:1}
    if [ -n "$q" ]; then [ "$c" = '"' ] && q=''; OUT+=$c; continue; fi
    case "$c" in
      '"') q=1; OUT+=$c ;;
      '(') d=$((d + 1)); OUT+=$c ;;
      ')') ((d > 0)) && d=$((d - 1)); OUT+=$c ;;
      "$sep") if [ "$d" -eq 0 ]; then REST=${s:i+1}; return 0; fi; OUT+=$c ;;
      *) OUT+=$c ;;
    esac
  done
  return 1
}

scan_topw() { # sama, pemisah kata ber-spasi (mis. " and "/" or ") di luar kutip/kurung
  local s=$1 w=$2 i c q='' d=0
  local n=${#w}
  OUT=''; REST=''
  for ((i = 0; i < ${#s}; i++)); do
    c=${s:i:1}
    if [ -n "$q" ]; then [ "$c" = '"' ] && q=''; OUT+=$c; continue; fi
    case "$c" in
      '"') q=1; OUT+=$c; continue ;;
      '(') d=$((d + 1)); OUT+=$c; continue ;;
      ')') ((d > 0)) && d=$((d - 1)); OUT+=$c; continue ;;
    esac
    if [ "$d" -eq 0 ] && [ "${s:i:n}" = "$w" ]; then REST=${s:i+n}; return 0; fi
    OUT+=$c
  done
  return 1
}

out_path() { # $1=label → berkas out; rc1 bila run belum ada
  local o=${OUT[$1]:-}
  [ -n "$o" ] && [ -f "$o" ] || return 1
  printf '%s' "$o"
}

ev_pred() { # $1=nama predikat $2=argumen dipisah ',' (top-level) → EV=nilai
  local name=$1 rest=$2 a1=$2 a2='' f c
  scan_top "$rest" ',' && { a1=$OUT; a2=$REST; }
  a1=${a1#" "}; a2=${a2#" "}   # spasi setelah ',' atau '('
  case "$name" in
    exists)    { [ -e "$(wpath "$a1")" ]; } && EV=1 || EV=0 ;;
    contains)  EV=0
               f=$(out_path "$(unq "$a1")") && grep -qF -- "$(unq "$a2")" "$f" && EV=1 ;;
    matches)   EV=0
               f=$(out_path "$(unq "$a1")") && grep -qE -- "$(unq "$a2")" "$f" && EV=1 ;;
    rc)        EV=${RC[$(unq "$a1")]:-} ;;
    lines)     EV=0; [ -f "$(wpath "$a1")" ] && EV=$(wc -l < "$(wpath "$a1")" | tr -d ' ') ;;
    count)     EV=0; [ -f "$(wpath "$a1")" ] && EV=$(grep -cE -- "$(unq "$a2")" "$(wpath "$a1")" 2>/dev/null) ;;
    size)      EV=0; [ -f "$(wpath "$a1")" ] && EV=$(wc -c < "$(wpath "$a1")" | tr -d ' ') ;;
    changed)   EV=0; [ -n "${MTIME[$a1]:-}" ] || return
               c=$(stat -c %y "${MTIME[$a1]}" 2>/dev/null)
               { [ -n "$c" ] && [ "$c" != "${MTIME_T[$a1]:-}" ]; } && EV=1 ;;
    unchanged) EV=0; [ -n "${MTIME[$a1]:-}" ] || return
               c=$(stat -c %y "${MTIME[$a1]}" 2>/dev/null)
               [ -n "$c" ] && [ "$c" = "${MTIME_T[$a1]:-}" ] && EV=1 ;;
    same)      { cmp -s "$(wpath "$a1")" "$(wpath "$a2")"; } && EV=1 || EV=0 ;;
    *) die "predikat tak dikenal: $name(...)" ;;
  esac
}

CUR=''
ev_primary() { # parse satu primary dari CUR → EV=nilai; CUR maju ke sisa
  local s=$CUR v c d=0 i
  while [ "${s:0:1}" = ' ' ]; do s=${s:1}; done
  case "${s:0:1}" in
    '')
      EV=''; CUR=''; return ;;
    '(')
      for ((i = 0; i < ${#s}; i++)); do
        c=${s:i:1}
        [ "$c" = '(' ] && d=$((d + 1))
        if [ "$c" = ')' ]; then d=$((d - 1)); [ "$d" -eq 0 ] && break; fi
      done
      v=${s:1:i-1}; CUR=${s:i+1}
      ev_run "$v"
      return ;;
    '"')
      for ((i = 1; i < ${#s}; i++)); do [ "${s:i:1}" = '"' ] && break; done
      v=${s:1:i-1}; CUR=${s:i+1}
      EV=$(unesc "$v")
      return ;;
  esac
  if [[ "$s" =~ ^([A-Za-z_][A-Za-z0-9_]*)\((.*)$ ]]; then
    local name=${BASH_REMATCH[1]} rest=${BASH_REMATCH[2]} a='' d2=1
    for ((i = 0; i < ${#rest}; i++)); do
      c=${rest:i:1}
      [ "$c" = '(' ] && d2=$((d2 + 1))
      if [ "$c" = ')' ]; then d2=$((d2 - 1)); [ "$d2" -eq 0 ] && break; fi
      a+=$c
    done
    CUR=${rest:i+1}
    ev_pred "$name" "$a"
    return
  fi
  v=${s%% *}   # bareword/angka: operator dipisah spasi
  CUR=${s:${#v}}
  EV=$v
}

ev_cmp() { # lhs [op rhs] → EV = 1/0 atau nilai mentah
  local op lhs rhs
  ev_primary
  while [ "${CUR:0:1}" = ' ' ]; do CUR=${CUR:1}; done
  if [[ "$CUR" =~ ^(==|!=|<=|>=|!~|<|>|~) ]]; then
    op=${BASH_REMATCH[1]}
    CUR=${CUR:${#op}}
    lhs=$EV
    ev_primary
    rhs=$EV
    case "$op" in
      '==') { [ "$lhs" = "$rhs" ]; } && EV=1 || EV=0 ;;
      '!=') { [ "$lhs" != "$rhs" ]; } && EV=1 || EV=0 ;;
      '<' | '>' | '<=' | '>=')
        if [[ "$lhs" =~ ^-?[0-9]+$ && "$rhs" =~ ^-?[0-9]+$ ]]; then
          case "$op" in
            '<') { ((lhs < rhs)); } && EV=1 || EV=0 ;;
            '>') { ((lhs > rhs)); } && EV=1 || EV=0 ;;
            '<=') { ((lhs <= rhs)); } && EV=1 || EV=0 ;;
            '>=') { ((lhs >= rhs)); } && EV=1 || EV=0 ;;
          esac
        else
          case "$op" in
            '<') { [[ "$lhs" < "$rhs" ]]; } && EV=1 || EV=0 ;;
            '>') { [[ "$lhs" > "$rhs" ]]; } && EV=1 || EV=0 ;;
            '<=') { [[ "$lhs" < "$rhs" || "$lhs" = "$rhs" ]]; } && EV=1 || EV=0 ;;
            '>=') { [[ "$lhs" > "$rhs" || "$lhs" = "$rhs" ]]; } && EV=1 || EV=0 ;;
          esac
        fi ;;
      '~') { [[ "$lhs" =~ $rhs ]]; } && EV=1 || EV=0 ;;
      '!~') { [[ "$lhs" =~ $rhs ]]; } && EV=0 || EV=1 ;;
    esac
    return
  fi
}

ev_not() {
  local s=$CUR
  while [ "${s:0:1}" = ' ' ]; do s=${s:1}; done
  CUR=$s
  if [[ "$CUR" =~ ^not[[:space:]] ]]; then
    CUR=${CUR:4}
    ev_cmp
    ev_truthy "$EV" && EV=0 || EV=1
  else
    ev_cmp
  fi
}

ev_and() {
  local l r
  if ! scan_topw "$CUR" ' and '; then ev_not; return; fi
  l=$OUT; r=$REST
  CUR=$l; ev_and
  if ! ev_truthy "$EV"; then CUR=$r; ev_and >/dev/null; EV=0; return; fi
  CUR=$r; ev_and
}

ev_or() {
  local l r
  if ! scan_topw "$CUR" ' or '; then ev_and; return; fi
  l=$OUT; r=$REST
  CUR=$l; ev_or
  if ev_truthy "$EV"; then CUR=$r; ev_or >/dev/null; EV=1; return; fi
  CUR=$r; ev_or
}

ev_run() { # $1=ekspresi (sudah resolve) → EV=hasil; ternary terendah, kanan-asosiatif
  CUR=$1
  local l c a b v
  if scan_top "$CUR" '?'; then
    l=$OUT; c=$REST
    CUR=$l; ev_or; v=$EV
    scan_top "$c" ':' || die "ternary tanpa ':': $1"
    a=$OUT; b=$REST
    if ev_truthy "$v"; then CUR=$a; ev_or; else ev_run "$b"; fi
    return
  fi
  ev_or
}

ev_expr() { # $1=ekspresi → echo nilai (API publik: dipakai via $( ))
  ev_run "$(resolve_args "$1")"
  printf '%s' "$EV"
}

wpath() { # argumen path → absolut: absolut diteruskan, relatif ditempel $WORK
  case "$1" in
    /*) printf '%s' "$1" ;;
    *)  printf '%s/%s' "$WORK" "$1" ;;
  esac
}

tick() {
  local p="$UNIT_TMP/.tick.$$" a b i=0
  : > "$p"; a=$(stat -c %y "$p")
  while [ "$i" -lt 200 ]; do
    printf x >> "$p"; b=$(stat -c %y "$p")
    [ "$a" != "$b" ] && break
    i=$((i+1)); sleep 0.01
  done
  rm -f "$p"
}

do_run() { # $1=label, sisanya argumen satria (verbatim)
  local label=$1 _rc; shift
  local outf="$WORK/.out.$label.txt"
  ( cd "$WORK" && env NO_COLOR=1 "$SATRIA" "$@" > "$outf" 2>&1 )
  _rc=$?   # assignment array me-reset $? — rekam dulu
  OUT[$label]=$outf
  RC[$label]=$_rc
}

label_out() { # $1=label → berkas out; gagal bila run belum ada
  local o=${OUT[$1]:-}
  [ -n "$o" ] && [ -f "$o" ] || { no "run berlabel $1" "tidak ada" "hasil run"; return 2; }
  printf '%s' "$o"
}

assert_exec() { # $1=obj $2=aksi $3=argumen(sudah resolve) $4=klausa
  local obj=$1 aksi=$2 args=$3 klausa=$4 label o a b cur
  label=${RESP:-last}
  case "$obj.$aksi" in
    rc.ok|rc.fail)
      [ -n "$args" ] && label=$args
      o=${RC[$label]:-}
      [ -n "$o" ] || { no "$klausa" "tidak ada run berlabel $label" "rc"; return; }
      if [ "$aksi" = ok ]; then
        [ "$o" -eq 0 ] && ok "$klausa" || no "$klausa" "$o" "0"
      else
        [ "$o" -ne 0 ] && ok "$klausa" || no "$klausa" "$o" "bukan 0"
      fi ;;
    out.contains|out.not-contains|out.matches)
      o=$(label_out "$label") || return
      args=$(unesc "$(unq "$args")")
      case "$aksi" in
        contains)      grep -qF -- "$args" "$o" && ok "$klausa" || no "$klausa" "tidak ada" "$args" ;;
        not-contains)  grep -qF -- "$args" "$o" && no "$klausa" "ada" "tiada $args" || ok "$klausa" ;;
        matches)       grep -qE -- "$args" "$o" && ok "$klausa" || no "$klausa" "tidak cocok" "$args" ;;
      esac ;;
    out.equals)
      read -r -a AB <<<"$args"
      a=$(label_out "${AB[0]}") || return
      b=$(label_out "${AB[1]}") || return
      cmp -s "$a" "$b" && ok "$klausa" || no "$klausa" "berbeda" "identik" ;;
    file.exists)
      [ -e "$(wpath "$args")" ] && ok "$klausa" || no "$klausa" "absen" "ada $args" ;;
    file.absent)
      [ ! -e "$(wpath "$args")" ] && ok "$klausa" || no "$klausa" "ada" "absen $args" ;;
    file.same)
      read -r -a AB <<<"$args"
      cmp -s "$(wpath "${AB[0]}")" "$(wpath "${AB[1]}")" && ok "$klausa" || no "$klausa" "berbeda" "identik" ;;
    file.contains)
      local p pat
      p=${args%% *}; pat=$(unesc "${args#* }")
      [ "$p" != "$pat" ] || die "bentuk: file.contains <path> <pola>"
      grep -qE -- "$pat" "$(wpath "$p")" && ok "$klausa" || no "$klausa" "tidak ada" "$pat" ;;
    mtime.changed|mtime.unchanged)
      [ -n "${MTIME[$args]:-}" ] || { no "$klausa" "rekaman $args tidak ada" "time.mark dulu"; return; }
      cur=$(stat -c %y "${MTIME[$args]}" 2>/dev/null)
      if [ "$aksi" = changed ]; then
        [ "$cur" != "${MTIME_T[$args]}" ] && ok "$klausa" || no "$klausa" "sama" "berubah"
      else
        [ "$cur" = "${MTIME_T[$args]}" ] && ok "$klausa" || no "$klausa" "berubah" "sama"
      fi ;;
  esac
}

flush_heredoc() {
  local path
  [ "$COLLECT" -eq 1 ] || return
  path=$(resolve_args "$PEND")
  mkdir -p "$WORK/$(dirname "$path")"
  interp "$CBUF" > "$WORK/$path"
  COLLECT=0; CBUF=''; PEND=''
}

dir_exec() { # $1=direktif $2=argumen
  local d=$1 rest=$2 args name val pat ip as list
  case "$d" in
    return)
      RET_VALUE=$(ev_expr "$rest"); RET_HIT=1 ;;
    guard)
      local gv rcgv=0
      gv=$(ev_expr "$rest") || rcgv=$?
      [ "$rcgv" -eq 3 ] && precond "@guard: $rest"
      [ "$rcgv" -eq 0 ] || die "@guard evaluasi gagal: $rest"
      ev_truthy "$gv" || precond "@guard salah: $rest" ;;
    set)
      local sv rcsv=0
      sv=$(ev_expr "${rest#* }") || rcsv=$?
      [ "$rcsv" -eq 3 ] && precond "@set: $rest"
      [ "$rcsv" -eq 0 ] || die "@set evaluasi gagal: $rest"
      VAR[${rest%% *}]=$sv ;;
    get)
      echo "INFO: $rest = ${VAR[$rest]:-}" ;;
    find)
      args=$(interp "$rest")
      name=${args%% *}; pat=${args#* }
      list=$(cd "$WORK" && compgen -G "$pat" | tr '\n' ' ')
      VAR[$name]=${list% } ;;
    include)
      read -r -a F <<<"$rest"
      ip="${F[0]}"; as=''
      [ "${#F[@]}" -eq 3 ] && as=${F[2]}
      [ -f "$UNIT_REPO/tests/unit/$ip" ] && ip="$UNIT_REPO/tests/unit/$ip" \
        || { [ -f "$ip" ] || die "@include tidak ada: $ip"; }
      local evf="$UNIT_TMP/ev.$$.inc"
      bash "$PARSER" "$ip" > "$evf" || die "parse gagal: $ip"
      execute_events < "$evf"
      rm -f "$evf"
      [ -n "$as" ] && { VAR[$as]=$RET_VALUE; RET_VALUE=''; }
      RET_HIT=0 ;;
  esac
}

stmt_exec() { # $1=obj $2=aksi $3=argumen mentah $4=baris
  local obj=$1 aksi=$2 rest=$3 ln=$4 args klausa label
  args=$(resolve_args "$rest")
  klausa="$obj.$aksi $rest"
  case "$obj.$aksi" in
    work.new)    set_work "${args:-$TESTNAME}" ;;
    work.clean)
      [ "$UNIT_CLASS" = snap ] && die "work.clean hanya untuk kelas [cold]"
      case "$args" in
        all)   rm -rf "$WORK/build" "$WORK/bin" "$WORK/lib" "$WORK/modules" "$WORK/.satria" ;;
        state) rm -rf "$WORK/.satria" ;;
        *) die "bentuk: work.clean all|state" ;;
      esac ;;
    fixture.copy)
      [ -n "$FIX" ] || die "fixture.copy tanpa @fixture"
      local fdir="${UNIT_FIXTURE:-$UNIT_REPO/tests/$FIX}"
      cp -a "$fdir/." "$WORK/" || die "gagal salin fixture $FIX" ;;
    file.write)
      COLLECT=1; CBUF=''; PEND=${args%% <<*} ;;
    file.append)
      local p=${args%% *} t
      t=$(unesc "$(unq "${args#* }")")
      mkdir -p "$WORK/$(dirname "$p")"
      printf '%s\n' "$t" >> "$WORK/$p" ;;
    file.touch)  touch "$(wpath "$args")" ;;
    file.delete) rm -f "$(wpath "$args")" ;;
    file.copy)
      local -a F=()
      read -r -a F <<<"$args"
      local src=${F[0]:-} dst=${F[1]:-} conv=${F[2]:-}
      [ -n "$src" ] && [ -n "$dst" ] || die "bentuk: file.copy <src> <dst> [lf|crlf]"
      [ -e "$src" ] || die "file.copy: sumber tidak ada: $src"
      mkdir -p "$WORK/$(dirname "$dst")"
      case "$conv" in
        '')    cp "$src" "$WORK/$dst" ;;
        lf)    tr -d '\r' < "$src" > "$WORK/$dst" ;;
        crlf)  awk '{ printf "%s\r\n", $0 }' "$src" > "$WORK/$dst" ;;
        *)     die "file.copy: konversi tidak dikenal: $conv" ;;
      esac ;;
    env.set)     export "${args%%=*}=${args#*=}" ;;
    env.unset)   unset "${args%%=*}" 2>/dev/null || true ;;
    time.tick)   tick ;;
    time.mark)
      local p=${args%%" as "*} n=${args##*" as "}
      MTIME[$n]=$(wpath "$p")
      MTIME_T[$n]=$(stat -c %y "$(wpath "$p")") ;;
    run.build|run.cmd|run.exec|run.bg|run.wait|run.stop|run.tool)
      [ -x "$SATRIA" ] || precond "binary satria tidak ada: $SATRIA (set SATRIA=...)"
      local -a A=()
      read -r -a A <<<"$args"
      label=''
      local n=${#A[@]}
      if [ "$n" -ge 2 ] && [ "${A[$((n-2))]}" = "as" ]; then
        label=${A[$((n-1))]}
        unset "A[$((n-1))]" "A[$((n-2))]"
      fi
      label=${label:-last}
      if [ "$obj.$aksi" = run.build ]; then
        case "$MODE" in workspace) do_run "$label" -j"$UNIT_JOBS" -w ;; *) do_run "$label" -j"$UNIT_JOBS" ;; esac
      elif [ "$obj.$aksi" = run.exec ]; then
        [ ${#A[@]} -gt 0 ] || die "run.exec butuh program"
        local rf="$WORK/.out.$label.txt" _rc
        ( cd "$WORK" && "${A[0]}" "${A[@]:1}" > "$rf" 2>&1 )
        _rc=$?   # assignment array me-reset $? — rekam dulu
        OUT[$label]=$rf
        RC[$label]=$_rc
      elif [ "$obj.$aksi" = run.bg ]; then
        [ ${#A[@]} -gt 0 ] || die "run.bg butuh argumen"
        local rf="$WORK/.out.$label.txt"
        # exec: PID latar = satria itu sendiri (run.stop menarget sinyal benar)
        ( cd "$WORK" && exec "$SATRIA" "${A[@]}" ) > "$rf" 2>&1 &
        BGPID[$label]=$!
        OUT[$label]=$rf
      elif [ "$obj.$aksi" = run.wait ]; then
        local wl=${A[0]:-last} lim=${A[1]:-120} n=0 _rc
        [ -n "${BGPID[$wl]:-}" ] || die "run.wait: tidak ada proses latar '$wl' (run.bg dulu)"
        while kill -0 "${BGPID[$wl]}" 2> /dev/null; do
          sleep 0.1; n=$((n + 1))
          [ "$n" -le $((lim * 10)) ] || die "run.wait: lewat batas ${lim}s: $wl"
        done
        wait "${BGPID[$wl]}"; _rc=$?; RC[$wl]=$_rc
      elif [ "$obj.$aksi" = run.stop ]; then
        local sl=${A[0]:-last}
        [ -n "${BGPID[$sl]:-}" ] || die "run.stop: tidak ada proses latar '$sl'"
        kill -INT "${BGPID[$sl]}" 2> /dev/null || true
      elif [ "$obj.$aksi" = run.tool ]; then
        [ ${#A[@]} -gt 0 ] || die "run.tool butuh skrip"
        local ts=${A[0]} rf="$WORK/.out.$label.txt"
        [ -f "$UNIT_REPO/$ts" ] || die "run.tool: skrip tidak ada: $ts"
        ( cd "$UNIT_REPO" && sh "$UNIT_REPO/$ts" "${A[@]:1}" > "$rf" 2>&1 )
        _rc=$?
        OUT[$label]=$rf
        RC[$label]=$_rc
      else
        [ ${#A[@]} -gt 0 ] || die "run.cmd butuh argumen"
        do_run "$label" "${A[@]}"
      fi ;;
    stage.set)   # <berkas> <isi> — rekaman staging sintetis (kontrak run)
      local sp=${args%% *} sc
      sc=$(unesc "$(unq "${args#* }")")
      mkdir -p "$WORK/$(dirname "$sp")"
      printf '%s\n' "$sc" > "$WORK/$sp" ;;
    stage.log)   # <berkas> <teks> — tambah satu baris log staging
      local lp=${args%% *} lc
      lc=$(unesc "$(unq "${args#* }")")
      mkdir -p "$WORK/$(dirname "$lp")"
      printf '%s\n' "$lc" >> "$WORK/$lp" ;;
    text.count)  # <path> <pola> [as <nama>] — jumlah baris cocok (grep -cE)
      local -a T=()
      read -r -a T <<<"$args"
      [ ${#T[@]} -ge 2 ] || die "bentuk: text.count <path> <pola> [as <nama>]"
      local tc
      tc=$(grep -cE -- "${T[1]}" "$WORK/${T[0]}" 2> /dev/null) || true
      if [ -n "${T[2]:-}" ] && [ "${T[2]}" = as ]; then
        [ -n "${T[3]:-}" ] || die "text.count: nama variabel kosong setelah as"
        VAR[${T[3]}]=$tc
      else
        echo "INFO: text.count ${T[0]} = $tc"
      fi ;;
    text.aggregate) # <src> <dst> — sort | uniq -c | sort -rn | head
      local -a T2=()
      read -r -a T2 <<<"$args"
      [ ${#T2[@]} -eq 2 ] || die "bentuk: text.aggregate <src> <dst>"
      sort "$WORK/${T2[0]}" | uniq -c | sort -rn | head > "$WORK/${T2[1]}" ;;
    rc.*|out.*|file.exists|file.absent|file.same|file.contains|mtime.*)
      assert_exec "$obj" "$aksi" "$args" "$klausa" ;;
    *)
      die "baris $ln: $obj.$aksi belum dieksekusi di M0" ;;
  esac
}

# ===== Blok (M2): @if/@else, @while, @for =====
BLKN=0
run_block() { # $1=if|while|for $2=isi-direktif $3=baris
  # stdin: event isi blok (pasca baris BLOCK) sampai ENDB pasangannya —
  # badan dikumpulkan ke berkas dulu, lalu diputar ulang per iterasi;
  # RET_HIT dan PRECONDITION (rc 3) memotong semuanya.
  local kind=$1 ex=$2 ln=$3 ev l a b c depth=0
  BLKN=$((BLKN + 1)); local id=$BLKN   # unik: run_block bersarang tidak boleh
  local mf="$UNIT_TMP/ev.$$.b$id" af="$UNIT_TMP/ev.$$.c$id"  # saling memotong
  : > "$mf"; : > "$af"; local tf=$mf
  while IFS=$'\t' read -r ev l a b c; do
    if [ "$depth" -eq 0 ] && [ "$ev" = DIR ] && [ "$a" = else ]; then
      tf=$af; continue
    fi
    [ "$ev" = BLOCK ] && depth=$((depth + 1))
    if [ "$ev" = ENDB ]; then
      [ "$depth" -eq 0 ] && break
      depth=$((depth - 1))
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$ev" "$l" "$a" "$b" "$c" >> "$tf"
  done
  [ "$ev" = ENDB ] || die "baris $ln: @$kind tanpa @end"
  local rf
  case "$kind" in
    if)
      local gv rcgv=0
      gv=$(ev_expr "$ex") || rcgv=$?
      [ "$rcgv" -eq 3 ] && precond "@if: $ex"
      [ "$rcgv" -eq 0 ] || die "@if evaluasi gagal: $ex"
      if ev_truthy "$gv"; then rf=$mf; else rf=$af; fi
      execute_events < "$rf" ;;
    while)
      local n=0 rcgv=0
      while :; do
        n=$((n + 1)); [ "$n" -le 1000 ] || die "@while lebih dari 1000 iterasi: $ex"
        gv=$(ev_expr "$ex") || rcgv=$?
        [ "$rcgv" -eq 3 ] && precond "@while: $ex"
        [ "$rcgv" -eq 0 ] || die "@while evaluasi gagal: $ex"
        ev_truthy "$gv" || break
        execute_events < "$mf"
        [ "$RET_HIT" -eq 1 ] && break
      done ;;
    for)
      local fvar rem spec lo hi st v
      fvar=${ex%% *}; rem=${ex#* }
      [[ "$rem" = "in "* ]] || die "baris $ln: bentuk @for: @for <var> in <rentang|koleksi>"
      spec=$(resolve_args "${rem#in }") || precond "@for: $rem"
      if [[ "$spec" =~ ^(-?[0-9]+)\.\.(-?[0-9]+)$ ]]; then
        lo=${BASH_REMATCH[1]} hi=${BASH_REMATCH[2]} st=1
        [ "$lo" -le "$hi" ] || st=-1
        for ((v = lo; st == 1 ? v <= hi : v >= hi; v += st)); do
          VAR[$fvar]=$v
          execute_events < "$mf"
          [ "$RET_HIT" -eq 1 ] && break
        done
      else
        for v in $spec; do
          VAR[$fvar]=$v
          execute_events < "$mf"
          [ "$RET_HIT" -eq 1 ] && break
        done
      fi ;;
  esac
  rm -f "$mf" "$af"
}

execute_events() { # event TSV dari parser di stdin
  local EV LN A B C
  while IFS=$'\t' read -r EV LN A B C; do
    case "$EV" in
      CMT)  echo "$A" ;;
      HDR)
        case "$A" in
          suite)    : ;;  # modes sudah dibaca main
          fixture)  FIX=$B ;;
          purpose)  echo "# @purpose $B" ;;
          requires) [ -z "$UNIT_CLASS" ] || [ "$UNIT_CLASS" = "$B" ] || die "kelas tidak cocok: @requires $B, run $UNIT_CLASS" ;;
          keep)     KEEP=1 ;;
        esac ;;
      DIR)  dir_exec "$A" "$B" ;;
      BLOCK)
        case "$A" in
          response) RESP=$B ;;
          if|while|for) run_block "$A" "$B" "$LN" ;;
          *) die "baris $LN: @$A belum dieksekusi (M3)" ;;
        esac ;;
      ENDB) RESP='' ;;
      OPEN) : ;;
      TEXT) [ "$COLLECT" -eq 1 ] && CBUF+="$A"$'\n' ;;
      CLOSE) flush_heredoc ;;
      STMT) stmt_exec "$A" "$B" "$C" "$LN" ;;
    esac
    [ "$RET_HIT" -eq 1 ] && break
  done
  [ "$COLLECT" -eq 0 ] || flush_heredoc   # heredoc tanpa penutup: EOF implisit
}

usage() { cat >&2 <<'U'
pemakaian: SATRIA=bin/satria bash tests/unit/lib/dsl.sh <file.test>
env: SATRIA UNIT_REPO UNIT_TMP UNIT_JOBS UNIT_CLASS(cold|snap) UNIT_DISCOVERY
U
  exit 2
}

[ $# -eq 1 ] || usage
FILE=$1
[ -f "$FILE" ] || die "berkas tidak ada: $FILE"
TESTNAME=$(basename "$FILE"); TESTNAME=${TESTNAME%.test}; TESTNAME=${TESTNAME%.t}

EVF="$UNIT_TMP/ev.$$.main"
bash "$PARSER" "$FILE" > "$EVF" || die "parse gagal: $FILE"

MODES=$(awk -F'\t' '$1=="HDR" && $3=="suite"{print $4; exit}' "$EVF")
[ -n "$MODES" ] || MODES=workspace
NMODE=$(printf '%s' "$MODES" | awk -F',' '{print NF}')

echo "== $TESTNAME"
for M in ${MODES//,/ }; do
  MODE=$M
  if [ "$NMODE" -gt 1 ]; then set_work "$TESTNAME.$M"; else set_work "$TESTNAME"; fi
  FIX='' RESP='' RET_VALUE='' RET_HIT=0 COLLECT=0
  execute_events < "$EVF"
  [ "$RET_HIT" -eq 0 ] || { [ "$RET_VALUE" != fail ] || FAILN=$((FAILN+1)); }
  [ "$KEEP" -eq 1 ] || true
done
rm -f "$EVF"

echo "lulus: $PASSN  gagal: $FAILN"
[ "$FAILN" -eq 0 ]

#!/bin/bash
# dsl.sh — eksekutor berkas `.test` (M0) di atas event dari scripts/lib/parser.sh.
#
# Menjalankan: work/fixture/file(write|append|touch|delete)/env/time.tick/
# time.mark/run.build/run.cmd, @response @guard @requires @include @set @get
# @find @keep @return, selektor project(p).source|binary dari sources.tsv,
# gate work.clean pada [snap], verdict PASS/FAIL/PRECONDITION + ringkasan.
# Belum (M1+/M2/M3): ekspresi, run.exec|bg|stop|wait|tool, stage, text,
# file.copy, @if/@while/@for (parse lolos, eksekusi ditolak).
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

declare -A VAR OUT RC MTIME
PASSN=0 FAILN=0 KEEP=0 WORK='' MODE='workspace' FIX='' RESP=''
COLLECT=0 CBUF='' PEND=''
COLLECT=0 CBUF='' PEND=''
RET_VALUE='' RET_HIT=0
WORKS=()
mkdir -p "$UNIT_TMP"

die()     { echo "FAIL dsl: $*" >&2; exit 1; }
precond() { echo "PRECONDITION $*"; exit 0; }
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

resolve_args() { # interpolasi {var} + selektor project(p).source|binary
  local s; s=$(interp "$1")
  local re='project\(([A-Za-z0-9_]+)\)\.(source|binary)' p at r
  while [[ "$s" =~ $re ]]; do
    p=${BASH_REMATCH[1]} at=${BASH_REMATCH[2]} r=''
    if [ "$at" = source ]; then r=$(sel_source "$p") || precond "selektor project($p).source tak terpenuhi"
    else r=$(sel_binary "$p") || precond "selektor project($p).binary tak terpenuhi"; fi
    s=${s/"project($p).$at"/$r}
  done
  printf '%s' "$s"
}

set_work() {
  WORK="$UNIT_TMP/$1"; WORKS+=("$WORK")
  rm -rf "$WORK"; mkdir -p "$WORK"
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
  local label=$1; shift
  local outf="$WORK/.out.$label.txt"
  ( cd "$WORK" && env NO_COLOR=1 "$SATRIA" "$@" > "$outf" 2>&1 )
  OUT[$label]=$outf
  RC[$label]=$?
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
      args=$(unq "$args")
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
      [ -e "$WORK/$args" ] && ok "$klausa" || no "$klausa" "absen" "ada $args" ;;
    file.absent)
      [ ! -e "$WORK/$args" ] && ok "$klausa" || no "$klausa" "ada" "absen $args" ;;
    file.same)
      read -r -a AB <<<"$args"
      cmp -s "$WORK/${AB[0]}" "$WORK/${AB[1]}" && ok "$klausa" || no "$klausa" "berbeda" "identik" ;;
    file.contains)
      local p pat
      p=${args%% *}; pat=${args#* }
      [ "$p" != "$pat" ] || die "bentuk: file.contains <path> <pola>"
      grep -qE -- "$pat" "$WORK/$p" && ok "$klausa" || no "$klausa" "tidak ada" "$pat" ;;
    mtime.changed|mtime.unchanged)
      cur=$(stat -c %y "$WORK/${MTIME[$args]:-}" 2>/dev/null)
      [ -n "${MTIME[$args]:-}" ] || { no "$klausa" "rekaman $args tidak ada" "time.mark dulu"; return; }
      cur=$(stat -c %y "$WORK/${MTIME[$args]}")
      if [ "$aksi" = changed ]; then
        [ "$cur" != "${MTIME[$args]}" ] && ok "$klausa" || no "$klausa" "sama" "berubah"
      else
        [ "$cur" = "${MTIME[$args]}" ] && ok "$klausa" || no "$klausa" "berubah" "sama"
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
      RET_VALUE=$(unq "$(interp "$rest")"); RET_HIT=1 ;;
    guard)
      [[ "$rest" =~ ^exists\((.*)\)$ ]] || die "@guard M0 hanya exists(<path>): $rest"
      args=$(resolve_args "${BASH_REMATCH[1]}")
      [ -e "$WORK/$args" ] || precond "@guard: $rest" ;;
    set)
      args=$(interp "$rest")
      name=${args%% *}; val=$(unq "${args#* }")
      VAR[$name]=$val ;;
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
      t=$(unq "${args#* }")
      mkdir -p "$WORK/$(dirname "$p")"
      printf '%s\n' "$t" >> "$WORK/$p" ;;
    file.touch)  touch "$WORK/$args" ;;
    file.delete) rm -f "$WORK/$args" ;;
    env.set)     export "${args%%=*}=${args#*=}" ;;
    env.unset)   unset "${args%%=*}" 2>/dev/null || true ;;
    time.tick)   tick ;;
    time.mark)
      local p=${args%%" as "*} n=${args##*" as "}
      MTIME[$n]=$(stat -c %y "$WORK/$p") ;;
    run.build|run.cmd)
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
      else
        [ ${#A[@]} -gt 0 ] || die "run.cmd butuh argumen"
        do_run "$label" "${A[@]}"
      fi ;;
    rc.*|out.*|file.exists|file.absent|file.same|file.contains|mtime.*)
      assert_exec "$obj" "$aksi" "$args" "$klausa" ;;
    *)
      die "baris $ln: $obj.$aksi belum dieksekusi di M0" ;;
  esac
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
          *) die "baris $LN: @$A belum dieksekusi (M2)" ;;
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

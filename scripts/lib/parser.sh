#!/bin/bash
# parser.sh — parser berkas DSL `.test` (design/unit_test_dsl.md).
#
# Parse-only: tokenisasi per baris, mesin status heredoc, tumpukan blok,
# validasi objek/aksi, kesalahan bernomor baris. TIDAK mengeksekusi apa pun.
#
# TERISOLASI: modul ini TIDAK di-consume unit_dev.sh/unit.sh sampai M0 stabil;
# suite `tests/unit/common/*.sh` yang berjalan hari ini tak tersentuh.
# Uji konforman berdiri sendiri: bash tests/unit/parser/run.sh
#
# Pemakaian:
#   bash scripts/lib/parser.sh <file.test>     # jejak event TSV ke stdout
#   bash scripts/lib/parser.sh --check <f>     # sunyi; rc 0=sah, 1=salah parse
#   bash scripts/lib/parser.sh --suite <dir>   # --check tiap *.test di <dir>
#
# Event jejak (dipisah TAB):
#   HDR   <baris> <direktif> <argumen>          @suite/@fixture/@purpose/@requires/@keep
#   DIR   <baris> <direktif> <argumen>          @include/@guard/@return/@else/@set/@get/@find
#   BLOCK <baris> <direktif> <label/ekspresi>   @response/@if/@while/@for (buka blok)
#   ENDB  <baris> <direktif>                    @end
#   STMT  <baris> <objek> <aksi> <argumen>
#   OPEN  <baris> heredoc <marker>              mulai isi berkas
#   TEXT  <baris> <isi>                         baris isi (apa adanya)
#   CLOSE <baris> heredoc <marker>              baris penutup marker
#   CMT   <baris> <teks>

set -u

# ---- kosakata (design/unit_test_dsl.md) ------------------------------------
HDRS='suite fixture purpose requires keep'
SINGLES='include guard return set get find'
OBJS='work fixture file env time run stage text out rc mtime'
OBJ_AKSI_work='new clean'
OBJ_AKSI_fixture='copy'
OBJ_AKSI_file='write append touch delete copy exists absent same contains'
OBJ_AKSI_env='set unset'
OBJ_AKSI_time='tick mark'
OBJ_AKSI_run='build cmd exec bg stop wait tool'
OBJ_AKSI_stage='set log'
OBJ_AKSI_text='count aggregate'
OBJ_AKSI_out='contains not-contains matches equals'
OBJ_AKSI_rc='ok fail'
OBJ_AKSI_mtime='changed unchanged'

in_list() { case " $2 " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

# ---- status parse ----------------------------------------------------------
MODE=trace FAIL=0 BODY=0 HEREDOC='' HD_START=0 LINE=0
STK_N=() STK_L=() STK_E=()   # tumpukan blok: nama, baris mulai, sudah-@else

emitf() { # emitf <format-dengan-\t> [argumen...] — hanya pada mode jejak
  [ "$MODE" = trace ] && printf "$1\n" "${@:2}"
  return 0
}
err_at() { printf 'baris %s: %s\n' "$1" "$2" >&2; FAIL=1; }

push_block() { STK_N+=("$1"); STK_L+=("$LINE"); STK_E+=(0); }
drop_block() {
  local i=$(( ${#STK_N[@]} - 1 ))
  unset "STK_N[$i]" "STK_L[$i]" "STK_E[$i]"
}

# ---- aturan per baris ------------------------------------------------------

handle_directive() { # $1 = @kata, $2 = sisa argumen
  local d=$1 rest=$2 re i
  if in_list "${d#@}" "$HDRS"; then
      [ "$BODY" -eq 1 ] && { err_at "$LINE" "direktif header setelah badan"; return; }
      case "$d" in
        @suite|@fixture|@purpose)
          [ -n "$rest" ] || { err_at "$LINE" "$d memerlukan argumen"; return; } ;;
        @requires)
          case "$rest" in cold|snap) ;; *) err_at "$LINE" "bentuk: @requires cold|snap"; return ;; esac ;;
        @keep)
          [ -z "$rest" ] || { err_at "$LINE" "@keep tidak menerima argumen"; return; } ;;
      esac
      emitf 'HDR\t%s\t%s\t%s' "$LINE" "${d#@}" "$rest"
      return
  fi

  case "$d" in
    @end)
      [ "${#STK_N[@]}" -gt 0 ] || { err_at "$LINE" "@end tanpa blok terbuka"; return; }
      i=$(( ${#STK_N[@]} - 1 ))
      emitf 'ENDB\t%s\t%s' "$LINE" "${STK_N[$i]}"
      drop_block; BODY=1
      return ;;
    @else)
      [ "${#STK_N[@]}" -gt 0 ] || { err_at "$LINE" "@else di luar @if"; return; }
      i=$(( ${#STK_N[@]} - 1 ))
      [ "${STK_N[$i]}" = "if" ] || { err_at "$LINE" "@else di luar @if"; return; }
      [ "${STK_E[$i]}" -eq 0 ] || { err_at "$LINE" "@else kedua pada @if yang sama"; return; }
      STK_E[$i]=1; emitf 'DIR\t%s\t%s\t%s' "$LINE" "else" ""
      BODY=1
      return ;;
  esac

  if in_list "${d#@}" "$SINGLES"; then
      local -a F=()
      case "$d" in
        @include)
          [ -n "$rest" ] || { err_at "$LINE" "bentuk: @include <path> [as <nama>]"; return; }
          read -r -a F <<<"$rest"
          if [ "${#F[@]}" -eq 3 ]; then
            [ "${F[1]}" = "as" ] || { err_at "$LINE" "bentuk: @include <path> [as <nama>]"; return; }
          elif [ "${#F[@]}" -ne 1 ]; then
            err_at "$LINE" "bentuk: @include <path> [as <nama>]"; return
          fi ;;
        @guard)
          [ -n "$rest" ] || { err_at "$LINE" "bentuk: @guard <ekspresi>"; return; } ;;
        @return)
          : ;;  # ekspresi opsional — fleksibel: fragmen = nilai, badan = berhenti
        @set)
          read -r -a F <<<"$rest"
          [ "${#F[@]}" -ge 2 ] || { err_at "$LINE" "bentuk: @set <nama> <ekspresi>"; return; }
          [[ "${F[0]}" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] ||
            { err_at "$LINE" "nama variabel tidak sah: ${F[0]}"; return; } ;;
        @get)
          read -r -a F <<<"$rest"
          [ "${#F[@]}" -eq 1 ] || { err_at "$LINE" "bentuk: @get <nama>"; return; } ;;
        @find)
          read -r -a F <<<"$rest"
          [ "${#F[@]}" -ge 2 ] || { err_at "$LINE" "bentuk: @find <nama> <pola>"; return; }
          [[ "${F[0]}" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] ||
            { err_at "$LINE" "nama variabel tidak sah: ${F[0]}"; return; } ;;
      esac
      emitf 'DIR\t%s\t%s\t%s' "$LINE" "${d#@}" "$rest"; BODY=1
      return
  fi

  case "$d" in
    @response)
      local -a W=()
      read -r -a W <<<"$rest"
      [ "${#W[@]}" -le 1 ] || { err_at "$LINE" "bentuk: @response [label]"; return; }
      push_block response; emitf 'BLOCK\t%s\t%s\t%s' "$LINE" "response" "$rest"
      BODY=1; return ;;
    @if|@while)
      [ -n "$rest" ] || { err_at "$LINE" "bentuk: $d <ekspresi>"; return; }
      push_block "${d#@}"; emitf 'BLOCK\t%s\t%s\t%s' "$LINE" "${d#@}" "$rest"
      BODY=1; return ;;
    @for)
      re='^([A-Za-z_][A-Za-z0-9_]*)[[:space:]]+in[[:space:]]+(.+)$'
      [[ "$rest" =~ $re ]] || { err_at "$LINE" "bentuk: @for <var> in <rentang|koleksi>"; return; }
      push_block for; emitf 'BLOCK\t%s\t%s\t%s' "$LINE" "for" "$rest"
      BODY=1; return ;;
  esac

  err_at "$LINE" "direktif tidak dikenal: $d"
}

handle_statement() { # $1 = baris (tanpa indentasi)
  local l=$1 re obj aksi rest marker var list after
  re='^([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_-]*)([[:space:]]+(.*))?$'
  if ! [[ "$l" =~ $re ]]; then
    err_at "$LINE" "pernyataan tidak dipahami: $l"; return
  fi
  obj=${BASH_REMATCH[1]} aksi=${BASH_REMATCH[2]} rest=${BASH_REMATCH[4]:-}

  in_list "$obj" "$OBJS" || { err_at "$LINE" "objek tidak dikenal: $obj"; return; }
  var="OBJ_AKSI_$obj" list=${!var}
  in_list "$aksi" "$list" || { err_at "$LINE" "aksi tidak dikenal: $obj.$aksi"; return; }

  marker=''
  if [[ "$rest" == *'<<'* ]]; then
    re='<<([A-Za-z_][A-Za-z0-9]*)'
    [[ "$rest" =~ $re ]] || { err_at "$LINE" "marker heredoc tidak sah di: $rest"; return; }
    marker=${BASH_REMATCH[1]}
    after=${rest#*"<<"$marker}
    [[ "$after" == *'<<'* ]] && { err_at "$LINE" "hanya satu heredoc per pernyataan"; return; }
    [ "$obj.$aksi" = "file.write" ] || { err_at "$LINE" "heredoc hanya sah pada file.write"; return; }
  elif [ "$obj.$aksi" = "file.write" ]; then
    err_at "$LINE" "file.write memerlukan heredoc: file.write <path> <<T"; return
  fi

  emitf 'STMT\t%s\t%s\t%s\t%s' "$LINE" "$obj" "$aksi" "$rest"
  if [ -n "$marker" ]; then
    HEREDOC=$marker HD_START=$LINE
    emitf 'OPEN\t%s\theredoc\t%s' "$LINE" "$marker"
  fi
  BODY=1
}

parse_file() { # $1 = berkas, $2 = mode (trace|check) — mode tak dipakai, MODE global
  local file=$1 raw l d rest
  [ -f "$file" ] || { printf 'parser: berkas tidak ada: %s\n' "$file" >&2; return 2; }

  while IFS= read -r raw || [ -n "$raw" ]; do
    LINE=$(( LINE + 1 ))
    l=${raw%$'\r'}

    # heredoc aktif: isi diteruskan apa adanya (komentar/klausa TIDAK diproses)
    if [ -n "$HEREDOC" ]; then
      if [ "$l" = "$HEREDOC" ]; then
        emitf 'CLOSE\t%s\theredoc\t%s' "$LINE" "$HEREDOC"; HEREDOC=''
      else
        emitf 'TEXT\t%s\t%s' "$LINE" "$l"
      fi
      continue
    fi

    l="${l#"${l%%[![:space:]]*}"}"          # buang indentasi
    [ -z "$l" ] && continue
    case "$l" in '#'*) emitf 'CMT\t%s\t%s' "$LINE" "$l"; continue ;; esac
    case "$l" in '@'*) read -r d rest <<<"$l"; handle_directive "$d" "${rest:-}"; continue ;; esac
    handle_statement "$l"
  done < "$file"

  # penutup heredoc opsional: tanpa penutup, isi berakhir di EOF (sah)
  if [ -n "$HEREDOC" ]; then
    emitf 'CLOSE\t%s\theredoc\t%s' "$LINE" "$HEREDOC"
    HEREDOC=''
  fi
  if [ "${#STK_N[@]}" -gt 0 ]; then
    local i=$(( ${#STK_N[@]} - 1 ))
    err_at "${STK_L[$i]}" "blok @${STK_N[$i]} (mulai baris ini) tanpa @end"
  fi
  [ "$FAIL" -eq 0 ] && return 0
  return 1
}

# ---- antarmuka -------------------------------------------------------------

usage() {
  cat >&2 <<'USAGE'
pemakaian: bash scripts/lib/parser.sh <file.test>     # jejak event TSV
           bash scripts/lib/parser.sh --check <file>  # sunyi; rc 0=sah 1=salah
           bash scripts/lib/parser.sh --suite <dir>   # --check tiap *.test di dir
USAGE
  exit 2
}

main() {
  [ $# -ge 1 ] || usage
  case "$1" in
    --check)
      [ $# -eq 2 ] || usage
      MODE=check parse_file "$2"; exit $? ;;
    --suite)
      [ $# -eq 2 ] || usage
      local dir=$2 f name ok=0 no=0 msg
      [ -d "$dir" ] || { printf 'parser: direktori tidak ada: %s\n' "$dir" >&2; exit 2; }
      for f in "$dir"/*.test "$dir"/*.t; do
        [ -e "$f" ] || continue
        name=$(basename "$f")
        if msg=$(bash "$0" --check "$f" 2>&1); then
          printf 'OK    %s\n' "$name"; ok=$(( ok + 1 ))
        else
          printf 'GAGAL %s — %s\n' "$name" "$(printf '%s' "$msg" | head -n 1)"; no=$(( no + 1 ))
        fi
      done
      printf 'Ringkasan parser: lulus: %d gagal: %d\n' "$ok" "$no"
      [ "$no" -eq 0 ] || exit 1
      exit 0 ;;
    --help|-h) usage ;;
    *) MODE=trace parse_file "$1"; exit $? ;;
  esac
}

main "$@"

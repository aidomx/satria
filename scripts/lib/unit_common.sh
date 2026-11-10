#!/usr/bin/env bash
# Shared utilities untuk unit_dev.sh

# Waktu dalam milidetik
now_ms() {
  local t
  t="$(date +%s%3N 2> /dev/null)"
  case "$t" in *N* | '') t=$(($(date +%s) * 1000)) ;; esac
  echo "$t"
}

# Format waktu HH:MM:SS
fmt_hms() {
  printf '%02d:%02d:%02d' $(($1 / 3600)) $(($1 % 3600 / 60)) $(($1 % 60))
}

# Format waktu HH.MM.SS
fmt_dot() {
  printf '%02d.%02d.%02d' $(($1 / 3600)) $(($1 % 3600 / 60)) $(($1 % 60))
}

# Nama pendek dengan ellipsis
short_name() {
  local max="${NAMEW:-12}"
  if [ "${#1}" -le "$max" ]; then
    printf '%s' "$1"
  else
    printf '%s...' "${1:0:$((max - 3))}"
  fi
}

# Cek apakah ada run aktif
any_active() {
  local d n rp
  for d in "$STAGE"/targets/*/; do
    [ -d "$d" ] || continue
    n="$(basename "$d")"
    [ -n "$WTARGET" ] && [ "$n" != "$WTARGET" ] && continue
    rp="$(cat "$d/log/run.pid" 2> /dev/null)"
    [ -n "${rp:-}" ] && kill -0 "$rp" 2> /dev/null && return 0
  done
  return 1
}

# Epoch dari timestamp ISO
epoch_of() {
  [ -n "${1:-}" ] || {
    echo 0
    return 0
  }
  date -d "$1" +%s 2> /dev/null || echo 0
}

# Kill process tree
kill_children() {
  local kid
  for kid in $(ps -eo pid=,ppid= 2> /dev/null | awk -v P="$1" '$2==P {print $1}'); do
    kill_children "$kid"
    kill -9 "$kid" 2> /dev/null
  done
}

# Fingerprint source files
src_fingerprint() {
  (
    cd "$TESTS_DIR" && find common lib -type f | sort | xargs sha256sum
    sha256sum classes.conf
  ) 2> /dev/null
}

# Validasi state staging
state_valid() {
  [ -f "$STATE" ] || return 1
  grep -q "^SRC_FINGERPRINT=$FP_NOW\$" "$STATE" || return 1
  local f sha
  while read -r f sha; do
    [ -f "$STAGE/$f" ] || return 1
    [ "$(cd "$STAGE" && sha256sum "$f" | cut -c1-64)" = "$sha" ] || return 1
  done < <(grep -E '^  ' "$STATE")
  return 0
}

#!/usr/bin/env bash
# scripts/ui.sh — pustaka tampilan untuk unit_dev.sh
# Konsumen utama saat ini: TUI watcher (TTY saja). Jalur LOG runner tetap
# teks murni — warna hanya menghidupkan tampilan interaktif.
#
# NO_COLOR (standar no-color.org) atau TERM=dumb: warna dimatikan.

ui_init() {
  if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-}" != "dumb" ]; then
    C_RESET=$'\033[0m'
    C_CYAN=$'\033[0;36m'
    C_BLUE=$'\033[0;34m'
    C_GREEN=$'\033[0;32m'
    C_YELLOW=$'\033[0;33m'
    C_RED=$'\033[0;31m'
    C_DIM=$'\033[1;30m'
  else
    C_RESET="" C_CYAN="" C_BLUE="" C_GREEN="" C_YELLOW="" C_RED="" C_DIM=""
  fi
}

# Frame spinner — digabik pola aksara dari spinstr.
UI_SPINFRAMES="⣷⣯⣟⡿⢿⣻⣽⣾"
ui_spin() { # ui_spin <i> → satu frame
  local len=${#UI_SPINFRAMES}
  echo "${UI_SPINFRAMES:$(($1 % len)):1}"
}

# [####----] pola bar (render_bar), tanpa dependensi eksternal.
ui_bar() { # ui_bar <percent 0..100> [width]
  local p=${1:-0} w=${2:-24} filled empty i bar
  [ "$p" -gt 100 ] 2> /dev/null && p=100
  filled=$((p * w / 100))
  empty=$((w - filled))
  bar="${C_CYAN}[${C_RESET}${C_BLUE}"
  for ((i = 0; i < filled; i++)); do bar+="━"; done
  for ((i = 0; i < empty; i++)); do bar+="─"; done
  printf '%s' "${bar}${C_RESET}${C_CYAN}]${C_RESET}"
}

#!/usr/bin/env bash
set -euo pipefail

# Di dalam Debian proot, tidak perlu cek proot — kita sudah di dalamnya.
# Yang perlu dicek adalah apakah ptrace benar-benar bisa dipakai.
ptrace_check() {
  if strace -qq -e trace=none /bin/true 2> /dev/null; then
    echo "> ptrace OK: strace bisa melacak proses"
  else
    echo "> PERINGATAN: ptrace tidak berfungsi di dalam proot ini." >&2
    echo "  Penyebab umum:" >&2
    echo "   - proot versi lama (butuh >= 4.0.0 dengan dukungan nested ptrace)" >&2
    echo "   - jalankan proot dengan PROOT_NO_SECCOMP=1 dari Termux" >&2
    return 1
  fi
}

# @param $1 Binary target (path absolut atau nama di PATH)
# @param $2 Direktori kerja target
track_ws() {
  local sat="${1:-}"
  local target="${2:-}"

  if [[ -z "$sat" || -z "$target" ]]; then
    echo "Usage: track_ws <binary> <dir>" >&2
    return 1
  fi

  if [[ -x "$sat" ]]; then
    :
  elif command -v "$sat" > /dev/null 2>&1; then
    sat="$(command -v "$sat")"
  else
    echo "Binary: $sat tidak ditemukan atau tidak executable." >&2
    return 1
  fi

  if [[ ! -d "$target" ]]; then
    echo "Direktori: $target tidak ada." >&2
    return 1
  fi

  if ! command -v strace > /dev/null 2>&1; then
    echo "strace tidak terpasang. Jalankan: apt install strace" >&2
    return 1
  fi

  (
    cd "$target"
    strace -c -f \
      -e trace=openat,stat,newfstatat,read,close,getdents64 \
      "$sat" -w core \
      2> /tmp/strace.out > /dev/null || true
  )

  if [[ -s /tmp/strace.out ]]; then
    head -30 /tmp/strace.out
  else
    echo "strace tidak menghasilkan output. Cek /tmp/strace.out" >&2
    return 1
  fi
}

# Peringatan saja — jangan hentikan skrip karena ptrace_check return 1
ptrace_check || true
track_ws "${HOME}/satria/bin/satria" "${HOME}/satria/tests/miniws"

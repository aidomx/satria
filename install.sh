#!/bin/sh
# satria installation script
#
# This is a standard, safe build/install script for the satria C project.
# It compiles the source code and places the binary in the bin/ directory.
#
# Pemakaian:
#   curl -fsSL https://raw.githubusercontent.com/aidomx/satria/main/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/aidomx/satria/main/install.sh | sh -s -- --dev
#   curl -fsSL https://raw.githubusercontent.com/aidomx/satria/main/install.sh | sh -s -- --dev --version v0.2.0
#   curl -fsSL https://raw.githubusercontent.com/aidomx/satria/main/install.sh | sh -s -- --version v0.2.0
#   ./install.sh --dev  (jika dijalankan dari dalam source tree lokal)
#
# Mode default (curl) mengunduh binary satria dari GitHub Releases ke /usr/local/bin/satria.
# Mode --dev melakukan bootstrap build satria dari source dengan compiler C lokal.
#
# Sumber versi (urutan prioritas):
#   1. --version <tag>    (argumen eksplisit)
#   2. $SATRIA_VERSION      (env, di-set CI dari git tag, mis. v0.2.0)
#   3. .satria-version      (file di source tree; hanya untuk --dev)
#   4. fallback hardcoded

set -eu

GITHUB_USER="aidomx"
REPO_NAME="satria"
BRANCH="main"
VERSION="v0.2.0" # fallback terakhir; ditimpa oleh --version / SATRIA_VERSION / .satria-version

# Nama asset di GitHub Releases (harus persis sama dengan yang
# di-upload oleh .github/workflows/release.yml).
ASSET_WINDOWS_X64="satria-windows-x64.exe"
ASSET_LINUX_X64="satria-linux-x64"
ASSET_LINUX_ARM64="satria-linux-arm64"
ASSET_MACOS_ARM64="satria-macos-arm64"
ASSET_MACOS_X64="satria-macos-x64"

# ---------------------------------------------------------------------------
# Parsing argumen
# ---------------------------------------------------------------------------
DEV=0
VERSION_TAG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dev)
      DEV=1
      shift
      ;;
    --version)
      if [ $# -lt 2 ]; then
        echo "satria: --version memerlukan argumen tag (mis. --version v0.2.0)" >&2
        exit 1
      fi
      VERSION_TAG="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

# ---------------------------------------------------------------------------
# Lokasi instalasi default. Termux memakai $PREFIX/bin.
# ---------------------------------------------------------------------------
case "$(uname -s 2> /dev/null || echo unknown)" in
  Darwin)
    INSTALL_PATH="/usr/local/bin/satria"
    ;;
  Linux)
    if [ -n "${PREFIX:-}" ] && [ -d "${PREFIX}" ]; then
      INSTALL_PATH="${PREFIX}/bin/satria"
    else
      INSTALL_PATH="/usr/local/bin/satria"
    fi
    ;;
  *)
    INSTALL_PATH="/usr/local/bin/satria"
    ;;
esac

# Prioritas: SATRIA_VERSION (env) > .satria-version (file). VERSION_TAG sudah
# ditangani di pemanggil sebelum fungsi ini dipanggil.
read_version() {
  # Prioritas 1: env SATRIA_VERSION (di-set CI dari git tag v*).
  if [ -n "${SATRIA_VERSION:-}" ]; then
    VERSION="$SATRIA_VERSION"
    return
  fi

  # Prioritas 2: file .satria-version (untuk dev lokal).
  file="$1/.satria-version"
  if [ -f "$file" ]; then
    v=$(sed -n "1p" "$file")
    [ -n "$v" ] && VERSION="$v"
  fi
}

# ---------------------------------------------------------------------------
# Installer
# ---------------------------------------------------------------------------
install_binary() {
  binary="$1"
  chmod +x "$binary"
  if ! "$binary" version > /dev/null 2>&1; then
    echo "satria: binary tidak bisa dijalankan di sistem ini: ${binary}" >&2
    return 1
  fi
  echo "> Installing : ${INSTALL_PATH}"

  install_dir="$(dirname "${INSTALL_PATH}")"

  if [ ! -d "$install_dir" ]; then
    if command -v sudo > /dev/null 2>&1; then
      sudo mkdir -p "$install_dir"
    else
      mkdir -p "$install_dir" || {
        echo "satria: tidak bisa membuat ${install_dir}" >&2
        return 1
      }
    fi
  fi

  if [ -w "$install_dir" ]; then
    mv "$binary" "${INSTALL_PATH}"
  elif command -v sudo > /dev/null 2>&1; then
    sudo mv "$binary" "${INSTALL_PATH}"
  else
    echo "satria: tidak punya izin tulis ke ${install_dir} dan sudo tidak tersedia" >&2
    return 1
  fi

  echo "> Installed  : ${INSTALL_PATH}"
  if command -v "${INSTALL_PATH}" > /dev/null 2>&1; then
    "${INSTALL_PATH}" version 2> /dev/null || true
  fi
}

# ---------------------------------------------------------------------------
# Helper untuk mode default (GitHub Releases)
# ---------------------------------------------------------------------------
detect_asset_name() {
  os="$(uname -s 2> /dev/null || echo unknown)"
  arch="$(uname -m 2> /dev/null || echo unknown)"

  case "$os" in
    Linux)
      case "$arch" in
        x86_64 | amd64) echo "$ASSET_LINUX_X64" ;;
        aarch64 | arm64) echo "$ASSET_LINUX_ARM64" ;;
        *)
          echo "satria: arsitektur Linux tidak didukung: $arch" >&2
          return 1
          ;;
      esac
      ;;
    Darwin)
      case "$arch" in
        arm64) echo "$ASSET_MACOS_ARM64" ;;
        x86_64) echo "$ASSET_MACOS_X64" ;;
        *)
          echo "satria: arsitektur macOS tidak didukung: $arch" >&2
          return 1
          ;;
      esac
      ;;
    MINGW* | MSYS* | CYGWIN*)
      echo "$ASSET_WINDOWS_X64"
      ;;
    *)
      echo "satria: OS tidak didukung: $os" >&2
      return 1
      ;;
  esac
}

# Ambil JSON metadata release (latest atau tag tertentu).
fetch_release_json() {
  tag="$1"
  if [ -n "$tag" ]; then
    api="https://api.github.com/repos/${GITHUB_USER}/${REPO_NAME}/releases/tags/${tag}"
  else
    api="https://api.github.com/repos/${GITHUB_USER}/${REPO_NAME}/releases/latest"
  fi

  curl -fsSL \
    -H 'User-Agent: satria-installer' \
    -H 'Accept: application/vnd.github+json' \
    "$api"
}

# Parse field JSON satu baris dengan split per-koma.
# Contoh: json_field "$json" '"tag_name"'  →  v0.2.0
json_field() {
  json="$1"
  key="$2"
  printf '%s' "$json" \
    | tr ',' '\n' \
    | grep -F "$key" \
    | head -n1 \
    | sed -E 's/.*:"([^"]+)".*/\1/'
}

# ---------------------------------------------------------------------------
# Termux (Android): binary release dibangun dengan glibc dan tidak bisa
# dijalankan di bionic. Di Termux satria dibangun dari source tag release.
# ---------------------------------------------------------------------------
is_termux() {
  case "${PREFIX:-}" in
    *com.termux*) return 0 ;;
  esac
  [ "$(uname -o 2> /dev/null || true)" = "Android" ]
}

if is_termux; then
  if [ "$DEV" -eq 0 ]; then
    echo "> Termux    : binary release (glibc) tidak kompatibel, build dari source"
    DEV=1
  fi
  if [ -z "${VERSION_TAG}" ] && [ -n "${SATRIA_VERSION:-}" ]; then
    VERSION_TAG="${SATRIA_VERSION}"
  fi
  if [ -z "${VERSION_TAG}" ]; then
    RELEASE_JSON_T="$(fetch_release_json "")" || {
      echo "satria: gagal mengambil metadata release dari GitHub API" >&2
      exit 1
    }
    VERSION_TAG="$(json_field "${RELEASE_JSON_T}" '"tag_name"')"
    if [ -z "${VERSION_TAG}" ]; then
      echo "satria: gagal menentukan tag release terbaru" >&2
      exit 1
    fi
  fi
  if [ -z "${CC:-}" ] && command -v clang > /dev/null 2>&1; then
    CC=clang
  fi
fi

# ---------------------------------------------------------------------------
# Mode --dev: bootstrap build dari source (clean build, parallel + progress)
# ---------------------------------------------------------------------------
if [ "$DEV" -eq 1 ]; then
  TMP_DIR="$(mktemp -d)"
  cleanup() { rm -rf "$TMP_DIR"; }
  trap cleanup EXIT INT TERM
  SRC_DIR=""

  # Deteksi apakah dijalankan dari dalam source tree lokal
  SCRIPT_DIR="$(cd "$(dirname "$0")" > /dev/null 2>&1 && pwd)"
  if [ -f "${SCRIPT_DIR}/src/main.c" ] && [ -f "${SCRIPT_DIR}/include/satria.h" ]; then
    SRC_DIR="${SCRIPT_DIR}"
  else
    command -v curl > /dev/null 2>&1 || {
      echo "satria: --dev membutuhkan curl untuk mengunduh source" >&2
      exit 1
    }
    command -v tar > /dev/null 2>&1 || {
      echo "satria: --dev membutuhkan tar untuk mengekstrak source" >&2
      exit 1
    }

    if [ -n "${VERSION_TAG}" ]; then
      SOURCE_URL="https://github.com/${GITHUB_USER}/${REPO_NAME}/archive/refs/tags/${VERSION_TAG}.tar.gz"
    else
      SOURCE_URL="https://github.com/${GITHUB_USER}/${REPO_NAME}/archive/refs/heads/${BRANCH}.tar.gz"
    fi
    echo "> Downloading source: ${SOURCE_URL}"
    curl -fsSL "${SOURCE_URL}" -o "${TMP_DIR}/satria.tar.gz"
    tar -xzf "${TMP_DIR}/satria.tar.gz" -C "${TMP_DIR}"
    SRC_DIR=$(find "${TMP_DIR}" -mindepth 1 -maxdepth 1 -type d | head -n 1)
    if [ -z "${SRC_DIR}" ] || [ ! -f "${SRC_DIR}/src/main.c" ]; then
      echo "satria: source tree tidak valid" >&2
      exit 1
    fi
  fi

  # Versi
  if [ -n "${VERSION_TAG}" ]; then
    VERSION="${VERSION_TAG}"
  else
    read_version "${SRC_DIR}"
  fi

  command -v "${CC:-cc}" > /dev/null 2>&1 || {
    echo "satria: compiler C '${CC:-cc}' tidak ditemukan; pasang gcc/clang terlebih dahulu (Termux: pkg install clang)" >&2
    exit 1
  }

  # Deteksi jumlah core untuk paralelisasi
  JOBS="$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 2)"
  [ "$JOBS" -lt 1 ] && JOBS=2

  echo "> Bootstrap  : satria ${VERSION}"
  echo "> Compiler   : ${CC:-cc}"
  echo "> Parallel   : ${JOBS} jobs"

  OBJ_DIR="${TMP_DIR}/build/obj"
  mkdir -p "${OBJ_DIR}"

  # List sumber
  SOURCES=$(find "${SRC_DIR}/src" -name '*.c' -type f | sort)
  TOTAL=$(printf '%s\n' "$SOURCES" | sed '/^$/d' | wc -l | tr -d ' ')
  if [ "$TOTAL" -eq 0 ]; then
    echo "satria: tidak ada file sumber (.c) ditemukan di ${SRC_DIR}/src" >&2
    exit 1
  fi

  # Batch mkdir: buat semua direktori obj dalam satu panggilan
  SUBDIRS=$(printf '%s\n' "$SOURCES" | while read -r s; do
    rel="${s#${SRC_DIR}/src/}"
    printf '%s\n' "${OBJ_DIR}/$(dirname "$rel")"
  done | sort -u)
  # shellcheck disable=SC2086
  mkdir -p $SUBDIRS

  echo "> Compile    : ${TOTAL} file(s)"

  # Buat script helper untuk compile satu file
  COMPILE_SCRIPT="${TMP_DIR}/compile.sh"
  cat > "$COMPILE_SCRIPT" << EOF
#!/bin/sh
src="\$1"
obj="${OBJ_DIR}/\${src#${SRC_DIR}/src/}"
obj="\${obj%.c}.o"
"${CC:-cc}" -std=gnu11 -O2 -Wall -Wextra \\
    -DSATRIA_VERSION_EMBEDDED=\\"${VERSION}\\" \\
    -I"${SRC_DIR}/include" -I"${SRC_DIR}/src" \\
    -c "\$src" -o "\$obj"
EOF
  chmod +x "$COMPILE_SCRIPT"

  # Progress bar function
  progress() {
    percent="$1"
    cols="${COLUMNS:-}"
    if [ -z "$cols" ] && command -v tput > /dev/null 2>&1; then
      cols=$(tput cols 2> /dev/null || true)
    fi
    case "$cols" in
      '' | *[!0-9]*) cols=24 ;;
    esac
    width=$((cols - 7))
    [ "$width" -lt 6 ] && width=6
    [ "$width" -gt 40 ] && width=40
    filled=$((percent * width / 100))
    empty=$((width - filled))
    bar=$(printf '%*s' "$filled" '' | tr ' ' '_')
    rest=$(printf '%*s' "$empty" '' | tr ' ' '.')
    if [ -t 1 ]; then
      green='\033[32m'
      dark='\033[90m'
      reset='\033[0m'
      printf '\r> %b%s%b%s%b %d%%' "$green" "$bar" "$dark" "$rest" "$reset" "$percent"
    else
      printf '\r> %s%s %d%%' "$bar" "$rest" "$percent"
    fi
  }

  # Helper: format ukuran file jadi human-readable (B/K/M)
  human_size() {
    bytes="$1"
    if [ "$bytes" -ge 1048576 ]; then
      awk "BEGIN {printf \"%.1fM\", $bytes/1048576}"
    elif [ "$bytes" -ge 1024 ]; then
      awk "BEGIN {printf \"%.1fK\", $bytes/1024}"
    else
      printf '%dB' "$bytes"
    fi
  }

  # Parallel compilation via xargs dengan progress monitoring
  (printf '%s\n' "$SOURCES" | xargs -P "$JOBS" -I {} "$COMPILE_SCRIPT" {}) &
  XARGS_PID=$!

  # Monitor progress berdasarkan jumlah file .o yang sudah dibuat
  LAST_DONE=0
  while kill -0 "$XARGS_PID" 2> /dev/null; do
    CURRENT=$(find "${OBJ_DIR}" -name '*.o' -type f 2> /dev/null | wc -l | tr -d ' ')
    if [ "$CURRENT" -gt "$LAST_DONE" ]; then
      LAST_DONE=$CURRENT
      PERCENT=$((CURRENT * 100 / TOTAL))
      progress "$PERCENT"
    fi
    sleep 0.2
  done

  # Tunggu xargs selesai dan cek exit code
  wait "$XARGS_PID"
  EXIT_CODE=$?

  # Final progress 100%
  progress 100
  printf '\n'

  if [ "$EXIT_CODE" -ne 0 ]; then
    echo "satria: kompilasi gagal" >&2
    exit 1
  fi

  # Link
  mkdir -p "${SRC_DIR}/bin"
  "${CC:-cc}" $(find "${OBJ_DIR}" -name '*.o' -type f | sort) \
    -o "${SRC_DIR}/bin/satria" -pthread -lm
  chmod +x "${SRC_DIR}/bin/satria"

  # Dapatkan ukuran binary
  SIZE_BYTES=$(wc -c < "${SRC_DIR}/bin/satria" | tr -d ' ')
  SIZE_HUMAN=$(human_size "$SIZE_BYTES")
  echo "> Linking    : bin/satria (${SIZE_HUMAN})"

  # Pesan akhir sesuai konteks
  if [ "${SRC_DIR}" = "${SCRIPT_DIR}" ]; then
    # Local source tree
    echo "> Satria is installed, for show help run : bin/satria"
  else
    # Downloaded source — coba install ke system path, fallback ke ./bin/satria
    if install_binary "${SRC_DIR}/bin/satria"; then
      echo "> Satria is installed, for show help run : satria"
    else
      mkdir -p ./bin
      cp "${SRC_DIR}/bin/satria" ./bin/satria \
        && echo "> Satria is installed, for show help run : ./bin/satria"
    fi
  fi
  exit 0
fi

# ---------------------------------------------------------------------------
# Mode default: unduh binary dari GitHub Releases
# ---------------------------------------------------------------------------

command -v curl > /dev/null 2>&1 || {
  echo "satria: membutuhkan curl untuk mengunduh binary" >&2
  exit 1
}

# Di mode default, --version eksplisit menang; kalau tidak, SATRIA_VERSION dipakai;
# kalau tidak ada keduanya, pakai release "latest".
if [ -z "${VERSION_TAG}" ] && [ -n "${SATRIA_VERSION:-}" ]; then
  VERSION_TAG="${SATRIA_VERSION}"
fi

ASSET="$(detect_asset_name)" || exit 1

echo "> Platform   : ${ASSET}"
if [ -n "${VERSION_TAG}" ]; then
  echo "> Requesting : tag ${VERSION_TAG}"
else
  echo "> Requesting : latest release"
fi

RELEASE_JSON="$(fetch_release_json "${VERSION_TAG}")" || {
  echo "satria: gagal mengambil metadata release dari GitHub API" >&2
  exit 1
}

TAG_NAME="$(json_field "${RELEASE_JSON}" '"tag_name"')"
if [ -n "${TAG_NAME}" ]; then
  echo "> Release    : ${TAG_NAME}"
fi

# Ambil URL browser_download_url untuk asset yang cocok.
URL="$(printf '%s' "${RELEASE_JSON}" \
  | tr ',' '\n' \
  | grep -F 'https://' \
  | grep -F "/${ASSET}\"" \
  | head -n1 \
  | sed -E 's/.*"(https[^"]+)".*/\1/')"

if [ -z "${URL}" ]; then
  echo "satria: asset '${ASSET}' tidak ada di release ${TAG_NAME:-latest}" >&2
  echo "satria: asset yang tersedia:" >&2
  printf '%s' "${RELEASE_JSON}" \
    | tr ',' '\n' \
    | grep -F '"name"' \
    | sed 's/^/  /' >&2
  exit 1
fi

TMP_FILE="$(mktemp)"
cleanup() { rm -f "${TMP_FILE}"; }
trap cleanup EXIT INT TERM

echo "> Downloading: ${URL}"
if ! curl -fsSL "${URL}" -o "${TMP_FILE}"; then
  echo "satria: gagal mengunduh binary dari ${URL}" >&2
  exit 1
fi

install_binary "${TMP_FILE}"
# Trap cleanup akan otomatis menghapus TMP_FILE saat skrip selesai

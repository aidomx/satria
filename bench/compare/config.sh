# bench/compare/config.sh  konfigurasi: opsi, pemeriksaan tool wajib, deteksi tool opsional, daftar tool.
# Dimuat oleh bench/compare.sh (cmp_source). Bukan skrip mandiri.

# ---------------------------- konfigurasi ----------------------------------
NSRC=150      # -n : jumlah source t1..tN.c
NHEAD=30      # -H : jumlah header berantai h1->hH
NRUN=5        # -r : pengulangan per skenario (ambil median)
SATRIA=bin/satria # -b : binary satria yang dipakai
AUDIT=1       # -A : disable untimed compile/link audit
KEEP=0        # -k : 1 = jangan hapus sys.tmp di akhir
ONLY=""       # -s : hanya skenario ini (comma-separated)
OUTDIR=""     # -o : direktori kerja benchmark (default sys.tmp di cwd)
NPROC=${NPROC:-$(nproc 2>/dev/null || echo 4)}
TARGETS=""

usage() { sed -n '2,/^# ====.*$/p' "$0"; exit 0; }
while getopts "n:H:r:b:t:ko:s:Ah" opt; do
  case $opt in
    n) NSRC=$OPTARG ;;
    H) NHEAD=$OPTARG ;;
    r) NRUN=$OPTARG ;;
    b) SATRIA=$OPTARG ;;
    t) TARGETS=$OPTARG ;;
    k) KEEP=1 ;;
    A) AUDIT=0 ;;
    o) OUTDIR=$OPTARG ;;
    s) ONLY=$OPTARG ;;
    h) usage ;;
    *) usage ;;
  esac
done

[ -x "$SATRIA" ] || { echo "satria tidak ditemukan/dieksekusi: $SATRIA" >&2; exit 1; }
SATRIA=$(cd "$(dirname "$SATRIA")" && pwd)/$(basename "$SATRIA")

for t in ninja make gcc; do
  command -v "$t" >/dev/null 2>&1 || { echo "tool wajib tidak ada: $t" >&2; exit 1; }
done

# Tool opsional
HAVE_BEAR=0;    command -v bear   >/dev/null 2>&1 && HAVE_BEAR=1
HAVE_STRACE=0;  command -v strace >/dev/null 2>&1 && HAVE_STRACE=1
HAVE_CMAKE=0;   command -v cmake  >/dev/null 2>&1 && HAVE_CMAKE=1
HAVE_MESON=0;   command -v meson  >/dev/null 2>&1 && HAVE_MESON=1
HAVE_XMAKE=0;   command -v xmake  >/dev/null 2>&1 && HAVE_XMAKE=1
HAVE_TUP=0;     command -v tup    >/dev/null 2>&1 && HAVE_TUP=1

# Daftar tool aktif (kunci internal); label tampilan lewat label_of()
TOOLS=(ninja make)
[ "$HAVE_BEAR"  = 1 ] && TOOLS+=(bear)
TOOLS+=(satria)
[ "$HAVE_CMAKE" = 1 ] && TOOLS+=(cmake)
[ "$HAVE_MESON" = 1 ] && TOOLS+=(meson)
[ "$HAVE_XMAKE" = 1 ] && TOOLS+=(xmake)
[ "$HAVE_TUP"   = 1 ] && TOOLS+=(tup)
CDB_TOOLS=()   # make & tup tidak punya compdb native (make => lihat bear)
# Filter tool benchmark jika -t diberikan.
# Urutan TOOLS mengikuti urutan yang diminta user.
if [ -n "$TARGETS" ]; then
  OLD_TOOLS=("${TOOLS[@]}")
  TOOLS=()
  IFS=',' read -r -a REQUESTED_TOOLS <<< "$TARGETS"
  for req in "${REQUESTED_TOOLS[@]}"; do
    found=0
    for k in "${OLD_TOOLS[@]}"; do
      if [ "$k" = "$req" ]; then
        TOOLS+=("$k")
        found=1
        break
      fi
    done
    [ "$found" = 1 ] || { echo "target tool tidak tersedia: $req" >&2; exit 1; }
  done
  [ "${#TOOLS[@]}" -gt 0 ] || { echo "target tool kosong" >&2; exit 1; }
fi

CDB_TOOLS=()   # make & tup tidak punya compdb native (make => lihat bear)
for k in "${TOOLS[@]}"; do
  case $k in make|tup) ;; *) CDB_TOOLS+=("$k") ;; esac
done

label_of() {
  case $1 in
    ninja) echo ninja ;;       make)  echo make ;;
    floor) echo gcc-floor ;;
    bear)  echo bear-make ;;   satria)  echo satria ;;
    cmake) echo cmake-ninja ;; meson) echo meson-ninja ;;
    xmake) echo xmake ;;       tup)   echo tup ;;
  esac
}

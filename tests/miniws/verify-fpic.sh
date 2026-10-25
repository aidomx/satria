#!/usr/bin/env bash
# ============================================================================
# verify-fpic.sh — verifikasi cepat fix fPIC + workspace dua fase, tanpa
# build penuh 176 source (build penuh tinggal jalankan ./run-errorlog.sh).
#
# Yang diuji:
#   1. Dua fase workspace: proyek core & bade SALING memakai library
#      (corelib.linux = bade, badelib.linux = core) tetap bisa dibangun
#      dari cold: fase library pass mengemas librupa.a/.so dan libbade.a/.so
#      tanpa link binary, lalu binary pass link keduanya.
#   2. embed codegen + fPIC: proyek mini dengan libraryShared=true HARUS
#      mengkompilasi embedded.c (codegen — design/embedded.md, jalur ld
#      sudah dihapus) dengan -fPIC.
#
# Pemakaian: ./verify-fpic.sh            (butuh bin/satria terbaru)
#            SATRIA=/path/satria ./verify-fpic.sh
# ============================================================================
set -u
cd "$(dirname "$0")"

SATRIA=${SATRIA:-../../bin/satria}
[ -x "$SATRIA" ] || {
  echo "satria tidak ditemukan: $SATRIA" >&2
  exit 1
}
SATRIA=$(cd "$(dirname "$SATRIA")" && pwd)/$(basename "$SATRIA")

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# ---------- Uji 1: sintesis workspace tanpa dependensi melingkar ----------
echo "== Uji 1: sintesis workspace =="
mkdir -p "$TMP/ws/core/src" "$TMP/ws/bade/src" "$TMP/ws/rupamod/stdlib"
cat > "$TMP/ws/Buildfile.ws" << 'EOF'
use workspace
projects.rupamod as mod
projects = rupamod, core, bade
compiler = gcc
flags = Wall
std = gnu11
mod.output.binary = false
mod.archive.src = .
mod.archive.name = rupa_modules
mod.archive.with = tar, gz
mod.archive.dir = modules
core.sources = src
core.output as coreout
core.library as corelib
coreout.binaryName = rupa
coreout.libraryName = rupa
coreout.libDir = lib
coreout.libraryShared = true
bade.sources = src
bade.output as badeout
bade.library as badelib
badeout.binaryName = bade
badeout.libraryName = bade
badeout.libDir = lib
badeout.libraryShared = true
bade.depends_on = rupamod
bade.embedded.modules as bademod
bademod.file = modules/rupa_modules.tar.gz
bademod.variable = MODULES
bademod.archive.pattern = .rp
bademod.extract = /tmp/rupa-system
# saling terikat: core <-> bade — menguji dua fase workspace
corelib.linux = bade
badelib.linux = core
EOF
printf 'module m1\n' > "$TMP/ws/rupamod/stdlib/m1.rp"
printf 'int main(void){return 0;}\n' > "$TMP/ws/core/src/main.c"
printf 'int main(void){return 0;}\n' > "$TMP/ws/bade/src/main.c"

if (cd "$TMP/ws" && "$SATRIA" -w > /dev/null 2> "$TMP/ws.err"); then
  # Sintesis HARUS memuat path library lintas-proyek (saling terikat)
  if grep -q 'lib=lib/libbade\.a' "$TMP/ws/.satria/caches/lib.cache" \
    && grep -q 'lib=lib/librupa\.a' "$TMP/ws/.satria/caches/lib.cache"; then
    echo "  ✓ library lintas-proyek tersintesis dua arah (core <-> bade)"
  else
    echo "  ✗ mapping library lintas-proyek tidak muncul di sintesis"
    fail=1
  fi
  # Artefak fase library & binary
  for f in lib/librupa.a lib/librupa.so bin/rupa \
    lib/libbade.a lib/libbade.so bin/bade \
    modules/rupa_modules.tar.gz; do
    [ -f "$TMP/ws/$f" ] || {
      echo "  ✗ artefak hilang: $f"
      fail=1
    }
  done
  [ "$fail" = 0 ] && echo "  ✓ cold build dua fase lulus (library pass dulu, binary pass belakangan)"
else
  echo "  ✗ cold build mini-workspace (saling terikat) gagal:"
  sed 's/^/      /' "$TMP/ws.err" 2> /dev/null | tail -15
  fail=1
fi

# Uji 1b: build selektif -w core harus menarik bade (library core) DAN
# rupamod (depends_on bade — arsip embed) lalu sukses dari cold.
if [ "$fail" = 0 ]; then
  (cd "$TMP/ws" && rm -rf core/build core/bin core/lib core/.satria \
    bade/build bade/bin bade/lib bade/.satria \
    rupamod/dist rupamod/.satria)
  if (cd "$TMP/ws" && "$SATRIA" -w core > /dev/null 2> "$TMP/sel.err"); then
    [ -f "$TMP/ws/bin/rupa" ] && [ -f "$TMP/ws/bin/bade" ] \
      && [ -f "$TMP/ws/modules/rupa_modules.tar.gz" ] \
      && echo "  ✓ -w core menarik bade + rupamod (transitif) dan sukses" \
      || {
        echo "  ✗ -w core sukses tapi artefak tidak lengkap"
        fail=1
      }
  else
    echo "  ✗ build selektif -w core gagal:"
    sed 's/^/      /' "$TMP/sel.err" 2> /dev/null | tail -10
    fail=1
  fi
fi

# ---------- Uji 2: embed codegen memakai -fPIC saat libraryShared ----------
echo "== Uji 2: embedded.c codegen -fPIC =="
mkdir -p "$TMP/emb/src" "$TMP/emb/stdlib"
cat > "$TMP/emb/Buildfile" << 'EOF'
use project
sources = src
std = gnu11
compiler = gcc
flags = Wall
output.binaryName = emb
output.libraryName = emb
output.libDir = lib
output.libraryShared = true
embedded.modules as m
m.src = stdlib
m.pattern = .rp
EOF
printf 'module x\n' > "$TMP/emb/stdlib/x.rp"
printf 'int main(void){return 0;}\n' > "$TMP/emb/src/main.c"

# Verifikasi langsung: shim gcc di PATH mencatat setiap invokasi, lalu
# kita cek baris kompilasi embedded.c (source hasil codegen — design/
# embedded.md) memuat -fPIC (ikut jalur source biasa karena
# libraryShared=true: compileLibraryOne/cmdsRunParallelJobs menambahkannya).
mkdir -p "$TMP/shim"
cat > "$TMP/shim/gcc" << SHIM
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"$TMP/gcc-calls.log"
exec /usr/bin/gcc "\$@"
SHIM
chmod +x "$TMP/shim/gcc"

if (cd "$TMP/emb" && PATH="$TMP/shim:$PATH" "$SATRIA" > /dev/null 2> "$TMP/emb.err"); then
  empline=$(grep -F 'embedded.c' "$TMP/gcc-calls.log" 2> /dev/null | head -1)
  if [ -n "$empline" ]; then
    case "$empline" in
      *-fPIC*)
        echo "  ✓ embedded.c (codegen) dikompilasi dengan -fPIC (libraryShared=true)"
        [ -f "$TMP/emb/lib/libemb.so" ] && echo "  ✓ libemb.so terbentuk dari object embed" \
          || {
            echo "  ✗ libemb.so tidak terbentuk"
            fail=1
          }
        ;;
      *)
        echo "  ✗ embedded.c TIDAK memakai -fPIC:"
        printf '      gcc %s\n' "$empline"
        fail=1
        ;;
    esac
  else
    echo "  ✗ kompilasi embedded.c tidak tercatat (codegen tidak berjalan?)"
    sed 's/^/      /' "$TMP/emb.err" 2> /dev/null | tail -10
    fail=1
  fi
else
  echo "  ✗ build proyek embed gagal:"
  sed 's/^/      /' "$TMP/emb.err" 2> /dev/null | tail -10
  fail=1
fi

# ---------- hasil ----------
echo
if [ "$fail" = 0 ]; then
  echo "SEMUA UJI LULUS"
else
  echo "ADA UJI GAGAL"
fi
exit $fail

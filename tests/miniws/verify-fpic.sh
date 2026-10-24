#!/usr/bin/env bash
# ============================================================================
# verify-fpic.sh — verifikasi cepat fix fPIC + workspace dua fase, tanpa
# build penuh 176 source (build penuh tinggal jalankan ./run-errorlog.sh).
#
# Yang diuji:
#   1. Dua fase workspace: proyek rupa & ruka SALING memakai library
#      (rupalib.linux = ruka, rukalib.linux = rupa) tetap bisa dibangun
#      dari cold: fase library pass mengemas librupa.a/.so dan libruka.a/.so
#      tanpa link binary, lalu binary pass link keduanya.
#   2. embed fPIC fallback: proyek mini dengan libraryShared=true tanpa GNU
#      ld (RBOT_NO_LD=1) harus mengkompilasi objek embed dengan -fPIC.
#
# Pemakaian: ./verify-fpic.sh            (butuh bin/rbot binary terbaru)
#            RBOT=/path/rbot ./verify-fpic.sh
# ============================================================================
set -u
cd "$(dirname "$0")"

RBOT=${RBOT:-../../bin/rbot}
[ -x "$RBOT" ] || { echo "rbot tidak ditemukan: $RBOT" >&2; exit 1; }
RBOT=$(cd "$(dirname "$RBOT")" && pwd)/$(basename "$RBOT")

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# ---------- Uji 1: sintesis workspace tanpa dependensi melingkar ----------
echo "== Uji 1: sintesis workspace =="
mkdir -p "$TMP/ws/rupa/src" "$TMP/ws/ruka/src" "$TMP/ws/rupamod/stdlib"
cat >"$TMP/ws/Buildfile.ws" <<'EOF'
use workspace
projects.rupamod as mod
projects = rupamod, rupa, ruka
compiler = gcc
flags = Wall
std = gnu11
mod.output.binary = false
mod.archive.src = stdlib
mod.archive.pattern = .rp
mod.archive.name = rupa_modules
mod.archive.with.tar = true
mod.archive.with.ext = gz
mod.archive.dir = dist
rupa.sources = src
rupa.output as rupaout
rupa.library as rupalib
rupaout.binaryName = rupa
rupaout.libraryName = rupa
rupaout.libDir = lib
rupaout.libraryShared = true
ruka.sources = src
ruka.output as rukaout
ruka.library as rukalib
rukaout.binaryName = ruka
rukaout.libraryName = ruka
rukaout.libDir = lib
rukaout.libraryShared = true
ruka.depends_on = rupamod
ruka.embedded.modules as rukamod
rukamod.file = ../rupamod/dist/rupa_modules.tar.gz
rukamod.variable = MODULES
# saling terikat: rupa <-> ruka — menguji dua fase workspace
rupalib.linux = ruka
rukalib.linux = rupa
EOF
printf 'module m1\n' >"$TMP/ws/rupamod/stdlib/m1.rp"
printf 'int main(void){return 0;}\n' >"$TMP/ws/rupa/src/main.c"
printf 'int main(void){return 0;}\n' >"$TMP/ws/ruka/src/main.c"

if (cd "$TMP/ws" && "$RBOT" -w >/dev/null 2>"$TMP/ws.err"); then
  # Sintesis HARUS memuat path library lintas-proyek (saling terikat)
  if grep -q 'library.linux = \.\./ruka/lib/libruka\.a' "$TMP/ws/.satria/workspace/rupa.Buildfile" && \
     grep -q 'library.linux = \.\./rupa/lib/librupa\.a' "$TMP/ws/.satria/workspace/ruka.Buildfile"; then
    echo "  ✓ library lintas-proyek tersintesis dua arah (rupa <-> ruka)"
  else
    echo "  ✗ mapping library lintas-proyek tidak muncul di sintesis"
    fail=1
  fi
  # Artefak fase library & binary
  for f in rupa/lib/librupa.a rupa/lib/librupa.so rupa/bin/rupa \
           ruka/lib/libruka.a ruka/lib/libruka.so ruka/bin/ruka \
           rupamod/dist/rupa_modules.tar.gz; do
    [ -f "$TMP/ws/$f" ] || { echo "  ✗ artefak hilang: $f"; fail=1; }
  done
  [ "$fail" = 0 ] && echo "  ✓ cold build dua fase lulus (library pass dulu, binary pass belakangan)"
else
  echo "  ✗ cold build mini-workspace (saling terikat) gagal:"
  sed 's/^/      /' "$TMP/ws.err" 2>/dev/null | tail -15
  fail=1
fi

# Uji 1b: build selektif -w rupa harus menarik ruka (library rupa) DAN
# rupamod (depends_on ruka — arsip embed) lalu sukses dari cold.
if [ "$fail" = 0 ]; then
  (cd "$TMP/ws" && rm -rf rupa/build rupa/bin rupa/lib rupa/.satria \
     ruka/build ruka/bin ruka/lib ruka/.satria \
     rupamod/dist rupamod/.satria)
  if (cd "$TMP/ws" && "$RBOT" -w rupa >/dev/null 2>"$TMP/sel.err"); then
    [ -f "$TMP/ws/rupa/bin/rupa" ] && [ -f "$TMP/ws/ruka/bin/ruka" ] && \
      [ -f "$TMP/ws/rupamod/dist/rupa_modules.tar.gz" ] && \
      echo "  ✓ -w rupa menarik ruka + rupamod (transitif) dan sukses" \
      || { echo "  ✗ -w rupa sukses tapi artefak tidak lengkap"; fail=1; }
  else
    echo "  ✗ build selektif -w rupa gagal:"
    sed 's/^/      /' "$TMP/sel.err" 2>/dev/null | tail -10
    fail=1
  fi
fi

# ---------- Uji 2: embed fallback memakai -fPIC saat libraryShared ----------
echo "== Uji 2: embed fallback -fPIC (RBOT_NO_LD=1) =="
mkdir -p "$TMP/emb/src" "$TMP/emb/stdlib"
cat >"$TMP/emb/Buildfile" <<'EOF'
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
printf 'module x\n' >"$TMP/emb/stdlib/x.rp"
printf 'int main(void){return 0;}\n' >"$TMP/emb/src/main.c"

# Verifikasi langsung: shim gcc di PATH mencatat setiap invokasi, lalu
# kita cek baris kompilasi modules.o.embed.c (jalur embedResourceCompile)
# memuat -fPIC (diteruskan via picWarningFlags karena libraryShared=true).
mkdir -p "$TMP/shim"
cat >"$TMP/shim/gcc" <<SHIM
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"$TMP/gcc-calls.log"
exec /usr/bin/gcc "\$@"
SHIM
chmod +x "$TMP/shim/gcc"

if (cd "$TMP/emb" && PATH="$TMP/shim:$PATH" RBOT_NO_LD=1 "$RBOT" >/dev/null 2>"$TMP/emb.err"); then
  empline=$(grep -F 'modules.o.embed.c' "$TMP/gcc-calls.log" 2>/dev/null | head -1)
  if [ -n "$empline" ]; then
    case "$empline" in
      *-fPIC*)
        echo "  ✓ objek embed dikompilasi dengan -fPIC (libraryShared=true)"
        [ -f "$TMP/emb/lib/libemb.so" ] && echo "  ✓ libemb.so terbentuk dari jalur fallback" \
          || { echo "  ✗ libemb.so tidak terbentuk"; fail=1; }
        ;;
      *)
        echo "  ✗ objek embed TIDAK memakai -fPIC:"
        printf '      gcc %s\n' "$empline"
        fail=1
        ;;
    esac
  else
    echo "  ✗ kompilasi objek embed tidak tercatat (embed fallback tidak berjalan?)"
    sed 's/^/      /' "$TMP/emb.err" 2>/dev/null | tail -10
    fail=1
  fi
else
  echo "  ✗ build fallback (RBOT_NO_LD=1) gagal:"
  sed 's/^/      /' "$TMP/emb.err" 2>/dev/null | tail -10
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

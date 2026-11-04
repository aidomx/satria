# bench/compare/tools.sh  versi tool (ninja, make, gcc, satria, ...) dan label tampilan.
# Dimuat oleh bench/compare.sh (cmp_source). Bukan skrip mandiri.

# ---------------------------- versi tool -----------------------------------
first_ver() { grep -oE '[0-9]+(\.[0-9]+)+' | head -1; }
satria_id() {
    local d c h v_bin
    d=$(dirname "$SATRIA")

    # 1. PRIORITAS: Cek apakah ini persis di Git Tag (Release Resmi)
    c=$(git -C "$d" describe --tags --exact-match 2>/dev/null)
    if [ -n "$c" ]; then
        c="release ${c}"
    else
        # 2. FALLBACK 1: Build Development (ada commit setelah tag, atau dirty)
        c=$(git -C "$d" describe --tags --always --dirty 2>/dev/null)
        if [ -n "$c" ]; then
            c="dev git:${c}"
        else
            # 3. FALLBACK 2: Binary sudah di-install (tidak ada folder .git)
            # Coba ambil versi dari output binary itu sendiri, atau dari strings
            v_bin=$($SATRIA version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
            if [ -z "$v_bin" ]; then
                v_bin=$(strings "$SATRIA" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
            fi
            c="release ${v_bin:-unknown}"
        fi
    fi

    # Hash unik untuk file binary (tetap berguna untuk melacak build yang sama)
    h=$(sha1sum "$SATRIA" 2>/dev/null | cut -c1-6)
    
    echo "${c} bin:${h:-n/a}"
}
declare -A VER
VER_GCC=$(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion 2>/dev/null)
VER[ninja]=$(ninja --version 2>&1 | first_ver)
VER[make]=$(make --version 2>&1 | first_ver)
VER[satria]=$(satria_id)
VER[gcc-floor]="gcc $VER_GCC"
[ "$HAVE_BEAR"  = 1 ] && VER[bear-make]="bear $(bear --version 2>&1 | first_ver) + make ${VER[make]}"
[ "$HAVE_CMAKE" = 1 ] && VER[cmake-ninja]="cmake $(cmake --version 2>&1 | first_ver) + ninja ${VER[ninja]}"
[ "$HAVE_MESON" = 1 ] && VER[meson-ninja]="meson $(meson --version 2>&1 | first_ver) + ninja ${VER[ninja]}"
[ "$HAVE_XMAKE" = 1 ] && VER[xmake]=$(xmake --version 2>&1 | first_ver)
[ "$HAVE_TUP"   = 1 ] && VER[tup]=$(tup --version 2>&1 | first_ver)

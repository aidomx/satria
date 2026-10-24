# ==============================================================================
# SATRIA BUILD CONFIGURATION (Single Project)
# ==============================================================================

# ─── MODE & ALIAS ─────────────────────────────────────────────────────────────
# Menandai file ini sebagai konfigurasi proyek tunggal (bukan workspace).
# Satria akan menerapkan konvensi default (src/, include/, bin/, build/).
use project

# Alias singkat untuk meningkatkan keterbacaan dan mengurangi pengetikan berulang.
clean as c
library as lib
output as o

# ─── ROOT & PROSES ────────────────────────────────────────────────────────────
# Direktori dasar untuk resolusi semua path relatif di file ini.
# '.' berarti direktori tempat Buildfile ini berada.
root = .

# Penanganan sinyal pembatalan (Ctrl+C / SIGINT).
# 'false' berarti Satria akan menangkap sinyal, membersihkannya dengan aman, 
# lalu meneruskannya ke proses child (compiler) agar tidak ada file state yang korup.
foreground = false

# ─── CLEAN CONFIGURATION ──────────────────────────────────────────────────────
# Mengontrol perilaku perintah 'satria clean'.
# false = aman (hanya menandai untuk dibersihkan nanti atau dry-run).
# true  = agresif (langsung menghapus direktori build dan compile_commands.json).
c.build = false
c.compdb = false

# ─── SUMBER & KOMPILASI ───────────────────────────────────────────────────────
# Direktori yang berisi file sumber (.c, .cpp) yang akan dikompilasi.
sources = src

# Flag compiler untuk warning, optimasi, dan pelacakan dependensi otomatis:
# - Wall, Wextra : Mengaktifkan peringatan compiler yang ketat.
# - O2           : Level optimasi standar untuk keseimbangan kecepatan dan ukuran.
# - MMD, MP      : (GNU/Clang) Menghasilkan file .d untuk pelacakan dependensi header 
#                  dan membuat target phony agar build tidak gagal jika header dihapus.
flags = Wall, Wextra, O2, MMD, MP

# Standar bahasa yang digunakan. 'gnu11' adalah default yang solid untuk C modern.
std = gnu11

# ─── HEADER & INCLUDE PATHS ───────────────────────────────────────────────────
# Direktori tambahan untuk pencarian header (#include "..."):
# - 'build' : Untuk header yang di-generate secara dinamis saat proses build (misal: version.h).
# - 'I.'    : Menambahkan direktori saat ini (.) ke include path.
headers = build, I.

# ─── TOOLCHAIN & PLATFORM ─────────────────────────────────────────────────────
# Daftar compiler yang diizinkan. Satria akan otomatis mendeteksi dan memilih 
# compiler yang tersedia dan kompatibel dengan sistem operasi saat ini.
# Urutan prioritas biasanya mengikuti urutan yang ditulis atau deteksi sistem.
compiler = cl, gcc, clang

# Dependensi library spesifik platform. 
# Satria akan otomatis menerjemahkannya ke flag linker yang benar 
# (misal: menambahkan '-l' di depan untuk GCC/Clang, atau '.lib' untuk MSVC).
lib.windows = shell32, kernel32, ws2_32

# ─── UI / UX CLI ──────────────────────────────────────────────────────────────
# Menampilkan progress bar visual selama proses kompilasi berlangsung.
progress.bar = true

# Kapan pesan error harus ditampilkan. 'always' memastikan error tidak tenggelam 
# di antara output lain, sangat berguna untuk debugging cepat.
progress.error = always

# ─── OUTPUT & ARTEFAK ─────────────────────────────────────────────────────────
# Konfigurasi penamaan dan lokasi file hasil build.
o.binaryName = satria          # Nama file eksekusi final
o.binaryDir = bin              # Lokasi penyimpanan binary final
o.buildDir = build             # Lokasi penyimpanan file objek (.o) dan dependensi (.d)

# Integrasi IDE: Otomatis menghasilkan 'compile_commands.json' di root proyek.
# Ini memungkinkan tools seperti clangd menyediakan code completion dan linting yang akurat.
o.compileCommands = auto

# ─── PACKAGING & RELEASE ──────────────────────────────────────────────────────
# Metadata dasar untuk paket distribusi.
release.name = satria
release.version = 0.2.4
# Pola nama file output. {name} dan {version} akan diganti secara otomatis.
release.output = dist/{name}-v{version}.tar.gz

# Pemetaan file untuk paket instalasi (format: <sumber_lokal>:<tujuan_rel_terhadap_prefix>).
# Ini memastikan binary masuk ke $PATH (<prefix>/bin/) dan aset masuk ke folder share.
release.files = bin/satria:bin/satria,
                README.md:share/satria/README.md,
                LICENSE:share/satria/LICENSE,
                src/cmd.txt:share/satria/cmd.txt

# Format paket yang akan dihasilkan dan algoritma checksum untuk verifikasi integritas.
release.format = deb
release.checksum = sha256

# Metadata khusus untuk paket Debian (.deb).
release.deb.install_prefix = /usr/local
release.deb.maintainer = "Aidomx <aidomxdev@gmail.com>"
release.deb.description = "A simple, fast, and declarative builder for you."

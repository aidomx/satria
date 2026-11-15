# Design: Compilation & CLI Behavior

Dokumen ini mendefinisikan perilaku Command Line Interface (CLI) dan alur kompilasi Satria. Prinsip utamanya adalah **Strict by Default**, **Zero Ambiguity**, dan **GCC-Floor Simplicity**.

## 1. Filosofi Inti
1. **No Config, No Compile**: Menjalankan `satria src/main.c` tanpa flag output atau konfigurasi adalah **ERROR**. Satria tidak menebak lokasi output.
2. **GCC-Floor Simplicity**: Menggantikan kompleksitas CMake/Make. Cukup tentukan source dan output, Satria yang mengurus sisanya.
3. **Auto-Detect Compiler**: Satria otomatis mendeteksi mesin compiler C yang tersedia di sistem (gcc, clang, tcc, cl, dll) tanpa konfigurasi manual.
4. **Content-Driven Mode**: Flag `-f` murni sebagai *pointer* ke file konfigurasi. Apakah file tersebut menjalankan mode *workspace* atau *single-project* ditentukan sepenuhnya oleh isi file tersebut.

---

## 2. Spesifikasi CLI & Mode Operasi

### A. Mode Ad-hoc Eksplisit (Tanpa File Config)
Kompilasi satu file dengan kontrol penuh atas path output.
```bash
# Hanya compile ke objek (.o)
satria src/bootstrap.c -o build/bootstrap.o

# Compile dan link langsung ke binary
satria src/bootstrap.c -o build/bootstrap.o -b bin/satria
```

### B. Mode Ad-hoc Konfigurasi (`-F`)
Kompilasi satu file tanpa ribet mengetik `-o` dan `-b`. Satria membaca file konfigurasi ringan, menerapkan `flags`, `headers`, dan aturan penempatan output (`o.buildDir`, `o.binaryDir`). **Me-skip** logika kompleks seperti `release`, `cdeps`, atau multi-project.
```bash
satria src/bootstrap.c -F Buildfile
```

### C. Mode Override Konfigurasi Build (`-f`)
Memberitahu Satria untuk **mengabaikan** file default (`Buildfile` / `Buildfile.ws`) dan menggunakan file konfigurasi yang ditentukan. Menjalankan **seluruh fase build** sesuai isi file.
```bash
# Menggunakan file konfigurasi alternatif (isi file yang menentukan workspace/single project)
satria -f Other-Buildfile
satria -f Buildfile.dev
```

---

## 3. Tabel Kebenaran (Behavior Truth Table)

| Perintah User | Respons Satria | Alasan |
| :--- | :--- | :--- |
| `satria src/main.c` | ❌ **ERROR** | Niat tidak eksplisit. Meminta `-o`/`-b` atau `-F`. |
| `satria src/main.c -o a.o -b bin/a` | ✅ **SUCCESS** | Ad-hoc murni. Mengabaikan semua file config. |
| `satria src/main.c -F Buildfile` | ✅ **SUCCESS** | Ad-hoc config. Baca `Buildfile`, ambil `o.*` & `flags`, skip `release`. |
| `satria -f Buildfile.ws` | ✅ **SUCCESS** | Full build. Baca `Buildfile.ws`, jalankan fase sesuai isi file. |
| `satria` | ✅ **SUCCESS** | Default behavior. Mencari `Buildfile` default di root. |

### 3.1 Keputusan implementasi (klarifikasi 15/11/2026, sematan §2)
1. **`-b` tanpa `-o` = ERROR.** Objek harus berbunyi jelas; contoh pesan:
   `satria: -b memerlukan -o (path objek eksplisit)`.
2. **`-F` = compile + link binary.** Objek → `o.buildDir/<basename>.o`,
   binary → `o.binaryDir/o.binaryName` (`o.binaryName` di quick.conf §4
   bukan atribut mati).
3. **Multi-sumber cermin gcc.** `-o` saat SATU sumber = path `.o` penuh;
   saat MULTI sumber = direktori objek (`<dir>/<basename>.o` per sumber),
   lalu semua di-link ke `-b`. Niat campur `-F` dengan `-o/-b` = ERROR.
4. **Nol efek samping.** Mode ad-hoc tidak menulis `.satria` apa pun
   (tidak ada state/cache/fingerprint); tidak bisa no-op. Kegagalan
   compiler diteruskan apa adanya (wrapper gcc-floor, perilaku identik).
5. **Flag CLI ad-hoc tidak ada.** Flag compiler (`-O2`, `-I`, dll.) hanya
   lewat konfigurasi `-F` (`flags`, `headers`, `defines`, `std`) — sesuai
   prinsip "didukung oleh konfigurasi", bukan pengetikan panjang di CLI.
6. **Alias bawaan `use project`/`use alias`.** `output as o`, `clean as c`,
   `library as lib` aktif otomatis — quick.conf §4 bekerja tanpa baris
   alias. Definisi alias user lain tetap bekerja normal (alias pertama
   menang; tiga bawaan tidak dapat ditimpa).
7. **`-jN` paralel di ad-hoc.** Opsi global `-j` berlaku juga: kompilasi
   memakai pola `cmdsRunParallelJobs` (`procStartCapture` per job — output
   compiler dicetak utuh per job selesai, anti-interleaving; kompaksi slot
   + `procStopAll` saat interupsi). Link selalu berjalan sekali di akhir.
   Default serial (tanpa `-j` atau `-j1`). Ukur self-host 94 sumber di
   proot: serial 175,9 s → `-j4` 50,9 s (3,46×) → `-j6` 37,5 s (4,69×);
   `-j8` datar (38,9 s) — plafon efektif proot ~5× gcc mentah.

---

## 4. Format File Konfigurasi

### File untuk `-F` (Lightweight Ad-hoc)
Minimalis. Hanya mengatur apa yang dibutuhkan untuk compile satu file.
```text
# quick.conf
use project c
root = .
flags = Wall, O2, MMD
headers = include
o.buildDir = build/obj
o.binaryDir = bin
o.binaryName = my_tool
```

### File untuk `-f` (Full Build Configuration)
Komprehensif. Bisa mendefinisikan workspace, release, packaging, dan dependensi.
```text
# Buildfile.dev
use project c
root = .
sources = src
flags = Wall, Wextra, O2, g
o.binaryName = satria_dev
o.binaryDir = bin
# ... (bisa termasuk release, cdeps, workspace, dll)
```

---

## 5. Alur Eksekusi (Execution Flow)

Mesin build Satria mengikuti alur ketat berikut:

1. **Parse CLI Arguments**: Kumpulkan semua flag (`-f`, `-F`, `-o`, `-b`, dll).
2. **Validasi Niat (Intent Validation)**:
   - Jika ada source file tapi **tidak ada** `-o`/`-b`, **tidak ada** `-F`, dan **tidak ada** `-f` → **Throw Error**.
3. **Resolusi Konfigurasi**:
   - Jika `-f` ada: Muat file tersebut. Jalankan *full build lifecycle*.
   - Jika `-F` ada: Muat file tersebut. Ambil properti `o.*`, `flags`, `headers`. Abaikan properti lain.
   - Jika `-o` / `-b` ada: Gunakan path tersebut secara langsung.
4. **Auto-Detect Compiler & Eksekusi**:
   - Satria otomatis mendeteksi compiler C yang tersedia di sistem (urutan: `cc`, `gcc`, `clang`, `tcc`, `cl`).
   - Generate perintah compile/link berdasarkan `flags` dan `headers`.
   - Compile source ke path output yang telah ditentukan.
5. **Output**: Tampilkan ringkasan build (atau error jika gagal).

---

## 6. Catatan Implementasi (Untuk Kode C)
- **Compiler Discovery**: Buat fungsi `find_compiler()` yang mengecek `PATH` secara berurutan.
- **Parser Config**: Parser harus aman mengabaikan (skip) blok konfigurasi yang tidak relevan saat mode `-F` aktif, tanpa menyebabkan error.
- **Error Message**: Pesan error "No Config, No Compile" harus informatif. Contoh: `Error: Output destination not specified. Use -o/-b for explicit paths, or -F to use a build configuration.`
- **Path Resolution**: Semua path relatif di file config harus diselesaikan (resolve) relatif terhadap `root = .` (direktori tempat file config berada).

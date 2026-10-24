# Embedded Assets & Archives

Dokumen ini merinci desain fitur *embedding* di Satria. Tujuannya adalah menyederhanakan cara developer menyisipkan file eksternal (arsip, gambar, teks, dll.) ke dalam binary, serta menjamin portabilitas lintas platform (termasuk MSVC di Windows) dan kemudahan penggunaan.

## Masalah Sebelumnya
Implementasi lama mengandalkan trik linker spesifik (seperti `ld -r -b binary` di GNU) yang rumit, tidak portabel (gagal di MSVC), dan menyulitkan developer untuk mengakses data yang di-embed di dalam kode C karena struktur datanya tidak terdefinisi dengan jelas.

## Solusi Desain: Code Generation & Single API
Alih-alih memaksa linker memproses file biner mentah, Satria akan **menghasilkan file C standar** (`build/embedded.c` dan `build/embedded.h`) yang berisi data tersebut sebagai array `static const`. File ini kemudian dikompilasi secara normal bersama source code proyek. 

Satria mengekspos satu fungsi tunggal (`getEmbed`) untuk mengakses semua data, sehingga fitur ini tidak terkunci hanya untuk arsip, tetapi bisa digunakan untuk aset apa pun.

## API & Pengalaman Pengguna (User Experience)

Penggunaan fitur embedded di kode C menjadi sangat bersih dan intuitif:

```c
// Satria otomatis menambahkan 'build' ke include path, 
// jadi cukup include seperti header biasa.
#include "build/embedded.h"

void test() {
    // Ambil data berdasarkan 'key' yang didefinisikan di Buildfile
    const Embedded *e = getEmbed("rupa");
    
    // Akses aman menggunakan pengecekan pointer/flag
    if (e && e->archive) {
        printf("Archive name: %s\n", e->archive->name);
        printf("Archive size: %zu bytes\n", e->archive->size);
    }

    // Contoh jika meng-embed aset lain (gambar, teks, dll.)
    if (e && e->asset) {
        printf("Image name: %s\n", e->asset->name);
    }
}
```

## Definisi Struct (`build/embedded.h`)

Untuk mencegah *undefined behavior* dan akses memori ilegal, Satria menghasilkan header dengan pola *Type Flag* (pointer null-check atau boolean) untuk menandai data apa yang tersedia.

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Struktur dasar untuk data mentah
typedef struct {
    const char* name;
    size_t size;
    const uint8_t* data;
} EmbedArchive;

typedef struct {
    const char* name;
    size_t size;
    const uint8_t* data;
} EmbedAsset;

// Struct utama yang dikembalikan oleh getEmbed
typedef struct {
    const char* key;          // Nama kunci (misal: "rupa")
    
    bool has_archive;
    const EmbedArchive* archive;
    
    bool has_asset;
    const EmbedAsset* asset;
    
    // Bisa ditambah tipe data lain di masa depan (text, json, dll.)
} Embedded;

// Fungsi tunggal untuk mengambil data berdasarkan key
const Embedded* getEmbed(const char* key);
```

## Pipeline Build di Satria

Satria menangani embedded dalam 4 langkah otomatis yang terintegrasi dengan pipeline build yang sudah ada:

1. **Fase Parse & Generate:** 
   Saat membaca `Buildfile`, jika ada blok `embedded`, Satria membaca file sumber (misal: `data.tar`) dan menulis `build/embedded.c` yang berisi array byte `static const uint8_t` dan implementasi fungsi `getEmbed()`.
   *Catatan: Langkah ini hanya dijalankan jika file sumber lebih baru dari `build/embedded.c`.*

2. **Fase Kompilasi:**
   `build/embedded.c` diperlakukan sama persis seperti `src/main.c`. Satria mengompilasinya menjadi `build/embedded.o` menggunakan compiler yang sama (GCC/Clang/MSVC). Ini menjamin 100% portabilitas.

3. **Fase Linking:**
   `build/embedded.o` di-link bersama object file lainnya menjadi binary final.

4. **Fase Akses:**
   Developer menggunakan `getEmbed("key")` di kode mereka.

## Konfigurasi Buildfile

Contoh penulisan di `Buildfile` atau `Buildfile.ws`:

```text
# Definisi embedded untuk proyek 'bade'
bade.embedded.modules as bademod

# File sumber yang akan di-embed
bademod.file = ../modules/rupa_modules.tar

# Key unik yang akan digunakan di fungsi getEmbed("rupa")
bademod.key = rupa

# Tipe data (menentukan field mana di struct Embedded yang akan diisi)
bademod.type = archive
```

## Keuntungan Desain Ini

1. **Portabilitas Mutlak:** Tidak ada ketergantungan pada fitur linker eksotis. MSVC, GCC, dan Clang semua bisa mengompilasi array `uint8_t` standar.
2. **Kemudahan Penggunaan (DX):** Developer tidak perlu berurusan dengan simbol linker aneh (seperti `_binary_data_tar_start`). Cukup panggil `getEmbed()`.
3. **Fleksibel:** Tidak terkunci hanya untuk archive. Bisa digunakan untuk gambar, konfigurasi JSON, atau teks dengan menambahkan field baru di struct `Embedded` dan tipe di `Buildfile`.
4. **Aman (Safe):** Penggunaan pointer `const EmbedArchive*` (yang akan di-set `NULL` jika tidak ada) mencegah developer mengakses data yang tidak diinisialisasi.
5. **Debugging Mudah:** Developer bisa membuka `build/embedded.c` untuk melihat persis bagaimana Satria menyimpan data. Tidak ada *black box*.

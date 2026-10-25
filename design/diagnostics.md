# Diagnostics: warna output compiler

Status: **rancangan** (belum diimplementasikan).

## Latar

Warning dan error compiler (gcc/clang) hanya berwarna bila stderr-nya terminal.
Satria menangkap stderr compiler lewat pipe (`log/build_log.c`), sehingga warna
hilang walaupun output akhirnya tampil di terminal. Kebijakan warna untuk prompt
satria sudah ada (`commands.c`: hanya saat stdout terminal; `NO_COLOR`,
`SATRIA_NO_COLOR`, dan `TERM=dumb` mematikan). Rancangan ini menyatukan keduanya
dengan satu key di Buildfile yang bisa berkembang.

## Key

```text
diagnostics.color = auto | always | never
```

Namespace `diagnostics.*` disediakan untuk pengembangan berikutnya (misalnya
`diagnostics.format`). Default: `auto`.

| Nilai | Perilaku |
|---|---|
| `auto` | Warna hanya jika output ke terminal dan tidak ada env pembatas. Perilaku ini sama dengan sekarang. |
| `always` | Warna selalu diminta ke compiler, termasuk saat output ke pipe, log, atau CI. |
| `never` | Tidak pernah berwarna. |

## Prioritas

1. `NO_COLOR`, `SATRIA_NO_COLOR`, atau `TERM=dumb` selalu mematikan warna
   (konvensi lingkungan, termasuk di atas `always`).
2. `diagnostics.color` dari Buildfile.
3. Deteksi terminal, hanya berlaku untuk `auto`.

Catatan: pada langkah 1, `always` tidak menang atas `NO_COLOR`. Ini perlu
dikonfirmasi (lihat pertanyaan terbuka).

## Mekanisme

- Flag `-fdiagnostics-color=always` (gcc dan clang) ditambahkan saat satria
  membentuk perintah compile dan menjalankannya. Flag ini **tidak** dimasukkan ke
  daftar `flags` yang di-hash, dan **tidak** muncul di `compile_commands.json`.
- Mengubah nilai `diagnostics.color` tidak memicu rebuild dan tidak mengubah
  compdb. Output yang berubah hanya warnanya.
- MSVC tidak memakai flag ini; nilai `always` pada MSVC tidak berpengaruh.
- Di workspace, key ditulis di `Buildfile.ws` sebagai setting bersama dan berlaku
  ke semua proyek.

## Dampak lain

- Key baru disimpan di config cache. Versi cache dinaikkan agar cache lama tidak
  dibaca dengan layout yang salah.
- Background (`background = true`) menulis ke `.satria/background.log`. Dengan
  `auto`, log tetap polos.

## Pengujian yang direncanakan

Dijalankan di pty supaya terminal sungguhan:

1. `auto` di terminal: warna muncul.
2. `auto` dengan output ke pipe atau log: polos.
3. `never` di terminal: polos.
4. `always` di pipe atau log: berwarna.
5. `NO_COLOR` di atas `always`: polos.
6. Mengubah nilai key: tidak ada kompilasi ulang, dan `compile_commands.json` tidak
   berubah.

## Pertanyaan terbuka

1. `always` ke `background.log` dan CI: disetujui sebagai perilaku eksplisit?
2. Di workspace, apakah override per proyek perlu, atau cukup satu nilai bersama?
3. Kenaikan versi config cache disetujui?
4. `NO_COLOR` menang atas `always` (langkah 1 di atas), atau `always` menang
   sebagai pilihan eksplisit pengguna?

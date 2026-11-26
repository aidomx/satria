# Maps

Tujuannya untuk mengurangi beban analisis kode secara liar, dan menjaga peta hubungan antar-domain dalam sistem Satria yang saling berhubungan.

## Contoh
Tidak perlu terlalu panjang, misalnya : `maps/touch.md`

```text
# Touch

## Domain flow

cache
  -> workspace
  -> runners

## Responsibilities

cache:
  Mendeteksi perubahan filesystem.

workspace:
  Menentukan dampak perubahan terhadap project.

runners:
  Menjalankan pekerjaan yang diperlukan.

## Related tests

- cache_noop
- crud_touch_delete
- ws_plan
```

Ini hanya ilustrasi, hubungan sebenarnya harus diambil dari source aktual.

## Pemeliharaan maps

Ada satu aturan yang perlu diterapkan:
* Setiap perubahan yang mengubah hubungan antar-domain wajib memperbarui maps terkait.
* Bukan setiap perubahan kode harus mengubah dokumentasi.

Contohnya:

| Perubahan                               | Perlu update maps? |
| --------------------------------------- | ------------------ |
| Memperbaiki kondisi dalam fungsi        | Tidak selalu       |
| Memindahkan tanggung jawab antar-domain | Ya                 |
| Mengubah urutan alur build              | Ya                 |
| Mengubah jalur invalidasi cache         | Ya                 |
| Mengubah nama variabel lokal            | Tidak              |

### Dampak pekerjaan

Tanpa maps:

```text
Masalah ditemukan
  -> baca banyak file
  -> cari pemanggil
  -> telusuri dependensi
  -> pahami hubungan
  -> mulai memperbaiki
```
Dengan maps:

```text
Masalah ditemukan
  -> identifikasi domain
  -> baca maps terkait
  -> verifikasi source yang relevan
  -> mulai memperbaiki
```

Source tetap menjadi kebenaran utama. Maps adalah indeks pengetahuan arsitektur, bukan pengganti source, dan investasi untuk mengurangi biaya pemahaman sistem pada setiap iterasi pengembangan.

## Daftar maps
- [plan.md](plan.md)
- [cold-build.md](cold-build.md)
- [edit-header.md](edit-header.md)
- [edit-source.md](edit-source.md)

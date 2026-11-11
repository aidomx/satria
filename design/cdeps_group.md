# Grup cdeps level atas

Status: diimplementasikan. Pemilik: Buildfile.ws (workspace).

## Latar belakang

`cdeps` per proyek menuliskan relasi dua arah dengan dua baris:

    a.cdeps = b
    b.cdeps = a

Dengan banyak proyek, relasi kelompok harus ditulis berulang dan mudah tidak konsisten.

## Sintaks

Satu baris di `Buildfile.ws` (key level atas, tanpa titik):

    cdeps = {a,b}, {b,c}, {a,c}

- Setiap grup `{...}` berarti **semua anggotanya saling bergantung** (dua arah).
- Jumlah anggota **tidak dibatasi**.
- Beberapa grup dipisah koma atau spasi.
- Relasi per proyek (`a.cdeps = b`) tetap didukung dan dapat dicampur dengan grup.

## Semantik

Setiap grup diekspansi ke relasi per proyek:

    cdeps = {a,b,c}   ==   a.cdeps = b, c
                           b.cdeps = a, c
                           c.cdeps = a, b

Relasi yang tumpang tindih digabung: tidak ada duplikat, dan relasi ke diri sendiri tidak dibuat.
Hasilnya diperlakukan sama dengan `cdeps` per proyek, sehingga jalur build lain tidak berubah.

## Validasi

Kesalahan dicetak dengan lokasi baris Buildfile.ws, dan parse gagal:

| Kondisi                                  | Pesan (potongan)                                  |
|------------------------------------------|---------------------------------------------------|
| Grup dengan kurang dari dua proyek unik  | `grup harus memuat minimal dua proyek berbeda`    |
| Nama proyek tidak ada di workspace       | `proyek '<nama>' tidak dikenal di workspace`      |
| Nama kosong, mis. `{a,,b}`               | `nama proyek kosong di dalam grup`                |
| Kurung `}` tidak ditutup                 | `kurung '}' penutup grup tidak ditemukan`         |
| Bentuk lama di level atas, `cdeps = a, b`| `grup harus berbentuk {a,b}`                      |

## Di luar cakupan

- Grup tidak memperluas closure build selektif. Ini sama dengan `cdeps` per proyek
  (lihat `ws_graph.c`); `--with-cdeps` tetap berlaku seperti sebelumnya.
- Grup satu arah belum didukung. Bila dibutuhkan, perlu sintaks tambahan.
- Alias proyek (`projects.<nama> as <alias>`) belum dipakai di dalam grup; nama yang
  ditulis harus nama proyek asli.

## Verifikasi

- `tests/unit/common/cdeps_group.sh`: kesetaraan `compile_commands.json` grup vs per proyek,
  penggabungan grup tumpang tindih, dan kelima kasus error di atas.

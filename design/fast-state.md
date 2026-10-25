# Fast-state: stamp sempit dan cache per proyek/workspace

Status: catatan desain + pengukuran. Belum diimplementasi (lihat bagian "Rencana").

## Tujuan

No-op `satria -w` harus membayar biaya yang sebanding dengan jumlah input yang
benar-benar dipakai build, bukan dengan isi folder. Target acuan: ninja di
workspace yang sama (65 stat), single project (102 stat).

## Pengukuran (bench/ws.sh, `-n 1 -r 1`, satria dihitung via LD_PRELOAD)

| Skenario | satria stat | satria opendir | ninja stat |
|---|---|---|---|
| no-op, workspace bench (awal) | 170 | 2 | 65 |
| no-op, sesudah buang `build` dari headers | 160 | 2 | 65 |
| no-op, sesudah kunci memo dinormalisasi | 153 | 2 | 65 |
| cold, workspace bench | 627 → 581 | 32 → 26 | 65 |
| no-op, single project (compare.sh, `headers = include`) | 102 | 0 | - |
| no-op, workspace besar lokal (3 proyek, 213 TU) | 1615 → 847 (memo) | 9 | 65 |

Jumlah path unik pada no-op bench: 110 dari 157 panggilan stat. Header
`library/` yang sama dicek dua kali (oleh `core` dan `bade`). Kunci memo
sekarang sudah dinormalisasi, tapi sisa 47 panggilan berasal dari jalur lain
(decide di `deps`, bukan lewat memo).

## Penyebab stamp lebar

`cmdsFastStateSave` (src/cmds/cmds_state.c) mencatat stamp dengan
`fastCollectTree(dir)` untuk setiap folder di `headers`, `headers.internal`,
dan sumber. Akibatnya:

1. Folder `include/` dan `library/` dicatat seluruh isinya, walaupun TU hanya
   meng-include sebagian.
2. `build` di `headers` menelusuri output (.o, .d, dan direktori build).
   Di workspace besar lokal ini 354 dari 547 stamp file `rupa`.
3. (Koreksi) `I.` bukan folder: `headerEntryDir` menerjemahkannya menjadi `.`, dan
   kode lama melewati `.`. Jadi `I.` tidak menelusuri root proyek; penyebab utamanya
   tetap entri `build`.

Pembatasan `build` dari `headers` sudah dipakai di generator bench dan terbukti
tidak mengubah artefak (sha256 identik pada workspace besar lokal).

## Prinsip desain yang diusulkan

Stamp dibagi dua jenis:

- **Stamp file (input yang dipakai):** source dan setiap header yang tercatat di
  closure dependensi tiap TU (dari .d, atau snapshot deps). Mtime + ukuran.
- **Stamp direktori (deteksi penambahan/penghapusan):** setiap folder yang
  pernah berisi header yang bisa resolve. Cukup mtime direktori.

Argumen kebenaran:

- Header yang tidak ada di closure tidak mempengaruhi TU mana pun, selama
  daftar include tidak berubah.
- Daftar include berubah hanya bila (a) source berubah (stamp source), atau
  (b) file baru muncul di folder yang lebih awal di jalur pencarian (mtime
  direktori itu berubah).
- Penghapusan header dari folder berubah mtime direktori.

Dengan ini `library/` dan `include/` tetap aman tanpa dicatat per file.

## Cache per proyek dan workspace

Yang sudah ada per proyek (cocok dengan single project): `build.state`,
`build.lib.state`, `build.fingerprint`, `deps.cache`, `compile_commands.*`,
`ld.cache`. Mode workspace kini memusatkan semuanya di root: config cache di
`.satria/startup.state`, fingerprint + fast state di
`.satria/caches/fingerprint.cache`, blob per proyek (`ws.state`, `src`) di
`.satria/caches/state.cache`, dan stamp fase library di
`.satria/caches/lib.cache` — tidak ada lagi `.satria/workspace.state`.

Catatan penting: gerbang no-op tidak mengurangi jumlah stat. Verifikasi
"tidak ada yang berubah" tetap perlu men-stat setiap input (ninja juga
begitu). Yang bisa dikurangi: overhead per proyek (membuka state, parse,
pindah cwd). Pada pengukuran, `open` sudah hanya 5 per run, jadi manfaatnya
kecil untuk syscall. Manfaat utama ada di jumlah stamp, bukan di file state.

## Memo stat per proses

Sudah diimplementasikan (src/cmds/cmds_state.c): hasil stat disimpan selama
tidak ada build di antaranya, kunci = path absolut ternormalisasi secara
leksikal. Di-reset di `cmdBuildEx` tepat sebelum build dimulai.

## Hash isi

Saat mtime berubah (mis. touch), satria membaca isi file dan membandingkan hash
dengan snapshot di `deps.cache`. Ini membuat `touch` benar tapi lebih mahal
(96 ms vs 50 ms ninja di single project). Usulan: cache hash per
(path, mtime, size) sehingga file yang tidak berubah tidak dibaca ulang.

## Rencana

Jalur workspace dipisah (`wsFastStateSave` di src/cmds/cmds_wsstate.c). Single
project tetap memakai `cmdsFastStateSave`/`cmdsFastStateValid` tanpa perubahan.

1. **Langkah 1, selesai: penulisan state workspace.**
   - `depsClosureVisit` (src/deps/deps_snapshot.c, API di src/deps.h).
   - `wsFastStateSave` menulis `.satria/ws.state` (fase binary) dan
     `.satria/ws.lib.state` (fase library) dari closure, dipanggil sebelum
     `depsFree` hanya saat `wsModeActive()`.
   - Verifikasi: closure cocok persis dengan header di file `.d` (core 2/2,
     bade 3/3, tanpa selisih). Benchmark `ninja,satria` tetap identik.
   - Belum dipakai untuk validasi.
2. **Langkah 2, selesai: `wsFastStateValid`** dengan file state sendiri, dipanggil
   dari `cmdBuildEx` hanya saat `wsModeActive()`.
   - Memeriksa F (mtime+ukuran), D (mtime), dan X (harus tetap tidak ada), memakai
     memo stat yang sama dengan single project.
   - Mencatat library, object dan archive embed, serta ld.cache; jalur library-only
     juga menyimpan state (sebelum `depsFree`).
   - Bug yang ditemukan dan diperbaiki: kunci memo memakai cwd, dan validator ws
     tidak menyetelnya, sehingga entri proyek lain terpakai (`cmdsStatMemoSetCwd`).
   - Hasil: no-op bench 153 -> 100 stat, opendir 19 -> 2. Benchmark identik dengan
     baseline. Terverifikasi: hapus object, hapus library, ubah Buildfile.ws, touch
     archive embed, tambah/hapus header, dan edit/revert/touch semuanya terdeteksi;
     no-op setelah tiap perubahan kembali ke jalur cached.
3. **Langkah 3, sebagian: jalur rebuild.** Di mode workspace, save single project
   (`cmdsFastStateSave`) dulu masih ikut dipanggil di empat titik, sehingga build
   bekerja dua kali (fastCollectTree + stat per stamp). Empat titik itu sekarang
   dilewati saat `wsModeActive()`.
   - Satu source diedit (bench): stat 392 -> 210, opendir 12 -> 6 (ninja: 77, 0).
   - No-op tetap 100 stat (ninja 65).
   - Benchmark identik dengan baseline; edit/revert/hapus library diuji di ws_int.
   Pengukuran di device (proot) menunjukkan: no-op sudah mendekati ninja, tetapi
   rebuild 2+ TU masih sekitar 1,6-1,8x lebih lambat. Selisih itu di jalur rebuild
   (decide, scan include, penulisan state), bukan di validasi no-op.
   Sisa yang belum dikerjakan: 210 stat per satu-edit masih lebih banyak dari ninja
   (77). Kandidat: scan include (resolveInclude) dan stat per stamp di decide.
4. (Dulu rencana 1, sekarang sudah termasuk langkah 1 di atas.)
   - Tambah API `depsClosureFiles(DepCache *dc, const char *src, List *out)`
     di src/deps (telusuri node, pola yang sama dengan `depNewestUnder`).
   - `cmdsFastStateSave` menerima closure, bukan `fastCollectTree` untuk
     folder header. Folder tetap dicatat sebagai stamp direktori.
   - Blocker: `DepCache` dibebaskan (`depsFree`) di commands.c:1151, tetapi
     `cmdsFastStateSave` juga dipanggil setelahnya (commands.c:1171). Harus
     dipastikan lifetime-nya sebelum memanggil API baru.
   - Ukur: stat no-op bench harus turun dan mendekati jumlah input. Tes
     benar: edit header yang ada di closure merebuild; header baru yang
     menimpa resolusi merebuild; touch tidak merebuild.
2. Hapus `build` dari `headers` di template workspace (sudah dilakukan di
   generator bench; workspace besar lokal belum diubah).
3. Pindahkan stat sisa di jalur decide (`src/deps/`) ke memo yang sama.
4. Cache hash isi per (path, mtime, size).
5. Ukur `touch` dan `cold` setelah 1–4 pada workspace, lalu bandingkan dengan
   single project.

## Batasan

- Closure dari .d hanya ada setelah build sukses. Stamp yang dibangun dari
  closure harus disimpan di save, bukan dihitung ulang saat validasi.
- Fallback pemindai #include (build pertama, MSVC) mungkin lebih lebar; itu
  aman tapi lebih mahal.
- Pengukuran syscall dari LD_PRELOAD hanya menghitung proses satria. Biaya di
  device (proot) belum diukur untuk perubahan ini.

## Pengukuran jalur rebuild: skenario `sel` (edit leaf + bade, lalu `satria -w core`)

Beban kerja sama untuk kedua tool: 8 proses (satria: 6 gcc + 2 ar; ninja: 8 perintah).

| | proses | stat | opendir |
|---|---|---|---|
| ninja | 8 | 85 | 0 |
| satria | 8 | 324 | 14 |

Rincian stat satria (324 total, 96 path unik):

- Penulisan state workspace (`wsFastStateSave`): 62 stat (diukur dengan build
  yang melewati penulisan: 324 -> 262).
- Sisa 262: path yang sama di-stat berulang lintas fase/proyek, karena jalur
  decide belum memakai memo. Contoh: `rupa_modules.tar` 8x, `ext.h` 8x, object 7x.
- `.rp` 40 stat: archive/embed rupamod ikut divalidasi walau yang dibangun core.
- Pencarian `gcc` di PATH berulang (tiap probe ~5x untuk 3 kandidat direktori).

Rincian opendir (14): `walkDir` di `commands.c:518-519` menelusuri folder sumber
setiap fase, dan `embed.c:177`/`438` menelusuri folder modul embed. Ninja tidak
melakukan ini karena daftar inputnya sudah tertulis di `build.ninja`.

Profil `SATRIA_PROFILE=1` (sandbox): decide 0.6 ms, fingerprint 0.2 ms, compile
110 ms, link 21 ms. Jadi selisih bukan di CPU decide; biayanya ada di syscall
(stat, opendir) yang mahal di proot.

## Langkah berikutnya (langkah 4, belum diimplementasi)

1. **Cache daftar sumber dengan stamp direktori.** Simpan hasil `walkDir` per
   folder di state workspace, dengan stamp D untuk setiap folder yang ditelusuri.
   Bila semua stamp cocok, pakai daftar tersimpan dan lewati `opendir`. Penambahan
   atau penghapusan source mengubah mtime folder, jadi invalidasi tetap tepat.
2. **Memo stat untuk jalur decide** (`src/deps/`), dengan reset setelah setiap
   compile/link/ar yang mengubah file.
3. **Cache hasil PATH untuk compiler** per proses.
4. **Penulisan state**: pakai ulang nilai stat yang sudah didapat saat decide,
   dan hanya stat ulang output yang baru dibangun.

Target yang realistis: stat sel dari 324 turun ke sekitar 150, opendir ke 0.

## Hasil langkah 1-4 (diuji)

| | stat | opendir | catatan |
|---|---|---|---|
| sel, sebelum | 324 | 14 | ninja: 85 / 0 |
| sel, langkah 1 (cache daftar sumber) | 333 | 3 | |
| sel, langkah 1+3 (cache probe PATH) | 317 | 3 | gcc di PATH: dari berulang ke sekali per proses |
| sel, langkah 1+3+4 (penulisan state) | **296** | **3** | |
| no-op bench, sebelum langkah 1 | 100 | 2 | |
| no-op bench, sesudah langkah 1-4 | 106 | 3 | regresi kecil (+6 stat, +1 opendir), belum ditelusuri |

Perubahan:

1. **Cache daftar sumber** (`.satria/sources.cache`, mode workspace): walk folder sumber
   dan folder embed disimpan bersama stamp mtime folder yang ditelusuri. Mtime
   dicatat sebelum folder dibaca. Dipakai di pengumpulan source build, scan
   `.cpp` di config, dan walk embed. Cache dikosongkan di awal setiap proyek.
2. **Memo stat untuk jalur decide**: belum dikerjakan. Jalur decide masih
   memakai stat langsung; sisa duplikasi terbesar ada di sini.
3. **Cache probe PATH** (POSIX, per proses): `probeAvailable` menyimpan hasil
   per nama exe. Setiap kandidat gcc sekarang di-stat sekali per proses.
4. **Penulisan state**: output (object, library, target, archive, ld.cache)
   dicatat sebagai 'O' dan di-stat langsung; input (source, header, folder)
   diambil dari memo. Memo dikosongkan setelah state ditulis.

Bug yang ditemukan dan diperbaiki selama langkah ini:

- Folder sumber tidak tercatat di `ws.state`, sehingga file .c baru tidak
  memicu kompilasi. Sekarang folder yang ditelusuri ikut dicatat sebagai D.
- Cache daftar sumber tidak dikosongkan antar proyek; record proyek sebelumnya
  terbawa dan ditulis ke `sources.cache` proyek berikutnya.
- Record folder yang tidak ada selalu dianggap tidak valid, sehingga setiap run
  membuka folder itu lagi (sekarang valid selama tetap tidak ada).

Uji yang lulus: bench `view.csv` identik dengan baseline; edit/revert header;
hapus object; hapus library (dibuat ulang); ubah `Buildfile.ws` (std); tambah
dan hapus source; no-op setelah tiap langkah cached; single project 102 stat.

Sisa yang belum dikerjakan: memo stat di jalur decide (langkah 2) untuk menurunkan
duplikasi stat di antara fase, dan penelusuran regresi no-op kecil.

## Hasil langkah 2 (memo stat di jalur decide)

Yang diubah:

- Node dependensi (source dan header dari graf deps) mtime-nya lewat memo.
  Hanya hasil positif yang dimemo; yang negatif di-stat langsung, karena file
  yang belum ada bisa dibuat build.
- Mtime source dan object di decide (mode workspace) lewat memo.
- Memo tidak lagi dikosongkan total sebelum setiap build di mode workspace.
  Sebagai gantinya, output yang ditulis build dibuang per entri saat state
  disimpan, dan folder `build/` dibuang seluruhnya. Bila build gagal, memo
  dikosongkan total di `wsExecOne`.
- Writer state men-stat langsung (bukan dari memo), sehingga file yang dibuat
  build tidak pernah ditulis dengan nilai pra-build.
- Mode single project: perilaku lama dipertahankan (reset total sebelum build).

Hasil sel:

| | stat | opendir |
|---|---|---|
| sebelum langkah 2 (langkah 1-4) | 296 | 3 |
| langkah 2 awal (reset per save) | 304 | 3 |
| langkah 2 (buang selektif + decide lewat memo) | **261** | **3** |
| ninja | 85 | 0 |

No-op: 107 stat (sebelumnya 106). Single project: 102 stat.

Uji yang lulus: bench view identik dengan baseline; edit/revert header; hapus
object; hapus library; hapus `build/embedded.h` (dibuat ulang); ubah `std` di
Buildfile.ws (dan revert); build gagal lalu diperbaiki (no-op cached setelahnya);
tambah dan hapus source; single project edit (compile 1), revert (cached), no-op 102.

Yang masih menyisakan selisih terhadap ninja: stat untuk folder yang tidak ada
(negatif, tidak dimemo), stat berulang lintas fase untuk `src/main.c` dan folder
header. Pengurangan lebih lanjut berarti mengubah model stamp (langkah 4 pada
catatan sebelumnya) atau memperbaiki jumlah fase yang dijalankan per sel.

## Hasil langkah 4 (penulisan state memakai ulang nilai)

Pra-pass di `wsFastStateSave`: output dan folder `build/` dibuang dari memo
sebelum writer membaca apa pun. Writer lalu men-stat output langsung, dan input
(source, header, folder) dari memo. Dengan urutan ini nilai memo untuk input
tidak pernah memuat hasil pra-build dari file yang dibuat build.

| | stat | opendir |
|---|---|---|
| sel, sebelum langkah 4 | 261 | 3 |
| **sel, langkah 4** | **223** | **3** |
| no-op bench | 107 | 3 |

Uji yang lulus (sama dengan langkah 2, diulang): edit/revert header, hapus object,
hapus library, hapus `build/embedded.h`, ubah `std` (dan revert), build rusak
lalu diperbaiki, tambah/hapus source, bench view identik, single project 102 stat
dan edit mengompilasi 1 file.

Rencana model stamp (closure per TU untuk header, bukan semua file folder)
sudah ada di `Rencana` poin 1 (state workspace memakai closure). Pengurangan
berikutnya ada di jalur no-op (107 vs 65 ninja): stat negatif, dan D untuk
folder sumber serta header yang masih dicek walau tidak dipakai TU mana pun.

## Model stamp: analisis no-op (97-98 stat) dan perbaikan kecil

Model stamp berbasis closure sudah ada di `ws.state` (langkah 1 Rencana).
Rincian 107 stat no-op sebelum perbaikan:

- 40 `.rp`: input embed archive rupamod. Ninja juga harus mengecek input ini.
- 7 probe PATH untuk gcc: sekali per proses (scan PATH lengkap).
- ~12 folder (D): dibutuhkan untuk mendeteksi file baru di folder sumber dan header.
- ~16 library dan target: sebagian duplikat (validasi dan pencetakan ukuran).
- Sisanya: source, header closure, object.

Perbaikan yang diterapkan: pencetakan ukuran library dan target di validasi
no-op memakai ukuran dari memo, bukan `fsFileSize` langsung. Hasil: 107 -> 97/98.

Yang dicoba dan dibatalkan: mtime object dan library di keputusan link/embed
dan di `cmdsLibVariantUpToDate` lewat memo. Itu membuat binary basi
(`runtime check gagal`), karena object sudah dikompilasi ulang dalam proses
yang sama. Memo hanya sah untuk keputusan sebelum compile. Dengan pembatalan
itu, sel kembali ke 223 stat.

Kesimpulan: sisa stat no-op hampir seluruhnya input dan output yang wajib
dicek dengan model stamp berbasis stat. Pengurangan lanjutan butuh perubahan
model (mis. mengurangi folder yang dicek), bukan optimasi memo.

Uji ulang yang lulus: bench (23 In accordance, 0 GAGAL), edit/revert header,
hapus object, hapus library, hapus embedded.h, ubah std, build rusak lalu
pulih, tambah/hapus source, single project (102 stat, edit 1 compile).


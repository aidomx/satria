# Background Build & Watch Mode

Dokumen ini merinci desain eksekusi build di latar belakang (*background*) dan mekanisme pemantauannya (*watch*) di Satria. Fitur ini memungkinkan developer memulai build proyek besar dan langsung menggunakan terminal untuk tugas lain, sambil tetap bisa memantau progresnya.

## 1. Konfigurasi
Fitur ini diaktifkan melalui deklarasi di `Buildfile` atau `Buildfile.ws`:
```text
background = true
```
Jika `true`, pemanggilan `satria` atau `satria -w` akan segera mengembalikan kontrol ke terminal (exit code `0` yang menandakan "berhasil memulai proses"), sementara proses build yang sebenarnya berjalan di latar belakang.

## 2. Mekanisme Inti (Core Implementation)
Ketika `background = true` terdeteksi, Satria melakukan langkah-langkah berikut secara otomatis:
1. **Fork/Spawn**: Satria membuat proses anak (*child process*) untuk menjalankan pipeline build yang sebenarnya.
2. **PID Tracking**: Proses anak segera menulis Process ID (PID)-nya ke file `.satria/build.pid` (atau `.satria/workspace.pid` untuk mode workspace).
3. **Redirection**: Output standar (`stdout`) dan error (`stderr`) dari proses anak dialihkan (*redirected*) ke file log: `.satria/background.log`.
4. **Parent Exit**: Proses induk (*parent*) segera keluar, mengembalikan prompt terminal ke user.

## 3. Antarmuka Pengguna (CLI)

### A. Memulai Build
```bash
satria        # Untuk single project
satria -w     # Untuk workspace
```
*Output:* Tidak ada. Terminal segera kembali ke prompt.

### B. Memantau Progress (Default Mode)
Untuk melihat status build yang sedang berjalan:
```bash
satria watch
# atau
satria -w watch
```
*Output:*
```text
> progress : 45% (Compiling: core/src/main.c)
[Tekan CTRL+C untuk detach dari watcher, build tetap berjalan di background]
```
**Perilaku saat Selesai:** 
Ketika proses background selesai, watcher akan otomatis mencetak **Build Summary** (tabel output standar Satria) ke terminal, lalu keluar.

### C. Memantau Progress (Full Stream Mode)
Jika user ingin melihat output mentah secara real-time (seperti build normal, tapi membaca dari log background):
```bash
satria watch --full
```
*Output:* Men-stream isi `.satria/background.log` secara real-time (mirip `tail -f`), diikuti oleh Build Summary saat selesai.

## 4. Aturan dan Penanganan Edge Cases

1. **Deteksi Build Ganda:** 
   Sebelum memulai build baru, Satria memeriksa keberadaan `.satria/build.pid`. Jika file ada **dan** PID tersebut masih aktif (divalidasi via `kill -0 $PID`), Satria menolak permintaan dengan pesan:
   > `Error: Background build is already running (PID: 12345). Use 'satria watch' to monitor.`

2. **Perintah `CTRL + C` saat `watch`:**
   Menekan `CTRL + C` saat mode `watch` **hanya menghentikan watcher** (*detach*), **TIDAK** membunuh proses build di background. Proses build akan tetap berjalan hingga selesai.

3. **Pembersihan Otomatis (Teardown):**
   Setelah proses background selesai (sukses atau gagal), Satria harus:
   - Menghapus file `.satria/build.pid`.
   - Menandai status akhir di `.satria/background.status` (berisi `success` atau `failed`).
   - Mempertahankan `.satria/background.log` untuk debugging (akan ditimpa pada build background berikutnya).

4. **Pemblokiran Perintah Destruktif:**
   Perintah yang memodifikasi state build (seperti `satria clean`) harus diblokir atau memberikan peringatan keras jika ada build background yang sedang berjalan, untuk mencegah korupsi file artefak.

## 5. Contoh Skenario Penggunaan

```bash
# 1. User memulai build dan langsung kembali bekerja
$ satria -w
$ 

# 2. User ingin mengecek apakah sudah selesai
$ satria -w watch
> progress : 80% (Linking: bin/bade)

# 3. Build selesai, watcher otomatis menampilkan hasil
✔ Selesai dalam 12.4s
[Build Summary Table muncul di sini]
$

# 4. Jika user terlanjur menekan CTRL+C saat watching
$ satria -w watch
> progress : 30%
^C
$ # User kembali ke prompt, tapi build tetap jalan di background
$ satria -w watch --full # User bisa attach kembali kapan saja
```

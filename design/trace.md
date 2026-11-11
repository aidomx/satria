# satria trace (design)

Tujuan: melihat file apa saja yang disentuh satria (dan child-nya) saat build,
dengan ringkasan yang sama seperti pipeline manual:

    LD_PRELOAD=<tracer> satria -w core
    sort log | uniq -c | sort -rn | head -30

## Bentuk

     satria trace [--raw] [--out FILE] [--top N] [--so FILE] [--] <args satria>

- Opsi trace harus di depan. Token pertama yang bukan opsi trace mulai argumen
  untuk satria (pola sama dengan `satria profile`: parsing opsi global berhenti).
- Default log: `.satria/trace/openat.log`, dikosongkan setiap run.
- Default top: 30.
- `--raw`: sesudah ringkasan, isi log diputar ke stdout (stream panggilan,
  seperti strace default; urutan kronologis antar proses).
- Bentuk:

## Tracer

Sumber tracer: `src/trace/tracer.src` (plain text, BUKAN `.c`, supaya
install.sh tidak mengompilasinya ke dalam satria). Dibuat menjadi
`src/trace/tracer_src.h` oleh `scripts/gen_tracer_src.sh`, berupa
`static const char kTracerSrc[]`. Setelah build, satria membawa sumber
tracernya sendiri dan tidak membaca berkas luar.

Hook (LD_PRELOAD, POSIX): open/open64/openat, fopen/fopen64,
stat/lstat/stat64/fstatat (+ __xstat varian lama), access/faccessat,
readlink/readlinkat, opendir. Setiap panggilan ditulis SATU baris gaya-strace:

    <op>(<args>) = <hasil>

Contoh:

    openat(AT_FDCWD, ".satria/opts.cache", O_RDONLY) = 5
    openat(AT_FDCWD, "build/x.h", O_RDONLY|O_CREAT|O_TRUNC, 0644) = 3
    stat(".satria/caches/fp.format") = 0
    openat("/tmp/x", O_RDONLY) = -1 ENOENT (No such file or directory)

- Bendera didekode (`O_RDONLY|O_CREAT|O_TRUNC`, mode `0644`, akses `R_OK|X_OK`);
  bit asing dicetak hex. dirfd `*at` ditulis `AT_FDCWD` atau angka fd.
- Hasil: `= <fd>` sukses (persis strace), `= ok` bila nilai balik bukan fd
  (fopen/opendir — modestring tetap ditulis), `= -1 ENAME (pesan strerror)`
  bila gagal.
- Deviasi dari strace sungguhan: stat hanya menampilkan path (tanpa dump
  struct), fopen/opendir mencetak `= ok`, dan tidak ada waktu per panggilan.
- Lokasi log: env `SATRIA_TRACE_LOG` (diset `satria trace`); fallback `/tmp/trace.log`.
- Log ditulis lewat syscall `openat` langsung di constructor, supaya tidak
  rekursi ke hook sendiri.

Build: saat `satria trace` berjalan, sumber ditulis ke
`.satria/trace/openat_trace.c` (hanya bila isinya berbeda) lalu dikompilasi
ke `.satria/trace/openat_trace.so` dengan `cc` (env `CC`). `.so` dibangun
ulang bila sumber berubah atau `.so` belum ada. `--so FILE` memakai tracer
prebuilt tanpa build.

Ubah tracer: edit `src/trace/tracer.src`, lalu jalankan
`sh scripts/gen_tracer_src.sh`. `tests/unit/common/trace_src_sync.sh` gagal bila
header belum digenerate ulang.

## Ringkasan

- Baris diparsing: op = teks sebelum `(`; path = antara pasangan kutip pertama;
  gagal = hasil ` = -1 ` setelah `)`. Key = `op<TAB>path` (flags/hasil dibuang).
  Diurutkan (qsort), lalu run-length: setara `sort | uniq -c`. Urutan teratas:
  hitungan turun, lalu path naik.
- Baris keempat mencetak total gagal + tally errno (gaya `strace -c` kolom errors).
- Total per op dihitung dari run yang sama: setelah diurutkan, key dengan op
  yang sama berdampingan (prefix `op<TAB>`), sehingga tidak perlu map; kelompok
  op dengan err>0 dicetak `(err N)`.
- `--raw` memutar isi log ke stdout sesudah ringkasan.
- Exit code = exit code child (satria). Ringkasan tetap dicetak walau child gagal.

## Batasan yang diterima

- Child (gcc, ld, ar) ikut di-trace karena mewarisi LD_PRELOAD. Ringkasan
  tidak difilter per proses. Akses compiler (probe PATH untuk `gcc`) ikut terhitung.
- Hanya POSIX (dlsym/RTLD_NEXT). Windows menolak dengan pesan jelas.
- Trace bersarang ditolak (env `SATRIA_TRACE_ACTIVE`).
- Hook hanya menangkap panggilan lewat PLT/symbol eksport. Akses yang dilakukan
  libc secara internal (mis. `fopen` memanggil `openat` di dalam libc) tidak
  terhitung terpisah bila lewat jalur internal glibc.

## Temuan dari contoh (fixture miniws)

No-op `satria -w core` menghasilkan ~90–230 akses. Yang dominan:
- `.satria/opts.cache` dan `.satria/sources.cache` dibuka/di-stat berulang
  (10–14 kali) dalam satu run.
- `access`/`stat` ke setiap folder di PATH untuk `gcc` (`/usr/bin/gcc`,
  `/usr/local/bin/gcc`, `~/.local/bin/gcc`, dst.) muncul di setiap build,
  termasuk build tanpa perubahan. Kandidat untuk di-cache.

## Tes

`tests/unit/common/trace_summary.sh`: total akses = `wc -l` log, dan hitungan teratas
= key `op+path` (dikstrak dari baris strace-gaya lalu `sort | uniq -c`), pada
log yang sama.
`tests/unit/common/trace_src_sync.sh`: `tracer_src.h` sama dengan hasil generate dari `tracer.src`.

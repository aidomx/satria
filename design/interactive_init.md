# Interactive satria init

`satria init` harusnya lebih sederhana dan interactive memang jalan yang cocok :

```bash
$ satria init -p
| Project name? [your project name or enter for random]
| Use workspace? [a, b, enter for cancel]
```

Setelah semua pertanyaan, scaffold menampilkan ringkasan jawaban dan meminta
konfirmasi sebelum menulis file. Bila stdin/stdout adalah terminal, scaffold
langsung dibangun dan dijalankan:

```bash
> Summary
  Project   : myapp
  Location  : myapp
  Mode      : single project
  Auto      : build + run
| Create? [Y/n]

> Build     : project
> Run       : bin/myapp
hello from myapp
```

Opsi:

- `--here` menaruh single-project langsung di direktori kerja (tanpa folder
  `<name>/`). Workspace tetap memakai folder proyek di dalam root ini.
- `--no-run` melewati build+run otomatis; perintah berikutnya dicetak seperti
  sebelumnya (`cd <name> && satria`, `satria`, atau `satria -w`).

Auto-run sengaja hanya aktif untuk sesi TTY. Pipe/CI tetap mendapat scaffold
tanpa build atau run yang tidak diminta. Untuk workspace dengan beberapa
proyek, setelah build pengguna memilih proyek yang dijalankan (Enter memilih
proyek utama, `n` melewati).

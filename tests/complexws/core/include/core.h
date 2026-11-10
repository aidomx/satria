#pragma once

#define CORE_VERSION "1.0.0"

/* Nilai tetap proyek core. bade_value() sengaja tidak dipanggil dari sini:
   simbol lintas shared library belum terselesaikan saat link di Windows.
   Relasi cdeps dua arah tetap dideklarasikan di Buildfile.ws. */
int core_value(void);
void thisCore(void);

/* Store catatan sederhana: satu catatan per baris di <dir>/notes.txt.
   Semua fungsi mengembalikan 0 saat sukses dan negatif saat gagal. */
int notes_add(const char *dir, const char *text);
int notes_clear(const char *dir);

/* notes_list mencetak catatan bernomor ke stdout dan mengembalikan jumlahnya
   (negatif saat gagal). notes_count hanya mengembalikan jumlah. */
int notes_list(const char *dir);
int notes_count(const char *dir);

/* Direktori store: $RUPA_HOME bila di-set, selain itu ".rupa". */
const char *core_store_dir(void);

/* Dispatcher `notes <sub>` bersama untuk bin/rupa dan bin/bade; membaca
   subperintah dari argv[2]. */
int core_cli_notes(int argc, char **argv);

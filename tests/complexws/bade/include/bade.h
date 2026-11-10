#pragma once

#define BADE_VERSION "1.0.0"

int bade_value(void);
void thisBade(void);

/* Tulis arsip modul yang tertanam (getEmbed("modules")) ke
   <extract>/<name>, lalu bongkar ke <extract>/extracted. 1 = siap,
   0 = gagal. Tanpa keluaran, supaya keluaran bin/bade tidak berubah. */
int badeExtractSystem(void);

/* Cetak ringkasan entri embed "modules": nama, ukuran, direktori ekstraksi.
   1 = ada, 0 = tabel embed tidak memuat entri itu. */
int badeEmbedInfo(void);

/* Titik masuk CLI bin/bade. */
int bade_cli(int argc, char **argv);

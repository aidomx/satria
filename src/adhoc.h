#pragma once

#include "bootstrap.h" /* Bootstrap (typedef struct anonim) */

/*
 * design/compile.md — mode ad-hoc CLI: compile/link satu atau beberapa
 * source TANPA lifecycle build (tanpa state .satria, tanpa no-op, tanpa
 * cache). Tiga bentuk:
 *
 *   A. Eksplisit      : satria src/x.c -o out.o [-b bin/app]
 *   B. Konfigurasi -F : satria src/x.c -F Buildfile (ambil o.* + flags)
 *   C. Override -f    : full build biasa (TIDAK lewat modul ini)
 *
 * Keputusan desain yang dikunci (sematan design + klarifikasi 15/11):
 *   - -b tanpa -o = ERROR (objek harus berbunyi jelas).
 *   - -F = compile + link binary (objek -> o.buildDir/<base>.o, binary ->
 *     o.binaryDir/o.binaryName).
 *   - Multi-sumber cermin gcc: -o saat satu sumber = path .o penuh; saat
 *     multi-sumber = direktori objek (<dir>/<basename>.o per sumber).
 *   - Nol efek samping: tidak menulis .satria apa pun; kegagalan compiler
 *     diteruskan apa adanya (wrapper gcc-floor, perilaku identik).
 */

int adhocRun(const Bootstrap *bs);

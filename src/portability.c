/*
 * portability — payung lapisan OS satria.
 *
 * Isi dipecah modular (folder src/port/):
 *   - port/port_proc.c : proses & waktu (probe PATH, Ctrl+C/foreground,
 *     spawn shell, job paralel, cpuCount, monotonicSeconds).
 *   - port/port_fs.c   : filesystem (stat/mtime/size, mkdir/remove,
 *     listdir, cwd) + fsNewerThan.
 *   - port/port_time.c : waktu lintas-platform (portMonoMs — wrapper ms
 *     monotonic; menggantikan pemakaian langsung CLOCK_MONOTONIC yang tidak
 *     ada di MSVC).
 * File ini menyimpan util path portabel yang dipakai lintas lapisan.
 * Deklarasi publik semua tetap di portability.h — pemakai tidak berubah.
 */
#include "portability.h"

/* Konvensi path: satria secara internal selalu memakai '/' (diterima GCC,
   Clang, maupun MSVC di Windows), jadi hasil OS yang memakai '\\' harus
   dinormalkan lewat sini. */
void pathNormalizeSlash(char *s) {
  for (; s && *s; s++)
    if (*s == '\\') *s = '/';
}

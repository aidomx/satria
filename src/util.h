#ifndef SATRIA_V0_1_0_UTIL_H
#define SATRIA_V0_1_0_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ==================== Batas ukuran umum ==================== */

/* MAX_PATH Windows = 260 (windows.h). Definisi kita hanya bila belum ada
   agar tidak bentrok (C4005) — POSIX memakai 1024 untuk path panjang. */
#ifndef MAX_PATH
#define MAX_PATH 1024
#endif

/* Daftar string dinamis sederhana (dipakai untuk sources, flags, dst). */
typedef struct {
  char **items;
  int count;
  int capacity;
} List;

bool listAdd(List *l, const char *s);
void listFree(List *l);
/* Pesan kapasitas SEKALI di awal agar listAdd tidak realloc bolak-balik
   saat jumlah item sudah diketahui kira-kira (mis. 1024 source). Gagal
   alokasi dibiarkan diam-diam: listAdd tetap tumbuh doubling seperti biasa. */
void listReserve(List *l, int want);
/* Urutkan isi List leksikografis (strcmp). TIDAK dipanggil walkDir lagi —
   sortir sekali pada data lengkap di pemanggil lebih murah daripada qsort
   per level rekursi. Wajib dipanggil pemakai walkDir yang urutannya
   menentukan output (daftar link, listfile tar, staging pack) agar hasil
   tidak bergantung urutan readdir filesystem. */
void listSort(List *l);
void copyStr(char *dst, size_t n, const char *src);

char *trim(char *s);
bool parseBool(const char *s, bool *out);

/* popen(cmd, "r")/pclose portabel — MSVC tidak punya keduanya (POSIX);
   di Windows diimplementasikan via _popen/_pclose (cmd.exe). */
FILE *popenRB(const char *cmd);
void pcloseRB(FILE *fp);

/* ==================== Filesystem ==================== */

void mkdirs(const char *path);
void mkparent(const char *path);
/* Rekursif kumpulkan path di bawah `dir` berakhiran `ext` (ext == "": semua
   file; ext == "/": hanya direktori). Hasil TIDAK DIURUTKAN — sortir sekali
   pada data lengkap di pemanggil lebih murah daripada qsort per level.
   Pemanggil yang urutannya menentukan output (link, listfile tar, staging
   pack) memanggil listSort(out) SETELAH walkDir selesai. */
void walkDir(const char *dir, const char *ext, List *out);
/* Sama dengan walkDir; di mode workspace hasilnya di-cache (.satria/sources.cache).
   Implementasi di cmds/cmds_wsstate.c. */
void stateWalkDirCached(const char *dir, const char *ext, List *out);
bool newerThan(const char *a, const char *b);
bool safeRelative(const char *path);

/* ==================== Proses & waktu ==================== */

bool runCmd(const char *cmd);
double nowSeconds(void);

/* ==================== Laporan build (design/build_report.md) ==================== */

/* Prefix baris laporan build: "|" bila terminal mendukung UTF-8, ">"
   sebagai fallback ASCII. Deteksi sekali per proses: LC_ALL -> LC_CTYPE
   -> LANG; Windows menambah cek codepage konsol (CP_UTF8). */
const char *repPrefix(void);

#endif /* SATRIA_V0_1_0_UTIL_H */

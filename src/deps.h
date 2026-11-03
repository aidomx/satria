#ifndef SATRIA_V0_1_0_DEPS_H
#define SATRIA_V0_1_0_DEPS_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

/*
 * deps — pelacak dependensi header untuk build incremental.
 *
 * Tanpa ini, satria hanya membandingkan mtime object dengan file .c-nya,
 * sehingga mengubah sebuah header tidak pernah memicu kompilasi ulang.
 * Sumber dependensi dua lapis, dipilih per source saat run:
 *   1. file .d dari compiler (flag -MMD -MP, GCC/Clang) — bila tersedia dan
 *      segar (mtime >= source), daftar header-nya dipakai langsung. Lengkap
 *      dan akurat: compiler yang meresolusi (menangani #if, makro, computed
 *      include), tanpa berlebih.
 *   2. fallback: pemindai #include ("..." dan <...>) transitif, meresolusi
 *      seperti preprocessor (dir file pengguna dulu untuk "...", lalu
 *      direktori -I dari Buildfile: headers) — untuk build pertama, MSVC,
 *      atau saat .d belum ada.
 *
 * Keduanya melaporkan mtime header terbaru yang dipakai sebuah source.
 *
 * Header yang tidak ditemukan di direktori project (mis. <stdio.h>)
 * dianggap header sistem dan diabaikan. Pemindaian mengabaikan #if/#ifdef,
 * jadi hasilnya bisa sedikit berlebih (kompilasi ulang yang tak perlu),
 * tidak pernah kurang.
 */

typedef struct DepCache DepCache;

/*
 * Buat cache dari konfigurasi build. `c->sources` dipakai memetakan source
 * -> object -> file .d (GCC/Clang), `c->headers` sebagai direktori -I
 * fallback pemindai #include. Boleh NULL (pemindai tanpa -I).
 */
DepCache *depsNew(const Config *c);
void depsFree(DepCache *dc);

/*
 * mtime nanodetik terbaru di antara seluruh header (transitif) yang di-include
 * `src`; 0 bila tidak ada header project. Header dibaca sekali per proses
 * dan di-cache, jadi aman dipanggil untuk ratusan source.
 */
int64_t depsNewestHeaderMTime(DepCache *dc, const char *src);

/* Sama, tetapi pemanggil yang sudah men-stat `src` menyerahkan mtime-nya
   (ns; -1 = tidak ada) sehingga tidak di-stat ulang. -2 = tidak diketahui. */
int64_t depsNewestHeaderMTimeAt(DepCache *dc, const char *src, int64_t srcMTimeNs);

/* Closure dependensi `src` (src + seluruh header yang dijangkau) dikirim ke
   `visit`. Path relatif root, valid hanya selama callback berjalan. Dipakai
   fast-state mode workspace sebelum DepCache dibebaskan. */
typedef void (*DepVisitFn)(const char *path, void *ctx);
void depsClosureVisit(DepCache *dc, const char *src, DepVisitFn visit, void *ctx);
/* Union closure banyak source dalam satu traversal generation (workspace state). */
void depsClosureVisitMany(DepCache *dc, const char *const *srcs, int count,
                          DepVisitFn visit, void *ctx);

/*
 * Verifikasi konten (anti recompile tanpa perubahan isi, mis. setelah
 * `touch`): `true` bila snapshot hash source + seluruh dependensinya dari
 * build sukses terakhir (persist di .satria/caches/deps.cache) sama dengan isi file
 * saat ini. Snapshot dibaca sekali lazily; jika file tak dikenal snapshot,
 * dianggap berubah (return false — perilaku aman).
 */
bool depsContentUpToDate(DepCache *dc, const char *src);

/*
 * Rekam hash konten source + seluruh dependensinya sebagai snapshot
 * "terkompilasi". Dipanggil setelah build sukses; ditulis ke disk oleh
 * depsSave() (cache_store: entri per alamat proyek, payload = path).
 */
void depsRecordUpdate(DepCache *dc, const char *src);

/*
 * true bila snapshot belum menutupi seluruh state (caches/deps.cache tidak ada,
 * ada file belum direkam, atau cache edges meleset). Bila false dan tidak
 * ada yang dikompilasi, pass rekam setelah build sukses boleh dilewati.
 */
bool depsSnapshotIncomplete(const DepCache *dc);

/* Tulis snapshot ke .satria/caches/deps.cache (atomik; diam bila gagal). */
void depsSave(DepCache *dc);


/*
 * Baca file dependensi compiler (.d, format make -MMD) untuk `src` dan
 * pasang sebagai edges node source-nya, menggantikan hasil pemindai
 * #include. Dipanggil internal; disediakan untuk pengujian.
 */
bool depsLoadDotD(DepCache *dc, const char *src);

#endif /* SATRIA_V0_1_0_DEPS_H */

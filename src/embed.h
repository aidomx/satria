#ifndef SATRIA_V0_1_0_EMBED_H
#define SATRIA_V0_1_0_EMBED_H

#include <stdbool.h>

#include "config.h"

/*
 * Embedding (design/embedded.md): codegen adalah SATU-SATUNYA mekanisme.
 * Tiap entri embedded diarsipkan (src -> tar, atau file jadi), lalu seluruh
 * arsip dibaca menjadi <outBuildDir>/embedded.c — array `static const
 * uint8_t` + tabel `Embedded` + definisi getEmbed()/getEmbedAt() — dan
 * <outBuildDir>/embedded.h — struct, deklarasi API, plus macro kompat
 * EMBED_<VAR>_* yang lama (fase 1 migrasi: simbolnya menunjuk array hasil
 * codegen, jadi konsumen lama tanpa getEmbed tetap jalan).
 *
 * embedded.c dikompilasi sebagai source biasa bersama source proyek
 * (fingerprint, decide, -jN, .d closure, ikut kemasan library). Jalur
 * `ld -r -b binary` dan fallback per-entrilama SUDAH DIHAPUS — tidak ada
 * simbol linker eksotis; MSVC, GCC, dan Clang hanya melihat array uint8_t.
 *
 * Keduanya diregenerasi berbasis KONTEN, bukan mtime: tulis .tmp, bandingkan
 * byte dengan file lama, identik -> buang temp (mtime tetap), beda -> rename
 * atomik — `touch` tanpa perubahan isi tidak memicu compile/relink.
 */

/*
 * Terbitkan <outBuildDir>/embedded.h. *changed memberi tahu pemanggil bahwa
 * konsumen header perlu dikompilasi ulang (key/type/macro berubah).
 */
bool emitEmbeddedHeader(const Config *c, bool *changed);

/*
 * Terbitkan <outBuildDir>/embedded.c (array byte + tabel getEmbed).
 * Arsip entri wajib sudah dibangun dan terbaca; *changed = isi file berubah
 * (regenerasi content-based — lihat atas).
 */
bool emitEmbeddedSource(const Config *c, bool *changed);

/* Bangun arsip tiap entri yang aktif (segar -> no-op). Harus dijalankan
   SEBELUM emitEmbeddedSource karena codegen membaca byte arsip. */
bool buildEmbeddedArchives(const Config *c);

/* buildEmbeddedArchivesEx — sama, dengan *anyRebuilt diisi true bila
   minimal satu arsip DIBANGUN ULANG (bukan no-op). Dipakai laporan
   archive pass (design/build_report.md): no-op cetak "Skipped : true". */
bool buildEmbeddedArchivesEx(const Config *c, bool *anyRebuilt);

/*
 * embedProbeGnuLd — cek GNU ld dengan probe fisik sekali panggil, TANPA
 * membaca/menulis cache. Dipakai HANYA `satria profile embed=` (benchmark
 * tool tar+ld); jalur build sendiri tidak lagi memakai ld.
 */
bool embedProbeGnuLd(void);

/* Finalisasi satu entri embedded: turunkan archivePath & objectPath dari
   setting arsip (archiveDir/name/tar/ext atau file prebuilt). Dipanggil
   configFinalize() di config.c; didefinisikan di embed.c agar pengetahuan
   penamaan arsip tinggal di modul embed. objectPath kini hanya dasar nama
   state sampingan <objectPath>.archive; object embed = <outBuildDir>/embedded.o. */
void configFinalizeEntry(EmbeddedEntry *e, const char *buildDir);

#endif /* SATRIA_V0_1_0_EMBED_H */

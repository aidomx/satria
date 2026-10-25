#ifndef SATRIA_V0_1_0_COMPILE_H
#define SATRIA_V0_1_0_COMPILE_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "portability.h" /* ProcHandle — tangkapan output compiler */

/*
 * Keluarga toolchain (design/flags.md: suffix Buildfile .gcc/.clang/.cl;
 * juga menentukan flag MSVC). CC_GXX = driver C++ (berakhiran "++":
 * g++, clang++, c++, g++-13, dst.) — runtime C++ sudah di-link-nya.
 */
typedef enum { CC_GCC, CC_GXX, CC_CLANG, CC_MSVC, CC_UNKNOWN } CompilerKind;

/* Klasifikasi driver dari nama basis (tanpa path/ekstensi). */
CompilerKind compilerKind(const char *cc);

bool resolveCompiler(Config *c);

/* true bila compiler aktif adalah MSVC (cl.exe) — flag & linker beda. */
bool compilerIsMSVC(const Config *c);

/* true bila cc adalah driver C++ (g++/clang++/cl) — runtime C++ (libstdc++)
   otomatis di-link; driver C butuh -lstdc++ eksplisit untuk proyek C++. */
bool compilerIsCppDriver(const Config *c);

char *includeFlags(const Config *c);

/* Direktori -I dari headers (tanpa flag non-I) — dipakai pelacak header. */
void includeDirs(const Config *c, List *out);
char *warningFlags(const Config *c);

/*
 * defines (design/flags.md §2.1): makro dari Buildfile TANPA prefix ->
 * "-DFOO -DBAR " (GNU/Clang) atau "/DFOO /DBAR " (MSVC). Toleran nilai
 * yang sudah berprefix (-DFOO / /DFOO dinormalisasi; flag penuh non-D
 * diteruskan). NULL bila kosong — pemanggil boleh memakai NULL apa adanya.
 */
char *definesFlags(const Config *c);

/* warningFlags + "-fPIC " — untuk library shared & jalur paralel. */
char *picWarningFlags(const Config *c);

/*
 * Susun command kompilasi satu source (malloc; pemanggil yang membebaskan)
 * — dipakai compileOne dan jalur kompilasi paralel -jN.
 */
char *compileCmd(const Config *c, const char *inc, const char *wf, const char *src,
                 const char *obj);

/* src/x/y.c -> build/y.o mapping per source root, meniru Makefile. */
bool objectPathFor(const Config *c, const char *src, char *out, size_t n);

/* src/x/y.c -> build/y.d — file dependensi (-MMD) milik object-nya. */
bool dotDPathFor(const Config *c, const char *src, char *out, size_t n);

/*
 * cap (opsional; NULL = tanpa tangkapan): bila diisi, seluruh output
 * compiler (stdout+stderr) diarahkan ke file sementara di cap->capPath.
 * Pemanggil mencetaknya UTUH setelah baris progress lewat
 * procCaptureTake + cmdsEmitCapture — urutan terjaga, anti-interleaving.
 */
bool compileOne(const Config *c, const char *inc, const char *wf, const char *src,
                const char *obj, ProcHandle *cap);

/* "src/main.c" tercantum di Buildfile: exclude -> dilepas dari build. */
bool excludedSource(const Config *c, const char *src);

/*
 * Flag arsitektur dari Buildfile: target (malloc; pemanggil yang
 * membebaskan). NULL bila target kosong/tak dikenal — kompilasi host.
 */
char *targetFlags(const Config *c);

/* Kompilasi satu source untuk library: identik compileOne, plus -fPIC
   bila library shared diminta (GNU/Clang; MSVC tidak butuh flag). */
bool compileLibraryOne(const Config *c, const char *inc, const char *wf, const char *src,
                       const char *obj, ProcHandle *cap);

#endif /* SATRIA_V0_1_0_COMPILE_H */

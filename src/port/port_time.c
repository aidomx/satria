/* port_time.c — waktu portabel (milidetik monotonic).
 *
 * Modul lintas-platform tidak boleh memanggil clock_gettime/CLOCK_MONOTONIC
 * langsung: MSVC tidak mendefinisikan CLOCK_MONOTONIC (build manual Windows
 * gagal dengan error C2065), sedangkan QueryPerformanceCounter hanya ada di
 * Windows. Wrapper ini satu-satunya titik untuk durasi berbasis monotonic
 * (trace audit [walk] / [collect] / save-walk — kolom `t=`), mengikuti pola
 * portability.h: deklarasi publik di sana, implementasi per-OS di sini.
 *
 * Bagian dari modul port (src/port/); lihat portability.c untuk pembagian file.
 */
#include "../portability.h"

/* Milidetik dari jam monotonic (basis bebas: hanya selisih yang bermakna).
   monotonicSeconds sudah diimplementasikan per-OS — clock_gettime di POSIX
   (port_proc_posix.c) dan QueryPerformanceCounter di Windows (port_proc.c) —
   jadi di sini cukup dikonversi ke milidetik. */
double portMonoMs(void) { return monotonicSeconds() * 1000.0; }

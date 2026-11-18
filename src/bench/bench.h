#pragma once

/*
 * bench — porting benchmark workspace bench/ws.sh ke C (design/bench_c.md).
 *
 *   satria bench --compare \
 *     -f tools=cmake,satria -f source=10 -f header=10 -f module=10 \
 *     -f scenario=all -f warmup=false -f trace=false -f artifact=true \
 *     -f keep=true -f rolling=true -f bin=a,b
 *
 * Semua kerja dijalankan tanpa shell: proses di-spawn langsung (fork/exec),
 * shim gcc/ld adalah satria sendiri (symlink + mode shim), dan pengukuran
 * memakai jam monotonic. Argumen `-f` membuat opsi mudah diperluas tanpa
 * menambah huruf pendek.
 */

/* Entry command `satria bench ...`. argv/argc adalah sisa argumen setelah
   kata "bench" (argv[0] = "--compare" atau opsi pertama). */
int cmdBench(const char *const argv[], int argc);

/* Mode shim: dipanggil main() saat binary ini dieksekusi sebagai `gcc`/`ld`
   dari direktori .wrap benchmark. Mencatat compile/link ke RB_BENCH_LOG lalu
   exec tool asli. Return hanya bila exec gagal. */
int benchShimMain(int argc, char **argv);

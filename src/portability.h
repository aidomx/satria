#ifndef SATRIA_V0_1_0_PORTABILITY_H
#define SATRIA_V0_1_0_PORTABILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "util.h"

/*
 * portability — lapisan tipis di atas API OS supaya satria bisa dibangun di
 * Linux/POSIX maupun Windows (MinGW & MSVC). Seluruh panggilan yang
 * POSIX-only (dirent.h, unistd.h, command -v, rm -rf, dst.) dipindahkan ke
 * sini; modul lain cukup memakai API netral di bawah.
 *
 * Konvensi path: satria secara internal selalu memakai '/' (diterima GCC,
 * Clang, maupun MSVC di Windows), jadi lapisan ini menormalkan hasil OS
 * (mis. GetCurrentDirectory memberi '\\') lewat pathNormalizeSlash().
 */

void pathNormalizeSlash(char *s);

/* ==================== Proses & waktu ==================== */

/* true bila `exe` bisa dieksekusi: path eksplisit, atau ada di PATH
   (di Windows otomatis mencoba sufiks .exe/.bat/.cmd). */
bool probeAvailable(const char *exe);

/*
 * Mode foreground (Buildfile: foreground, default true).
 *
 * foreground=true  : perilaku klasik — child berada di process group yang
 *                    sama dengan satria; Ctrl+C dari terminal menghentikan
 *                    satria dan compiler sekaligus (POSIX), sementara di
 *                    Windows child selalu dipisah dan diteruskan
 *                    CTRL_BREAK (perilaku lama tetap).
 * foreground=false : satria memasang handler SIGINT-nya sendiri. Ctrl+C hanya
 *                    sampai ke satria, lalu diteruskan ke child secara
 *                    eksplisit. Panggil SEBELUM procRun/procStart.
 */
bool procSetForeground(bool foreground);

/* true bila Ctrl+C tertangkap sejak panggilan procSetForeground terakhir. */
bool procInterrupted(void);

/* Jalankan command lewat shell OS (sh / cmd.exe); true bila exit 0.
   Return PROC_RUN_INTERRUPTED bila child berhenti karena Ctrl+C yang
   ditangani satria (foreground=false). */
#define PROC_RUN_INTERRUPTED (-1)
bool procRun(const char *cmd);

/*
 * Proses paralel (Buildfile: -jN, mirip make).
 * procStart menjalankan command tanpa menunggu; kumpulkan handle-nya lalu
 * panggil procWaitAny sampai selesai. Child paralel selalu di process group
 * terpisah sehingga SIGINT dari satria terarah per proses.
 */
typedef struct {
#ifdef _WIN32
  unsigned long pid; /* dwProcessId */
  void *hProcess;    /* HANDLE proses (dipakai ulang procWaitAny/procStopAll) */
#else
  int pid; /* pid_t */
#endif
  bool finished;    /* sudah di-reap oleh procWaitAny/procStopAll */
  bool ok;          /* exit code 0 */
  bool interrupted; /* mati karena Ctrl+C (bukan error kompilasi) */
  /* Tangkapan output child (procStartCapture / compileOne bercap): path file
     sementara berisi stdout+stderr child; "" = tanpa tangkapan. Ambil isinya
     dengan procCaptureTake setelah proses selesai. */
  char capPath[MAX_PATH];
} ProcHandle;

bool procStart(const char *cmd, ProcHandle *out);

/*
 * Sama dengan procStart, tetapi seluruh output child (stdout+stderr)
 * diarahkan ke file sementara — dipakai kompilasi paralel agar warning/error
 * compiler tidak menulis langsung ke terminal: parent mencetaknya UTUH
 * setelah baris progress (procCaptureTake), sehingga urutan terjaga dan blok
 * tidak bisa tercampur job lain (anti-interleaving).
 */
bool procStartCapture(const char *cmd, ProcHandle *out);

/* File sementara kosong untuk tangkapan manual (jalur serial meredirect lewat
   shell `> path 2>&1`). false bila gagal dibuat — pemanggil jalan tanpa cap. */
bool procCaptureTemp(char *pathOut, size_t n);

/*
 * Pindahkan isi tangkapan ke buffer malloc (NULL bila tidak ada tangkapan /
 * kosong), hapus file sementara dan kosongkan capPath. Pemanggil membebaskan.
 */
char *procCaptureTake(ProcHandle *h, size_t *outLen);

/* Tunggu satu proses selesai; index di `handles` atau -1 bila ada Ctrl+C.
   `finished` (boleh NULL) diisi handle yang baru selesai. */
int procWaitAny(ProcHandle *handles, int count, ProcHandle **finished);

/* Kirim Ctrl+C ke semua child yang masih jalan lalu tunggu sampai mati. */
void procStopAll(ProcHandle *handles, int count);

/* Jumlah core CPU (min. 1) — default jobs untuk -j tanpa angka. */
int cpuCount(void);

/* ==================== Background build (design/background.md) ============ */

/* PID proses berjalan ini (getpid / GetCurrentProcessId). */
int procSelfPid(void);

/* true bila proses `pid` masih hidup — POSIX: kill(pid, 0) (EPERM = ada,
   tetapi tanpa izin); Windows: handle terbuka + exit code STILL_ACTIVE. */
bool procAlive(int pid);

/* Tidur milidetik (polling watcher). */
void procSleepMs(int ms);

/* Set (value != NULL) atau hapus (value == NULL) variabel lingkungan proses.
   Dipakai background.c untuk menandai child build (SATRIA_BG_CHILD) sebelum
   spawn — child mewarisi env saat exec/CreateProcess. */
void procEnvSet(const char *name, const char *value);

/*
Spawn proses terpisah & lepas dari terminal induk (design/background.md §2):
  - POSIX   : fork + setsid — child jadi sesi baru tanpa controlling terminal;
  - Windows : CreateProcess DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP.
stdout+stderr child DIARAHKAN ke `logPath` (mode timpa), stdin dari /dev/null
(NUL di Windows) agar child tidak pernah membaca terminal induk. argv[0] boleh
nama di PATH; PID hasil spawn keluar lewat outPid (bila diset).
*/
bool procSpawnDetached(char *const argv[], const char *logPath, int *outPid);

/* Detik monotonic untuk pengukuran durasi build. */
double monotonicSeconds(void);

/* Milidetik monotonic — durasi/trace lintas platform (kolom `t=` pada
   [walk], [collect], dan audit save-walk). PEMAKAIAN WAJIB untuk kode
   bersama: clock_gettime/CLOCK_MONOTONIC tidak ada di MSVC (C2065),
   sedangkan QueryPerformanceCounter hanya Windows. Lihat port/port_time.c. */
double portMonoMs(void);

/* true bila stdout terhubung ke terminal (prompt interaktif & warna).
   Aman di-pipe: false saat output dialihkan ke file/pipe. */
bool termIsTTY(void);

/* ==================== Filesystem ==================== */

void fsMakeDir(const char *path); /* mkdir satu level, diam bila sudah ada */
bool fsFileExists(const char *path);
bool fsDirExists(const char *path);
time_t fsMTime(const char *path);    /* (time_t)-1 bila tidak ada */
int64_t fsMTimeNs(const char *path); /* -1 bila tidak ada */
long long fsFileSize(const char *path);
/* mtime ns + size dalam SATU stat (dipakai fastState validasi massal). */
bool fsStampNsSize(const char *path, int64_t *mtimeNs, long long *size);
bool fsNewerThan(const char *a, const char *b);
bool fsRemoveFile(const char *path);
/* Ganti `to` dengan `from` (atomik; Windows: MoveFileEx
   REPLACE_EXISTING|WRITE_THROUGH, fallback salin + hapus — design/startup.md §4). */
bool fsRenameReplace(const char *from, const char *to);
bool fsRemoveTree(const char *path); /* rm -rf portabel */
/* Set permission mode (POSIX: chmod; Windows: _chmod — hanya bit
   read-only yang berarti). Dipakai pack saat menyiapkan staging .deb
   (folder bin dan file .so perlu 0755). */
bool fsSetMode(const char *path, unsigned mode);
/* Baca permission mode file (POSIX: st_mode & 0777; Windows: bit
   _S_IREAD/_S_IWRITE). 0 bila gagal. Dipakai pack untuk mempertahankan
   bit executable file sumber ke paket. */
unsigned fsGetMode(const char *path);

/* Satu level isi direktori; subdirektori ke `dirs`, file biasa ke `files`
   (boleh NULL). Path hasil berformat '/' konsisten. */
void fsListDir(const char *dir, List *dirs, List *files);

bool fsGetCwd(char *out, size_t n);
bool fsSetCwd(const char *path);

/* ==================== Memory-mapped file (read-only) ==================== */

/*
 * Map seluruh file untuk DIBACA SAJA ke memori (design/bootstrap.md fase 2:
 * evaluasi state build tanpa membaca file secara tradisional).
 *
 * POSIX : mmap(PROT_READ, MAP_PRIVATE)
 * Windows: CreateFileMapping + MapViewOfFile(FILE_MAP_READ)
 *
 * Return true bila berhasil: *outData mengisi pointer yang VALID sampai
 * fsMapClose dipanggil, *outSize = ukuran file (0 untuk file kosong).
 * File kosong tetap sah (data != NULL, size 0). Gagal (missing file,
 * OOM, dsb.) => false tanpa efek samping.
 *
 * Fallback aman dijamin di level pemakai: bila fsMapRead gagal, pemakai
 * bebas membaca file secara tradisional (design aturan 4).
 */
bool fsMapRead(const char *path, void **outData, size_t *outSize);
void fsMapClose(void *data, size_t size); /* aman dipanggil dgn data=NULL */

#endif /* SATRIA_V0_1_0_PORTABILITY_H */

#ifdef _WIN32
#define strtok_r strtok_s
#endif

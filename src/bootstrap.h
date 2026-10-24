#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * design/bootstrap.md — Bootstrap & Dispatching CLI satria.
 *
 * Alur: bootstrapParse (murni argv) -> bootstrapApply (mutasi env) ->
 *       satriaRun (dispatcher switch murni) -> bootstrapCleanup.
 *
 * Kepemilikan memori: kecuali bidang yang diberi catatan "malloc" di bawah,
 * semua const char* menunjuk argv caller atau buffer statis di bootstrap.c
 * dan tidak perlu dibebaskan.
 */

typedef enum {
  CMD_NONE = 0,
  CMD_INIT,
  CMD_BUILD,
  CMD_CLEAN,
  CMD_HELP,
  CMD_VERSION,
  CMD_RELEASE,
  CMD_PROFILE,
  CMD_LOG,
  CMD_INTERACTIVE_INIT
} Command;

typedef struct {
  bool cold;      // True jika ini adalah cold build (tidak ada state sebelumnya)
  bool noop;      // True jika tidak ada yang perlu di-build (fingerprint cocok)
  bool dirty;     // True jika ada perubahan pada Buildfile atau source
  bool has_state; // True jika file state (.satria/build.state) berhasil dibaca

  // Pointer ke memory-mapped state (jika mmap diaktifkan)
  void *state_map;
  size_t state_size;
} BootstrapStatus;

typedef struct {
  // Perintah utama
  Command cmd;

  // Opsi Global
  const char *buildfile; // Default: "Buildfile"
  bool have_f;           // -f diberikan eksplisit (cmdInitWorkspace pakai ini)
  const char *cmd_token; // token command asli (untuk pesan error); NULL bila build polos
  const char *help_topic; // satria help <topic>: nama section di src/cmd.txt
  int jobs;              // Default: cpuCount()

  // Flags
  bool want_workspace;  // -w
  bool interactive;     // init -p
  bool generate_compdb; // -g compdb

  // Workspace specific
  const char *ws_only;      // -w <nama>
  const char **ws_sel_argv; // [malloc] token `-- key=value ...`, NULL-terminasi

  // Profile specific
  int profile_argc;
  const char **profile_argv;

  // Konversi Ninja/Make (-xf / -xcf)
  const char *conv_file;
  bool conv_keep;       // true jika -xcf, false jika -xf
  bool conv_make;       // true bila argumen "make" (cari Makefile/makefile/GNUmakefile)
  const char *conv_abs; // path absolut conv_file (buffer statis apply)
  bool conv_canceled;   // -xcf dabatalkan overwrite oleh pengguna

  // Status Evaluasi (Diisi pada Fase Apply)
  BootstrapStatus status;
} Bootstrap;

/* Fase 1 — murni parsing argv. Tidak ada chdir/fopen/mmap.
   Return 0 sukses; 1/2 = rc keluar (pesan sudah ke stderr). */
int bootstrapParse(int argc, const char *argv[], Bootstrap *bs);

/* Fase 2 — mutasi environment: chdir -f, konversi -xf/-xcf, evaluasi state.
   Return 0 lanjut ke dispatch; selain itu rc keluar. */
int bootstrapApply(Bootstrap *bs);

/* Gabungkan token `-- key=value ...` menjadi string koma-join ("a,b").
   [malloc — dibebaskan bootstrapCleanup]. NULL bila tidak ada / gagal
   (pesan ke stderr). */
char *bootstrapJoinSelector(const Bootstrap *bs);

/* Bebaskan alokasi milik Bootstrap. Wajib dipanggil sebelum main() keluar
   bila parse sukses. */
void bootstrapCleanup(Bootstrap *bs);

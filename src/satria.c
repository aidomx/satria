#include "satria.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootstrap.h"
#include "background.h"
#include "log/build_log.h"
#include "cmdhelp.h"
#include "commands.h"
#include "package_cli.h"
#include "portability.h"
#include "prof/prof.h"
#include "profile.h"
#include "uses/workspace.h"

#if __has_include("version.h")
#include "version.h" /* build/version.h — dihasilkan dari .satria-version */
#endif

/*
 * Versi satria. Sumber kebenaran: .satria-version di root — saat build satria
 * menuliskannya ke build/version.h (SATRIA_VERSION_EMBEDDED) sehingga versi
 * ter-embed ke binary dan `satria version` tetap benar di mana pun dijalankan.
 * Fallback literal hanya untuk build tanpa .satria-version (mis. CMake murni).
 */
const char *satriaVersion(void) {
#ifdef SATRIA_VERSION_EMBEDDED
  return SATRIA_VERSION_EMBEDDED;
#else
  return "v0.2.4";
#endif
}

/* helper fungsi
 * e.g: satria help|--help|-h <topic>
 *
 * Menerapkan clean code untuk memudahkan pembacaan.
 */
static bool cmdHelpCond(const char *args) {
  return !strcmp(args, "help") || !strcmp(args, "--help") || !strcmp(args, "-h");
}

/* Guard keanggotaan: build/clean/release dari direktori anggota sebuah
   workspace induk ditolak (design/workspace.md aturan 1 & 5). Dipakai baik
   untuk `-w` maupun build single-project di dalam folder anggota. */
static bool memberCwdRejected(void) {
  char wsRoot[MAX_PATH], member[64];
  if (!wsMemberCwdDetect(wsRoot, sizeof(wsRoot), member, sizeof(member))) return false;
  fprintf(stderr,
          "satria: direktori ini adalah anggota workspace '%s' (proyek '%s')\n"
          "        build hanya dari root workspace — build selektif: satria -w %s\n",
          wsRoot, member, member);
  return true;
}

/*
 * design/bootstrap.md fase 3 — satriaRun adalah CONSUMER MURNI: tidak ada
 * parsing argv, strcmp bercabang atas argv, atau mutasi filesystem di sini.
 * Ia membaca Bootstrap hasil parse+apply lalu dispatch lewat switch enum.
 * Semua logika parsing ada di bootstrap.c; logika per-command ada di
 * commands.c / workspace.c / profile.c (sama seperti sebelum refactor).
 */
int satriaRun(int argc, const char *argv[]) {
  int packageStatus = 0;
  if (packageCliRun(argc, argv, &packageStatus)) return packageStatus;
  profInit(); /* profil fase aktif bila SATRIA_PROFILE=1 (stderr, tanpa syscall) */
  /* Simpan argv (untuk spawn child background) & baca penanda SATRIA_BG_CHILD
     (design/background.md §2) — harus SEBELUM bootstrapApply yang bisa chdir -f. */
  bgNoteArgv(argc, argv);

  Bootstrap bs;
  int rc = bootstrapParse(argc, argv, &bs);
  if (rc != 0) return bootstrapCleanup(&bs), rc;

  /* help <topic>: topic diambil dari argv setelah command help
     (satu kata; kata berikutnya diabaikan seperti command lain). */
  if (bs.cmd == CMD_HELP) {
    for (int i = 0; i < argc; i++) {
      const char *args = argv[i];
      const char *next = argv[i + 1];
      if (args && cmdHelpCond(args)) {
        if (i + 1 < argc && next[0] && next[0] != '-') bs.help_topic = next;
        break;
      }
    }
  }

  /* version hanya cetak string — tanpa chdir/apply agar aman dari -f yang
     salah (perilaku sama dengan main.c lama yang memotong lebih awal). */
  if (bs.cmd == CMD_VERSION) {
    printf("satria %s\n", satriaVersion());
    bootstrapCleanup(&bs);
    return 0;
  }

  if (bs.cmd == CMD_LOG) {
    rc = buildLogShow();
    bootstrapCleanup(&bs);
    return rc;
  }

  rc = bootstrapApply(&bs);
  if (rc != 0) {
    bootstrapCleanup(&bs);
    return rc;
  }
  if (bs.conv_canceled) { /* -xcf dibatalkan oleh pengguna: selesai, rc 0 */
    bootstrapCleanup(&bs);
    return 0;
  }

  /* satria profile <context> [key=value ...] — non-mutating, standalone. */
  if (bs.cmd == CMD_PROFILE) {
    bootstrapCleanup(&bs);
    return cmdProfile(bs.profile_argv, bs.profile_argc);
  }

  /* Workspace hanya dipilih melalui -w. Keberadaan Buildfile.ws tidak
     boleh mengubah mode CLI secara implisit. */
  if (bs.want_workspace) {
    if ((bs.cmd == CMD_BUILD || bs.cmd == CMD_CLEAN || bs.cmd == CMD_RELEASE) &&
        memberCwdRejected()) {
      bootstrapCleanup(&bs);
      return 2;
    }
    /* init -p di mode workspace: scaffold interaktif bisa menghasilkan
       Buildfile.ws sendiri (pilihan "Uses workspace?"), jadi diajukan
       sebelum template cmdInitWorkspace. */
    if (bs.cmd == CMD_INIT) {
      if (bs.interactive) {
        bootstrapCleanup(&bs);
        return cmdInteractiveInit();
      }
      int irc = cmdInitWorkspace(bs.have_f ? bs.buildfile : WORKSPACE_FILENAME);
      bootstrapCleanup(&bs);
      return irc;
    }
    if (bs.cmd != CMD_BUILD && bs.cmd != CMD_CLEAN && bs.cmd != CMD_RELEASE &&
        bs.cmd != CMD_WATCH) {
      fprintf(stderr,
              "satria: command '%s' tidak berlaku di mode workspace (pakai build/clean/release)\n",
              bs.cmd_token ? bs.cmd_token : "");
      rc = 2;
      goto done;
    }
    if (bs.cmd == CMD_WATCH) {
      /* `satria -w watch`: pantau build background workspace (§3B/§3C). */
      rc = bgWatch(true, bs.watch_full);
      goto done;
    }
    /* `-- key=value` = override setting release run ini (design/release.md
       revisi 2); diterima bersama build polos maupun command release, dan
       diteruskan ke workspaceRun sebagai releaseSel. */
    if (bs.generate_compdb) {
      /* Query-only: CompDB merged seluruh workspace di root, tanpa build
         dan tanpa mengubah build state (design/workspace_v1.md §No-op
         dan CompDB). `--` selector tidak berlaku untuk compdb. */
      rc = workspaceRun("compdb", bs.jobs, NULL, NULL);
      bootstrapCleanup(&bs);
      return rc;
    }
    /* `-- key=value` = override release run ini; `release` tanpa `--` =
       build + release (releaseOnly aktif hanya bila selektor ada —
       coretan lama "release memerlukan selektor" sudah tidak berlaku). */
    char *sel = bootstrapJoinSelector(&bs);
    if (!sel && bs.ws_sel_argv) {
      bootstrapCleanup(&bs);
      return 2;
    }
    /* Workspace logging uses the already restored WsModel (startup.state). */
    rc = workspaceRun(bs.cmd == CMD_CLEAN ? "clean" : "build", bs.jobs, bs.ws_only, sel);
    if (sel) free(sel);
    bootstrapCleanup(&bs);
    return rc;
  }

  /* Di root workspace tanpa Buildfile single-project, jangan jatuh ke
     parser single-project; tampilkan petunjuk flag yang benar. */
  if ((bs.cmd == CMD_BUILD || bs.cmd == CMD_CLEAN || bs.cmd == CMD_RELEASE) &&
      !bs.have_f && !fsFileExists("Buildfile") && workspaceFileExists()) {
    fprintf(stderr, "satria: Buildfile single-project tidak ditemukan\n"
                    "        gunakan 'satria -w' untuk workspace\n");
    rc = 2;
    goto done;
  }

  /* Guard keanggotaan: build/clean dari direktori anggota sebuah workspace
     induk ditolak — keanggotaan divalidasi lewat model Buildfile.ws induk
     (design/workspace_v1.md aturan 1 & 5). `-g compdb` ikut tercakup (ada
     di jalur CMD_BUILD); proyek standalone di luar workspace tidak
     terpengaruh. */
  if ((bs.cmd == CMD_BUILD || bs.cmd == CMD_CLEAN) && memberCwdRejected()) {
    rc = 2;
    goto done;
  }

  /* Dispatch perintah standar — switch enum, bukan strcmp. */
  switch (bs.cmd) {
  case CMD_BUILD:
    if (bs.generate_compdb)
      rc = cmdCompdbGenerate(bs.buildfile);
    else {
      rc = cmdBuild(bs.jobs, bs.buildfile);
    }
    break;

  case CMD_INIT:
    rc = bs.interactive ? cmdInteractiveInit() : cmdInit(bs.buildfile);
    break;

  case CMD_CLEAN:
    rc = cmdClean(bs.buildfile);
    break;

  case CMD_HELP:
    if (bs.help_topic) {
      /* help <topic>: topic = nama section di src/cmd.txt. Tidak
           dikenal -> pesan + help utama (pola rupa/loader.c). */
      if (cmdHelpSection(bs.help_topic)) {
        rc = 0;
      } else {
        fprintf(stderr, "satria: unknown help topic '%s'\n", bs.help_topic);
        showHelp();
        rc = 1;
      }
    } else {
      showHelp();
      rc = 0;
    }
    break;

  case CMD_RELEASE: /* tanpa -w ditolak di bawah */
    fprintf(stderr,
            "satria: release memerlukan mode workspace (pakai 'satria -w release -- name=rupa')\n");
    rc = 2;
    break;

  case CMD_WATCH: /* `satria watch` — pantau build background satu-proyek (§3) */
    rc = bgWatch(false, bs.watch_full);
    break;

  default:
    rc = 1; /* tidak terjangkau: parser menolak command asing */
    break;
  }

  /* -xf: hapus file sementara setelah command selesai. Buildfile asli
     tidak pernah ditulis, jadi tidak ada yang berisiko hilang. */
  if (bs.conv_abs && !bs.conv_keep) {
    fsRemoveFile("Buildfile.xf.tmp");
    printf("> Removed    : %s (sementara, dari %s)\n", "Buildfile.xf.tmp", bs.conv_abs);
  }

done:
  bootstrapCleanup(&bs);
  return rc;
}

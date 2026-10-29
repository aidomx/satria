/*
 * cmds_passfinish — ekor pass build: fase library workspace (libOnly),
 * keputusan link, link command, pack, dan summary.
 * Badan dipindah apa adanya dari cmdsBuildPassRun (commands.c lama);
 * state dibuka dari CmdsPassTail sehingga logika tidak berubah.
 * Dipisah dari commands.c (audit refactor: berkas kandidat >600 baris).
 */
#include "../commands.h"
#include "../cache/artifact.h"
#include "../log/build_log.h"
#include "../log/report_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#endif

#include "../cmdhelp.h"
#include "../background.h"
#include "../startup.h"
#include "cmds_internal.h"
#include "../compdb.h"
#include "../compile.h"
#include "../config.h"
#include "../deps.h"
#include "../embed.h"
#include "../pack/pack.h"
#include "../portability.h"
#include "../prof/prof.h"
#include "../uses/workspace.h"
#include "../util.h"

int cmdsBuildPassFinish(CmdsPassTail *t) {
  /* Buka konteks serah dari cmds_pass.c — nama lokal dipertahankan agar
     badan di bawah bisa dipindah verbatim. */
  Config c = t->c;
  const char *buildfilePath = t->buildfilePath;
  bool libOnly = t->libOnly;
  List srcs = t->srcs;
  StatPlan plan = t->plan;
  struct DepCache *deps = t->deps;
  List pending = t->pending;
  char *inc = t->inc;
  char *wf = t->wf;
  char obj[MAX_PATH];
  memcpy(obj, t->obj, sizeof(obj));
  CmdsBuildFingerprint currentFp = t->currentFp;
  int compiled = t->compiled;
  int skipped = t->skipped;
  bool objectsChanged = t->objectsChanged;
  bool sourceSetChanged = t->sourceSetChanged;
  bool linkConfigChanged = t->linkConfigChanged;
  int64_t maxObjM = t->maxObjM;
  bool anyObjMissing = t->anyObjMissing;
  /* Fase library workspace (pass 1 dari cmdBuildEx, lihat commands.h):
     kemas lib<name>.a/.so dari object milik proyek ini lalu berhenti —
     link binary ditunda ke fase normal (pass 2) agar proyek yang saling
     memakai library lintas-proyek bisa link setelah SEMUA library ada.
     Library ber-path proyek lain (mis. ../ruka/lib/libruka.a) tidak
     disertakan pada link .so: .a proyek lain belum tentu ada di titik ini
     dan shared library memang tidak perlu inline-kan kode proyek lain. */
  if (libOnly) {
    if (!cmdsBuildLibraryEx(&c, &srcs, false)) {
      free(inc);
      free(wf);
      depsFree(deps);
      return 1;
    }
    profMark("libs");
    if (depsSnapshotIncomplete(deps)) {
      for (int i = 0; i < srcs.count; i++)
        depsRecordUpdate(deps, srcs.items[i]);
    } else if (compiled > 0) {
      for (int i = 0; i < pending.count; i++)
        depsRecordUpdate(deps, pending.items[i]);
    }
    depsSave(deps);
    if (wsModeActive()) wsFastStateSave(deps, &c, &srcs, "", buildfilePath, true);
    depsFree(deps);
    wsPhaseObjectChangeSet(objectsChanged);
    /* libOnly=true: commit hash compile saja — sources/link diserahkan ke
       pass binary supaya source set/link config berubah tetap terdeteksi
       di pass yang sama (lihat cmdsFingerprintSave). */
    cmdsFingerprintSave(&currentFp, libOnly);
    /* State fase library (miniws): no-op pass 1 berikutnya dilewati total.
       target "" = proyek tanpa library — direkam sebagai "-". */
    if (!wsModeActive()) cmdsFastStateSave(&c, &srcs, "", buildfilePath, true);
    printf("\n%s Summary :\n", repPrefix());
    if (c.libRequested) {
      char libp[MAX_PATH * 2];
      if (c.libStatic) {
        cmdsLibStaticPath(&c, libp, sizeof(libp));
        printf("    Library  : %s\n", libp);
      }
      if (c.libShared) {
        cmdsLibSharedPath(&c, libp, sizeof(libp));
        printf("    Library  : %s\n", libp);
      }
    }
    printf("    Compiled : %d\n", compiled);
    printf("    Skipped  : %d\n", skipped);
    printf("    Status   : Success\n");
    free(inc);
    free(wf);
    profReport();
    return 0;
  }

  /* link: hanya jalankan linker bila target belum ada atau salah satu
     input object lebih baru daripada target. Gunakan nanosecond mtime agar
     keputusan incremental tidak kehilangan perubahan yang terjadi dalam
     detik yang sama. */
  char target[MAX_PATH * 2];
  snprintf(target, sizeof(target), "%s/%s", c.outBinaryDir, c.outBinaryName);
#ifdef _WIN32
  /* Windows dapat menambahkan .exe otomatis saat link. */
  if (!fsFileExists(target)) {
    char withExe[sizeof(target) + 8];
    snprintf(withExe, sizeof(withExe), "%s.exe", target);
    if (fsFileExists(withExe)) snprintf(target, sizeof(target), "%s", withExe);
  }
#endif

  bool phaseObjectsChanged = false;
  bool phaseObjectVerdict = compiled == 0 && wsPhaseObjectChangeGet(&phaseObjectsChanged);
  bool linkNeeded = !fsFileExists(target) || linkConfigChanged || sourceSetChanged || objectsChanged ||
                    (phaseObjectVerdict && phaseObjectsChanged);
  if (!linkNeeded) {
    int64_t targetMtime = fsMTimeNs(target);
    if (targetMtime < 0) {
      linkNeeded = true;
    } else {
      if ((compiled > 0 && !objectsChanged) || (phaseObjectVerdict && !phaseObjectsChanged)) {
        /* Object pass ini atau library pass sebelumnya boleh saja menulis
           ulang object, tetapi byte hasilnya identik. Mtime baru bukan input
           linker baru, jadi jangan relink hanya karena timestamp. */
      } else if (compiled == 0 && !anyObjMissing) {
        /* Tidak ada object yang ditulis ulang sejak fase 1: mtime yang sudah
           dikumpulkan masih berlaku — nol stat tambahan pada build no-op. */
        if (maxObjM > targetMtime) linkNeeded = true;
      } else {
        for (int i = 0; i < srcs.count && !linkNeeded; i++) {
          if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
          int64_t objectMtime = fsMTimeNs(obj); /* sesudah compile: langsung, bukan memo */
          if (objectMtime < 0 || objectMtime > targetMtime) linkNeeded = true;
        }
      }

      /* Library lintas-proyek (cdeps -> `library = <dep>/lib/x.a`) adalah input
         link nyata: perubahan isi .a proyek lain harus me-relink binary ini,
         meski object milik proyek sendiri tidak berubah. Tanpa ini bin/bade
         menyimpan salinan basi kode core — audit ws-benchmark 2026-10-15. */
      for (int i = 0; i < c.libraries.count && !linkNeeded; i++) {
        const char *lib = c.libraries.items[i];
        if (!strchr(lib, '/') && !strchr(lib, '\\')) continue; /* bentuk -l: bukan path */
        int64_t libM = fsMTimeNs(lib);
        if (libM < 0 || libM > targetMtime) linkNeeded = true;
      }
    }
  }

  if (!linkNeeded) {
    printf("%s Linking : %s (up-to-date)\n", repPrefix(), target);
    /* Library bisa jadi masih perlu dibangun (baru diaktifkan di
       Buildfile / terhapus manual) meski binary sudah up-to-date. */
    if (c.libRequested && !cmdsBuildLibraryEx(&c, &srcs, true)) {
      free(inc);
      free(wf);
      return 1;
    }
    free(inc);
    free(wf);
    profReport();
    /* Build sukses: rekam snapshot hash source + dependensi agar build
       berikutnya bisa membedakan `touch` (isi sama) dari perubahan konten.
       No-op murni (tidak ada kompilasi, tidak ada yang berubah sejak rekam
       terakhir) melewatinya — DFS + ribuan snapshotSet murni CPU sia-sia
       pada project besar. depsSave tetap dipanggil: verifikasi konten
       (kasus touch) memperbarui mtime snapshot, dan depsSave sendiri murah
       bila tidak ada yang berubah. */
    if (depsSnapshotIncomplete(deps)) {
      for (int i = 0; i < srcs.count; i++)
        depsRecordUpdate(deps, srcs.items[i]);
    } else if (compiled > 0) {
      for (int i = 0; i < pending.count; i++)
        depsRecordUpdate(deps, pending.items[i]);
    }
    depsSave(deps);
    if (wsModeActive()) wsFastStateSave(deps, &c, &srcs, target, buildfilePath, libOnly);
    depsFree(deps);
    cmdsFingerprintSave(&currentFp, libOnly);
    /* Packaging (pack.*) sebelum summary; fast state disimpan di dalamnya
       bila pack sukses — gagal kemasan tidak merekam state. */
    bool fastSaved = false;
    if (!cmdsPackAfterBinary(&c, &fastSaved, &srcs, target, buildfilePath)) {
      statPlanFree(&plan);
      return 1;
    }
    printf("\n%s Summary :\n", repPrefix());
    long long sizeBytes = fsFileSize(target);
    double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;
    printf("    Target   : %s\n", target);
    printf("    Size     : %.1fKB\n", sizeKb);
    printf("    Compiled : %d\n", compiled);
    printf("    Skipped  : %d\n", skipped);
    printf("    Status   : Success\n");
    if (!fastSaved && !wsModeActive()) cmdsFastStateSave(&c, &srcs, target, buildfilePath, false);
    return 0;
  }

  size_t n = strlen(c.cc) + strlen(c.outBinaryDir) + strlen(c.outBinaryName) + 256;
  for (int i = 0; i < srcs.count; i++)
    if (objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) n += strlen(obj) + 2;
  for (int i = 0; i < c.libraries.count; i++)
    n += strlen(c.libraries.items[i]) + 8;

  char *cmd = malloc(n);
  strcpy(cmd, c.cc);
  for (int i = 0; i < srcs.count; i++) {
    if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
    strcat(cmd, " ");
    strcat(cmd, obj);
  }
  if (compilerIsMSVC(&c)) {
    /* MSVC: object dikumpulkan dulu, opsi linker setelah token /link.
       /OUT menentukan target; link.exe untuk EXE tanpa /LD tidak menulis
       .lib/.exp sampingan, jadi tidak perlu /IMPLIB. (Nama variabel beda
       dari `target` luar — MSVC /W4 memperingatkan shadowing, C4456.) */
    char linkTarget[MAX_PATH * 2];
    snprintf(linkTarget, sizeof(linkTarget), "%s/%s", c.outBinaryDir, c.outBinaryName);
    /* link.exe TIDAK menambahkan .exe pada /OUT yang tanpa ekstensi (beda dari
       MinGW gcc yang menambahkannya sendiri). Tanpa ekstensi, binary self-host
       berakhir sebagai file terpisah dari target inkremental bin/<name>.exe —
       ringkasan membaca .exe basi dan run berikutnya menautkan ulang. */
    {
      size_t ltLen = strlen(linkTarget);
      if (ltLen < 4 || strcmp(linkTarget + ltLen - 4, ".exe") != 0)
        strcat(linkTarget, ".exe");
    }
    strcat(cmd, " /link /nologo /INCREMENTAL:NO /OUT:");
    strcat(cmd, linkTarget);
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      if (lib[0] == '-') lib++; /* -lssl -> ssl */
      if (lib[0] == 'l') lib++; /* "lssl" -> "ssl", seperti konvensi headers "I." */
      strcat(cmd, " ");
      strcat(cmd, lib);
      strcat(cmd, ".lib");
    }
  } else {
    strcat(cmd, " -o ");
    strcat(cmd, c.outBinaryDir);
    strcat(cmd, "/");
    strcat(cmd, c.outBinaryName);
    /* Driver C (compiler = gcc eksplisit) + source C++: runtime libstdc++
       harus di-link manual; driver C++ (g++/cl) sudah membawanya sendiri. */
    if (c.langCpp && !compilerIsCppDriver(&c)) strcat(cmd, " -lstdc++");
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      /* "ssl" -> -lssl, "lm" -> -lm (leading 'l' sudah termasuk, seperti "I."
         di headers); entri yang sudah diawali '-' diteruskan apa adanya */
      strcat(cmd, " ");
      if (strchr(lib, '/') || strchr(lib, '\\')) {
        /* Workspace project-library resolution may provide an explicit
           artifact path (../project/lib/libname.a). Do not turn it into
           -l../...; pass the path directly to the linker. */
        strcat(cmd, lib);
      } else {
        if (lib[0] == '-' || lib[0] == 'l')
          strcat(cmd, "-");
        else
          strcat(cmd, "-l");
        strcat(cmd, lib);
      }
    }
  }

  /* Output link = <outBinaryDir>/<outBinaryName> (MSVC /OUT dan gcc -o sama).
     Di Windows exe yang masih dipakai proses lain (terkunci / discan AV) tidak
     bisa DITIMPA: linker gagal LNK1104. Exe lama dipindah ke <out>.old lebih
     dulu. Rename file yang sedang dieksekusi diizinkan di Windows. */
  char outPath[MAX_PATH * 2];
  char oldPath[MAX_PATH * 2 + 8];
  snprintf(outPath, sizeof(outPath), "%s/%s", c.outBinaryDir, c.outBinaryName);
#ifdef _WIN32
  /* gcc/ld di Windows menambah .exe sendiri: file aktual bisa <out>.exe, dan
     itulah yang terkunci saat self-rebuild. Pilih file yang benar-benar ada. */
  if (!fsFileExists(outPath)) {
    char withExeOut[sizeof(outPath) + 8];
    snprintf(withExeOut, sizeof(withExeOut), "%s.exe", outPath);
    if (fsFileExists(withExeOut)) snprintf(outPath, sizeof(outPath), "%s", withExeOut);
  }
#endif
  snprintf(oldPath, sizeof(oldPath), "%s.old", outPath);
  bool movedOld = false;
  if (fsFileExists(outPath)) {
    remove(oldPath); /* sisa build sebelumnya; gagal (masih terkunci) tidak fatal */
    if (rename(outPath, oldPath) != 0) {
      fprintf(stderr, "satria: tidak bisa menyiapkan %s (file dipakai proses lain?)\n", outPath);
      free(cmd);
      statPlanFree(&plan);
      return 1;
    }
    movedOld = true;
  }
  printf("%s Linking : %s/%s\n", repPrefix(), c.outBinaryDir, c.outBinaryName);
  bgProgress(95, "Linking: %s/%s", c.outBinaryDir, c.outBinaryName); /* §3B */
  bool linked = runCmd(cmd);
  profMark("link");
  /* Diagnosa: bila link gagal, cetak perintah lengkap yang dijalankan
     (objek, /OUT, library) agar penyebabnya terlihat di log CI. */
  if (!linked) fprintf(stderr, "satria: link command: %s\n", cmd);
  free(cmd);
  if (movedOld) {
    if (linked) remove(oldPath);                       /* best effort */
    else if (!fsFileExists(outPath)) rename(oldPath, outPath); /* kembalikan exe lama */
  }

  free(inc);
  free(wf);

  if (!linked) {
    /* Semua object sudah sukses, hanya link yang gagal: simpan kemajuan agar
       build berikutnya cukup me-link ulang (tidak mengompilasi ulang object).
       Catat deps object yang valid, simpan fingerprint compile, tandai link
       belum jadi (link = 0). */
    if (depsSnapshotIncomplete(deps)) {
      for (int i = 0; i < srcs.count; i++) depsRecordUpdate(deps, srcs.items[i]);
    } else {
      for (int i = 0; i < pending.count; i++) depsRecordUpdate(deps, pending.items[i]);
    }
    depsSave(deps);
    depsFree(deps);
    CmdsBuildFingerprint partial = currentFp;
    partial.link = 0;
    cmdsFingerprintSave(&partial, false);
    fprintf(stderr, "satria: link failed\n");
    statPlanFree(&plan);
    return 1;
  }

  /* Library statis/shared dari object yang sama — object tidak dihapus. */
  if (!cmdsBuildLibraryEx(&c, &srcs, true)) {
    statPlanFree(&plan);
    return 1;
  }

  /* Build sukses: rekam snapshot hash (lihat jalur up-to-date di atas). */
  if (depsSnapshotIncomplete(deps)) {
    for (int i = 0; i < srcs.count; i++)
      depsRecordUpdate(deps, srcs.items[i]);
  } else if (compiled > 0) {
    for (int i = 0; i < pending.count; i++)
      depsRecordUpdate(deps, pending.items[i]);
  }
  depsSave(deps);
  if (wsModeActive()) wsFastStateSave(deps, &c, &srcs, target, buildfilePath, libOnly);
  depsFree(deps);
  cmdsFingerprintSave(&currentFp, libOnly);

  /* Packaging (pack.*) setelah link & state — lihat jalur up-to-date. */
  bool fastSaved = false;
  if (!cmdsPackAfterBinary(&c, &fastSaved, &srcs, target, buildfilePath)) {
    statPlanFree(&plan);
    return 1;
  }
  profReport();

  long long sizeBytes = fsFileSize(target);
  double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;

  printf("\n%s Summary :\n", repPrefix());
  printf("    Target   : %s\n", target);
  printf("    Size     : %.1fKB\n", sizeKb);
  printf("    Compiled : %d\n", compiled);
  printf("    Skipped  : %d\n", skipped);
  printf("    Status   : Success\n");
  if (!fastSaved && !wsModeActive()) cmdsFastStateSave(&c, &srcs, target, buildfilePath, false);
  statPlanFree(&plan);
  return 0;
}

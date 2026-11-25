/*
 * cmds_lib — fase library, kompilasi paralel, dan emit header versi
 * (dipindah apa adanya dari commands.c).
 *
 *   - Nama file library per toolchain + cek up-to-date per varian,
 *   - buildLibrary: kemas object source + embedded jadi .a/.lib dan/atau
 *     .so/.dylib/.dll,
 *   - runParallelJobs: baris status per job dicetak parent, tanpa interleaving,
 *   - emitVersionHeader: build/version.h dari .satria-version.
 */
#include "cmds_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../background.h"
#include "../compile.h"
#include "../embed.h"
#include "../log/build_log.h"
#include "../log/build_report.h"
#include "../pack/pack.h"
#include "../portability.h"
#include "../util.h"

/* ==================== fase pack (proyek binary + pack.*) ==================== */

/* Wrapper fase normal: library lintas-proyek disertakan pada link shared. */
bool cmdsBuildLibrary(const Config *c, const List *srcs) {
  return cmdsBuildLibraryEx2(c, srcs, true, true);
}

/*
 * Proyek binary dengan pack.*: summary build tidak lengkap tanpa status
 * kemasan, jadi packRun dipanggil DI DALAM kedua jalur sukses cmdBuild —
 * sebelum summary, setelah fingerprint disimpan (gagal kemasan tidak
 * memicu kompilasi ulang; fast state dilewati untuk proyek pack karena
 * artefak kemasan divalidasi mtime oleh packRun tiap run).
 */
bool cmdsPackAfterBinary(const Config *c, bool *fastStateSaved, const List *srcs,
                         const char *target, const char *buildfilePath) {
  /* --dev: pack.* diabaikan run ini (build = compile + link saja) —
     perlakuan sama seperti proyek tanpa pack: state tersimpan normal. */
  if (!c->pack.requested || cliDevActive()) {
    *fastStateSaved = false; /* pemanggil menyimpan state seperti biasa */
    return true;
  }
  if (!packRun(c)) return false; /* gagal kemasan: state tidak direkam */
  if (!wsModeActive()) cmdsFastStateSave(c, srcs, target, buildfilePath, false);
  *fastStateSaved = true;
  return true;
}

/* Proyek kemasan murni (output.binary = false): embedded dulu (arsip
   untuk proyek lain), lalu packaging, lalu summary ringkas. --dev:
   packaging dilewati (pack.* diabaikan), arsip embedded TETAP dibangun —
   proyek lain bergantung padanya (design/embedded.md). */
int cmdsPackOnlyProject(const Config *c) {
  bool anyRebuilt = false;
  bool repActive = buildReportActive();
  bool devSkipPack = cliDevActive();
  if (c->embCount > 0) {
    mkdirs(c->outBuildDir);
    if (!buildEmbeddedArchivesEx(c, &anyRebuilt)) return 1;
  }
  if (c->pack.requested && !devSkipPack) {
    if (!packRun(c)) return 1;
    if (packLastRebuilt()) anyRebuilt = true;
  }
  /* Format lama `Summary :` hanya saat Build Report TIDAK aktif. Saat aktif:
     arsip & paket tercatat pada # Archive / # Release (step 2 & 8). */
  if (!repActive) {
    printf("\n%s Summary :\n", repPrefix());
    for (int i = 0; i < c->embCount; i++) {
      if (!c->emb[i].enable) continue;
      printf("    Archive  : %s%s\n", c->emb[i].archivePath,
             fsFileExists(c->emb[i].archivePath) ? "" : " (missing)");
    }
    /* No-op: tidak ada arsip/paket yang dibangun ulang (design/build_report.md). */
    if (!anyRebuilt) printf("    Skipped  : true\n");
    printf("    Status   : Success\n");
  } else if (!wsModeActive()) {
    /* Single-project: header dirender setelah record arsip/paket masuk
       (workspace: header sudah dirender wsRunCore). */
    buildReportSetCompdb(REP_COMPDB_UNKNOWN, "not applicable (pack-only)");
    buildReportRenderHeader();
  }
  return 0;
}

/* Nama file library sesuai toolchain: lib<name>.a / <name>.lib (statis). */
void cmdsLibStaticPath(const Config *c, char *out, size_t n) {
#ifdef _WIN32
  if (compilerIsMSVC(c))
    snprintf(out, n, "%s/%s.lib", c->outLibDir, c->outLibName);
  else
    snprintf(out, n, "%s/lib%s.a", c->outLibDir, c->outLibName);
#else
  (void)c;
  snprintf(out, n, "%s/lib%s.a", c->outLibDir, c->outLibName);
#endif
}

/* Nama file library dinamis: lib<name>.so / .dylib / <name>.dll. */
void cmdsLibSharedPath(const Config *c, char *out, size_t n) {
#if defined(_WIN32)
  (void)c;
  snprintf(out, n, "%s/%s.dll", c->outLibDir, c->outLibName);
#elif defined(__APPLE__)
  (void)c;
  snprintf(out, n, "%s/lib%s.dylib", c->outLibDir, c->outLibName);
#else
  (void)c;
  snprintf(out, n, "%s/lib%s.so", c->outLibDir, c->outLibName);
#endif
}

typedef struct { const char *p; int i; } LinkDedupKey;
static int linkDedupCmp(const void *a, const void *b) {
  const LinkDedupKey *x = a, *y = b;
  int c = strcmp(x->p, y->p);
  if (c) return c;
  return (x->i > y->i) - (x->i < y->i);
}

/*
 * Tulis satu header anggota ar (60 byte, format Unix). Nama anggota sengaja
 * pendek dan unik: arsip ini privat untuk linker, jadi nama path asli tidak
 * dibutuhkan dan batas 16 byte klasik tidak jadi masalah.
 */
static bool arMemberHeader(FILE *f, const char *name, long long size) {
  char hdr[64];
  int n = snprintf(hdr, sizeof(hdr), "%-16.16s%-12lld%-6d%-6d%-8o%-10lld`\n",
                   name, 0LL, 0, 0, 0100644, size);
  if (n != 60) return false;
  return fwrite(hdr, 1, 60, f) == 60;
}

/* Salin tepat `n` byte dari satu FILE ke FILE lain lewat buffer bersama. */
static bool streamCopy(FILE *in, FILE *out, long long n, char *buf, size_t cap) {
  while (n > 0) {
    size_t want = n > (long long)cap ? cap : (size_t)n;
    size_t got = fread(buf, 1, want, in);
    if (got == 0 || fwrite(buf, 1, got, out) != got) return false;
    n -= (long long)got;
  }
  return true;
}

/* Verifikasi cepat: format ar dan jumlah anggota cocok. Dipakai sebelum
   memakai ulang arsip tanpa menulis, supaya arsip rusak (disk/manual) tidak
   langsung menghasilkan link gagal. */
static bool arArchiveLooksValid(const char *path, int members) {
  void *map = NULL;
  size_t size = 0;
  if (!fsMapRead(path, &map, &size) || size < 8 || memcmp(map, "!<arch>\n", 8) != 0) {
    if (map && size > 0) fsMapClose(map, size);
    return false;
  }
  const unsigned char *base = map;
  const unsigned char *p = base + 8, *end = base + size;
  bool ok = true;
  for (int i = 0; i < members; i++) {
    if ((size_t)(end - p) < 60 || memcmp(p + 58, "`\n", 2) != 0) {
      ok = false;
      break;
    }
    char szbuf[11];
    memcpy(szbuf, p + 48, 10);
    szbuf[10] = '\0';
    long long sz = strtoll(szbuf, NULL, 10);
    if (sz < 0) {
      ok = false;
      break;
    }
    size_t next = (size_t)(p - base) + 60 + (size_t)sz + ((size_t)sz & 1);
    if (next > size) {
      ok = false;
      break;
    }
    p = base + next;
  }
  if (p != end) ok = false;
  fsMapClose(map, size);
  return ok;
}

/* Tulis satu anggota ar baru dari object (header + isi + padding). */
static bool arWriteObject(FILE *out, const char *name, const char *obj, long long size,
                          char *buf, size_t cap) {
  if (!arMemberHeader(out, name, size)) return false;
  FILE *in = fopen(obj, "rb");
  if (!in) return false;
  bool ok = streamCopy(in, out, size, buf, cap);
  fclose(in);
  if (ok && (size & 1) && fputc('\n', out) == EOF) ok = false;
  return ok;
}

/*
 * Bangun arsip link privat dari object proyek. Tidak memakai `ar` eksternal:
 * pada proot `ar rcs` 2.000 object ~5-19 dtk karena setiap object di-stat dan
 * arsip ditulis ulang penuh. Di sini object dibaca sekali dan ditulis sebagai
 * anggota ar polos (tanpa symbol table — linker selalu memakai
 * --whole-archive sehingga semua anggota pasti dimuat; ini sudah diuji
 * dengan `ar rcS`). Urutan object dipertahankan agar urutan constructor sama
 * dengan link object langsung.
 *
 * Bila daftar object sama dengan run sebelumnya (manifest <out>.list), hanya
 * anggota yang object-nya lebih baru dari arsip yang ditulis ulang; anggota
 * lain disalin dari arsip lama. Ini menghindari membuka ribuan object pada
 * setiap editrel. Arsip ditulis ke <out>.tmp lalu di-rename, sehingga linker
 * tidak pernah melihat arsip setengah jadi. Gagal -> false dan pemanggil
 * jatuh kembali ke daftar object langsung.
 */
bool cmdsLinkArchiveBuild(const Config *c, const List *srcs, const char *outPath) {
  List objs = {0};
  char obj[SATRIA_MAX_PATH];
  for (int i = 0; i < srcs->count; i++) {
    if (excludedSource(c, srcs->items[i])) continue;
    if (!objectPathFor(c, srcs->items[i], obj, sizeof(obj))) continue;
    listAdd(&objs, obj);
  }

  /* Dedup path (source bisa tumpang tindih) tanpa mengubah urutan asli. */
  if (objs.count > 1) {
    LinkDedupKey *dk = malloc(sizeof(*dk) * (size_t)objs.count);
    bool *drop = calloc((size_t)objs.count, sizeof(bool));
    if (!dk || !drop) {
      free(dk);
      free(drop);
      listFree(&objs);
      return false;
    }
    for (int i = 0; i < objs.count; i++) {
      dk[i].p = objs.items[i];
      dk[i].i = i;
    }
    qsort(dk, (size_t)objs.count, sizeof(*dk), linkDedupCmp);
    for (int i = 1; i < objs.count; i++)
      if (strcmp(dk[i - 1].p, dk[i].p) == 0) drop[dk[i].i] = true;
    List uniq = {0};
    for (int i = 0; i < objs.count; i++)
      if (!drop[i]) listAdd(&uniq, objs.items[i]);
    listFree(&objs);
    objs = uniq;
    free(dk);
    free(drop);
  }

  if (objs.count == 0) {
    listFree(&objs);
    return false;
  }

  char tmp[SATRIA_MAX_PATH * 2 + 8];
  char listPath[SATRIA_MAX_PATH * 2 + 8];
  char listTmp[SATRIA_MAX_PATH * 2 + 12];
  int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", outPath);
  int ln = snprintf(listPath, sizeof(listPath), "%s.list", outPath);
  int lt = snprintf(listTmp, sizeof(listTmp), "%s.list.tmp", outPath);
  if (tn < 0 || (size_t)tn >= sizeof(tmp) || ln < 0 || (size_t)ln >= sizeof(listPath) ||
      lt < 0 || (size_t)lt >= sizeof(listTmp)) {
    listFree(&objs);
    return false;
  }

  /* Daftar object harus sama persis dengan manifest agar indeks anggota tidak
     bergeser. Bila manifest hilang/berbeda -> bangun penuh. */
  bool sameSet = false;
  FILE *lf = fopen(listPath, "rb");
  if (lf) {
    sameSet = true;
    char line[SATRIA_MAX_PATH * 2];
    for (int i = 0; i < objs.count; i++) {
      if (!fgets(line, sizeof(line), lf)) {
        sameSet = false;
        break;
      }
      size_t l = strlen(line);
      while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
      if (strcmp(line, objs.items[i]) != 0) {
        sameSet = false;
        break;
      }
    }
    if (sameSet && fgetc(lf) != EOF) sameSet = false;
    fclose(lf);
  }

  long long *sizes = calloc((size_t)objs.count, sizeof(long long));
  bool *changed = calloc((size_t)objs.count, sizeof(bool));
  char *buf = malloc(64 * 1024);
  if (!sizes || !changed || !buf) {
    free(sizes);
    free(changed);
    free(buf);
    listFree(&objs);
    return false;
  }

  int changedCount = 0;
  int64_t archiveM = sameSet ? fsMTimeNs(outPath) : -1;
  for (int i = 0; i < objs.count; i++) {
    int64_t m = -1;
    sizes[i] = 0;
    if (!memoStampNsSize(objs.items[i], &m, &sizes[i]) || m < 0 || sizes[i] < 0) {
      sizes[i] = -1;
      changed[i] = true;
      changedCount++;
      continue;
    }
    if (archiveM >= 0 && m > archiveM) {
      changed[i] = true;
      changedCount++;
    }
  }

  if (sameSet && archiveM >= 0 && changedCount == 0 &&
      arArchiveLooksValid(outPath, objs.count)) {
    /* Semua object sudah tercakup arsip: tidak perlu menulis apa pun. */
    free(sizes);
    free(changed);
    free(buf);
    listFree(&objs);
    return true;
  }

  /* Jalur inkremental: mmap arsip lama, salin blok yang tidak berubah dan
     tulis ulang hanya anggota berubah. Satu fwrite besar untuk rentang utuh
     menghindari ribuan fread/fwrite anggota (mahal di proot). */
  if (sameSet && archiveM >= 0 && changedCount > 0) {
    void *map = NULL;
    size_t mapSize = 0;
    bool ok = fsMapRead(outPath, &map, &mapSize) && mapSize >= 8 &&
              memcmp(map, "!<arch>\n", 8) == 0;
    FILE *out = ok ? fopen(tmp, "wb") : NULL;
    if (out) ok = fwrite("!<arch>\n", 1, 8, out) == 8;
    size_t runStart = 8;
    const unsigned char *base = ok ? (const unsigned char *)map : NULL;
    const unsigned char *p = base ? base + 8 : NULL;
    const unsigned char *end = base ? base + mapSize : NULL;
    for (int i = 0; ok && i < objs.count; i++) {
      if ((size_t)(end - p) < 60 || memcmp(p + 58, "`\n", 2) != 0) {
        ok = false;
        break;
      }
      char szbuf[11];
      memcpy(szbuf, p + 48, 10);
      szbuf[10] = '\0';
      long long oldSize = strtoll(szbuf, NULL, 10);
      if (oldSize < 0 || (changed[i] && sizes[i] < 0)) {
        ok = false;
        break;
      }
      size_t memberStart = (size_t)(p - base);
      size_t memberEnd = memberStart + 60 + (size_t)oldSize + ((size_t)oldSize & 1);
      if (memberEnd > mapSize) {
        ok = false;
        break;
      }
      if (changed[i]) {
        size_t plen = memberStart - runStart;
        if (plen && !fwrite(base + runStart, 1, plen, out)) {
          ok = false;
          break;
        }
        char name[16];
        snprintf(name, sizeof(name), "%05d.o", i);
        if (!arWriteObject(out, name, objs.items[i], sizes[i], buf, 64 * 1024)) {
          ok = false;
          break;
        }
        runStart = memberEnd;
      }
      p = base + memberEnd;
    }
    /* Anggota berlebih = arsip/manifest tidak sinkron; paksa bangun penuh. */
    if (ok && p != end) ok = false;
    if (ok) {
      size_t tail = mapSize - runStart;
      if (tail && !fwrite(base + runStart, 1, tail, out)) ok = false;
    }
    if (out && fclose(out) != 0) ok = false;
    if (map && mapSize > 0) fsMapClose(map, mapSize);
    if (ok && rename(tmp, outPath) == 0) {
      free(sizes);
      free(changed);
      free(buf);
      listFree(&objs);
      return true;
    }
    remove(tmp);
  }

  /* Bangun penuh (arsip pertama, set berubah, atau arsip lama rusak). */
  FILE *f = fopen(tmp, "wb");
  bool ok = f != NULL;
  if (ok) ok = fwrite("!<arch>\n", 1, 8, f) == 8;
  char name[16];
  for (int i = 0; ok && i < objs.count; i++) {
    if (sizes[i] < 0) {
      int64_t m = -1;
      long long sz = 0;
      if (!memoStampNsSize(objs.items[i], &m, &sz) || m < 0 || sz < 0) {
        ok = false;
        break;
      }
      sizes[i] = sz;
    }
    snprintf(name, sizeof(name), "%05d.o", i);
    ok = arWriteObject(f, name, objs.items[i], sizes[i], buf, 64 * 1024);
  }
  if (f && fclose(f) != 0) ok = false;
  if (ok && rename(tmp, outPath) != 0) ok = false;
  if (!ok) remove(tmp);

  /* Manifest hanya ditulis setelah arsip valid. */
  if (ok) {
    FILE *mf = fopen(listTmp, "wb");
    bool mok = mf != NULL;
    for (int i = 0; mok && i < objs.count; i++)
      mok = fprintf(mf, "%s\n", objs.items[i]) > 0;
    if (mf && fclose(mf) != 0) mok = false;
    if (mok) mok = rename(listTmp, listPath) == 0;
    if (!mok) remove(listTmp);
    /* Gagal tulis manifest bukan alasan membatalkan link: run berikutnya hanya
       akan membangun arsip penuh sekali lagi. */
  }

  free(sizes);
  free(changed);
  free(buf);
  listFree(&objs);
  return ok;
}

/*
 * Kompilasi paralel (satria -jN, mirip make). Baris status tiap job dicetak
 * oleh parent tepat setelah job selesai — satu printf utuh per baris, jadi
 * tidak ada interleaving antar job. Return false bila ada yang gagal atau
 * build di-interupsi Ctrl+C.
 */
typedef struct {
  char src[SATRIA_MAX_PATH]; /* source yang dikompilasi slot ini */
  double start;       /* untuk durasi per job */
  int idx;            /* indeks source di srcs (untuk okOut) */
} JobSlot;

bool cmdsRunParallelJobs(const Config *c, const char *inc, const char *wf, const List *srcs,
                         char *objPath, size_t objCap, int jobs, int *outCompiled,
                         int *outFailed, int *outInterrupted, bool quiet, bool *okOut) {
  JobSlot *slots = calloc((size_t)jobs, sizeof(JobSlot));
  ProcHandle *handles = calloc((size_t)jobs, sizeof(ProcHandle));
  if (!slots || !handles) {
    free(slots);
    free(handles);
    return false;
  }

  /* -fPIC untuk library shared (GNU/Clang) — setara compileLibraryOne. */
#ifndef _WIN32
  char *picwf = (c->libShared && !compilerIsMSVC(c)) ? picWarningFlags(c) : NULL;
  const char *wfUse = picwf ? picwf : wf;
#else
  const char *wfUse = wf;
#endif

  int total = srcs->count;
  int compiled = 0, failed = 0, interrupted = 0;
  int nextSrc = 0, active = 0;

  while (nextSrc < total || active > 0) {
    /* isi slot kosong dengan job baru (berhenti juga saat Ctrl+C) */
    while (active < jobs && nextSrc < total && !procInterrupted()) {
      const char *src = srcs->items[nextSrc];
      if (!objectPathFor(c, src, objPath, objCap)) {
        nextSrc++;
        continue;
      }
      mkparent(objPath);
      char *cmd = compileCmd(c, inc, wfUse, src, objPath);
      bool started = procStartCapture(cmd, &handles[active]);
      free(cmd);
      if (!started) {
        fprintf(stderr, "satria: cannot start compiler for %s\n", src);
        failed++;
        nextSrc++;
        continue;
      }
      snprintf(slots[active].src, sizeof(slots[active].src), "%s", src);
      slots[active].start = nowSeconds();
      slots[active].idx = nextSrc;
      nextSrc++;
      active++;
    }
    if (active == 0) break;

    ProcHandle *fin = NULL;
    int w = procWaitAny(handles, active, &fin);
    if (w < 0) break; /* tidak ada job tersisa (atau wait gagal) */

    double dt = nowSeconds() - slots[w].start;
    if (okOut) okOut[slots[w].idx] = fin->ok;
    {
      /* Compile paralel: compiler menulis ulang object & .d -> buang memo. */
      char objF[SATRIA_MAX_PATH * 2], dF[SATRIA_MAX_PATH * 2];
      if (objectPathFor(c, slots[w].src, objF, sizeof(objF))) cmdsStatMemoForget(objF);
      if (dotDPathFor(c, slots[w].src, dF, sizeof(dF))) cmdsStatMemoForget(dF);
    }
    compiled++;
    bgProgress(5 + (compiled * 80) / total, "Compiling: %s", slots[w].src); /* §3B */
    const char *tag = fin->interrupted ? "INT" : (fin->ok ? "OK" : "FAIL");
    if (!quiet) {
      printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, tag, dt, slots[w].src);
      fflush(stdout);
    }
    /* Output compiler (warning/error) dicetak UTUH setelah baris progress:
       stream yang sama -> urutan terjaga dan blok tidak bisa tercampur job
       lain yang selesai bersamaan (anti-interleaving). Warning di buffer
       dihitung ke akumulator pass (cmdsWarningsTake). */
    cmdsEmitCapture(fin, fin->ok && !fin->interrupted);
    if (!fin->ok) {
      if (fin->interrupted)
        interrupted++;
      else
        failed++;
    }

    /* kompakkan: pindahkan slot terakhir ke slot yang baru kosong */
    handles[w] = handles[active - 1];
    slots[w] = slots[active - 1];
    active--;
  }

  /* Tangkapan job yang berhenti tanpa sempat dicetak (break saat Ctrl+C /
     wait gagal) dibuang supaya file sementara tidak tertinggal. */
  for (int i = 0; i < active; i++) {
    size_t n = 0;
    free(procCaptureTake(&handles[i], &n));
  }
  free(slots);
  free(handles);
#ifndef _WIN32
  free(picwf);
#endif
  *outCompiled = compiled;
  *outFailed = failed;
  *outInterrupted = interrupted;
  return failed == 0 && interrupted == 0;
}

/* Salin baris [i,e) dari buf tanpa escape ANSI (CSI: ESC '[' ... huruf
   akhir) ke out (NUL-terminated); kembalikan panjang hasil. Dipakaihitung
   warning DAN baris WARN — pola diagnostic harus dikenali sama walau output
   compiler berwarna (-fdiagnostics-color=always saat stderr TTY). */
static size_t cmdsStripLine(const char *buf, size_t i, size_t e, char *out, size_t cap) {
  size_t m = 0;
  for (size_t k = i; k < e && m + 1 < cap; k++) {
    if (buf[k] == '\x1b' && k + 1 < e && buf[k + 1] == '[') {
      k += 2;
      while (k < e && !(buf[k] >= '@' && buf[k] <= '~')) k++; /* huruf akhir CSI */
      continue;
    }
    out[m++] = buf[k];
  }
  out[m] = '\0';
  return m;
}

/* Satu baris diagnostic warning (setelah ANSI dibuang): GCC/Clang
   "<...>: warning: ..." atau MSVC "<...>: warning [C]<digit>...".
   Kata "warning" pada command, teks source, atau ringkasan TIDAK cocok
   karena hanya pola "<...>: warning" yang dikenali. */
static bool cmdsLineIsWarning(const char *line, size_t m) {
  for (size_t k = 0; k + 9 <= m; k++) {
    if (line[k] != ':' || line[k + 1] != ' ') continue;
    if (strncmp(line + k + 2, "warning:", 8) == 0) return true; /* GCC/Clang */
    if (strncmp(line + k + 2, "warning ", 8) == 0) {
      /* "warning C4996:" bentuk MSVC — C opsional, kode diawali digit */
      size_t d = k + 10;
      if (d < m && (line[d] == 'C' || line[d] == 'c')) d++;
      if (d < m && line[d] >= '0' && line[d] <= '9') return true;
    }
  }
  return false;
}

/* Hitung diagnostic warning pada buffer compiler (design §5). Lihat
   cmdsStripLine/cmdsLineIsWarning untuk cakupan pola. */
static int cmdsCountWarnings(const char *buf, size_t n) {
  int w = 0;
  size_t i = 0;
  while (i < n) {
    size_t e = i;
    while (e < n && buf[e] != '\n') e++; /* satu baris */
    char line[4096];
    size_t m = cmdsStripLine(buf, i, e, line, sizeof(line));
    if (cmdsLineIsWarning(line, m)) w++;
    i = e + 1;
  }
  return w;
}

void cmdsEmitWarnLines(const char *buf, size_t n) {
#ifndef _WIN32
  /* Warna dipulihkan oleh satria sendiri: dengan log = true flag compiler
     dimatikan (compileSetDiagColor) sehingga warning sampai tanpa warna —
     baris WARN inilah yang membawanya kembali (diagColorWanted: stderr TTY,
     NO_COLOR/SATRIA_NO_COLOR/TERM=dumb mematikan). Di Windows relay log tidak
     ada (buildLogActive 0) — jalur ini tak tercapai. */
  bool color = diagColorWanted();
#else
  bool color = false;
#endif
  size_t i = 0;
  while (i < n) {
    size_t e = i;
    while (e < n && buf[e] != '\n') e++;
    char line[4096];
    size_t m = cmdsStripLine(buf, i, e, line, sizeof(line));
    if (cmdsLineIsWarning(line, m)) {
      if (color) printf("\033[33mWARN %s\033[0m\n", line);
      else printf("WARN %s\n", line);
    }
    i = e + 1;
  }
}

/* Akumulator warning pass berjalan (per-produk: dipakai lintas job paralel
   maupun serial dalam satu pass). Dikuras oleh cmdsWarningsTake(). */
static int cmdsWarnAcc = 0;

/* Salin file biner utuh (object yang diberi nama anggota baru). */
static bool copyFileBytes(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in) return false;
  FILE *out = fopen(to, "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  char buf[65536];
  size_t r;
  bool ok = true;
  while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
    if (fwrite(buf, 1, r, out) != r) {
      ok = false;
      break;
    }
  if (ferror(in)) ok = false;
  fclose(in);
  if (fclose(out) != 0) ok = false;
  return ok;
}

/*
 * Stage object yang basenamenya bertabrakan (lihat komentar pemanggil).
 *
 * stageDir: diisi path direktori stage bila ada object yang distage,
 * kosong bila tidak ada tabrakan (pemanggil membuangnya via fsRemoveTree).
 * Return false bila stage tak bisa dibuat padahal dibutuhkan — pemanggil
 * membatalkan fase library dengan pesan ini.
 */
/* Pembanding untuk dedup path dan pengelompokan basename (lihat pemakainya). */
typedef struct { const char *p; int i; } DedupKey_;
typedef struct { const char *b; int i; } BaseKey_;
int cmpDedup(const void *a, const void *b) {
  const DedupKey_ *x = a, *y = b;
  int c = strcmp(x->p, y->p);
  if (c) return c;
  return (x->i > y->i) - (x->i < y->i);
}
int cmpBase(const void *a, const void *b) {
  const BaseKey_ *x = a, *y = b;
  int c = strcmp(x->b, y->b);
  if (c) return c;
  return (x->i > y->i) - (x->i < y->i);
}

static bool objsStageCollisions(const Config *c, List *objs, char *stageDir, size_t stageCap) {
  /* 1) Tandai object dengan basename yang muncul lebih dari sekali. */
  bool *dup = calloc((size_t)objs->count, sizeof(bool));
  if (!dup) return false;
  bool any = false;
  /* Urutkan indeks menurut basename, lalu tandai setiap run berbasename sama
     (panjang >= 2). Setara dengan pemindaian O(N^2) lama, O(N log N). */
  if (objs->count > 1) {
    typedef struct { const char *b; int i; } BaseKey;
    BaseKey *bk = malloc(sizeof(BaseKey) * (size_t)objs->count);
    if (!bk) { free(dup); return false; }
    for (int i = 0; i < objs->count; i++) {
      const char *bi = strrchr(objs->items[i], '/');
      const char *bj = strrchr(objs->items[i], '\\');
      if (bj > bi) bi = bj;
      bk[i].b = bi ? bi + 1 : objs->items[i];
      bk[i].i = i;
    }
    int cmpBase(const void *a, const void *b);
    qsort(bk, (size_t)objs->count, sizeof(BaseKey), cmpBase);
    for (int i = 0; i < objs->count;) {
      int j = i + 1;
      while (j < objs->count && strcmp(bk[i].b, bk[j].b) == 0) j++;
      if (j - i >= 2) {
        for (int k = i; k < j; k++) dup[bk[k].i] = true;
        any = true;
      }
      i = j;
    }
    free(bk);
  }
  if (!any) {
    free(dup);
    return true; /* tidak ada tabrakan — path asli dipakai apa adanya */
  }

  /* 2) Direktori stage DI LUAR pohon build (TMPDIR, fallback /tmp —
     Windows pakai TEMP), pola profTmpDirMake: unik lewat jam monotonic. */
  const char *base = getenv("TMPDIR");
#ifdef _WIN32
  if (!base || !*base) base = getenv("TEMP");
#endif
  if (!base || !*base) base = "/tmp";
  for (int i = 0; i < 64; i++) {
    snprintf(stageDir, stageCap, "%s/satria-arstage-%ld-%d", base,
             (long)(monotonicSeconds() * 1e6), i);
    if (!fsDirExists(stageDir) && !fsFileExists(stageDir)) {
      fsMakeDir(stageDir);
      if (fsDirExists(stageDir)) break;
    }
    if (i == 63) {
      fprintf(stderr, "satria: cannot create archive-member stage dir in %s\n", base);
      free(dup);
      stageDir[0] = '\0';
      return false;
    }
  }

  /* 3) Salin tiap object tabrakan dengan nama mangle: TAG_<path relatif thd
     build dir>, pemisah direktori jadi '_'. Path object selalu di bawah
     outBuildDir (objectPathFor), jadi rel selalu valid.

     TAG = segmen terakhir outBuildDir (build/bade -> "bade") bila build dir
     berada di subdirektori, supaya tabrakan yang sama di DUA proyek
     workspace (build/bade/install.o vs build/rupamod/install.o) tidak lagi
     menghasilkan anggota bernama sama: bade_install.o vs rupamod_install.o.
     Single-project (outBuildDir = "build") tidak butuh tag. */
  const char *tag = strrchr(c->outBuildDir, '/');
  char tagBuf[128] = {0};
  if (tag && tag[1]) {
    snprintf(tagBuf, sizeof(tagBuf), "%s_", tag + 1);
    for (char *s = tagBuf; *s; s++)
      if (*s == '/' || *s == '\\' || *s == ' ') *s = '_';
  }
  char **used = calloc((size_t)objs->count, sizeof(char *));
  int usedCount = 0;
  if (!used) {
    free(dup);
    return false;
  }
  size_t pl = strlen(c->outBuildDir);
  for (int i = 0; i < objs->count; i++) {
    if (!dup[i]) continue;
    const char *p = objs->items[i];
    if (strncmp(p, c->outBuildDir, pl) != 0 || p[pl] != '/') {
      fprintf(stderr, "satria: unexpected object path outside build dir: %s\n", p);
      free(dup);
      free(used);
      stageDir[0] = '\0';
      return false;
    }
    char mangle[SATRIA_MAX_PATH];
    snprintf(mangle, sizeof(mangle), "%s%s", tagBuf, p + pl + 1);
    for (char *s = mangle; *s; s++)
      if (*s == '/' || *s == '\\') *s = '_';

    /* Jaring pengaman: mangle harus unik (mis. tag sama karena outBuildDir
       dipakai bersama). Beri sufiks _2, _3, ... pada pengulangan. */
    char uniq[SATRIA_MAX_PATH];
    snprintf(uniq, sizeof(uniq), "%s", mangle);
    for (int k = 2;; k++) {
      bool taken = false;
      for (int u = 0; u < usedCount; u++)
        if (strcmp(used[u], uniq) == 0) {
          taken = true;
          break;
        }
      if (!taken) break;
      char stem[SATRIA_MAX_PATH];
      snprintf(stem, sizeof(stem), "%s", mangle);
      char *dot = strrchr(stem, '.');
      if (dot) *dot = '\0';
      /* stem dipotong agar suffix "_<k>.o" selalu muat dalam uniq (tanpa warning). */
      snprintf(uniq, sizeof(uniq), "%.*s_%d%s", (int)(sizeof(uniq) - 16), stem, k, dot ? ".o" : "");
    }
    used[usedCount++] = strdup(uniq);

    char staged[SATRIA_MAX_PATH * 2];
    snprintf(staged, sizeof(staged), "%s/%s", stageDir, uniq);
    if (!copyFileBytes(p, staged)) {
      fprintf(stderr, "satria: cannot stage archive member: %s -> %s\n", p, staged);
      for (int u = 0; u < usedCount; u++) free(used[u]);
      free(used);
      free(dup);
      stageDir[0] = '\0';
      return false;
    }
    /* listAdd mem-strdup — bebas-kan lama, ganti dengan path stage. */
    free(objs->items[i]);
    objs->items[i] = strdup(staged);
    if (!objs->items[i]) {
      for (int u = 0; u < usedCount; u++) free(used[u]);
      free(used);
      free(dup);
      stageDir[0] = '\0';
      return false;
    }
  }
  for (int u = 0; u < usedCount; u++) free(used[u]);
  free(used);
  free(dup);
  return true;
}

/* Nama anggota yang dilihat `ar` (dan lib.exe): basename object. */
static const char *objMemberName(const char *obj) {
  const char *b = strrchr(obj, '/');
  const char *b2 = strrchr(obj, '\\');
  if (b2 > b) b = b2;
  return b ? b + 1 : obj;
}

/*
 * Isi arsip statis harus PERSIS daftar object fase ini. Dua alasan:
 *   - `ar rcs` menambah anggota bernama sama dua kali bila dua path berbeda
 *     berbagi basename (perilaku ar, bukan bug kita); hasil lama dari
 *     sebelum mangle bisa bertahan.
 *   - `ar rcs` tidak pernah menghapus anggota yang object-nya sudah tidak
 *     ada (source di-rename/dihapus), jadi arsip lama bisa basi.
 * Karena itu, saat ada tabrakan nama, arsip yang "up-to-date" menurut mtime
 * tetap diverifikasi isinya; bila tidak cocok ia dibangun ulang dari nol.
 * Return true bila verifikasi tidak bisa dilakukan (ar tak ada) — rebuild
 * paksa tanpa bukti hanya memperlambat, bukan memperbaiki.
 */
static bool archiveMembersMatch(const Config *c, const char *target, const List *objs) {
  if (compilerIsMSVC(c)) return true; /* lib.exe: biarkan jalur lama */
  char cmd[SATRIA_MAX_PATH * 2 + 16];
  snprintf(cmd, sizeof(cmd), "ar t %s", target);
  ProcHandle h;
  if (!procStartCapture(cmd, &h)) return true;
  ProcHandle *fin = NULL;
  if (procWaitAny(&h, 1, &fin) < 0 || !fin || !fin->ok) {
    size_t drop = 0;
    free(procCaptureTake(&h, &drop));
    return true;
  }
  size_t n = 0;
  char *buf = procCaptureTake(&h, &n);
  if (!buf) return n == 0 ? objs->count == 0 : true;

  bool ok = true;
  int lines = 0;
  for (size_t i = 0; i < n;) {
    size_t e = i;
    while (e < n && buf[e] != '\n') e++;
    size_t len = e - i;
    if (len && buf[i + len - 1] == '\r') len--;
    if (len) lines++;
    i = e + 1;
  }
  if (lines != objs->count) ok = false;

  for (int i = 0; ok && i < objs->count; i++) {
    const char *name = objMemberName(objs->items[i]);
    size_t nl = strlen(name);
    int found = 0;
    for (size_t k = 0; k < n;) {
      size_t e = k;
      while (e < n && buf[e] != '\n') e++;
      size_t len = e - k;
      if (len && buf[k + len - 1] == '\r') len--;
      if (len == nl && memcmp(buf + k, name, nl) == 0) found++;
      k = e + 1;
    }
    if (found != 1) ok = false;
  }
  free(buf);
  return ok;
}

int cmdsWarningsTake(void) {
  int w = cmdsWarnAcc;
  cmdsWarnAcc = 0;
  return w;
}

void cmdsEmitCapture(ProcHandle *h, bool unitOk) {
  size_t n = 0;
  char *buf = procCaptureTake(h, &n);
  if (!buf) return;
  if (n > 0) {
    cmdsWarnAcc += cmdsCountWarnings(buf, n);
    if (buildLogActive() && unitOk) {
      /* Log = true (keputusan O2): output mentah pindah ke build.log —
         report menggantinya dengan baris WARN ringkas berwarna, sehingga
         bukan duplikasi; `satria log` memperlihatkan detail penuh. */
      cmdsEmitWarnLines(buf, n);
    } else {
      /* Log mati, atau unit GAGAL: cetak UTUH — error butuh konteks penuh
         (baris source, note) di terminal. */
      fwrite(buf, 1, n, stdout);
      if (buf[n - 1] != '\n') fputc('\n', stdout);
    }
    fflush(stdout);
    buildLogCapture(buf, n); /* relay stderr tidak melihat output ini */
  }
  free(buf);
}

/* ================= arsip statis inkremental (armap GNU) ================= */

/*
 * `ar rcs` membaca dan menulis ulang SELURUH arsip, dan `ranlib` sama
 * mahalnya. Pada arsip 5.000 anggota di proot satu object yang berubah
 * berbiaya ~13-14 dtk, padahal hanya beberapa anggota berbeda. Fungsi di
 * bawah menulis arsip baru dengan menyalin blok anggota yang tidak berubah
 * apa adanya dan membangun ulang tabel simbol (armap GNU, anggota "/") dari
 * dua sumber murah:
 *   - nama simbol anggota lama, dipetakan lewat offset header di armap, dan
 *   - `nm -g --defined-only` untuk anggota yang berubah saja.
 * Berlaku hanya bila set & urutan anggota sama dengan build sebelumnya
 * (dijamin pemanggil lewat incrementalStatic), header masih memakai format
 * GNU 32-bit, dan nama anggota cocok dengan basename object. Kondisi lain
 * (MSVC, macOS, arsip rusak, objek LTO, nm gagal) -> false dan pemanggil
 * memakai `ar rcs` seperti biasa.
 */

typedef struct {
  size_t off; /* offset header anggota di arsip lama */
  size_t size;
  int obj;    /* indeks object, -1 untuk armap / string table */
  bool isIdx; /* anggota armap lama (dibuang, diganti armap baru) */
} ArMember;

typedef struct {
  const char *name;
  int obj;
  int seq; /* urutan sisip: menjaga urutan simbol di dalam satu anggota */
} ArSymEnt;

static uint32_t arBe32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

/* Nama anggota: "name/" polos atau "/<offset>" ke anggota string table "//". */
static bool arMemberName(const unsigned char *base, const ArMember *m, size_t strOff,
                         size_t strSize, char *out, size_t cap) {
  char raw[17];
  memcpy(raw, base + m->off, 16);
  raw[16] = '\0';
  for (int k = 15; k >= 0 && raw[k] == ' '; k--) raw[k] = '\0';
  size_t n = strlen(raw);
  if (n && raw[0] == '/') {
    if (n < 2 || raw[1] < '0' || raw[1] > '9') return false;
    unsigned long so = strtoul(raw + 1, NULL, 10);
    if (!strSize || so >= strSize) return false;
    const char *s = (const char *)(base + strOff + so);
    size_t lim = strSize - so;
    const char *nl = memchr(s, '\n', lim);
    size_t len = nl ? (size_t)(nl - s) : lim;
    while (len && (s[len - 1] == '/' || s[len - 1] == '\r')) len--;
    if (len == 0 || len >= cap) return false;
    memcpy(out, s, len);
    out[len] = '\0';
    return true;
  }
  while (n && raw[n - 1] == '/') raw[--n] = '\0';
  if (n == 0 || n >= cap) return false;
  memcpy(out, raw, n + 1);
  return true;
}

/* Simbol global yang didefinisikan satu object, via nm (hanya object berubah). */
static bool arNmSymbols(const char *obj, List *names) {
  if (strpbrk(obj, " \t\r\n'\"\\")) return false;
  char cmd[SATRIA_MAX_PATH * 2 + 48];
  int n = snprintf(cmd, sizeof(cmd), "nm -g --defined-only '%s'", obj);
  if (n < 0 || (size_t)n >= sizeof(cmd)) return false;
  ProcHandle h;
  if (!procStartCapture(cmd, &h)) return false;
  ProcHandle *fin = NULL;
  if (procWaitAny(&h, 1, &fin) < 0 || !fin || !fin->ok) {
    size_t drop = 0;
    free(procCaptureTake(&h, &drop));
    return false;
  }
  size_t len = 0;
  char *buf = procCaptureTake(&h, &len);
  if (!buf) return true;
  bool ok = true;
  size_t i = 0;
  while (i < len) {
    size_t e = i;
    while (e < len && buf[e] != '\n') e++;
    size_t b = e;
    if (b > i && buf[b - 1] == '\r') b--;
    size_t a = i;
    while (a < b && (buf[a] == ' ' || buf[a] == '\t')) a++;
    /* Nama simbol = token terakhir; baris tanpa kolom tipe (mis. "file:")
       dilewati karena t == a. */
    size_t t = b;
    while (t > a && buf[t - 1] != ' ' && buf[t - 1] != '\t') t--;
    if (t > a && b > t) {
      char sym[SATRIA_MAX_PATH * 2];
      size_t sl = b - t;
      if (sl >= sizeof(sym)) {
        ok = false;
        break;
      }
      memcpy(sym, buf + t, sl);
      sym[sl] = '\0';
      listAdd(names, sym);
    }
    i = e + 1;
  }
  free(buf);
  return ok;
}

/* `ar` menyusun armap menurut urutan anggota (lalu urutan simbol di dalam
   anggota), bukan abjad. Tiru itu agar isi arsip identik dengan hasil ar. */
static int arSymEntCmp(const void *a, const void *b) {
  const ArSymEnt *x = a, *y = b;
  if (x->obj != y->obj) return x->obj - y->obj;
  return x->seq - y->seq;
}

/* Cari anggota yang header-nya berada tepat di offset `off` (anggota urut
   menaik di file, jadi binary search cukup). */
static int arOwnerAt(const ArMember *mem, size_t nmem, uint32_t off) {
  size_t lo = 0, hi = nmem;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (mem[mid].off == off) return mem[mid].obj;
    if (mem[mid].off < off)
      lo = mid + 1;
    else
      hi = mid;
  }
  return -1;
}

/*
 * Tulis ulang arsip statis secara inkremental. `changed` = object yang lebih
 * baru dari arsip (dipakai pemanggil untuk incrementalStatic).
 */
/* Batas anggota berubah: tiap object berubah memanggil `nm` sekali. Di atas
   batas ini `ar rcs` (satu proses untuk seluruh arsip) lebih murah daripada
   ratusan spawn nm — perubahan header besar tetap memakai jalur lama. */
#define AR_REWRITE_MAX_CHANGED 128

static bool arRewriteIndexed(const char *archive, const List *objs, const List *changed) {
  if (changed->count <= 0 || changed->count > AR_REWRITE_MAX_CHANGED) return false;
  void *map = NULL;
  size_t mapSize = 0;
  if (!fsMapRead(archive, &map, &mapSize) || mapSize < 68) return false;
  const unsigned char *base = map;

  ArMember *mem = NULL;
  size_t nmem = 0, memCap = 0;
  int *oldCount = NULL;
  bool *isChg = NULL;
  ArSymEnt *sym = NULL;
  List owned = {0};
  unsigned char *ibuf = NULL;
  size_t *objOff = NULL;
  const ArMember **objMem = NULL;
  int nobj = 0;
  bool ok = false;

  if (memcmp(base, "!<arch>\n", 8) != 0) goto done;

  /* 1) Parsing anggota lama. */
  size_t idxOff = 0, idxSize = 0, strOff = 0, strSize = 0;
  bool haveIdx = false;
  size_t p = 8;
  bool isIdx = false;
  while (p < mapSize) {
    if (p + 60 > mapSize || memcmp(base + p + 58, "`\n", 2) != 0) goto done;
    const unsigned char *h = base + p;
    char sz[11];
    memcpy(sz, h + 48, 10);
    sz[10] = '\0';
    char *endp = NULL;
    long long msz = strtoll(sz, &endp, 10);
    if (endp == sz || msz < 0) goto done;
    size_t usz = (size_t)msz;
    if (p + 60 + usz > mapSize) goto done;
    if (nmem == memCap) {
      size_t nc = memCap ? memCap * 2 : 64;
      ArMember *nm = realloc(mem, nc * sizeof(*nm));
      if (!nm) goto done;
      mem = nm;
      memCap = nc;
    }
    int obj = -1;
    const unsigned char *hn = base + p;
    isIdx = false;
    if (hn[0] == '/' && hn[1] == '/') {
      strOff = p + 60;
      strSize = usz;
    } else if (hn[0] == '/' && (hn[1] == ' ' || hn[1] == '\0')) {
      if (haveIdx) goto done; /* armap ganda: arsip tak dikenal */
      idxOff = p;
      idxSize = usz;
      haveIdx = true;
      isIdx = true;
    } else if (hn[0] == '/') {
      goto done; /* /SYM64/: arsip > 4 GB, tidak didukung */
    } else {
      obj = nobj++;
    }
    mem[nmem].off = p;
    mem[nmem].size = usz;
    mem[nmem].obj = obj;
    mem[nmem].isIdx = isIdx;
    nmem++;
    p += 60 + usz + (usz & 1);
  }
  if (p != mapSize || !haveIdx || idxSize < 4) goto done;
  if (nobj != objs->count) goto done;

  /* 2) Nama anggota harus sama dengan basename object pada urutan yang sama. */
  objMem = calloc((size_t)nobj, sizeof(*objMem));
  if (!objMem) goto done;
  for (size_t k = 0; k < nmem; k++)
    if (mem[k].obj >= 0) objMem[mem[k].obj] = &mem[k];
  for (int i = 0; i < nobj; i++) {
    char name[SATRIA_MAX_PATH];
    if (!objMem[i] || !arMemberName(base, objMem[i], strOff, strSize, name, sizeof(name)))
      goto done;
    if (strcmp(name, objMemberName(objs->items[i])) != 0) goto done;
  }

  /* 3) Parsing armap lama: offset header per simbol + nama. */
  const unsigned char *ib = base + idxOff + 60;
  size_t nsyms = arBe32(ib);
  if (nsyms > (idxSize - 4) / 4) goto done;
  const unsigned char *sOff = ib + 4;
  const char *sName = (const char *)(ib + 4 + (size_t)4 * nsyms);
  size_t sNameLen = idxSize - 4 - (size_t)4 * nsyms;

  oldCount = calloc((size_t)nobj, sizeof(*oldCount));
  isChg = calloc((size_t)nobj, sizeof(*isChg));
  objOff = calloc((size_t)nobj, sizeof(*objOff));
  if (!oldCount || !isChg || !objOff) goto done;

  /* Offset di armap harus menunjuk tepat ke header anggota (urutan file). */
  size_t namePos = 0;
  for (size_t i = 0; i < nsyms; i++) {
    int found = arOwnerAt(mem, nmem, arBe32(sOff + 4 * i));
    if (found < 0) goto done;
    if (namePos >= sNameLen) goto done;
    size_t nl = 0;
    while (namePos + nl < sNameLen && sName[namePos + nl] != '\0') nl++;
    if (namePos + nl >= sNameLen) goto done;
    namePos += nl + 1;
    oldCount[found]++;
  }

  for (int i = 0; i < changed->count; i++)
    for (int k = 0; k < nobj; k++)
      if (strcmp(objs->items[k], changed->items[i]) == 0) isChg[k] = true;

  /* 4) Susun tabel simbol baru: lama (anggota tetap) + nm (anggota berubah). */
  size_t symCap = nsyms + 64, nsym = 0;
  sym = malloc(symCap * sizeof(*sym));
  if (!sym) goto done;
  namePos = 0;
  for (size_t i = 0; i < nsyms; i++) {
    int obj = arOwnerAt(mem, nmem, arBe32(sOff + 4 * i));
    size_t nl = 0;
    while (namePos + nl < sNameLen && sName[namePos + nl] != '\0') nl++;
    const char *nm = sName + namePos;
    namePos += nl + 1;
    if (obj < 0 || isChg[obj]) continue; /* simbol anggota berubah diganti nm */
    sym[nsym].name = nm;
    sym[nsym].obj = obj;
    sym[nsym].seq = (int)nsym;
    nsym++;
  }
  for (int i = 0; i < nobj; i++) {
    if (!isChg[i]) continue;
    int before = owned.count;
    if (!arNmSymbols(objs->items[i], &owned)) goto done;
    int added = owned.count - before;
    /* Object lama punya simbol tapi nm kosong: objek LTO/format asing -> ar. */
    if (oldCount[i] > 0 && added == 0) goto done;
    for (int k = before; k < owned.count; k++) {
      if (nsym == symCap) {
        size_t nc = symCap * 2;
        ArSymEnt *ns = realloc(sym, nc * sizeof(*ns));
        if (!ns) goto done;
        sym = ns;
        symCap = nc;
      }
      sym[nsym].name = owned.items[k];
      sym[nsym].obj = i;
      sym[nsym].seq = (int)nsym;
      nsym++;
    }
  }
  if (nsym > 0) qsort(sym, nsym, sizeof(*sym), arSymEntCmp);

  /* 5) Tata letak baru: armap lebih dulu, lalu anggota lama berurutan. */
  size_t namesBytes = 0;
  for (size_t i = 0; i < nsym; i++) namesBytes += strlen(sym[i].name) + 1;
  if (nsym > (size_t)0x3fffffff || namesBytes > (size_t)0x3fffffff) goto done;
  size_t idxBody = 4 + 4 * nsym + namesBytes;
  size_t cur = 8 + 60 + idxBody + (idxBody & 1);
  if (cur > 0xffffffffull) goto done;

  size_t *newSize = calloc(nmem, sizeof(*newSize));
  size_t *newOff = calloc(nmem, sizeof(*newOff));
  if (!newSize || !newOff) {
    free(newSize);
    free(newOff);
    goto done;
  }
  for (size_t k = 0; k < nmem; k++) {
    if (mem[k].isIdx) continue; /* armap lama diganti armap di depan */
    size_t sz = mem[k].size;
    if (mem[k].obj >= 0) objOff[mem[k].obj] = cur;
    if (mem[k].obj >= 0 && isChg[mem[k].obj]) {
      long long fs = fsFileSize(objs->items[mem[k].obj]);
      if (fs < 0 || (unsigned long long)fs > 0xffffffffull) {
        free(newSize);
        free(newOff);
        goto done;
      }
      sz = (size_t)fs;
    }
    newSize[k] = sz;
    newOff[k] = cur;
    cur += 60 + sz + (sz & 1);
    if (cur > 0xffffffffull) {
      free(newSize);
      free(newOff);
      goto done;
    }
  }

  ibuf = malloc(idxBody);
  if (!ibuf) {
    free(newSize);
    free(newOff);
    goto done;
  }
  ibuf[0] = (unsigned char)(nsym >> 24);
  ibuf[1] = (unsigned char)(nsym >> 16);
  ibuf[2] = (unsigned char)(nsym >> 8);
  ibuf[3] = (unsigned char)nsym;
  size_t at = 4;
  for (size_t i = 0; i < nsym; i++, at += 4) {
    uint32_t so = (uint32_t)objOff[sym[i].obj];
    ibuf[at] = (unsigned char)(so >> 24);
    ibuf[at + 1] = (unsigned char)(so >> 16);
    ibuf[at + 2] = (unsigned char)(so >> 8);
    ibuf[at + 3] = (unsigned char)so;
  }
  for (size_t i = 0; i < nsym; i++) {
    size_t nl = strlen(sym[i].name) + 1;
    memcpy(ibuf + at, sym[i].name, nl);
    at += nl;
  }

  /* 6) Tulis arsip baru (tmp + rename, linker tak pernah lihat setengah jadi). */
  char tmp[SATRIA_MAX_PATH * 2 + 8];
  int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", archive);
  if (tn < 0 || (size_t)tn >= sizeof(tmp)) {
    free(newSize);
    free(newOff);
    goto done;
  }
  FILE *out = fopen(tmp, "wb");
  if (out) {
    bool w = fwrite("!<arch>\n", 1, 8, out) == 8;
    w = w && arMemberHeader(out, "/", (long long)idxBody);
    w = w && fwrite(ibuf, 1, idxBody, out) == idxBody;
    if (w && (idxBody & 1) && fputc('\n', out) == EOF) w = false;
    char *cpbuf = w ? malloc(64 * 1024) : NULL;
    if (w && !cpbuf) w = false;
    for (size_t k = 0; w && k < nmem; k++) {
      if (mem[k].isIdx) continue;
      if (mem[k].obj >= 0 && isChg[mem[k].obj]) {
        unsigned char hdr[60];
        memcpy(hdr, base + mem[k].off, 60);
        char szf[11];
        int sl = snprintf(szf, sizeof(szf), "%-10llu", (unsigned long long)newSize[k]);
        if (sl != 10) {
          w = false;
          break;
        }
        memcpy(hdr + 48, szf, 10);
        if (fwrite(hdr, 1, 60, out) != 60) {
          w = false;
          break;
        }
        FILE *in = fopen(objs->items[mem[k].obj], "rb");
        if (!in) {
          w = false;
          break;
        }
        w = streamCopy(in, out, (long long)newSize[k], cpbuf, 64 * 1024);
        fclose(in);
        if (w && (newSize[k] & 1) && fputc('\n', out) == EOF) w = false;
      } else {
        size_t blk = 60 + mem[k].size + (mem[k].size & 1);
        if (fwrite(base + mem[k].off, 1, blk, out) != blk) w = false;
      }
    }
    free(cpbuf);
    if (w && fclose(out) != 0) w = false;
    if (w && rename(tmp, archive) == 0) ok = true;
  }
  if (!ok) remove(tmp);
  free(newSize);
  free(newOff);

done:
  free(mem);
  free(oldCount);
  free(isChg);
  free(objOff);
  free(objMem);
  free(sym);
  free(ibuf);
  listFree(&owned);
  if (map) fsMapClose(map, mapSize);
  return ok;
}

/*
 * Fase library: kumpulkan object hasil kompilasi (source + embedded),
 * kemas statis (ar / lib) dan/atau link shared (-shared / /LD).
 * Object perantara TIDAK dihapus — tetap dipakai link binary dan
 * agar build incremental berikutnya tidak kompilasi ulang total.
 *
 * linkPathLibs=false (fase library workspace): entri library ber-path
 * (artifact proyek lain, mis. ../ruka/lib/libruka.a) DILEWATI pada link
 * shared — .a proyek lain belum tentu ada saat fase library berjalan dan
 * .so tidak perlu inline kode proyek lain. Library sistem (-lm, -lssl)
 * tetap disertakan.
 */
bool cmdsBuildLibraryEx(const Config *c, const List *srcs, bool linkPathLibs) {
  return cmdsBuildLibraryEx2(c, srcs, linkPathLibs, true);
}

bool cmdsBuildLibraryEx2(const Config *c, const List *srcs, bool linkPathLibs,
                         bool sourceSetChanged) {
  if (!c->libRequested) return true;

  /* Jalur no-op: hindari membangun daftar object, alokasi string, dan mkdir
     bila kedua varian library sudah mutakhir. */
  bool staticUpToDate = !c->libStatic || cmdsLibVariantUpToDate(c, true, srcs);
  bool sharedUpToDate = !c->libShared || cmdsLibVariantUpToDate(c, false, srcs);
  if (staticUpToDate && sharedUpToDate) {
    char target[SATRIA_MAX_PATH * 2];
    if (c->libStatic) {
      cmdsLibStaticPath(c, target, sizeof(target));
      buildReportAddLibrary(target, REP_CACHED, fsFileSize(target));
    }
    if (c->libShared) {
      cmdsLibSharedPath(c, target, sizeof(target));
      buildReportAddLibrary(target, REP_CACHED, fsFileSize(target));
    }
    return true;
  }

  List objs = {0};
  char obj[SATRIA_MAX_PATH];
  for (int i = 0; i < srcs->count; i++) {
    /* exclude: source tidak ikut DIKEMAS ke library (mis. main.c milik
       binary) — tetap dikompilasi untuk executable. Object embed kini
       object source biasa (embedded.c ikut srcs) — tanpa perlakuan khusus. */
    if (excludedSource(c, srcs->items[i])) continue;
    if (!objectPathFor(c, srcs->items[i], obj, sizeof(obj))) continue;
    /* sources bisa tumpang tindih (dir induk + dir anak terdaftar dua kali):
       object yang sama JANGAN dimasukkan dua kali — `ar rcs a.o a.o`
       menghasilkan dua anggota bernama sama. */
    listAdd(&objs, obj); /* dedup di bawah (O(N log N)) */
  }

  /* Dedup path object: sort (path, indeks) lalu buang salinan selain kemunculan
     pertama. Urutan asli dipertahankan (isi arsip identik). O(N log N). */
  if (objs.count > 1) {
    typedef struct { const char *p; int i; } DedupKey;
    DedupKey *dk = malloc(sizeof(DedupKey) * (size_t)objs.count);
    bool *drop = calloc((size_t)objs.count, sizeof(bool));
    if (dk && drop) {
      for (int i = 0; i < objs.count; i++) { dk[i].p = objs.items[i]; dk[i].i = i; }
      int cmpDedup(const void *a, const void *b);
      qsort(dk, (size_t)objs.count, sizeof(DedupKey), cmpDedup);
      for (int i = 1; i < objs.count; i++)
        if (strcmp(dk[i - 1].p, dk[i].p) == 0) drop[dk[i].i] = true;
      List uniq = {0};
      for (int i = 0; i < objs.count; i++) if (!drop[i]) listAdd(&uniq, objs.items[i]);
      listFree(&objs);
      objs = uniq;
    }
    free(dk);
    free(drop);
  }

  if (objs.count == 0) {
    fprintf(stderr, "satria: library requested but no object files found\n");
    listFree(&objs);
    return false;
  }

  /* Nama anggota di dalam .a/.lib adalah BASENAME object (ar/lib memotong
     direktori), jadi dua source dengan basename sama dari root sumber
     berbeda (mis. src/install.c vs manager/install.c -> build/install.o vs
     build/manager/install.o) menghasilkan anggota duplikat: `ar r` hanya
     menimpa, `ar x`/ekstraksi per anggota jadi ambigu, dan audit isi arsip
     (ar t) sulit dibaca. Object yang bertabrakan disalin dulu ke direktori
     stage sementara dengan nama mangle — path object relatif thd build dir
     dengan '/' -> '_' (manager/install.o -> manager_install.o) — sehingga
     member name unik. Object tanpa tabrakan memakai path aslinya, jadi
     layout anggota yang sudah ada tidak berubah. Stage dibuang segera
     setelah kemasan selesai (TMPDIR) — state build & prune tak tersentuh. */
  char stageDir[SATRIA_MAX_PATH] = {0};
  if (!objsStageCollisions(c, &objs, stageDir, sizeof(stageDir))) {
    listFree(&objs);
    return false;
  }

  /* Update arsip statis inkremental: hanya object yang lebih baru dari arsip
     yang diserahkan ke penulis arsip (arRewriteIndexed) atau, bila jalur itu
     tidak berlaku, ke `ar rcs`. `ar rcs` membaca/menulis ulang semua anggota —
     di proot 5001 object = ~75 dtk; update satu object ~16 dtk, sedangkan
     penulis arsip sendiri ~0,2 dtk karena blok anggota tetap hanya disalin.
     Aman hanya bila set sumber tidak berubah dan tidak ada tabrakan basename:
     `ar r` tidak menghapus anggota basi, dan object stage hasil mangle punya
     nama anggota berbeda. Selain itu arsip dibangun ulang penuh. */
  bool incrementalStatic = false;
  List changedObjs = {0};
  char staticPath[SATRIA_MAX_PATH * 2] = {0};
  if (c->libStatic) cmdsLibStaticPath(c, staticPath, sizeof(staticPath));
  if (c->libStatic && !staticUpToDate && !sourceSetChanged &&
      !compilerIsMSVC(c) && !stageDir[0] && fsFileExists(staticPath)) {
    int64_t archiveM = fsMTimeNs(staticPath);
    if (archiveM >= 0) {
      int missing = 0;
      for (int i = 0; i < objs.count; i++) {
        int64_t om = -1;
        long long osz = 0;
        if (!memoStampNsSize(objs.items[i], &om, &osz) || om < 0) {
          missing++;
          continue;
        }
        if (om > archiveM) listAdd(&changedObjs, objs.items[i]);
      }
      /* Semua object berubah = rebuild penuh; tidak ada yang berubah berarti
         arsip seharusnya sudah up-to-date (atau ada kondisi tak terduga) —
         jalur penuh yang aman. */
      incrementalStatic = missing == 0 && changedObjs.count > 0 &&
                          changedObjs.count < objs.count;
    }
  }

  mkdirs(c->outLibDir);

  bool staticDone = true, sharedDone = true;
  char target[SATRIA_MAX_PATH * 2];
  /* ar (static) dan cc -shared (shared) membaca object yang sama dan menulis
     artefak berbeda: jalankan keduanya bersamaan. Maksimal dua proses; hasil
     dilaporkan setelah keduanya selesai agar urutan laporan deterministik. */
  char *staticCmd = NULL, *sharedCmd = NULL;
  ProcHandle ph[2];
  int nph = 0, staticSlot = -1, sharedSlot = -1;

  if (c->libStatic) {
    cmdsLibStaticPath(c, target, sizeof(target));
    /* Ada tabrakan basename: arsip lama bisa berisi anggota dobel/basi yang
       tak akan dibersihkan `ar rcs`. Verifikasi isinya dulu; hanya bila
       tidak cocok, paksa rebuild (bukan tiap run — mtime tetap dipakai). */
    if (staticUpToDate && stageDir[0] && fsFileExists(target) &&
        !archiveMembersMatch(c, target, &objs))
      staticUpToDate = false;
    /* Update inkremental: tulis arsip sendiri (salin blok anggota tetap +
       armap baru) alih-alih `ar rcs` yang menulis ulang 5.000 anggota.
       Gagal/kondisi tak dikenal -> jalur `ar` di bawah. */
    bool staticRewritten = incrementalStatic &&
                           arRewriteIndexed(target, &objs, &changedObjs);
    if (!staticUpToDate && staticRewritten) {
      staticDone = true;
    } else if (!staticUpToDate) {
      const List *inputs = incrementalStatic ? &changedObjs : &objs;
      size_t n = 64;
      for (int i = 0; i < inputs->count; i++)
        n += strlen(inputs->items[i]) + 3;
      char *cmd = malloc(n);
      if (compilerIsMSVC(c))
        snprintf(cmd, n, "lib /nologo /OUT:%s", target);
      else
        snprintf(cmd, n, "ar rcs %s", target);
      for (int i = 0; i < inputs->count; i++) {
        strcat(cmd, " ");
        strcat(cmd, inputs->items[i]);
      }
      /* Buang arsip lama agar isinya PERSIS objs: `ar rcs` hanya menimpa
         anggota bernama sama dan tidak menghapus anggota basi. Pada update
         inkremental arsip dipertahankan: posisi anggota lama tidak berubah. */
      if (!incrementalStatic && fsFileExists(target)) remove(target);
      staticCmd = cmd;
      staticSlot = nph;
      if (!procStartCapture(staticCmd, &ph[nph])) {
        staticSlot = -1;
        staticDone = false;
        fprintf(stderr, "satria: static library command: %s\n", staticCmd);
        fprintf(stderr, "satria: static library build failed\n");
      } else {
        nph++;
      }
    }
  }

  /* Shared library dari arsip statis: satu file untuk dibaca linker, bukan
     ribuan object. Diukur pada 5001 object/proot: 58,8 dtk -> 4,9 dtk.
     Hanya dipakai bila arsip statis pasti ada dan tidak dibangun ulang penuh
     (up-to-date atau baru di-update inkremental). Bila arsip sedang dibangun
     inkremental, tunggu dulu; bila gagal, kembali ke object langsung agar
     kegagalan ar tidak menyeret link .so. */
  bool sharedFromArchive = false;
  if (c->libShared && c->libStatic && !compilerIsMSVC(c) && !sourceSetChanged &&
      (staticUpToDate || incrementalStatic)) {
    if (staticSlot >= 0) {
      ProcHandle *fin = NULL;
      while (!ph[staticSlot].finished) {
        if (procWaitAny(&ph[staticSlot], 1, &fin) < 0) break;
      }
      if (!ph[staticSlot].finished) procStopAll(&ph[staticSlot], 1);
      staticDone = ph[staticSlot].finished && ph[staticSlot].ok;
      cmdsEmitCapture(&ph[staticSlot], staticDone);
      if (!staticDone) {
        fprintf(stderr, "satria: static library command: %s\n", staticCmd);
        fprintf(stderr, "satria: static library build failed\n");
      }
      staticSlot = -1; /* output sudah dikonsumsi di sini */
    }
    sharedFromArchive = staticDone && fsFileExists(staticPath);
  }

  if (c->libShared) {
    cmdsLibSharedPath(c, target, sizeof(target));
    if (!sharedUpToDate) {
      size_t n = strlen(c->cc) + strlen(target) + 160;
      if (sharedFromArchive) {
        n += strlen(staticPath) + 48;
      } else {
        for (int i = 0; i < objs.count; i++)
          n += strlen(objs.items[i]) + 3;
      }
      for (int i = 0; i < c->libraries.count; i++)
        n += strlen(c->libraries.items[i]) + 8;
      char *cmd = malloc(n);
      if (compilerIsMSVC(c)) {
        char *p = cmd;
        p += sprintf(p, "cl /nologo /LD");
        for (int i = 0; i < objs.count; i++) p += sprintf(p, " %s", objs.items[i]);
        p += sprintf(p, " /link /nologo /INCREMENTAL:NO /OUT:%s", target);
        for (int i = 0; i < c->libraries.count; i++) {
          const char *lib = c->libraries.items[i];
          if (!linkPathLibs && (strchr(lib, '/') || strchr(lib, '\\'))) continue;
          if (lib[0] == '-') lib++;
          if (lib[0] == 'l') lib++;
          p += sprintf(p, " %s.lib", lib);
        }
      } else {
        /* Driver C (mis. compiler = gcc eksplisit) tetap harus bisa me-link
           object C++: tambahkan -lstdc++ saat proyek berbahasa C++. */
        snprintf(cmd, n, c->langCpp && !compilerIsCppDriver(c) ? "%s -shared -lstdc++"
                                                               : "%s -shared",
                 c->cc);
        if (sharedFromArchive) {
          /* Semua anggota arsip ikut, semantik sama dengan mendaftar seluruh
             object langsung. macOS memakai -force_load; GNU/ELF memakai
             --whole-archive. */
#ifdef __APPLE__
          strcat(cmd, " -Wl,-force_load,");
          strcat(cmd, staticPath);
#else
          strcat(cmd, " -Wl,--whole-archive ");
          strcat(cmd, staticPath);
          strcat(cmd, " -Wl,--no-whole-archive");
#endif
        } else {
          for (int i = 0; i < objs.count; i++) {
            strcat(cmd, " ");
            strcat(cmd, objs.items[i]);
          }
        }
        strcat(cmd, " -o ");
        strcat(cmd, target);
        for (int i = 0; i < c->libraries.count; i++) {
          const char *lib = c->libraries.items[i];
          /* Fase library workspace: artifact proyek lain dilewati (belum
             tentu ada); simbol lintas diselesaikan saat link binary. */
          if (!linkPathLibs && (strchr(lib, '/') || strchr(lib, '\\'))) continue;
          strcat(cmd, " ");
          /* Explicit workspace artifact paths (e.g. ../ruka/lib/libruka.a)
             must be passed directly to the linker. Only logical/system
             library names use -l. */
          if (strchr(lib, '/') || strchr(lib, '\\')) {
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
      sharedCmd = cmd;
      sharedSlot = nph;
      if (!procStartCapture(sharedCmd, &ph[nph])) {
        sharedSlot = -1;
        sharedDone = false;
        fprintf(stderr, "satria: shared library command: %s\n", sharedCmd);
        fprintf(stderr, "satria: shared library build failed\n");
      } else {
        nph++;
      }
    }
  }

  /* Tunggu semua proses library (maks 2). Jika satu gagal start, handle itu
     tidak masuk `ph`; hasilnya sudah dicatat sebagai gagal di atas. */
  while (nph > 0) {
    bool any = false;
    for (int i = 0; i < nph; i++)
      if (!ph[i].finished) any = true;
    if (!any) break;
    ProcHandle *fin = NULL;
    if (procWaitAny(ph, nph, &fin) < 0) break;
  }
  for (int i = 0; i < nph; i++)
    if (!ph[i].finished) {
      procStopAll(ph, nph);
      break;
    }

  /* Emit output setelah kedua proses selesai: urutan static lalu shared,
     tidak tergantung siapa yang finish lebih dulu. */
  if (staticSlot >= 0) {
    staticDone = ph[staticSlot].ok;
    cmdsEmitCapture(&ph[staticSlot], staticDone);
    if (!staticDone) {
      fprintf(stderr, "satria: static library command: %s\n", staticCmd);
      fprintf(stderr, "satria: static library build failed\n");
    }
  }
  if (sharedSlot >= 0) {
    sharedDone = ph[sharedSlot].ok;
    cmdsEmitCapture(&ph[sharedSlot], sharedDone);
    if (!sharedDone) {
      fprintf(stderr, "satria: shared library command: %s\n", sharedCmd);
      fprintf(stderr, "satria: shared library build failed\n");
    }
  }

  /* Laporan artefak dalam urutan tetap (static lalu shared), baik cached
     maupun built. */
  if (c->libStatic) {
    cmdsLibStaticPath(c, target, sizeof(target));
    if (staticUpToDate)
      buildReportAddLibrary(target, REP_CACHED, fsFileSize(target));
    else if (staticDone)
      buildReportAddLibrary(target, REP_BUILT, fsFileSize(target));
  }
  if (c->libShared) {
    cmdsLibSharedPath(c, target, sizeof(target));
    if (sharedUpToDate)
      buildReportAddLibrary(target, REP_CACHED, fsFileSize(target));
    else if (sharedDone)
      buildReportAddLibrary(target, REP_BUILT, fsFileSize(target));
  }

  if (stageDir[0]) fsRemoveTree(stageDir);
  free(staticCmd);
  free(sharedCmd);
  listFree(&changedObjs);
  listFree(&objs);
  return staticDone && sharedDone;
}

/* True bila SATU varian library (statis/shared) sudah ada dan lebih baru
   daripada seluruh object inputnya — varian itu boleh dilewati. Dipakai
   per varian di cmdsBuildLibrary: menghapus .so saja tidak meng-rebuild .a. */
bool cmdsLibVariantUpToDate(const Config *c, bool isStatic, const List *srcs) {
  char obj[SATRIA_MAX_PATH];
  char target[SATRIA_MAX_PATH * 2];

  if (isStatic)
    cmdsLibStaticPath(c, target, sizeof(target));
  else
    cmdsLibSharedPath(c, target, sizeof(target));

  int64_t mtime = fsMTimeNs(target);
  if (mtime < 0) return false; /* target belum ada */

  for (int i = 0; i < srcs->count; i++) {
    if (excludedSource(c, srcs->items[i])) continue;
    if (!objectPathFor(c, srcs->items[i], obj, sizeof(obj))) continue;
    long long osz = 0;
    int64_t om = -1;
    if (!memoStampNsSize(obj, &om, &osz)) om = -1; /* object: lewat memo */
    if (om < 0 || om > mtime) return false;
  }
  return true;
}

/*
 * Terbitkan build/version.h dari .satria-version (root project). Bila file
 * itu ada, project yang menambahkan build ke headers bisa memakai
 * SATRIA_VERSION_EMBEDDED — versi ikut ter-embed ke binary saat build, bukan
 * dibaca ulang saat runtime, sehingga `satria version` tetap benar di mana
 * pun binary dijalankan (tidak tergantung path lokal/repo). Header hanya
 * ditulis ulang saat isinya berubah; perubahan memaksa kompilasi ulang.
 */
bool cmdsEmitVersionHeader(const Config *c, bool *changed) {
  if (!fsFileExists(".satria-version")) return true;

  FILE *rf = fopen(".satria-version", "rb");
  if (!rf) return true;
  char raw[64] = {0};
  size_t n = fread(raw, 1, sizeof(raw) - 1, rf);
  fclose(rf);
  while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r' || raw[n - 1] == ' ' || raw[n - 1] == '\t'))
    raw[--n] = '\0';
  char *s = raw;
  while (*s == ' ' || *s == '\t')
    s++;
  if (!*s) return true; /* kosong: jangan terbitkan apa pun */

  char body[192];
  snprintf(body, sizeof(body),
           "/* Auto-generated by satria from .satria-version. Do not edit. */\n"
           "#define SATRIA_VERSION_EMBEDDED \"%s\"\n",
           s);

  char path[SATRIA_MAX_PATH + 16];
  snprintf(path, sizeof(path), "%s/version.h", c->outBuildDir);
  mkparent(path);

  /* lewati penulisan ulang bila tidak berubah agar mtime tetap stabil */
  char existing[192] = {0};
  FILE *ef = fopen(path, "rb");
  if (ef) {
    size_t en = fread(existing, 1, sizeof(existing) - 1, ef);
    fclose(ef);
    if (en == strlen(body) && memcmp(existing, body, en) == 0) return true;
  }

  /* Mode BINER: penulisan teks mengubah \n menjadi \r\n di Windows sehingga
     perbandingan biner berikutnya selalu beda dan versi dianggap berubah —
     memaksa recompile penuh di setiap run (gagal inkremental build-manual). */
  FILE *wf = fopen(path, "wb");
  if (!wf) {
    fprintf(stderr, "satria: cannot write %s\n", path);
    return false;
  }
  fwrite(body, 1, strlen(body), wf);
  fclose(wf);
  /* Header versi baru: invalidasi memo stat (validasi fast-state bisa sudah
     men-cache stamp lama sebelum terbitan ini). */
  cmdsStatMemoForget(path);
  if (changed) *changed = true;
  return true;
}

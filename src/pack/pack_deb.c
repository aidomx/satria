/*
 * pack_deb — pembangun artefak .deb TANPA dpkg-deb.
 *
 * Struktur .deb = ar archive berisi tiga member (urutan wajib):
 *   debian-binary   ("2.0\n"), control.tar.gz, data.tar.gz.
 * Header ar 60 byte ditulis sendiri (nama space-padded gaya dpkg-deb);
 * tar+gzip staging memakai shell tar (dependensi yang sama dengan embed).
 *
 * Pemetaan data: setiap entri pack.files dipasang di
 *   <pack.deb.install_prefix>/<entri>  (mis. bin/app -> /usr/local/bin/app)
 * Mode file: 0755 untuk komponen pertama bin/ dan file .so*, 0644 lainnya.
 * Staging di .satria/pack/deb/{control,data} lalu dihapus setelah jadi.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../portability.h"
#include "../util.h"
#include "pack_internal.h"
#include "pack_stage.h"

/* ==================== arsitektur Debian ==================== */

/* Arsitektur host saat ini (arsitektur yang dipakai compiler untuk
   membangun binary), dalam penamaan Debian. Dipakai sebagai default
   ketika pack.deb.architecture tak diset, dan sebagai pembanding untuk
   peringatan mismatch ketika diset manual (lihat debArch). */
static const char *debHostArch(void) {
#if defined(__x86_64__) || defined(_M_X64)
  return "amd64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__i386__) || defined(_M_X86)
  return "i386";
#elif defined(__riscv) && defined(__riscv_xlen) && __riscv_xlen == 64
  return "riscv64";
#elif defined(__arm__)
  return "armhf";
#else
  return "all";
#endif
}

static void debArch(const Config *c, char *out, size_t n) {
  const char *host = debHostArch();

  if (c->pack.debArchitecture[0]) {
    copyStr(out, n, c->pack.debArchitecture);

    /* pack.deb.architecture diset manual (lihat Buildfile) tapi tidak
       cocok dengan arsitektur yang sebenarnya dipakai compiler untuk
       binary ini. Ini bisa disengaja (cross-compile dengan CC khusus),
       jadi hanya peringatan, bukan gagal — tapi tanpa peringatan,
       label arsitektur yang salah di .deb bisa lolos tak terdeteksi
       sampai orang lain mencoba memasangnya di perangkat target. */
    if (strcmp(host, "all") != 0 && strcmp(c->pack.debArchitecture, host) != 0) {
      fprintf(stderr,
              "satria: pack: peringatan: pack.deb.architecture='%s' tidak cocok dengan "
              "arsitektur compiler saat ini ('%s'); binary yang dikemas kemungkinan "
              "dikompilasi untuk '%s', bukan '%s'. Pastikan ini memang cross-build yang "
              "disengaja — jika tidak, paket .deb ini tidak akan bisa dijalankan di "
              "perangkat '%s'.\n",
              c->pack.debArchitecture, host, host, c->pack.debArchitecture,
              c->pack.debArchitecture);
    }
    return;
  }

  copyStr(out, n, host);
}

/* ==================== staging data ==================== */

/* Mode file di paket: komponen pertama bin/ dan file .so* executable. */
static unsigned debFileMode(const char *relPath) {
  const char *slash = strchr(relPath, '/');
  size_t firstLen = slash ? (size_t)(slash - relPath) : strlen(relPath);
  bool isBin = firstLen == 3 && strncmp(relPath, "bin", 3) == 0;
  const char *base = slash ? slash + 1 : relPath;
  bool isLib = strstr(base, ".so") != NULL;
  return (isBin || isLib) ? 0755 : 0644;
}

static bool copyFileBytes(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (!in) return false;
  FILE *out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  unsigned char buf[65536];
  size_t got;
  bool ok = true;
  while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
    if (fwrite(buf, 1, got, out) != got) {
      ok = false;
      break;
    }
  ok = ok && feof(in) != 0; /* fread berhenti karena EOF, bukan error */
  fclose(in);
  if (fclose(out) != 0) ok = false;
  return ok;
}

/* Mode final file di paket: executable bila aturan (bin/.so) ATAU file
   sumbernya executable (bit +x dipertahankan, mis. app/bin/app). */
static void debSetFileMode(const char *srcPath, const char *relPath, const char *dest) {
  unsigned mode = debFileMode(relPath);
  unsigned srcMode = fsGetMode(srcPath);
  if (srcMode & 0111u) mode = 0755;
  fsSetMode(dest, mode);
}

/* Salin seluruh entri pack.files ke <dataDir>/<prefixRel>/... .
   Folder di-walk rekursif (walkDir menghasilkan path file relatif root). */
static bool debStageData(const Config *c, const char *dataDir, const char *prefixRel) {
  for (int i = 0; i < c->pack.files.count; i++) {
    char src[MAX_PATH * 2], dstRoot[MAX_PATH * 2];
    if (!packEntryParts(c->pack.files.items[i], src, sizeof(src), dstRoot, sizeof(dstRoot)))
      return false;

    /* Fallback .exe — sama dengan packStageEntries; tanpa ini .deb di
       Windows dibangun tanpa binary secara diam-diam. */
    char eff[MAX_PATH * 2];
    packResolveSource(src, eff, sizeof(eff));

    if (fsFileExists(eff)) {
      char dest[MAX_PATH * 3];
      snprintf(dest, sizeof(dest), "%s/%s/%s", dataDir, prefixRel, dstRoot);
      mkparent(dest);
      if (!copyFileBytes(eff, dest)) {
        fprintf(stderr, "satria: pack: gagal menyalin %s\n", eff);
        return false;
      }
      debSetFileMode(eff, dstRoot, dest);
      continue;
    }

    List files = {0};
    walkDir(eff, "", &files);
    /* Urutan salin = urutan file di staging data/ dan tar.gz yang dirakit
       darinya: sortir sekali pada data lengkap agar paket .deb reprodusible
       antar mesin (urutan readdir tidak lagi melekat pada hasil). */
    listSort(&files);
    size_t srcLen = strlen(eff);
    for (int j = 0; j < files.count; j++) {
      const char *file = files.items[j];
      const char *suffix = file + srcLen;
      if (*suffix == '/') suffix++;
      char dest[MAX_PATH * 3];
      if (*suffix)
        snprintf(dest, sizeof(dest), "%s/%s/%s/%s", dataDir, prefixRel, dstRoot, suffix);
      else
        snprintf(dest, sizeof(dest), "%s/%s/%s", dataDir, prefixRel, dstRoot);

      mkparent(dest);
      char rel[MAX_PATH * 3];
      if (*suffix) {
        snprintf(rel, sizeof(rel), "%s/%s", dstRoot, suffix);
      } else {
        copyStr(rel, sizeof(rel), dstRoot);
      }

      if (!copyFileBytes(file, dest)) {
        fprintf(stderr, "satria: pack: gagal menyalin %s\n", file);
        listFree(&files);
        return false;
      }
      debSetFileMode(file, rel, dest);
    }
    listFree(&files);
  }
  return true;
}

/* ==================== control ==================== */

static bool debWriteControl(const Config *c, const char *ctlDir, const char *arch,
                            long long installedKb) {
  /* ctlDir berakar di stage pendek (".satria/pack") — rantai buffer di bawah
     diukur dari situ agar -Wformat-truncation senyap. */
  char path[128];
  snprintf(path, sizeof(path), "%s/control", ctlDir);
  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "satria: pack: tidak bisa menulis %s\n", path);
    return false;
  }
  fprintf(fp, "Package: %s\n", c->pack.name);
  fprintf(fp, "Version: %s\n", c->pack.version);
  fprintf(fp, "Architecture: %s\n", arch);
  fprintf(fp, "Maintainer: %s\n",
          c->pack.debMaintainer[0] ? c->pack.debMaintainer : "satria <satria@localhost>");
  fprintf(fp, "Installed-Size: %lld\n", installedKb);
  fprintf(fp, "Description: %s\n",
          c->pack.debDescription[0] ? c->pack.debDescription : c->pack.name);
  bool ok = fclose(fp) == 0;
  if (!ok) fprintf(stderr, "satria: pack: gagal menulis control\n");
  return ok;
}

/* Total byte file di bawah dir (untuk Installed-Size). */
static long long debDataSize(const char *dir) {
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  long long total = 0;
  for (int i = 0; i < files.count; i++) {
    long long sz = fsFileSize(files.items[i]);
    if (sz > 0) total += sz;
  }
  for (int i = 0; i < dirs.count; i++)
    total += debDataSize(dirs.items[i]);
  listFree(&dirs);
  listFree(&files);
  return total;
}

/* ==================== ar archive ==================== */

/* Header member 60 byte: nama(16) mtime(12) uid(6) gid(6) mode(8) size(10)
   magic("`\\n")(2) — gaya dpkg-deb (nama space-padded, tanpa '/'). */
static bool arHeader(FILE *fp, const char *name, long long size) {
  char hdr[64];
  int nw = snprintf(hdr, sizeof(hdr), "%-16.16s%-12ld%-6s%-6s%-8s%-10lld`\n", name,
                    (long)time(NULL), "0", "0", "100644", size);
  if (nw != 60) return false;
  return fwrite(hdr, 1, 60, fp) == 60;
}

static bool arMemberBuf(FILE *fp, const char *name, const void *data, size_t n) {
  if (!arHeader(fp, name, (long long)n)) return false;
  if (n && fwrite(data, 1, n, fp) != n) return false;
  if (n & 1 && fputc('\n', fp) == EOF) return false; /* data di-pad genap */
  return true;
}

static bool arMemberFile(FILE *fp, const char *name, const char *path) {
  long long size = fsFileSize(path);
  if (size < 0) return false;
  if (!arHeader(fp, name, size)) return false;
  FILE *in = fopen(path, "rb");
  if (!in) return false;
  unsigned char buf[65536];
  size_t got;
  long long copied = 0;
  bool ok = true;
  while (ok && copied < size && (got = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, got, fp) != got)
      ok = false;
    else
      copied += (long long)got;
  }
  fclose(in);
  if (!ok || copied != size) return false;
  if (size & 1 && fputc('\n', fp) == EOF) return false;
  return true;
}

/* ==================== packBuildDeb ==================== */

bool packBuildDeb(const Config *c, const PackArtifact *a) {
  char stage[64];
  packStageDir(stage, sizeof(stage));

  /* install_prefix wajib absolut; relatif terhadap '/' jadi path staging. */
  const char *prefix = c->pack.debInstallPrefix;
  while (*prefix == '/')
    prefix++;
  if (!*prefix) {
    fprintf(stderr, "satria: pack: pack.deb.install_prefix harus absolut (mis. /usr/local)\n");
    return false;
  }

  /* Rantai staging: debRoot/ctlDir/dataDir/tar — semuanya turunan stage
     (".satria/pack", <64 char); ukuran buffer dihitung dari rantai itu. */
  char debRoot[96], ctlDir[112], dataDir[112];
  snprintf(debRoot, sizeof(debRoot), "%s/deb", stage);
  snprintf(ctlDir, sizeof(ctlDir), "%s/control", debRoot);
  snprintf(dataDir, sizeof(dataDir), "%s/data", debRoot);
  fsRemoveTree(debRoot);
  mkdirs(ctlDir);
  mkdirs(dataDir);

  if (!debStageData(c, dataDir, prefix)) return false;

  char arch[32];
  debArch(c, arch, sizeof(arch));
  long long kb = (debDataSize(dataDir) + 1023) / 1024;
  if (!debWriteControl(c, ctlDir, arch, kb)) return false;

  /* tar+gzip kedua pohon staging. */
  char ctlTar[128], dataTar[128];
  char cmd[320];
  snprintf(ctlTar, sizeof(ctlTar), "%s/control.tar.gz", debRoot);
  snprintf(dataTar, sizeof(dataTar), "%s/data.tar.gz", debRoot);
  fsRemoveFile(ctlTar);
  fsRemoveFile(dataTar);
  snprintf(cmd, sizeof(cmd), "tar czf %s -C %s .", ctlTar, ctlDir);
  if (!runCmd(cmd)) {
    fprintf(stderr, "satria: pack: tar control gagal\n");
    return false;
  }
  snprintf(cmd, sizeof(cmd), "tar czf %s -C %s .", dataTar, dataDir);
  if (!runCmd(cmd)) {
    fprintf(stderr, "satria: pack: tar data gagal\n");
    return false;
  }

  /* Rakit ar archive: debian-binary + control.tar.gz + data.tar.gz. */
  fsRemoveFile(a->tmp);
  mkparent(a->tmp);
  FILE *fp = fopen(a->tmp, "wb");
  if (!fp) {
    fprintf(stderr, "satria: pack: tidak bisa menulis %s\n", a->tmp);
    return false;
  }
  bool ok = fwrite("!<arch>\n", 1, 8, fp) == 8 && arMemberBuf(fp, "debian-binary", "2.0\n", 4) &&
            arMemberFile(fp, "control.tar.gz", ctlTar) && arMemberFile(fp, "data.tar.gz", dataTar);
  if (fclose(fp) != 0) ok = false;
  if (!ok) {
    fsRemoveFile(a->tmp);
    fprintf(stderr, "satria: pack: gagal merakit ar %s\n", a->path);
    return false;
  }

  mkparent(a->path);
  if (!fsRenameReplace(a->tmp, a->path)) {
    fsRemoveFile(a->tmp);
    fprintf(stderr, "satria: pack: tidak bisa menempatkan %s\n", a->path);
    return false;
  }

  fsRemoveTree(debRoot); /* staging tar.gz tidak dipakai lagi */
  return true;
}

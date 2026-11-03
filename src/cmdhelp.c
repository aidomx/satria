/*
 * cmdhelp — sumber bantuan tunggal src/cmd.txt.
 *
 * Meniru ~/rupa/core/src/prompt/prompt.c: file berisi section
 * `---<name> ... ---end<name>`; loader membaca file, mengekstrak section
 * by name, dan mencetaknya. Tidak ada konten help yang di-hardcode di C
 * (kecuali fallback saat cmd.txt tidak ditemukan di jalur manapun).
 */
#include "cmdhelp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#define CMD_TXT_MAX (64 * 1024)

static char *cmdReadFile(const char *path, char *buf, size_t n) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  size_t got = fread(buf, 1, n - 1, fp);
  fclose(fp);
  buf[got] = '\0';
  /* Normalisasi CRLF/CR -> LF. File yang di-checkout di Windows (git
     core.autocrlf) memakai "\r\n", sehingga baris marker terbaca
     "---help\r": panjang namanya ikut menghitung '\r' dan section tidak
     pernah cocok — `satria help` keluar kosong tanpa pesan apa pun. Selain
     itu isi section dicetak lewat fwrite ke stdout mode teks (di Windows
     '\n' sudah diterjemahkan jadi CRLF), jadi '\r' sisa akan tampil sebagai
     "\r\r\n". */
  size_t w = 0;
  for (size_t r = 0; r < got; r++) {
    if (buf[r] == '\r') {
      if (r + 1 < got && buf[r + 1] == '\n') continue; /* CRLF -> LF */
      buf[w++] = '\n';                                 /* CR tunggal -> LF */
      continue;
    }
    buf[w++] = buf[r];
  }
  buf[w] = '\0';
  return strstr(buf, "---") ? buf : NULL;
}

/* Direktori binary yang sedang berjalan (tanpa nama file). */
static bool cmdExeDir(char *buf, size_t n) {
#if defined(__linux__)
  ssize_t len = readlink("/proc/self/exe", buf, n - 1);
  if (len <= 0) return false;
  buf[len] = '\0';
#elif defined(__APPLE__)
  uint32_t size = (uint32_t)n;
  if (_NSGetExecutablePath(buf, &size) != 0) return false;
#elif defined(_WIN32)
  /* GetModuleFileNameA memberi path ber-backslash; dinormalkan ke '/' agar
     sisa fungsi ini (dan pencarian jalur di bawah) platform-netral. */
  DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)n);
  if (len == 0 || len >= (DWORD)n) return false;
  buf[len] = '\0';
  pathNormalizeSlash(buf);
#else
  (void)n;
  return false;
#endif
  char *slash = strrchr(buf, '/');
  if (!slash) return false;
  *slash = '\0';
  return true;
}

static bool cmdLoadText(char *buf, size_t n) {
  const char *env = getenv("SATRIA_CMD");
  if (env && cmdReadFile(env, buf, n)) return true;
  if (cmdReadFile("src/cmd.txt", buf, n)) return true;
  char base[1024];
  if (cmdExeDir(base, sizeof(base))) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/../src/cmd.txt", base);
    if (cmdReadFile(path, buf, n)) return true;
    snprintf(path, sizeof(path), "%s/cmd.txt", base);
    if (cmdReadFile(path, buf, n)) return true;
    snprintf(path, sizeof(path), "%s/../share/satria/cmd.txt", base);
    if (cmdReadFile(path, buf, n)) return true;
  }
  return false;
}

/* Cari section `---<name>` ... `---end<name>` dalam text. */
static bool cmdExtractSection(const char *text, const char *name,
                              const char **out, size_t *outLen) {
  char *p = (char *)text;
  char *start = NULL;
  bool open = false;
  while (*p) {
    char *lineEnd = strchr(p, '\n');
    size_t lineLen = lineEnd ? (size_t)(lineEnd - p) : strlen(p);
    bool isMarker = lineLen >= 3 && strncmp(p, "---", 3) == 0;
    if (isMarker && lineLen > 6 && strncmp(p, "---end", 6) == 0) {
      if (open) {
        size_t content = (size_t)(p - start);
        if (content > 0 && start[content - 1] == '\n') content--;
        *out = start;
        *outLen = content;
        return true;
      }
    } else if (isMarker && !open) {
      size_t nameLen = lineLen - 3;
      if (nameLen == strlen(name) && strncmp(p + 3, name, nameLen) == 0) {
        open = true;
        start = lineEnd ? lineEnd + 1 : p + lineLen;
      }
    }
    p = lineEnd ? lineEnd + 1 : p + lineLen;
  }
  return false;
}

static bool helpPrintSection(const char *name) {
  static char text[CMD_TXT_MAX];
  if (!cmdLoadText(text, sizeof(text))) {
    /* Fallback minimal bila cmd.txt tidak ditemukan di semua jalur
       (mis. binary terpasang tanpa file), agar `satria help` tidak
       pernah kosong. */
    if (strcmp(name, "help") == 0) {
      printf("Satria - A simple builder for you\n\n");
      printf("> satria [-f <file>] [-jN] <command>\n\n");
      printf("(no command)  build project from Buildfile\n");
      printf("init          create a minimal Buildfile\n");
      printf("clean         remove build artifacts\n");
      printf("profile <ctx> measure operation costs (see: help profile)\n");
      printf("-w            workspace mode (Buildfile.ws)\n");
      printf("help <topic>  show topic help (build, profile, workspace,\n");
      printf("              init, clean, release, compdb, ninja)\n");
      printf("version       show version\n");
      fprintf(stderr,
              "satria: cmd.txt tidak ditemukan — set $SATRIA_CMD atau jalankan dari repo\n");
      return true;
    }
    return false;
  }
  const char *content = NULL;
  size_t len = 0;
  if (!cmdExtractSection(text, name, &content, &len)) return false;
  fwrite(content, 1, len, stdout);
  if (len == 0 || content[len - 1] != '\n') printf("\n");
  return true;
}

bool cmdHelpSection(const char *name) {
  if (!name || !*name) return false;
  /* Alias topic lama. */
  if (strcmp(name, "compdb") == 0) return helpPrintSection("compdb");
  return helpPrintSection(name);
}

void cmdHelpMain(void) { helpPrintSection("help"); }

bool cmdHelpKnown(const char *name) {
  if (!name || !*name) return false;
  static char text[CMD_TXT_MAX];
  if (!cmdLoadText(text, sizeof(text))) return cmdHelpSection(name);
  const char *content = NULL;
  size_t len = 0;
  return cmdExtractSection(text, name, &content, &len);
}

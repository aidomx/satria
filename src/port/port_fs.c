/*
 * port_fs — lapisan FILESYSTEM dari portability (kedua OS).
 *
 * Dipindah apa adanya dari portability.c: stat/exists/mtime/size,
 * mkdir/remove (termasuk rm -rf), listdir, dan cwd. Satu-satunya helper
 * internal adalah toBackslash() untuk API Win32 yang menolak '/'.
 * Deklarasi publik tetap di portability.h.
 */
#include "../portability.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <io.h>
#include <windows.h>
/* windows.h WAJIB paling depan di antara header Windows.
   shellapi.h memakai DECLSPEC_IMPORT dan STDAPICALLTYPE yang
   didefinisikan di winnt.h — hanya ditarik oleh windows.h. */
#include <memoryapi.h>
#include <shellapi.h>

/* MSVC tidak (selalu) menyediakan typedef mode_t — itu POSIX-only.
   struct _stat memakai unsigned short untuk st_mode. */
#ifndef SATRIA_MODE_T_DEFINED
typedef unsigned short mode_t;
#define SATRIA_MODE_T_DEFINED
#endif

#include <sys/stat.h>
#include <sys/types.h>

#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef _WIN32
/* ========== Windows: Filesystem ========== */

/* Ganti '/' ke '\\' — hanya untuk API Win32 yang menolak '/'. Jangan dipakai
   untuk command line: cmd.exe menerima '/' di path, sedangkan switch-nya
   (mis. /I, /Fo, /link) justru rusak bila diubah jadi '\\'. */
static void toBackslash(char *dst, size_t n, const char *src) {
  size_t j = 0;
  for (const char *p = src; *p && j + 1 < n; p++, j++)
    dst[j] = (*p == '/') ? '\\' : *p;
  dst[j] = '\0';
}

static bool statInfo(const char *path, WIN32_FILE_ATTRIBUTE_DATA *fad) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  return GetFileAttributesExA(tmp, GetFileExInfoStandard, fad) != 0;
}

bool fsFileExists(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return false;
  return (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool fsDirExists(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return false;
  return (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static time_t filetimeToTimeT(const FILETIME *ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft->dwLowDateTime;
  u.HighPart = ft->dwHighDateTime;
  if (u.QuadPart == 0) return (time_t)-1;
  /* 100ns sejak 1601 -> detik sejak epoch UNIX */
  return (time_t)((u.QuadPart / 10000000ULL) - 11644473600ULL);
}

time_t fsMTime(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return (time_t)-1;
  return filetimeToTimeT(&fad.ftLastWriteTime);
}

int64_t fsMTimeNs(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return -1;
  ULARGE_INTEGER u;
  u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
  u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
  if (u.QuadPart == 0) return -1;
  /* FILETIME is already expressed in 100ns units since 1601. */
  return (int64_t)u.QuadPart * 100LL;
}

long long fsFileSize(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return -1;
  ULARGE_INTEGER u;
  u.LowPart = fad.nFileSizeLow;
  u.HighPart = fad.nFileSizeHigh;
  return (long long)u.QuadPart;
}

bool fsStampNsSize(const char *path, int64_t *mtimeNs, long long *size) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return false;
  ULARGE_INTEGER u;
  u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
  u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
  if (u.QuadPart == 0) return false;
  *mtimeNs = (int64_t)u.QuadPart * 100LL;
  u.LowPart = fad.nFileSizeLow;
  u.HighPart = fad.nFileSizeHigh;
  *size = (long long)u.QuadPart;
  return true;
}

void fsMakeDir(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  CreateDirectoryA(tmp, NULL); /* diam bila sudah ada */
}

bool fsRemoveFile(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  return DeleteFileA(tmp) != 0;
}

/* Ganti `to` dengan `from`: MoveFileEx REPLACE_EXISTING|WRITE_THROUGH —
   atomik pada filesystem yang mendukung. Bila `to` ditahan handle pihak
   ketiga (antivirus/indexer, ERROR_ACCESS_DENIED), fallback salin + hapus
   (rekomendasi design/startup.md §4). */
bool fsRenameReplace(const char *from, const char *to) {
  char f[MAX_PATH * 2], t[MAX_PATH * 2];
  toBackslash(f, sizeof(f), from);
  toBackslash(t, sizeof(t), to);
  if (MoveFileExA(f, t, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
  if (CopyFileA(f, t, FALSE)) {
    DeleteFileA(f);
    return true;
  }
  return false;
}

bool fsSetMode(const char *path, unsigned mode) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  return _chmod(tmp, (int)mode) == 0;
}

unsigned fsGetMode(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  struct _stat st;
  if (_stat(tmp, &st) != 0) return 0;
  return (unsigned)st.st_mode & (unsigned)(_S_IREAD | _S_IWRITE | _S_IEXEC);
}

bool fsRemoveTree(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  /* SHFileOperation butuh path dobel-NUL-terminated */
  char doubled[MAX_PATH * 2 + 2];
  size_t len = strlen(tmp);
  if (len + 2 > sizeof(doubled)) return false;
  memcpy(doubled, tmp, len + 1);
  doubled[len + 1] = '\0';

  SHFILEOPSTRUCTA op;
  memset(&op, 0, sizeof(op));
  op.hwnd = NULL;
  op.wFunc = FO_DELETE;
  op.pFrom = doubled;
  op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  return SHFileOperationA(&op) == 0;
}

void fsListDir(const char *dir, List *dirs, List *files) {
  char pattern[MAX_PATH * 2];
  toBackslash(pattern, sizeof(pattern), dir);
  size_t plen = strlen(pattern);
  if (plen + 3 > sizeof(pattern)) return;
  pattern[plen] = '\\';
  pattern[plen + 1] = '*';
  pattern[plen + 2] = '\0';

  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const char *name = fd.cFileName;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (dirs) listAdd(dirs, path);
    } else {
      if (files) listAdd(files, path);
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

bool fsGetCwd(char *out, size_t n) {
  char tmp[MAX_PATH * 2];
  DWORD len = GetCurrentDirectoryA((DWORD)sizeof(tmp), tmp);
  if (len == 0 || len > sizeof(tmp)) return false;
  pathNormalizeSlash(tmp);
  if ((size_t)len >= n) return false;
  memcpy(out, tmp, len + 1);
  return true;
}

bool fsSetCwd(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  return SetCurrentDirectoryA(tmp) != 0;
}

/* MapViewOfFile read-only (Windows). Deallocasi: UnmapViewOfFile. */
bool fsMapRead(const char *path, void **outData, size_t *outSize) {
  *outData = NULL;
  *outSize = 0;
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  HANDLE hFile = CreateFileA(tmp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(hFile, &sz)) {
    CloseHandle(hFile);
    return false;
  }
  HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!hMap) {
    CloseHandle(hFile);
    return false;
  }
  if (sz.QuadPart <= 0) { /* file kosong: marker valid, size 0 */
    CloseHandle(hMap);
    CloseHandle(hFile);
    *outData = (void *)(intptr_t)1;
    *outSize = 0;
    return true;
  }
  void *view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(hMap);
  CloseHandle(hFile);
  if (!view) return false;
  *outData = view;
  *outSize = (size_t)sz.QuadPart;
  return true;
}

void fsMapClose(void *data, size_t size) {
  if (!data) return;
  if (size > 0) {
    if (!UnmapViewOfFile(data)) {
      fprintf(stderr, "UnmapViewOfFile is failed\n");
    }
  }
  /* marker file kosong (1) tidak perlu dibebaskan */
}

#else /* !_WIN32 */

/* ========== POSIX: Filesystem ========== */

static bool statMode(const char *path, mode_t *mode) {
  struct stat st;
  if (stat(path, &st) != 0) return false;
  *mode = st.st_mode;
  return true;
}

bool fsFileExists(const char *path) {
  mode_t m;
  return statMode(path, &m) && S_ISREG(m);
}

bool fsDirExists(const char *path) {
  mode_t m;
  return statMode(path, &m) && S_ISDIR(m);
}

time_t fsMTime(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return (time_t)-1;
  return st.st_mtime;
}

int64_t fsMTimeNs(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
#if defined(__APPLE__)
  return (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

long long fsFileSize(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long long)st.st_size;
}

bool fsStampNsSize(const char *path, int64_t *mtimeNs, long long *size) {
  struct stat st;
  if (stat(path, &st) != 0) return false;
#if defined(__APPLE__)
  *mtimeNs = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  *mtimeNs = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
  *size = (long long)st.st_size;
  return true;
}

void fsMakeDir(const char *path) {
  mkdir(path, 0755);
}

bool fsRemoveFile(const char *path) {
  return unlink(path) == 0;
}

/* POSIX: rename() sudah atomic dan mengganti tujuan — pembungkus seragam
   dengan jalur Windows (MoveFileEx) di port_fs.c bagian _WIN32. */
bool fsRenameReplace(const char *from, const char *to) {
  return rename(from, to) == 0;
}

bool fsSetMode(const char *path, unsigned mode) {
  return chmod(path, (int)mode) == 0;
}

unsigned fsGetMode(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  return (unsigned)st.st_mode & 0777u;
}

bool fsRemoveTree(const char *path) {
  /* lstat prevents following symlinks while removing package archives or
     ~/.satria; otherwise a symlink to a directory could delete external data. */
  struct stat rootStat;
  if (lstat(path, &rootStat) != 0) return true;
  if (S_ISLNK(rootStat.st_mode) || !S_ISDIR(rootStat.st_mode)) return unlink(path) == 0;
  List dirs = {0}, files = {0};
  fsListDir(path, &dirs, &files);
  bool ok = true;
  for (int i = 0; i < files.count; i++) {
    if (!fsRemoveFile(files.items[i])) ok = false;
  }
  /* hapus subdirektori paling dalam dulu (list dari fsListDir tidak
     terjamin urut, jadi recurse; urutan aman karena anak dihapus sebelum
     induknya lewat rekursi fsRemoveTree sendiri) */
  for (int i = 0; i < dirs.count; i++) {
    if (!fsRemoveTree(dirs.items[i])) ok = false;
  }
  listFree(&dirs);
  listFree(&files);
  return ok && rmdir(path) == 0;
}

void fsListDir(const char *dir, List *dirs, List *files) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    bool isDir;
#ifdef DT_DIR
    /* d_type sudah diberikan readdir: tanpa stat per entri (satu syscall per
       file/direktori yang dipindai). DT_UNKNOWN/DT_LNK/lainnya jatuh ke stat
       agar symlink tetap diikuti seperti sebelumnya. */
    if (ent->d_type == DT_DIR) {
      isDir = true;
    } else if (ent->d_type == DT_REG) {
      isDir = false;
    } else
#endif
    {
      struct stat st;
      if (stat(path, &st) != 0) continue;
      isDir = S_ISDIR(st.st_mode);
    }
    if (isDir) {
      if (dirs) listAdd(dirs, path);
    } else {
      if (files) listAdd(files, path);
    }
  }
  closedir(d);
}

bool fsGetCwd(char *out, size_t n) {
  return getcwd(out, n) != NULL;
}

bool fsSetCwd(const char *path) {
  return chdir(path) == 0;
}

/* mmap read-only (POSIX). */
bool fsMapRead(const char *path, void **outData, size_t *outSize) {
  *outData = NULL;
  *outSize = 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return false;
  }
  if (st.st_size <= 0) { /* kosong: map tak diperlukan — dibaca dgn read */
    close(fd);
    *outData = (void *)(intptr_t)1; /* marker valid, size 0, aman free */
    *outSize = 0;
    return true;
  }
  void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED) return false;
  *outData = map;
  *outSize = (size_t)st.st_size;
  return true;
}

void fsMapClose(void *data, size_t size) {
  if (!data) return;
  if (size > 0) munmap(data, size);
  /* data marker file kosong (1) tidak perlu dibebaskan */
}

#endif /* !_WIN32 */

/* ==================== Portabel (kedua OS) ==================== */

bool fsNewerThan(const char *a, const char *b) {
  int64_t ta = fsMTimeNs(a);
  if (ta < 0) return false;
  int64_t tb = fsMTimeNs(b);
  if (tb < 0) return true;
  return ta > tb;
}

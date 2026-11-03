#include "package_cli.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "portability.h"
#include "satria.h" /* satriaVersion (dipakai jalur self-upgrade) */
#include "util.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define SATRIA_GETPID() ((long)GetCurrentProcessId())
#else
#include <unistd.h>
#define SATRIA_GETPID() ((long)getpid())
#endif

#define PKG_PATH_MAX 4096

#ifndef _WIN32 /* pathJoin hanya dipakai cabang POSIX (#else di bawah) */
static bool pathJoin(char *out, size_t cap, const char *a, const char *b) {
  size_t na = strlen(a), nb = strlen(b);
  bool slash = na > 0 && a[na - 1] != '/';
  if (na + (slash ? 1u : 0u) + nb + 1 > cap) return false;
  memcpy(out, a, na);
  size_t at = na;
  if (slash) out[at++] = '/';
  memcpy(out + at, b, nb + 1);
  return true;
}
#endif

static bool expandPath(const char *in, char *out, size_t cap) {
  char tmp[PKG_PATH_MAX];
  if (!in || !*in) return false;
  if (in[0] == '~' && (in[1] == '/' || in[1] == '\\' || in[1] == '\0')) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (!home || !*home) return false;
    if (snprintf(tmp, sizeof(tmp), "%s%s", home, in + 1) >= (int)sizeof(tmp)) return false;
  } else {
    if (snprintf(tmp, sizeof(tmp), "%s", in) >= (int)sizeof(tmp)) return false;
  }
  if (tmp[0] == '/' || (isalpha((unsigned char)tmp[0]) && tmp[1] == ':')) {
    if (snprintf(out, cap, "%s", tmp) >= (int)cap) return false;
  } else {
    char cwd[PKG_PATH_MAX];
    if (!fsGetCwd(cwd, sizeof(cwd))) return false;
    if (snprintf(out, cap, "%s/%s", cwd, tmp) >= (int)cap) return false;
  }
  pathNormalizeSlash(out);
  return true;
}

static bool homeRoot(char *out, size_t cap) {
  const char *root = getenv("SATRIA_HOME");
  if (!root || !*root) {
    root = getenv("HOME");
#ifdef _WIN32
    if (!root || !*root) root = getenv("USERPROFILE");
#endif
    if (!root || !*root) {
      fprintf(stderr, "satria: HOME is not set; set SATRIA_HOME explicitly\n");
      return false;
    }
    if (snprintf(out, cap, "%s/.satria", root) >= (int)cap) return false;
  } else if (!expandPath(root, out, cap))
    return false;
  pathNormalizeSlash(out);
  /* Refuse dangerous roots. */
  if (!strcmp(out, "/") || !strcmp(out, ".") || !strcmp(out, "..") || !*out) {
    fprintf(stderr, "satria: unsafe SATRIA_HOME path\n");
    return false;
  }
  return true;
}

/* Quote a value for the command shell. POSIX uses single quotes; Windows
   command strings are double-quoted and reject shell metacharacters in paths. */
static bool shellQuote(const char *s, char *out, size_t cap) {
  size_t w = 0;
#ifdef _WIN32
  if (w + 1 >= cap) return false;
  out[w++] = '"';
  for (; *s; ++s) {
    if (*s == '"' || *s == '%' || *s == '!' || *s == '\n' || *s == '\r') return false;
    if (*s == '\\' && s[1] == '"') {
      if (w + 2 >= cap) return false;
      out[w++] = '\\';
    }
    if (w + 1 >= cap) return false;
    out[w++] = *s;
  }
  if (w + 2 > cap) return false;
  out[w++] = '"';
  out[w] = '\0';
#else
  if (w + 1 >= cap) return false;
  out[w++] = '\'';
  for (; *s; ++s) {
    if (*s == '\'') {
      const char *q = "'\\''";
      for (int i = 0; q[i]; ++i) {
        if (w + 1 >= cap) return false;
        out[w++] = q[i];
      }
    } else {
      if (w + 1 >= cap) return false;
      out[w++] = *s;
    }
  }
  if (w + 2 > cap) return false;
  out[w++] = '\'';
  out[w] = '\0';
#endif
  return true;
}

static int cmdSelfUpgrade(int argc, const char *argv0) {
  (void)argv0; /* dipakai hanya di cabang POSIX/Windows tertentu */
  if (argc != 2) {
    fprintf(stderr, "usage: satria self-upgrade\n");
    return 2;
  }
#ifdef _WIN32
  /* Windows locks the running executable. Validate the standard per-user
     install path, then launch a separate PowerShell process that waits for
     this process to exit before downloading/installing the selected release. */
  char exe[PKG_PATH_MAX], expected[PKG_PATH_MAX], localAppData[PKG_PATH_MAX];
  DWORD exeLen = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
  DWORD homeLen = GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                           (DWORD)sizeof(localAppData));
  if (!exeLen || exeLen >= sizeof(exe) || !homeLen || homeLen >= sizeof(localAppData)) {
    fprintf(stderr, "satria: cannot resolve the Windows installation path\n");
    return 2;
  }
  while (homeLen > 0 && (localAppData[homeLen - 1] == '\\' ||
                         localAppData[homeLen - 1] == '/'))
    localAppData[--homeLen] = '\0';
  if (fsFileExists("src/main.c") && fsDirExists(".git")) {
    fprintf(stderr,
            "satria: this is a source checkout; update it with 'git pull' and rebuild satria\n");
    return 2;
  }
  if (snprintf(expected, sizeof(expected), "%s\\satria\\satria.exe", localAppData) >=
      (int)sizeof(expected)) {
    fprintf(stderr, "satria: Windows installation path is too long\n");
    return 2;
  }
  for (char *p = exe; *p; ++p) if (*p == '/') *p = '\\';
  for (char *p = expected; *p; ++p) if (*p == '/') *p = '\\';
  if (_stricmp(exe, expected) != 0) {
    fprintf(stderr,
            "satria: refusing to replace a custom binary path: %s\n"
            "hint: run the official install.ps1 manually for that installation\n", exe);
    return 2;
  }

  char tempDir[SATRIA_MAX_PATH], tempFile[SATRIA_MAX_PATH], scriptPath[SATRIA_MAX_PATH];
  DWORD tempLen = GetTempPathA((DWORD)sizeof(tempDir), tempDir);
  if (!tempLen || tempLen >= sizeof(tempDir) ||
      !GetTempFileNameA(tempDir, "rbu", 0, tempFile)) {
    fprintf(stderr, "satria: cannot create the self-upgrade helper script\n");
    return 1;
  }
  if (snprintf(scriptPath, sizeof(scriptPath), "%s", tempFile) >= (int)sizeof(scriptPath)) {
    DeleteFileA(tempFile);
    return 1;
  }
  char *ext = strrchr(scriptPath, '.');
  if (!ext || (size_t)(ext - scriptPath) + 5 >= sizeof(scriptPath)) {
    DeleteFileA(tempFile);
    return 1;
  }
  strcpy(ext, ".ps1");
  if (!MoveFileA(tempFile, scriptPath)) {
    DeleteFileA(tempFile);
    fprintf(stderr, "satria: cannot prepare the self-upgrade helper script\n");
    return 1;
  }
  FILE *script = fopen(scriptPath, "wb");
  if (!script) {
    DeleteFileA(scriptPath);
    fprintf(stderr, "satria: cannot write the self-upgrade helper script\n");
    return 1;
  }
  static const char helper[] =
      "param([int]$ParentPid, [string]$CurrentVersion)\r\n"
      "$ErrorActionPreference = 'Stop'\r\n"
      "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12\r\n"
      "$installer = $null\r\n"
      "try {\r\n"
      "  try { Wait-Process -Id $ParentPid -ErrorAction Stop } catch {}\r\n"
      "  $api = 'https://api.github.com/repos/aidomx/satria/releases/latest'\r\n"
      "  $release = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = 'satria-self-upgrade' }\r\n"
      "  $latest = [string]$release.tag_name\r\n"
      "  if ($latest -notmatch '^[A-Za-z0-9][A-Za-z0-9._+-]{0,126}$') { throw 'Invalid release tag from GitHub' }\r\n"
      "  if ($latest -ceq $CurrentVersion) { Write-Host ('satria is already up to date (' + $CurrentVersion + ')'); exit 0 }\r\n"
      "  Write-Host ('Upgrading satria: ' + $CurrentVersion + ' -> ' + $latest)\r\n"
      "  $installer = Join-Path $env:TEMP ('satria-install-' + [guid]::NewGuid().ToString('N') + '.ps1')\r\n"
      "  Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/aidomx/satria/$latest/install.ps1' -OutFile $installer\r\n"
      "  & $installer --version $latest\r\n"
      "  Write-Host 'satria self-upgrade completed.'\r\n"
      "} catch { Write-Error ('satria self-upgrade failed: ' + $_.Exception.Message); exit 1 }\r\n"
      "finally { if ($installer -and (Test-Path $installer)) { Remove-Item -Force $installer -ErrorAction SilentlyContinue }; Remove-Item -Force $PSCommandPath -ErrorAction SilentlyContinue }\r\n";
  size_t helperSize = sizeof(helper) - 1;
  size_t helperWritten = fwrite(helper, 1, helperSize, script);
  int helperClose = fclose(script);
  if (helperWritten != helperSize || helperClose != 0) {
    DeleteFileA(scriptPath);
    fprintf(stderr, "satria: cannot finish the self-upgrade helper script\n");
    return 1;
  }
  char powerShell[PKG_PATH_MAX], qpowershell[PKG_PATH_MAX * 2];
  DWORD psLen = SearchPathA(NULL, "powershell.exe", NULL, (DWORD)sizeof(powerShell),
                            powerShell, NULL);
  if (!psLen || psLen >= sizeof(powerShell)) {
    const char *windir = getenv("WINDIR");
    if (!windir || snprintf(powerShell, sizeof(powerShell),
                            "%s\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                            windir) >= (int)sizeof(powerShell) || !fsFileExists(powerShell)) {
      DeleteFileA(scriptPath);
      fprintf(stderr, "satria: PowerShell was not found; cannot self-upgrade\n");
      return 1;
    }
  }
  char qscript[PKG_PATH_MAX * 2], qversion[256], command[PKG_PATH_MAX * 4];
  const char *current = satriaVersion();
  if (!shellQuote(powerShell, qpowershell, sizeof(qpowershell)) ||
      !shellQuote(scriptPath, qscript, sizeof(qscript)) ||
      !shellQuote(current ? current : "unknown", qversion, sizeof(qversion))) {
    DeleteFileA(scriptPath);
    return 2;
  }
  int cmdLen = snprintf(command, sizeof(command),
                        "%s -NoProfile -ExecutionPolicy Bypass -File %s -ParentPid %lu -CurrentVersion %s",
                        qpowershell, qscript, (unsigned long)GetCurrentProcessId(), qversion);
  if (cmdLen < 0 || (size_t)cmdLen >= sizeof(command)) {
    DeleteFileA(scriptPath);
    return 2;
  }
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  memset(&pi, 0, sizeof(pi));
  si.cb = sizeof(si);
  if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
    DeleteFileA(scriptPath);
    fprintf(stderr, "satria: cannot start PowerShell self-upgrade helper (error %lu)\n",
            (unsigned long)GetLastError());
    return 1;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  printf("> Self-upgrade: PowerShell helper started; it will update satria after this process exits.\n");
  return 0;
#else
  char cwd[PKG_PATH_MAX], exe[PKG_PATH_MAX] = {0};
  if (fsGetCwd(cwd, sizeof(cwd)) && fsFileExists("src/main.c") && fsDirExists(".git")) {
    fprintf(stderr,
            "satria: this is a source checkout; update it with 'git pull' and rebuild satria\n");
    return 2;
  }
  if (argv0 && (strchr(argv0, '/') || strchr(argv0, '\\'))) {
    if (!expandPath(argv0, exe, sizeof(exe))) return 2;
  } else {
    FILE *fp = popenRB("command -v satria 2>/dev/null");
    if (fp) {
      if (fgets(exe, sizeof(exe), fp)) {
        size_t n = strlen(exe);
        while (n && (exe[n - 1] == '\n' || exe[n - 1] == '\r'))
          exe[--n] = '\0';
      }
      pcloseRB(fp);
    }
    if (*exe && exe[0] != '/') {
      char resolved[PKG_PATH_MAX];
      if (!expandPath(exe, resolved, sizeof(resolved)))
        exe[0] = '\0';
      else
        strcpy(exe, resolved);
    }
  }
  if (!*exe) {
    fprintf(stderr, "satria: cannot resolve the installed satria executable path\n");
    return 2;
  }
  char expected[PKG_PATH_MAX];
  const char *prefix = getenv("PREFIX");
  if (prefix && *prefix && fsDirExists(prefix)) {
    if (!pathJoin(expected, sizeof(expected), prefix, "bin/satria")) return 2;
  } else {
    if (strlen("/usr/local/bin/satria") >= sizeof(expected)) return 2;
    strcpy(expected, "/usr/local/bin/satria");
  }
  if (strcmp(exe, expected) != 0) {
    if (strstr(exe, "/build/bin/satria") || strstr(exe, "/bin/satria"))
      fprintf(
          stderr,
          "satria: this binary is not at the official installer path\nhint: for a source build, run "
          "'git pull' and rebuild; otherwise use install.sh for the intended install path\n");
    else
      fprintf(stderr,
              "satria: refusing to replace a custom binary path: %s\nhint: run the official "
              "installer manually for that installation\n",
              exe);
    return 2;
  }
  /* Query the official GitHub Releases API. Only accept a conservative tag
     alphabet before passing the tag to the official installer. */
  const char *api = "https://api.github.com/repos/aidomx/satria/releases/latest";
  char query[1024];
  if (probeAvailable("curl"))
    snprintf(query, sizeof(query),
             "curl -fsSL '%s' | sed -n "
             "'s/.*\"tag_name\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p' | head -n 1",
             api);
  else if (probeAvailable("wget"))
    snprintf(query, sizeof(query),
             "wget -qO- '%s' | sed -n "
             "'s/.*\"tag_name\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p' | head -n 1",
             api);
  else {
    fprintf(stderr, "satria: self-upgrade requires curl or wget\n");
    return 2;
  }
  FILE *tagPipe = popenRB(query);
  char latest[128] = {0};
  if (tagPipe) {
    if (fgets(latest, sizeof(latest), tagPipe)) {
      size_t n = strlen(latest);
      while (n && (latest[n - 1] == '\n' || latest[n - 1] == '\r'))
        latest[--n] = '\0';
    }
    pcloseRB(tagPipe);
  }
  if (!*latest || strlen(latest) >= sizeof(latest) - 1) {
    fprintf(stderr, "satria: cannot read latest release tag from GitHub\n");
    return 1;
  }
  for (const char *p = latest; *p; ++p) {
    if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_' || *p == '+')) {
      fprintf(stderr, "satria: GitHub returned an invalid release tag\n");
      return 1;
    }
  }
  extern const char *satriaVersion(void);
  const char *current = satriaVersion();
  if (current && !strcmp(current, latest)) {
    printf("> satria       : already up to date (%s)\n", current);
    return 0;
  }
  printf("> Current    : %s\n", current ? current : "unknown");
  printf("> Latest     : %s\n", latest);
  const char *tmpRoot = getenv("TMPDIR");
  if (!tmpRoot || !*tmpRoot) tmpRoot = "/tmp";
  char tmp[PKG_PATH_MAX], tmpName[128], qtmp[PKG_PATH_MAX * 2], qtag[256], cmd[PKG_PATH_MAX * 7];
  snprintf(tmpName, sizeof(tmpName), "satria-self-upgrade-%ld.sh", SATRIA_GETPID());
  if (!pathJoin(tmp, sizeof(tmp), tmpRoot, tmpName) || !shellQuote(tmp, qtmp, sizeof(qtmp)) ||
      !shellQuote(latest, qtag, sizeof(qtag)))
    return 2;
  if (probeAvailable("curl"))
    snprintf(cmd, sizeof(cmd),
             "curl -fsSL https://raw.githubusercontent.com/aidomx/satria/main/install.sh -o %s && sh "
             "%s --version %s; rc=$?; rm -f %s; exit $rc",
             qtmp, qtmp, qtag, qtmp);
  else if (probeAvailable("wget"))
    snprintf(cmd, sizeof(cmd),
             "wget -q https://raw.githubusercontent.com/aidomx/satria/main/install.sh -O %s && sh %s "
             "--version %s; rc=$?; rm -f %s; exit $rc",
             qtmp, qtmp, qtag, qtmp);
  else {
    fprintf(stderr, "satria: self-upgrade requires curl or wget\n");
    return 2;
  }
  printf("> Self-upgrade: official release installer\n");
  if (!procRun(cmd)) {
    fprintf(stderr, "satria: self-upgrade failed; the installer did not report success\n");
    return 1;
  }
  return 0;
#endif
}

static bool confirmSelfUninstall(void) {
  if (!termIsTTY()) return false;
  char line[64];
  printf("This will remove satria and all globally managed state.\nAre you sure? [y/N] ");
  fflush(stdout);
  if (!fgets(line, sizeof(line), stdin)) return false;
  return line[0] == 'y' || line[0] == 'Y';
}

static int cmdSelfUninstall(int argc, const char *argv[], const char *argv0) {
  bool yes = false;
  if (argc == 3 && (!strcmp(argv[2], "--yes") || !strcmp(argv[2], "-y")))
    yes = true;
  else if (argc != 2) {
    fprintf(stderr, "usage: satria self-uninstall [--yes|-y]\n");
    return 2;
  }
  if (!termIsTTY() && !yes) {
    fprintf(stderr, "satria: self-uninstall requires a TTY confirmation or --yes\n");
    return 2;
  }
  if (termIsTTY() && !yes && !confirmSelfUninstall()) {
    printf("> Cancelled\n");
    return 1;
  }
  char home[PKG_PATH_MAX];
  if (!homeRoot(home, sizeof(home))) return 1;
  /* Guard against accidental deletion of HOME or an arbitrary root. */
  if (!strstr(home, "/.satria") && !getenv("SATRIA_HOME")) {
    fprintf(stderr, "satria: refusing to remove unexpected global state path\n");
    return 2;
  }
  if (fsDirExists(home) && !fsRemoveTree(home)) {
    fprintf(stderr, "satria: could not remove global state at %s\n", home);
    return 1;
  }
  /* Only remove the executable if it is explicitly inside SATRIA_HOME/bin. */
  if (argv0 && *argv0) {
    char exe[PKG_PATH_MAX], bin[PKG_PATH_MAX];
    if (expandPath(argv0, exe, sizeof(exe)) &&
        snprintf(bin, sizeof(bin), "%s/bin/", home) < (int)sizeof(bin)) {
      size_t n = strlen(bin);
      if (!strncmp(exe, bin, n) && fsFileExists(exe)) fsRemoveFile(exe);
    }
  }
  printf("> Removed    : satria global state (%s)\n", home);
  return 0;
}

int packageCliRun(int argc, const char *argv[], int *status) {
  if (!argv || argc < 2 || !argv[1]) return 0;
  const char *cmd = argv[1];
  int rc;
  if (!strcmp(cmd, "self-upgrade"))
    rc = cmdSelfUpgrade(argc, argv[0]);
  else if (!strcmp(cmd, "self-uninstall"))
    rc = cmdSelfUninstall(argc, argv, argv[0]);
  else
    return 0;
  if (status) *status = rc;
  return 1;
}

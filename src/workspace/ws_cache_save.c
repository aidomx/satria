/* ws_cache_save.c — Cache save: menulis fingerprint.cache dan peta P bermetadata
 * di sources.cache sesudah build sukses. Baris P: <alamat> <tipe> <content>
 * <time> <size> <path> — metadata inilah yang dipakai fast path no-op (§4).
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"
#include "../cache/artifact.h"
#include "../cache/router.h"
#include "../cache/state_blob.h"
#include "../prof/prof.h" /* profMark: sub-fase ws-cachesave (audit overhead) */
#include <errno.h> /* wsCacheAddDepFile: ENOENT membedakan .d hilang dari gagal baca */
#include <time.h> /* wsNowNs: clock_gettime */

static bool wsCacheAdd(FILE *pf, CsStore *st, uint64_t proj, const char *path, char type);
static void wsCacheDirOf(const char *path, char *out, size_t n);
/* 1 = ok; 0 = file .d tak ada (ENOENT); -1 = gagal baca/parse. Satu fopen
   menggantikan fsFileExists+fopen (dua syscall path-walk per object). */
static int wsCacheAddDepFile(FILE *pf, CsStore *st, uint64_t proj, const char *dfile);
static bool wsCacheAddProjectDeps(FILE *pf, CsStore *st, uint64_t proj, const char *name);

/* Hash isi dari fingerprint.cache sebelumnya (PERF-01). Dipakai ulang hanya
   bila (time, size) cocok DAN entri tidak racy: time < mtime file cache lama
   (startup.md, pengaman racy timestamp). Selain itu isi di-hash ulang. */
static CsStore *g_prevFp = NULL;
static long long g_prevFpMtime = 0;
static void wsCacheSaveBody(const WsModel *m, const char *wsName);

static bool wsCacheAdd(FILE *pf, CsStore *st, uint64_t proj, const char *path, char type) {
  /* Outputs belong to the workspace as a whole: project-specific discovery
     and the final bin/lib scan may encounter the same physical output.
     Store each output path once, while retaining all alias-only outputs. */
  if (type == 'O') {
    proj = csHash("workspace");
    CsEntry existing;
    if (csGet(st, proj, csHash(path), &existing)) return true;
  }
  struct stat sb;
  if (stat(path, &sb) != 0) return false;
  CsEntry e = {0}; /* wajib: payload harus NULL — csPut menyalinnya (strdup) */
  e.proj = proj;
  e.file = csHash(path);
  e.time = wsCacheStampNs(&sb);
  if (type == 'D') {
    e.size = 0;
    e.content = 0;
  } else {
    e.size = (long long)sb.st_size;
    CsEntry prev;
    if (g_prevFp && csGet(g_prevFp, proj, e.file, &prev) && prev.time == e.time &&
        prev.size == e.size && e.time < g_prevFpMtime) {
      e.content = prev.content; /* metadata cocok & tidak racy: isi tidak berubah */
    } else {
      e.content = wsStartupHash(path);
    }
    if (!e.content) return false;
  }
  fprintf(pf, "%016llx %c %016llx %lld %lld %s\n", (unsigned long long)e.file, type,
          (unsigned long long)e.content, e.time, e.size, path);
  return csPut(st, &e);
}
/* Direktori induk sebuah path relatif ("a/src/main.c" -> "a/src", "main.c" -> "."). */
static void wsCacheDirOf(const char *path, char *out, size_t n) {
  snprintf(out, n, "%s", path);
  char *slash = strrchr(out, '/');
  if (slash) *slash = '\0';
  else snprintf(out, n, ".");
}
/* Dependensi header dari file .d (hasil -MMD): setiap token sesudah ':' dicatat
   sebagai entri H, divalidasi seperti sumber. 0 = .d tak ada, -1 = gagal. */
static int wsCacheAddDepFile(FILE *pf, CsStore *st, uint64_t proj, const char *dfile) {
  FILE *f = fopen(dfile, "rb");
  if (!f) return errno == ENOENT ? 0 : -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return -1;
  }
  char *buf = malloc((size_t)n + 2);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  for (size_t i = 0; i + 1 < got; i++) /* sambung baris: backslash+newline -> spasi */
    if (buf[i] == '\\' && (buf[i + 1] == '\n' || buf[i + 1] == '\r')) {
      buf[i] = ' ';
      buf[i + 1] = ' ';
    }
  char *colon = strchr(buf, ':');
  bool ok = colon != NULL;
  for (char *tok = ok ? strtok(colon + 1, " \t\r\n") : NULL; ok && tok; tok = strtok(NULL, " \t\r\n")) {
    /* -MP menulis baris target kosong "header:" setelah aturan utama; buang ':'
       agar path-nya sama dengan dependensi yang sudah tercatat. */
    size_t tl = strlen(tok);
    if (tl && tok[tl - 1] == ':') tok[tl - 1] = '\0';
    if (!tok[0]) continue;
    CsEntry probe;
    if (csGet(st, proj, csHash(tok), &probe)) continue; /* sudah tercatat */
    struct stat sb;
    if (stat(tok, &sb) != 0) {
      ok = false;
      break;
    }
    CsEntry e = {0};
    e.proj = proj;
    e.file = csHash(tok);
    e.time = wsCacheStampNs(&sb);
    e.size = (long long)sb.st_size;
    /* Reuse hash isi dari simpanan sebelumnya bila (time, size) cocok dan
       tidak racy — sama seperti wsCacheAdd. Header tak berubah tidak dibaca
       ulang: tiap fopen+read+hash header berbayar ~1.5 ms di mesin dengan
       stat/path-walk mahal (proot/Termux/LSM), ~44 header = ~60 ms/save. */
    CsEntry prev;
    if (g_prevFp && csGet(g_prevFp, proj, e.file, &prev) && prev.time == e.time &&
        prev.size == e.size && e.time < g_prevFpMtime) {
      e.content = prev.content; /* metadata cocok & tidak racy: isi tidak berubah */
    } else {
      e.content = wsStartupHash(tok);
    }
    if (!e.content) {
      ok = false;
      break;
    }
    fprintf(pf, "%016llx H %016llx %lld %lld %s\n", (unsigned long long)e.file,
            (unsigned long long)e.content, e.time, e.size, tok);
    ok = csPut(st, &e);
  }
  free(buf);
  return ok ? 1 : -1;
}
/* Recursively inspect actual compiler objects. Generated embed objects have no
   compiler depfile; track their output and the generated embedded header. */
static bool wsCacheAddObjectTree(FILE *pf, CsStore *st, uint64_t proj,
                                 const char *dir, const char *root, unsigned depth,
                                 int *objects) {
  if (depth > 64) return false;
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  bool ok = true;
  for (int i = 0; ok && i < files.count; i++) {
    const char *o = files.items[i];
    size_t len = strlen(o);
    if (len < 3 || strcmp(o + len - 2, ".o")) continue;
    (*objects)++;
    char dfile[WS_PATH_LEN];
    snprintf(dfile, sizeof(dfile), "%.*s.d", (int)(len - 2), o);
    /* Satu fopen per object: 0 (ENOENT) = .d tak ada -> cabang embedded di
       bawah; -1 (gagal baca/parse) = save batal konservatif. */
    int dr = wsCacheAddDepFile(pf, st, proj, dfile);
    if (dr > 0) ok = true;
    else if (dr < 0) ok = false;
    else {
      char header[WS_PATH_LEN];
      snprintf(header, sizeof(header), "%s/embedded.h", root);
      /* Only generated objects at the root of the project build tree
         may omit .d; missing depfiles for compiled C objects are errors. */
      ok = !strcmp(dir, root) && fsFileExists(header) &&
           wsCacheAdd(pf, st, proj, header, 'O') &&
           wsCacheAdd(pf, st, proj, o, 'O');
    }
  }
  for (int i = 0; ok && i < dirs.count; i++)
    ok = wsCacheAddObjectTree(pf, st, proj, dirs.items[i], root, depth + 1, objects);
  listFree(&dirs);
  listFree(&files);
  return ok;
}
static bool wsCacheAddProjectDeps(FILE *pf, CsStore *st, uint64_t proj, const char *name) {
  char dir[WS_PATH_LEN];
  snprintf(dir, sizeof(dir), "build/%s", name);
  int objects = 0;
  /* Proyek tanpa objek C (mis. archive-only) tidak punya dependensi header
     yang perlu dilacak; output-nya tetap dicek lewat entri O. */
  return wsCacheAddObjectTree(pf, st, proj, dir, dir, 0, &objects);
}
/* Archive-only projects have no C object/.d files. Track the real archive
   inputs recursively, plus the archive output, rather than requiring .o.
   A directory entry detects additions/deletions; file hashes detect edits. */
static bool wsCacheAddArchiveTree(FILE *pf, CsStore *st, uint64_t proj,
                                  const char *dir, unsigned depth) {
  if (depth > 64 || !wsCacheAdd(pf, st, proj, dir, 'D')) return false;
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  bool ok = true;
  for (int i = 0; ok && i < files.count; i++)
    ok = wsCacheAdd(pf, st, proj, files.items[i], 'S');
  for (int i = 0; ok && i < dirs.count; i++) {
    const char *sub = strrchr(dirs.items[i], '/');
    sub = sub ? sub + 1 : dirs.items[i];
    if (!strcmp(sub, "build") || !strcmp(sub, "dist") ||
        !strcmp(sub, ".git") || !strcmp(sub, ".satria") ||
        !strcmp(sub, "self_archive")) continue;
    ok = wsCacheAddArchiveTree(pf, st, proj, dirs.items[i], depth + 1);
  }
  listFree(&dirs);
  listFree(&files);
  return ok;
}

/* Stamp grup untuk metadata P (§4): content = hash konfigurasi + daftar
   sumber (wsProjStamp — sama dengan gate no-op), time = stempel ns save,
   size = jumlah entri grup. */
typedef struct {
  uint64_t proj, content;
  long long time, size;
} WsProjStamp;

static long long wsNowNs(void) {
#if defined(_WIN32)
  return (long long)time(NULL) * 1000000000LL;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

/* Metadata grup di opts.cache memakai stamp yang sama dengan fingerprint:
   router ikut terverifikasi tanpa membuka cache lain (§4). Gagal -> biarkan
   (format lama tetap sah; simpan berikutnya menulis ulang). */
static void wsOptsMetaSave(const WsProjStamp *st, int n) {
  CsStore *o = csOpen(WS_ROUTER_FILE);
  if (!o) return;
  for (int i = 0; i < n; i++)
    csProjMetaSet(o, st[i].proj, st[i].content, st[i].time, st[i].size);
  csCommit(o);
  csClose(o);
}

/* Simpan cache sesudah build sukses. Gagal -> kedua file dibuang (tidak ada cache palsu). */
void wsCacheSave(const WsModel *m, const char *wsName) {
  struct stat pfs;
  g_prevFp = NULL;
  g_prevFpMtime = 0;
  if (stat(WS_CACHE_FP, &pfs) == 0) {
    g_prevFpMtime = wsCacheStampNs(&pfs);
    g_prevFp = csOpen(WS_CACHE_FP);
  }
  wsCacheSaveBody(m, wsName);
  if (g_prevFp) csClose(g_prevFp);
  g_prevFp = NULL;
  g_prevFpMtime = 0;
}

static void wsCacheSaveBody(const WsModel *m, const char *wsName) {
  mkdirs(WS_CACHE_DIR);
  remove(WS_CACHE_FP);
  char tmpPaths[WS_PATH_LEN + 8];
  snprintf(tmpPaths, sizeof(tmpPaths), "%s.map.tmp", WS_CACHE_SOURCES);
  FILE *pf = fopen(tmpPaths, "w");
  if (!pf) return;
  CsStore *st = csOpen(WS_CACHE_FP); /* sudah dihapus -> store kosong */
  if (!st) {
    fclose(pf);
    remove(tmpPaths);
    return;
  }
  bool ok = true;
  uint64_t wsProj = csHash("workspace");
  ok = ok && wsCacheAdd(pf, st, wsProj, wsName, 'W');
  char dirPath[WS_PATH_LEN];
  for (int i = 0; ok && i < m->projectCount; i++) {
    const char *name = m->projects[i].name;
    uint64_t proj = csHash(name);
    List srcs = {0};
    bool archiveOnly = wsProjectArchiveOnly(&m->projects[i]);
    if (archiveOnly) {
      /* The project root and its contents are inputs to the archive. */
      ok = wsCacheAddArchiveTree(pf, st, proj, m->projects[i].root, 0);
      /* Require the generated archive, not an unrelated object file. */
      const char *archiveName = NULL, *archiveDir = NULL;
      for (int k = 0; k < m->projects[i].lines.count; k++) {
        const char *line = m->projects[i].lines.items[k];
        if (!strncmp(line, "archive.name = ", 15)) archiveName = line + 15;
        if (!strncmp(line, "archive.dir = ", 14)) archiveDir = line + 14;
      }
      if (!archiveName || !archiveDir) ok = false;
      if (ok) {
        char archive[WS_PATH_LEN];
        snprintf(archive, sizeof(archive), "%s/%s.tar", archiveDir, archiveName);
        ok = wsCacheAdd(pf, st, proj, archive, 'O');
        if (ok) artifactCacheAdd(archive); /* arsip tanpa kode C -> artifact.cache §9 */
      }
      if (!ok) break;
      continue;
    }
    if (!wsLogLoadSrcs(name, &srcs)) {
      ok = false;
      break;
    }
    for (int j = 0; ok && j < srcs.count; j++) {
      ok = wsCacheAdd(pf, st, proj, srcs.items[j], 'S');
      wsCacheDirOf(srcs.items[j], dirPath, sizeof(dirPath));
      CsEntry probe;
      if (ok && !csGet(st, proj, csHash(dirPath), &probe)) ok = wsCacheAdd(pf, st, proj, dirPath, 'D');
    }
    listFree(&srcs);
    if (ok) ok = wsCacheAddProjectDeps(pf, st, proj, name);
    char out[WS_PATH_LEN];
    snprintf(out, sizeof(out), "bin/%s", name);
    if (ok && fsFileExists(out)) ok = wsCacheAdd(pf, st, proj, out, 'O');
    snprintf(out, sizeof(out), "lib/lib%s.a", name);
    if (ok && fsFileExists(out)) ok = wsCacheAdd(pf, st, proj, out, 'O');
    snprintf(out, sizeof(out), "lib/lib%s.so", name);
    if (ok && fsFileExists(out)) ok = wsCacheAdd(pf, st, proj, out, 'O');
  }
  /* Workspace outputs may be renamed by output aliases (e.g. core -> rupa).
     Record actual root bin/lib files, not only bin/<project-name>. */
  const char *outDirs[] = {"bin", "lib"};
  for (size_t d = 0; ok && d < 2; d++) {
    List dirs = {0}, files = {0};
    fsListDir(outDirs[d], &dirs, &files);
    for (int j = 0; ok && j < files.count; j++)
      ok = wsCacheAdd(pf, st, wsProj, files.items[j], 'O');
    listFree(&dirs);
    listFree(&files);
  }
  if (ok && fsFileExists("compile_commands.json")) ok = wsCacheAdd(pf, st, wsProj, "compile_commands.json", 'O');
  profMark("save-walk"); /* profil: stat+hash seluruh entri (sumber/header/objek/output) */
  /* Metadata grup P (§4): stamp per proyek + grup workspace (content = hash
     Buildfile.ws dari entri W). Disimpan SEBELUM commit agar baris P di
     fingerprint.cache memuat <content> <time> <size>. */
  WsProjStamp stamps[WS_MAX_PROJECTS + 1];
  int sn = 0;
  long long saveNs = wsNowNs();
  if (ok) {
    for (int i = 0; i < m->projectCount; i++) {
      uint64_t proj = csHash(m->projects[i].name);
      size_t cnt = csCountProj(st, proj);
      if (cnt == 0) continue; /* grup kosong: P pun tidak ditulis */
      stamps[sn].proj = proj;
      stamps[sn].content = wsProjStamp(m, i);
      stamps[sn].time = saveNs;
      stamps[sn].size = (long long)cnt;
      sn++;
      /* state.cache ikut distamp dengan content/time yang sama (§4 seragam);
         state_blob memakai kunci nama proyek. */
      stateBlobProjMeta(m->projects[i].name, stamps[sn - 1].content, saveNs);
    }
    CsEntry we;
    if (csGet(st, wsProj, csHash(wsName), &we)) {
      stamps[sn].proj = wsProj;
      stamps[sn].content = we.content;
      stamps[sn].time = saveNs;
      stamps[sn].size = (long long)csCountProj(st, wsProj);
      sn++;
    }
    for (int i = 0; i < sn; i++)
      csProjMetaSet(st, stamps[i].proj, stamps[i].content, stamps[i].time, stamps[i].size);
  }
  if (fclose(pf) != 0) ok = false;
  if (ok) ok = csCommit(st);
  profMark("save-commit"); /* profil: tulis fingerprint.cache */
  /* Gabungkan peta P ke sources.cache tanpa mengubah format SATRIASRC1.
     Tulis atomik: cache lama tidak boleh hilang jika penulisan gagal. */
  if (ok) {
    FILE *src = fopen(WS_CACHE_SOURCES, "r");
    FILE *map = fopen(tmpPaths, "r");
    char combined[WS_PATH_LEN + 16];
    snprintf(combined, sizeof(combined), "%s.merge.tmp", WS_CACHE_SOURCES);
    FILE *dst = (src && map) ? fopen(combined, "w") : NULL;
    if (!dst) ok = false;
    if (ok) {
      char line[WS_PATH_LEN + 128];
      while (fgets(line, sizeof(line), src)) {
        if (line[0] == 'P' && line[1] == ' ') continue;
        if (fputs(line, dst) == EOF) { ok = false; break; }
      }
      while (ok && fgets(line, sizeof(line), map)) {
        if (fprintf(dst, "P %s", line) < 0) { ok = false; break; }
      }
    }
    if (src) fclose(src);
    if (map) fclose(map);
    if (dst && fclose(dst) != 0) ok = false;
    if (ok) ok = fsRenameReplace(combined, WS_CACHE_SOURCES);
    if (!ok) remove(combined);
  }
  profMark("save-merge"); /* profil: gabung baris P ke sources.cache */
  remove(tmpPaths);
  csClose(st);
  if (!ok) {
    remove(tmpPaths);
    remove(WS_CACHE_FP);
    /* Jangan hapus sources.cache: indeks discovery dipakai kedua mode. */
    wsRouterDrop();
  } else {
    remove(".satria/caches/paths.cache"); /* bersihkan artefak legacy */
    wsRouterRecord();
    if (sn > 0) wsOptsMetaSave(stamps, sn); /* stamp grup di opts.cache (§4) */
    /* Metadata grup P seragam (§4): deps/embed/lib memakai content/time/skema
       yang sama dengan baris P di fingerprint.cache & opts.cache. Satu
       buka+tulis per file untuk SEMUA grup (batch) — loop per grup dulu
       membuka 4 file × sn kali per run. */
    if (sn > 0) {
      uint64_t projs[WS_MAX_PROJECTS + 1];
      uint64_t contents[WS_MAX_PROJECTS + 1];
      long long times[WS_MAX_PROJECTS + 1];
      for (int i = 0; i < sn; i++) {
        projs[i] = stamps[i].proj;
        contents[i] = stamps[i].content;
        times[i] = stamps[i].time;
      }
      csProjMetaStampN(projs, contents, times, sn);
    }
    /* Grup CompDB global di state.cache (__satria_compdb__) ikut distamp
       dengan content/time grup workspace — penutup terakhir tanpa P meta. */
    if (sn > 0 && stamps[sn - 1].proj == wsProj)
      stateBlobProjMeta(WS_COMPDB_GLOBAL, stamps[sn - 1].content, stamps[sn - 1].time);
    artifactCacheSave(); /* flush metadata artefak/input release (§9) */
  }
  profMark("save-tail"); /* profil: router/opts/state.blob/artifact */
}

#include "embed.h"
#include "cmds/cmds_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache/cache_store.h"
#include "compile.h"
#include "portability.h"
#include "util.h"

/* Deteksi GNU ld HANYA untuk `satria profile embed=` (benchmark tool
   tar+ld — embedProbeGnuLd). Jalur build TIDAK lagi memakai ld: codegen
   embedded.c adalah satu-satunya mekanisme embed (design/embedded.md),
   jadi tidak ada probe maupun verdict ld.cache yang dipakai build. */

/* Cek apakah 'ld' yang ada di PATH adalah GNU ld (bukan LLD/LLD-compatible).
 * GNU ld mendukung '-r -b binary', LLD tidak. */
static bool isGnuLd(void) {
  FILE *fp = popenRB("ld --version 2>&1");
  if (!fp) return false;

  char buf[512];
  bool gnu = false;
  while (fgets(buf, sizeof(buf), fp)) {
    /* LLD output: "LLD 17.0.6 ..." atau "Debian LLD ..."
         * GNU ld output: "GNU ld (GNU Binutils) 2.38"
         * Catatan: cek LLD dulu karena Debian kadang output "GNU LLD" */
    if (strstr(buf, "LLD") != NULL) {
      gnu = false;
      break;
    }
    if (strstr(buf, "GNU ld") != NULL) {
      gnu = true;
      /* Jangan break — lanjut baca untuk pastikan tidak ada "LLD" di baris berikutnya */
    }
  }
  pcloseRB(fp);
  return gnu;
}

/* Probe tanpa cache untuk jalur non-mutating — lihat embed.h. */
bool embedProbeGnuLd(void) {
  if (!probeAvailable("ld")) return false;
  return isGnuLd();
}

static uint64_t embArchiveConfigHash(const EmbeddedEntry *e) {
  uint64_t h = UINT64_C(1469598103934665603);
#define HASH_BYTES(p, n)                                                                           \
  do {                                                                                             \
    const unsigned char *_b = (const unsigned char *)(p);                                          \
    for (size_t _i = 0; _i < (n); _i++) {                                                          \
      h ^= _b[_i];                                                                                 \
      h *= UINT64_C(1099511628211);                                                                \
    }                                                                                              \
  } while (0)
#define HASH_STR(x)                                                                                \
  do {                                                                                             \
    const char *_s = (x);                                                                          \
    HASH_BYTES(_s, strlen(_s) + 1);                                                                \
  } while (0)
  HASH_STR(e->src);
  HASH_STR(e->pattern);
  HASH_STR(e->archiveDir);
  HASH_STR(e->archiveName);
  HASH_STR(e->ext);
  HASH_BYTES(&e->tar, sizeof(e->tar));
  for (int i = 0; i < e->excludes.count; i++)
    HASH_STR(e->excludes.items[i]);
#undef HASH_STR
#undef HASH_BYTES
  return h;
}

static bool embArchiveStateMatches(const EmbeddedEntry *e) {
  char path[MAX_PATH + 96];
  snprintf(path, sizeof(path), "%s.archive", e->objectPath);
  FILE *fp = fopen(path, "rb");
  if (!fp) return false;
  uint64_t stored = 0;
  bool ok = fread(&stored, 1, sizeof(stored), fp) == sizeof(stored);
  fclose(fp);
  return ok && stored == embArchiveConfigHash(e);
}

void embArchiveStateSave(const EmbeddedEntry *e) {
  char path[MAX_PATH + 96];
  snprintf(path, sizeof(path), "%s.archive", e->objectPath);
  mkparent(path);
  FILE *fp = fopen(path, "wb");
  if (!fp) return;
  uint64_t h = embArchiveConfigHash(e);
  fwrite(&h, 1, sizeof(h), fp);
  fclose(fp);
}

/* ==================== caches/embed.cache (design/startup.md §3/§4) ====================
   Validasi arsip embed lewat entri terpusat: satu entri grup (config hash +
   stamp arsip) + satu entri per input (file: stamp + hash isi; folder: stamp
   mtime), semua berpayload "<kind> <archivePath> <path>". Bila SEMUA entri
   grup masih cocok, arsip segar TANPA walk file (lompatan cepat); sentuhan
   (mtime beda, isi sama) dipulihkan lewat hash — tanpa re-archive, tanpa
   membaca seluruh tree. Bila ada yang meleset, keputusan tetap di jalur lama
   (embArchiveFresh) — cache ini murni lompatan, tidak pernah menggugurkan
   rebuild yang seharusnya terjadi. */

#define EMBED_CACHE_FILE WS_CACHES_DIR "/embed.cache"

bool embExcluded(const EmbeddedEntry *e, const char *rel); /* di bawah */
void embRelPath(const EmbeddedEntry *e, const char *path, char *rel, size_t n);

/* FNV-1a 64 atas isi file (streaming 4KB); 0 = tak terbaca. */
static uint64_t embFileHash(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  uint64_t h = 14695981039346656037ULL;
  unsigned char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    for (size_t i = 0; i < n; i++) {
      h ^= buf[i];
      h *= 1099511628211ULL;
    }
  fclose(fp);
  return h;
}

static uint64_t embCacheGrpAddr(const EmbeddedEntry *e) { return csHash(e->archivePath); }
static uint64_t embCacheEntryAddr(const EmbeddedEntry *e, char kind, const char *path) {
  char key[MAX_PATH * 4 + 8];
  snprintf(key, sizeof(key), "%s|%c|%s", e->archivePath, kind, path);
  return csHash(key);
}

/* Parse payload "<kind> <archivePath> [path]" (path boleh berisi spasi). */
static char embPayloadParse(const char *payload, const char **archive, const char **path) {
  if (!payload || !payload[0] || payload[1] != ' ') return 0;
  const char *a = payload + 2;
  const char *sp = strchr(a, ' ');
  *archive = a;
  *path = sp ? sp + 1 : "";
  return payload[0];
}

/* Stamp folder rekursif (mode single — stateWalkDirStamps hanya ada di cache
   walk workspace). Set = root + seluruh subfolder; filter exclude/ad
   diterapkan pemanggil saat mencatat. */
static void embCollectDirStamps(const char *dir, List *out) {
  int64_t mt = 0;
  long long sz = 0;
  if (!fsStampNsSize(dir, &mt, &sz)) return;
  char entry[WS_PATH_LEN + 32];
  snprintf(entry, sizeof(entry), "D %lld %s", (long long)mt, dir);
  listAdd(out, entry);
  List dirs = {0}, files = {0};
  listReserve(&dirs, 64);
  listReserve(&files, 64);
  fsListDir(dir, &dirs, &files);
  for (int i = 0; i < dirs.count; i++) embCollectDirStamps(dirs.items[i], out);
  listFree(&dirs);
  listFree(&files);
}

/* true bila token archivePath pada payload == want. Payload TIDAK diputus
   (tanpa menulis NUL di tengah) — dibandingkan sepanjang want lalu dicek
   boundary-nya. */
static bool embPayloadArchiveIs(const char *payload, const char *want) {
  if (!payload || !payload[0] || payload[1] != ' ') return false;
  const char *a = payload + 2;
  size_t wl = strlen(want);
  return strncmp(a, want, wl) == 0 && (a[wl] == ' ' || a[wl] == '\0');
}

typedef struct {
  CsStore *st;
  uint64_t proj;
  const EmbeddedEntry *e;
  int seen;
  bool ok;
  uint64_t *prunes; /* kunci entri file yang hilang dari disk (dihapus setelah iterasi) */
  size_t pn, pcap;
} EmbValCtx;

/* Validasi satu entri input milik grup e. PATCH aman saat iterasi (hanya
   mengubah isi node, bukan struktur hash). */
static bool embCacheValidateVisit(const CsEntry *ent, CsOp op, void *ctxp) {
  (void)op;
  EmbValCtx *ctx = ctxp;
  if (!ctx->ok || ent->proj != ctx->proj || !ent->payload) return true;
  if (ent->payload[0] == 'G') return true;
  const char *archive = NULL, *path = NULL;
  char kind = embPayloadParse(ent->payload, &archive, &path);
  (void)archive;
  if (!kind || !embPayloadArchiveIs(ent->payload, ctx->e->archivePath)) return true;

  ctx->seen++;
  int64_t mt = 0;
  long long sz = 0;
  if (!cmdsStatCached(path, &mt, &sz)) {
    if (kind == 'D') {
      ctx->ok = false; /* folder hilang: biarkan jalur lama memutuskan */
      return true;     /* tetap lanjut: entri lain masih bisa di-prune */
    }
    /* File hilang dari disk: prune entri (penghapusan selalu diikuti
       perubahan stamp folder, jadi D-entry menangkap dampaknya). */
    if (ctx->pn == ctx->pcap) {
      size_t nc = ctx->pcap ? ctx->pcap * 2 : 16;
      uint64_t *nv = realloc(ctx->prunes, nc * sizeof(uint64_t));
      if (!nv) return true;
      ctx->prunes = nv;
      ctx->pcap = nc;
    }
    ctx->prunes[ctx->pn++] = ent->file;
    return true;
  }
  if (kind == 'D') {
    if ((long long)mt != ent->time) ctx->ok = false;
    return true;
  }
  if (kind != 'F') return true;
  if ((long long)mt == ent->time && sz == ent->size) return true;
  /* Stamp beda: baca isi — sama (mis. touch) -> PATCH stamp; beda -> gugur. */
  uint64_t h = embFileHash(path);
  if (h == 0 || h != ent->content) {
    ctx->ok = false;
    return false;
  }
  CsPatch p = {0};
  p.hasContent = true;
  p.content = h;
  p.hasTime = true;
  p.time = (long long)mt;
  p.hasSize = true;
  p.size = sz;
  return csPatch(ctx->st, ent->proj, ent->file, &p);
}

/* True bila seluruh entri grup arsip masih cocok — arsip segar TANPA walk. */
static bool embEmbedCacheFresh(const EmbeddedEntry *e) {
  if (e->usePrebuilt || !e->archivePath[0]) return false;
  if (wsModeActive()) cmdsStatMemoSetCwd();
  CsStore *st = csOpen(EMBED_CACHE_FILE);
  if (!st) return false;
  uint64_t proj = cmdsCacheProjAddr();
  CsEntry g;
  if (!csGet(st, proj, embCacheGrpAddr(e), &g) || !g.payload || g.payload[0] != 'G') {
    csClose(st);
    return false;
  }
  int64_t at = 0;
  long long asz = 0;
  bool archiveOk = fsStampNsSize(e->archivePath, &at, &asz) && g.time == (long long)at &&
                   g.size == asz && g.content == embArchiveConfigHash(e);
  if (!archiveOk) {
    csClose(st);
    return false;
  }
  EmbValCtx ctx = {st, proj, e, 0, true, NULL, 0, 0};
  csForEach(st, embCacheValidateVisit, &ctx);
  for (size_t i = 0; i < ctx.pn; i++) csDelete(st, proj, ctx.prunes[i]);
  free(ctx.prunes);
  bool fresh = ctx.ok && ctx.seen > 0;
  if (csIsDirty(st)) csCommit(st); /* PATCH stamp (touch) & prune dipersist */
  csClose(st);
  return fresh;
}

typedef struct {
  uint64_t proj;
  const EmbeddedEntry *e;
  uint64_t *keys;
  size_t kn, kcap;
} EmbKeyCtx;

static bool embCacheCollectVisit(const CsEntry *ent, CsOp op, void *ctxp) {
  (void)op;
  EmbKeyCtx *ctx = ctxp;
  if (ent->proj != ctx->proj || !ent->payload) return true;
  const char *archive = NULL, *path = NULL;
  char kind = embPayloadParse(ent->payload, &archive, &path);
  (void)archive;
  (void)path;
  if (!kind || !embPayloadArchiveIs(ent->payload, ctx->e->archivePath)) return true;
  if (ctx->kn == ctx->kcap) {
    size_t nc = ctx->kcap ? ctx->kcap * 2 : 64;
    uint64_t *nv = realloc(ctx->keys, nc * sizeof(uint64_t));
    if (!nv) return false;
    ctx->keys = nv;
    ctx->kcap = nc;
  }
  ctx->keys[ctx->kn++] = ent->file;
  return true;
}

/* Entri grup sudah ada untuk arsip ini? (dipakai recordIfMissing) */
static bool embEmbedCacheHasGroup(const EmbeddedEntry *e) {
  CsStore *st = csOpen(EMBED_CACHE_FILE);
  if (!st) return false;
  CsEntry g;
  bool has = csGet(st, cmdsCacheProjAddr(), embCacheGrpAddr(e), &g) && g.payload &&
             g.payload[0] == 'G';
  csClose(st);
  return has;
}

void embEmbedCacheRecord(const EmbeddedEntry *e); /* di bawah */

/* Self-healing: arsip segar lewat jalur lama tetapi entri grup hilang (mis.
   cache baru dihapus) -> rekam sekarang, supaya run berikutnya bisa
   melompati walk. Tanpa ini cache tidak pernah terbentuk kembali sampai
   rebuild berikutnya. */
void embEmbedCacheRecordIfMissing(const EmbeddedEntry *e) {
  if (e->usePrebuilt || !e->archivePath[0]) return;
  if (!embEmbedCacheHasGroup(e)) embEmbedCacheRecord(e);
}

/* Rekam entri grup + input setelah arsip dibangun ulang. Himpunan input =
   persis yang divalidasi embArchiveFresh (walk file + stamp folder). */
void embEmbedCacheRecord(const EmbeddedEntry *e) {
  if (e->usePrebuilt || !e->archivePath[0]) return;
  mkdirs(WS_CACHES_DIR);
  CsStore *st = csOpen(EMBED_CACHE_FILE);
  if (!st) {
    remove(EMBED_CACHE_FILE); /* rusak: tulis ulang bersih */
    st = csOpen(EMBED_CACHE_FILE);
  }
  if (!st) return;
  uint64_t proj = cmdsCacheProjAddr();

  EmbKeyCtx col = {proj, e, NULL, 0, 0};
  bool collected = csForEach(st, embCacheCollectVisit, &col);
  if (collected)
    for (size_t i = 0; i < col.kn; i++) csDelete(st, proj, col.keys[i]);
  free(col.keys);
  if (!collected) {
    csClose(st);
    return;
  }

  int64_t at = 0;
  long long asz = 0;
  if (!fsStampNsSize(e->archivePath, &at, &asz)) {
    csClose(st);
    return; /* arsip hilang sesudah rebuild? jangan catat apa pun */
  }
  char gpay[MAX_PATH * 2 + 4];
  snprintf(gpay, sizeof(gpay), "G %s", e->archivePath);
  CsEntry g = {proj, embCacheGrpAddr(e), embArchiveConfigHash(e), (long long)at, asz, gpay};
  csPut(st, &g);

  List files = {0};
  if (e->pattern[0])
    stateWalkDirCached(e->src, e->pattern, &files);
  else
    stateWalkDirCached(e->src, "", &files);
  char rel[MAX_PATH * 2];
  for (int i = 0; i < files.count; i++) {
    const char *path = files.items[i];
    embRelPath(e, path, rel, sizeof(rel));
    if (!*rel || embExcluded(e, rel)) continue;
    int64_t mt = 0;
    long long sz = 0;
    if (!fsStampNsSize(path, &mt, &sz)) continue;
    size_t pl = strlen(e->archivePath) + strlen(path) + 8;
    char *pay = malloc(pl);
    if (!pay) continue;
    snprintf(pay, pl, "F %s %s", e->archivePath, path);
    CsEntry en = {proj, embCacheEntryAddr(e, 'F', path), embFileHash(path), (long long)mt, sz, pay};
    csPut(st, &en);
    free(pay);
  }
  listFree(&files);

  /* Folder sumber: penambahan/penghapusan file mengubah mtime folder —
     tanpa stamp folder, lompatan cache melewatkan file BARU (walk tidak
     dijalankan). ws: dari cache walk; single: fsListDir rekursif. */
  {
    char adir[MAX_PATH * 2];
    snprintf(adir, sizeof(adir), "%s", e->archivePath);
    char *slash = strrchr(adir, '/');
    if (slash)
      *slash = '\0';
    else
      snprintf(adir, sizeof(adir), ".");
    const char *ad = adir;
    if (ad[0] == '.' && ad[1] == '/') ad += 2;

    List dirs = {0};
    if (wsModeActive())
      stateWalkDirStamps(e->src, e->pattern[0] ? e->pattern : "", &dirs);
    else
      embCollectDirStamps(e->src, &dirs);
    for (int i = 0; i < dirs.count; i++) {
      long long mt = -1;
      char p[WS_PATH_LEN];
      if (sscanf(dirs.items[i], "D %lld %" WS_SCAN "s", &mt, p) != 2 || mt < 0) continue;
      const char *relp = p;
      if (relp[0] == '.' && relp[1] == '/') relp += 2;
      if (strcmp(relp, ".") != 0 && *relp && embExcluded(e, relp)) continue;
      if (strcmp(relp, ad) == 0 || (strcmp(relp, ".") == 0 && strcmp(ad, ".") == 0)) continue;
      size_t pl = strlen(e->archivePath) + strlen(p) + 8;
      char *pay = malloc(pl);
      if (!pay) continue;
      snprintf(pay, pl, "D %s %s", e->archivePath, p);
      CsEntry en = {proj, embCacheEntryAddr(e, 'D', p), 0, mt, 0, pay};
      csPut(st, &en);
      free(pay);
    }
    listFree(&dirs);
  }
  csCommit(st);
  csClose(st);
}

/* Entri exclude arsip (archive.exclude / embedded.<n>.exclude): nama file,
   path relatif, atau direktori. Pencocokan konsisten dengan excludedSource
   (compile.c), plus prefix direktori untuk entri tanpa ekstensi:
   - path relatif sama persis ("LICENSE"), atau
   - basename sama persis ("main.c" vs "src/main.c"), atau
   - path di dalam direktori yang dinamai entri ("build" -> "build/x").
   Dipakai oleh DAFTAR FILE tar dan SCAN FRESHNESS — tanpa ini, artefak
   build di dalam tree sumber (mis. build/<n>.o.archive) ikut terhitung
   sebagai input dan membuat arsip di-rebuild tiap run. */
bool embExcluded(const EmbeddedEntry *e, const char *rel) {
  for (int i = 0; i < e->excludes.count; i++) {
    const char *ex = e->excludes.items[i];
    if (!ex || !*ex) continue;
    if (strcmp(rel, ex) == 0) return true;
    const char *base = strrchr(rel, '/');
    if (base && strcmp(base + 1, ex) == 0) return true;
    size_t el = strlen(ex);
    if (strncmp(rel, ex, el) == 0 && rel[el] == '/') return true;
  }
  return false;
}

/* Path relatif terhadap e->src untuk pencocokan excludes. */
void embRelPath(const EmbeddedEntry *e, const char *path, char *rel, size_t n) {
  size_t srcLen = strlen(e->src);
  if (strncmp(path, e->src, srcLen) == 0 &&
      (path[srcLen] == '/' || path[srcLen] == '\\' || path[srcLen] == '\0')) {
    const char *r = path + srcLen;
    while (*r == '/' || *r == '\\')
      r++;
    snprintf(rel, n, "%s", r);
    return;
  }
  snprintf(rel, n, "%s", path);
}

bool embArchiveFresh(const EmbeddedEntry *e) {
  /* Arsip jadi (embedded.<n>.file): cukup bandingkan mtime file — tidak
     ada direktori sumber untuk discan. */
  if (e->usePrebuilt) return fsFileExists(e->archivePath);
  /* Lompatan cepat caches/embed.cache: semua entri grup masih cocok ->
     segar tanpa walk; kalau meleset, jalur normal di bawah memutuskan. */
  if (embEmbedCacheFresh(e)) return true;
  int64_t at = fsMTimeNs(e->archivePath);
  if (at < 0 || !embArchiveStateMatches(e)) return false;

  /* Scan freshness dengan resolusi nanodetik: perubahan source dan archive
     dapat terjadi dalam detik yang sama, sehingga time_t/fsMTime() terlalu
     kasar untuk invalidasi build. Entri yang di-exclude tidak dihitung —
     sama seperti saat arsip dibangun. */
  List files = {0};
  if (e->pattern[0])
    stateWalkDirCached(e->src, e->pattern, &files);
  else
    stateWalkDirCached(e->src, "", &files);
  int64_t newest = 0;
  char rel[MAX_PATH * 2];
  /* Mode workspace: mtime lewat memo stat (kunci memo butuh cwd proyek ini). */
  if (wsModeActive()) cmdsStatMemoSetCwd();
  for (int i = 0; i < files.count; i++) {
    embRelPath(e, files.items[i], rel, sizeof(rel));
    if (!*rel || embExcluded(e, rel)) continue;
    int64_t mt = cmdsMTimeDecide(files.items[i]);
    if (mt >= 0 && mt > newest) newest = mt;
  }
  listFree(&files);
  /* Penambahan/penghapusan file mengubah mtime folder: stamp folder dari cache
     walk (sudah divalidasi) ikut menentukan kesegaran. Folder exclude diabaikan. */
  if (wsModeActive()) {
    /* Folder tempat arsip itu sendiri ditulis ikut berubah mtime-nya setiap kali
       arsip dibuat ulang (tmp + rename). Folder itu tidak boleh dihitung sebagai
       input, kalau tidak arsip di dalam folder sumber basi oleh dirinya sendiri
       dan dibangun ulang di setiap run. */
    char adir[MAX_PATH * 2];
    snprintf(adir, sizeof(adir), "%s", e->archivePath);
    char *slash = strrchr(adir, '/');
    if (slash) *slash = '\0';
    else snprintf(adir, sizeof(adir), ".");
    const char *ad = adir;
    if (ad[0] == '.' && ad[1] == '/') ad += 2;

    List st = {0};
    stateWalkDirStamps(e->src, e->pattern[0] ? e->pattern : "", &st);
    for (int i = 0; i < st.count; i++) {
      long long mt = -1;
      char p[WS_PATH_LEN];
      if (sscanf(st.items[i], "D %lld %" WS_SCAN "s", &mt, p) != 2 || mt < 0) continue;
      const char *rel = p;
      if (rel[0] == '.' && rel[1] == '/') rel += 2;
      if (strcmp(rel, ".") != 0 && *rel && embExcluded(e, rel)) continue;
      if (strcmp(rel, ad) == 0 || (strcmp(rel, ".") == 0 && strcmp(ad, ".") == 0)) continue;
      if (mt > newest) newest = mt;
    }
    listFree(&st);
  }
  return newest <= at;
}

/* "modules/rupa.tar.gz" -> "_binary_modules_rupa_tar_gz" (basis simbol) */

/* ws_cache_noop.c — Cache no-op: validasi entri fingerprint.cache (GET) untuk melewati build.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"
#include "../cache/artifact.h"
#include "../cache/router.h"
#include <errno.h>

static int wsCacheRecCmp(const void *a, const void *b);
static bool wsCacheTrace(void);
static bool wsCacheVisit(const CsEntry *e, CsOp op, void *ctx);

/* ==================== cache GET no-op (design/startup.md §4, opt-in) ====================
   Default AKTIF; nonaktifkan dengan SATRIA_CACHE_NOOP=0 (lihat
   wsCacheEnabled).
   Build sukses menulis:
     .satria/caches/fingerprint.cache  entri (alamat proyek, alamat file, content,
                                       time, size) — sumber S, direktori D, Buildfile
                                       (W), dan output O (bin/lib/compile_commands).
     .satria/sources.cache            P <alamat> <tipe> <content> <time> <size> <path>
                                       (peta fingerprint + metadata §4: fast path
                                       memvalidasi dari file ini saja — N stat,
                                       tanpa membuka fingerprint.cache).
   No-op memvalidasi entri itu (GET) dan keluar bila semuanya cocok. Gagal apa
   pun (file hilang/rusak, entri tak terpetakan, output hilang) -> jalur normal. */
bool wsCacheEnabled(void) {
  /* Default AKTIF (tukar default 2026-10-19; bench hijau dua jalur + 16/16
     unit test). Nonaktifkan dengan SATRIA_CACHE_NOOP=0. Semantik nilai
     dipertahankan: selain "0" = on (termasuk "1" lama & varian apa pun). */
  const char *e = getenv("SATRIA_CACHE_NOOP");
  if (!e || !*e) return true; /* tak diset -> default on */
  return strcmp(e, "0") != 0;
}
void wsCacheMapFree(WsCacheMap *m) {
  free(m->recs);
  m->recs = NULL;
  m->n = m->cap = 0;
}
static int wsCacheRecCmp(const void *a, const void *b) {
  uint64_t x = ((const WsCachePath *)a)->key, y = ((const WsCachePath *)b)->key;
  return x < y ? -1 : x > y;
}
const WsCachePath *wsCacheMapFind(const WsCacheMap *m, uint64_t key) {
  size_t lo = 0, hi = m->n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (m->recs[mid].key == key) return &m->recs[mid];
    if (m->recs[mid].key < key) lo = mid + 1;
    else hi = mid;
  }
  return NULL;
}
/* Tulis satu path ke tabel dan catat entri-nya. false = path tak bisa distat. */
/* Nanosecond timestamps avoid permanent false misses when a directory and
   fingerprint.cache are written during the same wall-clock second. */
long long wsCacheStampNs(const struct stat *sb) {
#if defined(_WIN32)
  return (long long)sb->st_mtime * 1000000000LL;
#elif defined(__APPLE__)
  return (long long)sb->st_mtimespec.tv_sec * 1000000000LL + sb->st_mtimespec.tv_nsec;
#else
  return (long long)sb->st_mtim.tv_sec * 1000000000LL + sb->st_mtim.tv_nsec;
#endif
}
static bool wsCacheTrace(void) {
  const char *v = getenv("SATRIA_CACHE_TRACE");
  return v && *v && strcmp(v, "0") != 0;
}
/* Parse satu baris P (rec = sesudah "P "):
     format baru: <alamat16> <tipe> <content16> <time> <size> <path>
     format lama: <alamat16> <tipe> <path> (cache transisi, tanpa metadata)
   Metadata sah bila ketiga kolom terbaca persis (heksa 16 digit + dua
   bilangan, dipisah spasi) dan path sisanya non-kosong; selain itu baris
   diperlakukan format lama -> meta = 0 -> fast path ditolak (jalur penuh
   yang menilai). false = struktur bukan format P yang dikenal. */
static bool wsParseP(const char *rec, WsCachePath *r) {
  size_t len = strlen(rec);
  if (len < 20 || rec[16] != ' ' || rec[18] != ' ') return false;
  char *end = NULL;
  r->key = strtoull(rec, &end, 16);
  if (end != rec + 16 || !strchr("SDOWH", rec[17])) return false;
  r->type = rec[17];
  r->hasMeta = 0;
  r->content = 0;
  r->time = r->size = 0;
  const char *path = rec + 19;
  errno = 0;
  unsigned long long content = strtoull(rec + 19, &end, 16);
  if (end == rec + 35 && *end == ' ' && errno == 0) {
    const char *q = end + 1;
    errno = 0;
    long long time = strtoll(q, &end, 10);
    if (end > q && errno == 0 && *end == ' ') {
      const char *q2 = end + 1;
      errno = 0;
      long long size = strtoll(q2, &end, 10);
      if (end > q2 && errno == 0 && *end == ' ' && end[1]) {
        r->hasMeta = 1;
        r->content = (uint64_t)content;
        r->time = time;
        r->size = size;
        path = end + 1;
      }
    }
  }
  if (strlen(path) >= sizeof(r->path)) return false;
  snprintf(r->path, sizeof(r->path), "%s", path);
  return true;
}

/* Muat tabel path; false bila tidak ada/rusak. */
bool wsCacheLoadMap(WsCacheMap *m) {
  FILE *f = fopen(WS_CACHE_SOURCES, "r");
  if (!f) return false;
  char line[WS_PATH_LEN + 160]; /* format baru: +metadata (~76 kolom) */
  bool ok = true;
  int metaAll = 1;
  while (ok && fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    if (len && line[len - 1] == '\n') line[--len] = '\0';
    if (len == 0 || line[0] != 'P' || line[1] != ' ') continue;
    if (m->n == m->cap) {
      size_t nc = m->cap ? m->cap * 2 : 64;
      WsCachePath *nr = realloc(m->recs, nc * sizeof(WsCachePath));
      if (!nr) {
        ok = false;
        break;
      }
      m->recs = nr;
      m->cap = nc;
    }
    WsCachePath *r = &m->recs[m->n];
    if (!wsParseP(line + 2, r)) { /* baris P rusak -> tabel dianggap rusak */
      ok = false;
      break;
    }
    if (!r->hasMeta) metaAll = 0;
    m->n++;
  }
  fclose(f);
  if (ok) qsort(m->recs, m->n, sizeof(WsCachePath), wsCacheRecCmp);
  m->meta = ok && m->n > 0 && metaAll;
  return ok && m->n > 0;
}

/* Perbarui metadata P dari map yang sudah divalidasi dengan hash. Isi konten
   tidak berubah; hanya time/size yang disegarkan. Penulisan atomik mencegah
   cache parsial jika proses berhenti di tengah jalan. */
static bool wsCachePRewrite(WsCacheMap *m, bool *changed) {
  *changed = false;
  FILE *src = fopen(WS_CACHE_SOURCES, "r");
  if (!src) return false;
  char tmpPath[WS_PATH_LEN + 16];
  snprintf(tmpPath, sizeof(tmpPath), "%s.meta.tmp", WS_CACHE_SOURCES);
  FILE *dst = fopen(tmpPath, "w");
  if (!dst) { fclose(src); return false; }
  char line[WS_PATH_LEN + 160];
  bool ok = true;
  while (ok && fgets(line, sizeof(line), src)) {
    if (line[0] == 'P' && line[1] == ' ') {
      char *end = NULL;
      uint64_t addr = strtoull(line + 2, &end, 16);
      const WsCachePath *found = end == line + 18 ? wsCacheMapFind(m, addr) : NULL;
      if (found) {
        char out[WS_PATH_LEN + 160];
        int n = snprintf(out, sizeof(out), "P %016llx %c %016llx %lld %lld %s",
                         (unsigned long long)found->key, found->type,
                         (unsigned long long)found->content, found->time, found->size,
                         found->path);
        size_t len = strlen(line);
        bool nl = len && line[len - 1] == '\n';
        if (n < 0 || (size_t)n >= sizeof(out) || (nl && (size_t)n + 1 >= sizeof(out))) {
          ok = false; break;
        }
        if (nl) out[n++] = '\n';
        out[n] = '\0';
        if (strcmp(out, line) != 0) *changed = true;
        if (fputs(out, dst) == EOF) ok = false;
        continue;
      }
    }
    if (fputs(line, dst) == EOF) ok = false;
  }
  if (fclose(dst) != 0) ok = false;
  fclose(src);
  if (!ok || !*changed) { remove(tmpPath); return ok; }
  if (!fsRenameReplace(tmpPath, WS_CACHE_SOURCES)) { remove(tmpPath); return false; }
  return true;
}

typedef struct {
  const WsCacheMap *map;
  CsStore *store;
  bool ok;
  bool patched;
} WsCachePRefresh;

/* Jaga fingerprint.cache tetap sinkron dengan sources.cache ketika P-fast
   membuktikan bahwa file hanya mengalami perubahan metadata. */
static bool wsCachePRefreshEntry(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  WsCachePRefresh *c = ctx;
  const WsCachePath *r = wsCacheMapFind(c->map, e->file);
  if (!r || r->content != e->content) return c->ok = false;
  if (r->time == e->time && r->size == e->size) return true;
  CsPatch patch = {0};
  patch.hasTime = true;
  patch.time = r->time;
  patch.hasSize = true;
  patch.size = r->size;
  if (!csPatch(c->store, e->proj, e->file, &patch)) return c->ok = false;
  c->patched = true;
  return true;
}

/* Direktori tidak memiliki hash konten di P. Bila mtime-nya berubah, pastikan
   daftar entri langsungnya tetap sama: setiap entri di disk harus memiliki
   record P dengan tipe yang sesuai. Ini membedakan touch direktori dari
   penambahan/penghapusan/rename file tanpa memaksa build untuk metadata saja.
   Direktori yang sengaja dikecualikan dari archive tree tetap diabaikan. */
static bool wsCacheDirEntriesMatch(const WsCacheMap *m, const char *path) {
  List dirs = {0}, files = {0};
  fsListDir(path, &dirs, &files);
  bool ok = true;
  for (int pass = 0; ok && pass < 2; pass++) {
    List *items = pass == 0 ? &dirs : &files;
    for (int i = 0; ok && i < items->count; i++) {
      const char *child = items->items[i];
      const char *base = strrchr(child, '/');
      base = base ? base + 1 : child;
      if (pass == 0 && (!strcmp(base, "build") || !strcmp(base, "dist") ||
                        !strcmp(base, ".git") || !strcmp(base, ".satria") ||
                        !strcmp(base, "self_archive")))
        continue;
      const WsCachePath *r = wsCacheMapFind(m, csHash(child));
      if (!r || strcmp(r->path, child) != 0 ||
          (pass == 0 ? r->type != 'D' : r->type == 'D')) {
        ok = false;
        break;
      }
    }
  }
  listFree(&dirs);
  listFree(&files);
  return ok;
}

/* ===== Fast path metadata P (design/startup.md §4) =====
   Metadata cocok -> no-op tanpa fingerprint.cache. Bila hanya time/size yang
   berubah, hash file tersebut: konten sama berarti cukup menyegarkan metadata
   P dan fingerprint secara internal; konten berbeda jatuh ke validasi penuh. */
static int wsCachePFast(WsCacheMap *m, long long srcMtime) {
  if (!m->meta || m->n == 0) return -1;
  bool metadataChanged = false;
  for (size_t i = 0; i < m->n; i++) {
    WsCachePath *r = &m->recs[i];
    struct stat sb;
    if (stat(r->path, &sb) != 0) return -1;
    long long now = wsCacheStampNs(&sb);
    long long size = (long long)sb.st_size;
    /* Direktori tidak memiliki hash konten. Jika mtime berubah, validasi
       daftar entri langsung; jika tetap identik, hanya metadata waktunya yang
       perlu disegarkan. Penambahan/penghapusan/rename membuat fast path miss. */
    if (r->type == 'D') {
      if (now == r->time && r->time < srcMtime) continue;
      if (!wsCacheDirEntriesMatch(m, r->path)) return -1;
      struct stat after;
      if (stat(r->path, &after) != 0 || wsCacheStampNs(&after) != now) return -1;
      if (r->time != now) {
        r->time = now;
        metadataChanged = true;
      }
      continue;
    }
    if (now == r->time && size == r->size && r->time < srcMtime) continue;
    /* Mtime/ukuran berubah atau timestamp tampak racy. Ukuran sama saja tidak
       membuktikan konten sama, jadi hash wajib sebelum metadata diperbarui. */
    uint64_t h = wsStartupHash(r->path);
    if (!h || h != r->content) return -1;
    /* Cegah menerima file yang berubah saat hash sedang dibaca. */
    struct stat after;
    if (stat(r->path, &after) != 0 || wsCacheStampNs(&after) != now ||
        (long long)after.st_size != size) return -1;
    if (r->time != now || r->size != size) {
      r->time = now;
      r->size = size;
      metadataChanged = true;
    }
  }
  if (metadataChanged) {
    /* Hash sudah memverifikasi semua file yang metadata-nya berubah. PATCH
       fingerprint dahulu, lalu tulis ulang P dan sinkronkan router. */
    CsStore *store = csOpen(WS_CACHE_FP);
    if (!store) return -1;
    WsCachePRefresh refresh = {m, store, true, false};
    bool walked = csForEach(store, wsCachePRefreshEntry, &refresh);
    bool committed = walked && refresh.ok && refresh.patched && csCommit(store);
    csClose(store);
    if (wsCacheTrace()) fprintf(stderr, "satria cache: PATCH %s\n", committed ? "committed" : "failed");
    if (!committed) return -1;
    bool changed = false;
    if (!wsCachePRewrite(m, &changed)) return -1;
    if (changed && !optsSourcesRouteSync()) return -1;
    if (wsCacheTrace()) fprintf(stderr, "satria cache: P metadata %s\n", changed ? "refreshed" : "unchanged");
    return 0;
  }
  return 1;
}

/* ===== Penyegaran metadata P (jalur penuh) =====
   Saat fast path menolak (format lama, sentuh, racy), pemeriksaan penuh
   memvalidasi lewat fingerprint.cache. Bila hasilnya valid, entri fingerprint
   (sudah PATCH utk sentuh) menjadi sumber kebenaran untuk menulis ulang
   metadata P — agar run berikutnya kembali bisa fast path. */
typedef struct {
  uint64_t file, content;
  long long time, size;
} WsCacheEnt;
typedef struct {
  WsCacheEnt *tab;
  size_t tn, tcap;
} WsCacheMetaTab;

/* Pass kedua di atas store yang SUDAH di-PATCH (dan di-commit): e membawa
   nilai final. */
static bool wsCacheMetaCollect(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  WsCacheMetaTab *t = ctx;
  if (t->tn == t->tcap) {
    size_t nc = t->tcap ? t->tcap * 2 : 64;
    WsCacheEnt *nr = realloc(t->tab, nc * sizeof(WsCacheEnt));
    if (!nr) return false;
    t->tab = nr;
    t->tcap = nc;
  }
  WsCacheEnt *x = &t->tab[t->tn++];
  x->file = e->file;
  x->content = e->content;
  x->time = e->time;
  x->size = e->size;
  return true;
}

static int wsCacheEntCmp(const void *a, const void *b) {
  uint64_t x = ((const WsCacheEnt *)a)->file, y = ((const WsCacheEnt *)b)->file;
  return x < y ? -1 : x > y;
}

/* Tulis ulang baris P dengan metadata dari entri yang baru divalidasi; baris
   P tanpa entri pasangan dipertahankan apa adanya. Tulis atomik (tmp lalu
   rename); false = gagal tulis -> metadata lama tetap dipakai (bukan
   kegagalan no-op). *changed = ada baris yang berubah -> pemicu sync route
   sources <-> opts (stempel sources.cache ikut berubah). */
static bool wsCacheMetaRewrite(const WsCacheMap *m, const WsCacheEnt *tab, size_t tn,
                               bool *changed) {
  *changed = false;
  FILE *src = fopen(WS_CACHE_SOURCES, "r");
  if (!src) return false;
  char tmpPath[WS_PATH_LEN + 16];
  snprintf(tmpPath, sizeof(tmpPath), "%s.meta.tmp", WS_CACHE_SOURCES);
  FILE *dst = fopen(tmpPath, "w");
  if (!dst) {
    fclose(src);
    return false;
  }
  char line[WS_PATH_LEN + 160];
  bool ok = true;
  while (ok && fgets(line, sizeof(line), src)) {
    bool emit = true; /* baris P tanpa pasangan / format aneh: pertahankan */
    if (line[0] == 'P' && line[1] == ' ') {
      char *end = NULL;
      uint64_t addr = strtoull(line + 2, &end, 16);
      const WsCacheEnt *x =
          end == line + 18 ? bsearch(&addr, tab, tn, sizeof(WsCacheEnt), wsCacheEntCmp) : NULL;
      const WsCachePath *r = x ? wsCacheMapFind(m, addr) : NULL;
      if (x && r) {
        char out[WS_PATH_LEN + 160];
        int n = snprintf(out, sizeof(out), "P %016llx %c %016llx %lld %lld %s",
                         (unsigned long long)addr, r->type, (unsigned long long)x->content,
                         x->time, x->size, r->path);
        size_t l0 = strlen(line);
        bool nl = l0 > 0 && line[l0 - 1] == '\n';
        if (n > 0 && (size_t)n < sizeof(out)) {
          if (nl && (size_t)n + 2 > sizeof(out)) { ok = false; break; }
          if (nl) out[n++] = '\n';
          out[n] = '\0';
          if (strcmp(out, line) != 0) *changed = true;
          emit = fputs(out, dst) != EOF;
          ok = emit;
          continue;
        }
        ok = false; /* baris hasil terlalu panjang: rusak -> batal */
        break;
      }
    }
    if (emit && fputs(line, dst) == EOF) ok = false;
  }
  if (fclose(dst) != 0) ok = false;
  fclose(src);
  if (!ok || !*changed) {
    remove(tmpPath);
    return ok;
  }
  if (!fsRenameReplace(tmpPath, WS_CACHE_SOURCES)) {
    remove(tmpPath);
    return false;
  }
  return true;
}
typedef struct {
  const WsCacheMap *map;
  long long cacheMtime;
  bool ok;
  size_t seen;
  CsStore *store;
  bool patched;
} WsCacheCheck;
/* Satu entri: cocokkan dengan keadaan file. false = entri invalid (hentikan). */
static bool wsCacheVisit(const CsEntry *e, CsOp op, void *ctx) {
  (void)op;
  WsCacheCheck *c = ctx;
  c->seen++;
  const WsCachePath *r = wsCacheMapFind(c->map, e->file);
  if (!r) { if (wsCacheTrace()) fprintf(stderr, "satria cache: missing path mapping %016llx\n", (unsigned long long)e->file); return c->ok = false; }
  struct stat sb;
  if (stat(r->path, &sb) != 0) { if (wsCacheTrace()) fprintf(stderr, "satria cache: missing %s\n", r->path); return c->ok = false; }
  /* Direktori tak bisa di-hash: mtime harus sama DAN tidak racy. Racy (diubah
     dalam detik yang sama dengan penulisan cache) -> invalid, jalur normal. */
  if (r->type == 'D') {
    bool valid = wsCacheStampNs(&sb) == e->time && e->time < c->cacheMtime;
    if (!valid && wsCacheTrace()) fprintf(stderr, "satria cache: directory changed/racy %s\n", r->path);
    return c->ok = valid;
  }
  /* Fast path: size dan time cocok dan tidak racy -> tanpa membaca isi. */
  if ((long long)sb.st_size == e->size && wsCacheStampNs(&sb) == e->time &&
      e->time < (long long)c->cacheMtime)
    return true;
  /* Time/size beda ATAU racy: hitung hash isi. Sama (mis. touch) -> valid;
     beda -> invalid. Berlaku juga untuk output (O) dan Buildfile (W). */
  uint64_t h = wsStartupHash(r->path);
  bool valid = h != 0 && h == e->content;
  if (valid && (e->time != wsCacheStampNs(&sb) || e->size != (long long)sb.st_size)) {
    CsPatch patch = {0};
    patch.hasTime = true;
    patch.time = wsCacheStampNs(&sb);
    patch.hasSize = true;
    patch.size = (long long)sb.st_size;
    if (!csPatch(c->store, e->proj, e->file, &patch)) return c->ok = false;
    c->patched = true;
  }
  if (!valid && wsCacheTrace()) fprintf(stderr, "satria cache: content changed %s\n", r->path);
  return c->ok = valid;
}
bool wsCacheNoopValid(const WsModel *m) {
  if (!wsRouterValidate()) return false; /* router opts.cache dibaca lebih dulu */
  profMark("gate-router"); /* profil */
  /* Metadata artefak (design/startup.md §9): artefak/input release hilang atau
     berubah -> batal no-op SEBELUM membaca fingerprint. */
  if (!artifactCacheValid()) {
    if (wsCacheTrace()) fprintf(stderr, "satria cache: artifact metadata invalid\n");
    return false;
  }
  struct stat fsb;
  if (stat(WS_CACHE_FP, &fsb) != 0) return false;
  struct stat ssb;
  if (stat(WS_CACHE_SOURCES, &ssb) != 0) return false;
  profMark("gate-artifact"); /* profil */
  WsCacheMap map = {0};
  if (!wsCacheLoadMap(&map)) {
    wsCacheMapFree(&map);
    return false;
  }
  profMark("gate-map"); /* profil */
  /* Fast path (§4): semua baris P + stat cocok -> selesai tanpa membuka
     fingerprint.cache. Menolak di sini bukan vonis — jalur penuh menilai. */
  int pfast = wsCachePFast(&map, wsCacheStampNs(&ssb));
  profMark("gate-pfast"); /* profil */
  if (pfast >= 0) {
    if (wsCacheTrace())
      fprintf(stderr, "satria cache: P-fast %s (checked=%zu)\n",
              pfast == 1 ? "hit" : "refreshed", map.n);
    wsCacheMapFree(&map);
    return true;
  }
  CsStore *st = csOpen(WS_CACHE_FP);
  if (!st) {
    wsCacheMapFree(&map);
    return false;
  }
  /* Stamp grup per proyek (§4): konfigurasi / daftar sumber berubah -> miss
     SEBELUM membuka seluruh entri; jejak menyebut proyeknya. Grup tanpa
     metadata (cache versi lama) dilewati — kompatibel mundur; simpan
     berikutnya menulis stamp baru. */
  for (int i = 0; m && i < m->projectCount; i++) {
    uint64_t proj = csHash(m->projects[i].name), c0 = 0;
    long long t0 = 0, sz0 = 0;
    if (!csProjMetaGet(st, proj, &c0, &t0, &sz0)) continue;
    if (c0 != wsProjStamp(m, i) || sz0 != (long long)csCountProj(st, proj)) {
      if (wsCacheTrace())
        fprintf(stderr, "satria cache: project stamp changed %s\n", m->projects[i].name);
      csClose(st);
      wsCacheMapFree(&map);
      return false;
    }
  }
  WsCacheCheck c = {&map, wsCacheStampNs(&fsb), true, 0, st, false};
  bool walked = csForEach(st, wsCacheVisit, &c);
  bool valid = walked && c.ok && c.seen > 0;
  if (valid && c.patched) {
    valid = csCommit(st);
    if (wsCacheTrace()) fprintf(stderr, "satria cache: PATCH %s\n", valid ? "committed" : "failed");
  }
  /* Segarkan metadata P bila fast path menolak (format lama / sentuh / racy):
     entri yang baru divalidasi menulis ulang baris P. Bila ada baris berubah,
     sinkronkan route sources di opts.cache — stempel sources.cache ikut
     berubah — supaya no-op hit ini kembali fast path pada run berikutnya. */
  if (valid && (!map.meta || c.patched)) {
    WsCacheMetaTab t = {0};
    if (csForEach(st, wsCacheMetaCollect, &t)) {
      bool changed = false;
      if (t.tn > 1) qsort(t.tab, t.tn, sizeof(WsCacheEnt), wsCacheEntCmp);
      if (wsCacheMetaRewrite(&map, t.tab, t.tn, &changed)) {
        if (wsCacheTrace())
          fprintf(stderr, "satria cache: P metadata %s\n", changed ? "refreshed" : "unchanged");
        if (changed && !optsSourcesRouteSync()) {
          if (wsCacheTrace()) fprintf(stderr, "satria cache: route sync failed\n");
          valid = false; /* konservatif: sync ulang terjadi di run sukses berikutnya */
        }
      } else if (wsCacheTrace()) {
        fprintf(stderr, "satria cache: P refresh failed\n");
      }
    }
    free(t.tab);
  }
  if (wsCacheTrace()) fprintf(stderr, "satria cache: %s (checked=%zu)\n", valid ? "hit" : "miss", c.seen);
  csClose(st);
  wsCacheMapFree(&map);
  return valid;
}

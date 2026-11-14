/* Shared opts.cache implementation: single-project and workspace. */
#include "opts_internal.h"
#include "../cmds/cmds_internal.h"
#include "../portability.h"
#include "../prof/prof.h" /* profMark: audit overhead fs-router */
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  uint64_t proj;
  char names[16][32];
  long long sourceTime;
  long long sourceSize;
  bool hasSource;
  int n;
} OptNameCtx;

static bool optNameVisit(const CsEntry *e, CsOp op, void *ctxp) {
  (void)op;
  OptNameCtx *ctx = ctxp;
  if (e->proj != ctx->proj || !e->payload || ctx->n >= 16) return true;
  size_t len = strlen(e->payload);
  if (!len || len >= sizeof(ctx->names[0])) return true;
  for (size_t i = 0; i < len; i++) {
    char ch = e->payload[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
      return true; /* nama tidak sah: entri diabaikan (guard path) */
  }
  if (strcmp(e->payload, WS_SOURCE_ROUTE) == 0) {
    ctx->hasSource = true;
    ctx->sourceTime = e->time;
    ctx->sourceSize = e->size;
    return true;
  }
  snprintf(ctx->names[ctx->n], sizeof(ctx->names[0]), "%s", e->payload);
  ctx->n++;
  return true;
}

/* Dibaca LEBIH DULU oleh no-op (cmdsFastStateValid/wsFastStateValid).
   true = lanjut ke validasi fast-state; false = router menyatakan cache yang
   dirutekan hilang/kosong untuk proyek ini — snapshot fast-state fase binary
   (entri `build.state` di caches/fingerprint.cache) tidak perlu dimuat.
   Tanpa opts.cache atau tanpa entri proyek: tanpa gate (jalur normal).
   Kontrak opts <-> sources: bila .satria/sources.cache ADA, route `sources`
   wajib ada dan stempelnya (ns + ukuran) harus cocok; route menunjuk indeks
   yang hilang juga menolak. Route opsional hanya untuk proyek yang memang
   belum dirutekan atau tidak punya indeks sumber. */
/* Diagnostik gerbang no-op (SATRIA_GATE_DEBUG=1): alasan router menolak. */
static bool optsDbg(void) {
  static int on = -1;
  if (on < 0) on = getenv("SATRIA_GATE_DEBUG") ? 1 : 0;
  return on == 1;
}

/* Ekor optsRouterValidate di atas OptNameCtx hasil scan (atau memo-nya):
   kontrak opts <-> sources + presence grup cache dirutekan. */
static bool optsRouterNamesValid(const OptNameCtx *names);

bool optsRouterValidate(void) {
  /* Memo hasil scan router per proses (audit 4/11 item 5, lanjutan item 10):
     fungsi ini dijalankan per proyek per fase — rt-scan (csOpen + csForEach
     opts.cache) berulang untuk isi file yang sama. Kunci = (hash FNV-1a isi
     opts.cache, ukuran, proj): hasil murni fungsi byte file — penulisan
     mengubah isi -> hash berubah -> memo basi alami; tidak ada invalidasi
     manual yang bisa terlewat (kelas bug audit 31/10 §10). Ekor validasi
     (stamp sources + presence grup) tetap jalan per panggilan: membaca file
     LAIN yang tidak masuk kunci. */
#define OPT_RN_MEMO 16
  typedef struct {
    unsigned long long h;
    long long size;
    uint64_t proj;
    OptNameCtx names;
  } OptRnMemo;
  static OptRnMemo memo[OPT_RN_MEMO];
  static int memoN = 0, memoNext = 0;

  uint64_t proj = cmdsCacheProjAddr();
  unsigned long long h;
  long long fsize;
  if (optsCacheFileHash(OPTS_CACHE_FILE, &h, &fsize)) {
    for (int i = 0; i < memoN; i++) {
      if (memo[i].h == h && memo[i].size == fsize && memo[i].proj == proj) {
        profMark("rt-scan-hit"); /* profil: memo scan router (item 5) */
        return optsRouterNamesValid(&memo[i].names);
      }
    }
  }
  CsStore *st = csOpen(OPTS_CACHE_FILE);
  if (!st) return true;
  OptNameCtx names;
  memset(&names, 0, sizeof(names));
  names.proj = proj;
  bool scanned = csForEach(st, optNameVisit, &names);
  csClose(st);
  profMark("rt-scan"); /* profil: csOpen+scrap opts.cache */
  if (!scanned) return true; /* rusak: serahkan ke jalur normal */
  /* Anti-poisoning: simpan hanya bila isi stabil selama parse — bila berubah
     di antara hash dan parse, pasangan (hash, names) tidak konsisten. */
  unsigned long long h2;
  long long fsize2;
  if (optsCacheFileHash(OPTS_CACHE_FILE, &h2, &fsize2) && h2 == h && fsize2 == fsize) {
    OptRnMemo *slot;
    if (memoN < OPT_RN_MEMO) {
      slot = &memo[memoN++];
    } else {
      slot = &memo[memoNext];
      memoNext = (memoNext + 1) % OPT_RN_MEMO;
    }
    slot->h = h;
    slot->size = fsize;
    slot->proj = proj;
    slot->names = names;
  }
  return optsRouterNamesValid(&names);
}

/* Ekor optsRouterValidate di atas OptNameCtx hasil scan (atau memo-nya):
   kontrak opts <-> sources + presence grup cache dirutekan. */
static bool optsRouterNamesValid(const OptNameCtx *names) {
  if (!names->n && !names->hasSource) return true; /* proyek belum dirutekan */
  long long mt = 0, sz = 0;
  bool idxOk = optsSourcesStamp(&mt, &sz);
  if (idxOk) {
    if (!names->hasSource || names->sourceTime != mt || names->sourceSize != sz) {
      if (optsDbg())
        fprintf(stderr,
                "satria: gate: route sources basi (route ada=%d %lld/%lld, indeks %lld/%lld)\n",
                (int)names->hasSource, names->sourceTime, names->sourceSize, mt, sz);
      return false; /* indeks ada tapi route hilang / stempel basi */
    }
  } else if (names->hasSource) {
    if (optsDbg())
      fprintf(stderr,
              "satria: gate: route sources ada tetapi indeks %s hilang\n",
              WS_SOURCE_FILE);
    return false; /* route menunjuk indeks yang hilang */
  }
  for (int i = 0; i < names->n; i++) {
    char path[128];
    snprintf(path, sizeof(path), ".satria/caches/%s.cache", names->names[i]);
    if (!optsCacheHasGroup(path, names->proj)) {
      if (optsDbg())
        fprintf(stderr, "satria: gate: grup '%s' tidak ada di %s\n", names->names[i], path);
      return false;
    }
  }
  profMark("rt-groups"); /* profil: buka tiap cache dirutekan utk cek grup */
  return true;
}


/* true bila router ada dan merutekan ke fingerprint. Payload dibaca sebelum
   store ditutup (kepemilikan pointer milik store). Kontrak sources sama dengan
   optsRouterValidate: indeks ada -> route wajib + cocok; indeks hilang -> route
   tak boleh ada. */
bool wsRouterValidate(void) {
  CsStore *st = csOpen(WS_ROUTER_FILE);
  if (!st) return false;
  CsEntry e;
  bool ok = csGet(st, csHash(WS_ROUTER_KEY), csHash(WS_ROUTER_NAME), &e) && e.payload &&
            strcmp(e.payload, WS_ROUTER_NAME) == 0;
  if (ok) {
    CsEntry src;
    bool hasRoute = csGet(st, csHash(WS_ROUTER_KEY), csHash(WS_SOURCE_ROUTE), &src) &&
                    src.payload && strcmp(src.payload, WS_SOURCE_ROUTE) == 0;
    long long mt = 0, sz = 0;
    bool idxOk = optsSourcesStamp(&mt, &sz);
    if (idxOk) ok = hasRoute && src.time == mt && src.size == sz;
    else ok = !hasRoute;
  }
  csClose(st);
  return ok;
}

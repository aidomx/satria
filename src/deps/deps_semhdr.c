/*
 * deps_semhdr — pemeriksaan semantik perubahan header pada workspace.
 *
 * Konservatif: header yang berubah hanya dimaafkan bila SEMUA perbedaan barisnya
 * adalah prototype fungsi (tanpa struct/enum/typedef/macro/static/inline), dan
 * nama fungsi-nya tidak muncul di TU mana pun dalam subtree dependensi (selain
 * baris prototype itu sendiri). Selain itu, TU dianggap berubah seperti biasa.
 *
 * Salinan header disimpan di .satria/hdr/<hash-path>.txt saat snapshot direkam,
 * sehingga perbedaan dapat dihitung terhadap isi yang terakhir terkompilasi.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deps_internal.h"
#include "../cmds/cmds_internal.h"
#include "../cache/state_blob.h"

#define SEM_MAX_NAMES 64

bool depsSemEnabled(void) {
  const char *e = getenv("SATRIA_HDR_SEM");
  /* Eksplisit 1 mengaktifkan di semua mode; eksplisit 0 menonaktifkan.
     Default hanya workspace: single-project tetap memakai invalidasi lama. */
  if (e) return strcmp(e, "1") == 0;
  return wsModeActive();
}

static bool isHeaderPath(const char *p) {
  size_t n = strlen(p);
  return n >= 2 && strcmp(p + n - 2, ".h") == 0;
}

/* Nama relatif salinan header (stabil per path sumber). */
static void copyRelFor(const char *path, char *out, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (const char *q = path; *q; q++) {
    h ^= (unsigned char)*q;
    h *= 1099511628211ULL;
  }
  snprintf(out, n, "hdr/%016llx.txt", (unsigned long long)h);
}

static char *readWhole(const char *path, size_t *outLen) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  rewind(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  if (outLen) *outLen = got;
  return buf;
}

/* Salinan header: mode workspace -> record caches/state.cache; single -> file
   .satria/hdr/<hash>.txt (jalur lama, tak berubah). */
static char *semCopyRead(const char *rel, size_t *outLen) {
  const char *wsp = cmdsStateWsProject();
  if (wsp) {
    char *b = NULL;
    size_t n = 0;
    if (!stateBlobGet(wsp, rel, &b, &n)) return NULL;
    if (outLen) *outLen = n;
    return b;
  }
  char cp[MAX_PATH + 64];
  cmdsStatePath(cp, sizeof(cp), rel);
  return readWhole(cp, outLen);
}

static bool semCopyWrite(const char *rel, const char *data, size_t n) {
  const char *wsp = cmdsStateWsProject();
  if (wsp) return stateBlobPut(wsp, rel, data, n);
  char cp[MAX_PATH + 64];
  cmdsStatePath(cp, sizeof(cp), rel);
  cmdsStateMkdirFor(cp);
  FILE *f = fopen(cp, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  if (fclose(f) != 0) ok = false;
  return ok;
}

/* Isi header immutable selama satu build. Simpan satu salinan per DepNode;
   pemanggil yang perlu memodifikasi buffer tetap meminta salinan terpisah. */
static const char *semEnsureNodeText(DepCache *dc, int idx, size_t *outLen) {
  if (idx < 0 || idx >= dc->count) return NULL;
  DepNode *node = &dc->nodes[idx];
  if (!isHeaderPath(node->path)) return NULL;
  if (!node->semTextLoaded || node->semTextMtime != node->mtime) {
    size_t len = 0;
    char *buf = readWhole(node->path, &len);
    if (!buf) return NULL;
    free(node->semText);
    node->semText = buf;
    node->semTextLen = len;
    node->semTextMtime = node->mtime;
    node->semTextLoaded = true;
  }
  if (outLen) *outLen = node->semTextLen;
  return node->semText;
}

static char *semReadNode(DepCache *dc, int idx, size_t *outLen) {
  if (idx < 0 || idx >= dc->count) return NULL;
  if (isHeaderPath(dc->nodes[idx].path)) {
    size_t len = 0;
    const char *text = semEnsureNodeText(dc, idx, &len);
    if (!text) return NULL;
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len + 1);
    if (outLen) *outLen = len;
    return copy;
  }
  return readWhole(dc->nodes[idx].path, outLen);
}

/* Simpan isi header saat snapshot direkam (dipanggil dari subtreeRecord).
   Node yang sama dapat muncul di subtree banyak TU; cukup rekam sekali selama
   mtime-nya tidak berubah. */
void depsSemRecordNode(DepCache *dc, int idx) {
  if (!depsSemEnabled() || idx < 0 || idx >= dc->count ||
      !isHeaderPath(dc->nodes[idx].path)) return;
  DepNode *node = &dc->nodes[idx];
  if (node->semRecorded && node->semRecordedMtime == node->mtime) return;
  char rel[64];
  copyRelFor(node->path, rel, sizeof(rel));
  size_t blen = 0;
  char *buf = semReadNode(dc, idx, &blen);
  if (!buf) return;
  bool saved = semCopyWrite(rel, buf, blen);
  free(buf);
  if (!saved) return;
  node->semRecorded = true;
  node->semRecordedMtime = node->mtime;
}

/* Identifier boundary: kata utuh (bukan bagian dari identifier yang lebih panjang). */
static bool isIdentCh(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool hasWord(const char *s, const char *w) {
  size_t wl = strlen(w);
  if (wl == 0) return false;
  for (const char *p = s; (p = strstr(p, w)) != NULL; p += wl) {
    bool lb = (p == s) || !isIdentCh(p[-1]);
    bool rb = !isIdentCh(p[wl]);
    if (lb && rb) return true;
  }
  return false;
}

static bool hasWordSpan(const char *s, size_t n, const char *w) {
  size_t wl = strlen(w);
  if (!wl || wl > n) return false;
  for (size_t i = 0; i + wl <= n; i++) {
    if (memcmp(s + i, w, wl) != 0) continue;
    bool lb = i == 0 || !isIdentCh(s[i - 1]);
    bool rb = i + wl == n || !isIdentCh(s[i + wl]);
    if (lb && rb) return true;
  }
  return false;
}

static bool lineInSetSpan(const char *s, size_t n, char **set, int count) {
  while (n && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r')) { s++; n--; }
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
  for (int i = 0; i < count; i++)
    if (strlen(set[i]) == n && memcmp(set[i], s, n) == 0) return true;
  return false;
}

static char *trimInPlace(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r') s++;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';
  return s;
}

/* Pecah buffer per baris (in-place), trim, buang baris kosong. */
static int splitLines(char *buf, char ***out) {
  int cap = 64, n = 0;
  char **v = malloc((size_t)cap * sizeof(char *));
  if (!v) return -1;
  char *p = buf;
  while (p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    char *t = trimInPlace(p);
    if (*t) {
      if (n == cap) {
        cap *= 2;
        char **nv = realloc(v, (size_t)cap * sizeof(char *));
        if (!nv) {
          free(v);
          return -1;
        }
        v = nv;
      }
      v[n++] = t;
    }
    p = nl ? nl + 1 : NULL;
  }
  *out = v;
  return n;
}

static bool inSet(char **set, int n, const char *s) {
  for (int i = 0; i < n; i++)
    if (strcmp(set[i], s) == 0) return true;
  return false;
}

/* Prototype fungsi tunggal: `<tipe> <nama>(<args>);` tanpa badan/keyword berbahaya. */
static bool isProtoLine(const char *ln, char *name, size_t nsz) {
  if (!ln[0] || ln[0] == '#' || ln[0] == '/' || ln[0] == '*') return false;
  size_t n = strlen(ln);
  if (n < 4 || ln[n - 1] != ';') return false;
  if (strpbrk(ln, "{}=\"'")) return false;
  const char *bad[] = {"struct", "union", "enum", "typedef", "static", "inline", "define"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    if (hasWord(ln, bad[i])) return false;
  const char *lp = strchr(ln, '(');
  const char *rp = strrchr(ln, ')');
  if (!lp || !rp || rp < lp || strcmp(rp, ");") != 0) return false;
  /* nama = identifier tepat sebelum '(' (boleh spasi di antara) */
  const char *e = lp;
  while (e > ln && (e[-1] == ' ' || e[-1] == '\t')) e--;
  const char *b = e;
  while (b > ln && isIdentCh(b[-1])) b--;
  size_t nl = (size_t)(e - b);
  if (nl == 0 || nl >= nsz) return false;
  /* butuh tipe di depan (bukan panggilan makro seperti `FOO(x);`) */
  const char *pre = ln;
  while (pre < b && (*pre == ' ' || *pre == '\t')) pre++;
  if (pre >= b) return false;
  memcpy(name, b, nl);
  name[nl] = '\0';
  return true;
}

/* Hitung nama prototype dari baris yang berubah; false bila ada baris berubah
   yang bukan prototype murni. */
static bool collectNames(char **chg, int nchg, char names[][128], int *nnames) {
  *nnames = 0;
  for (int i = 0; i < nchg; i++) {
    char nm[128];
    if (!isProtoLine(chg[i], nm, sizeof(nm))) return false;
    bool dup = false;
    for (int k = 0; k < *nnames; k++)
      if (strcmp(names[k], nm) == 0) dup = true;
    if (!dup) {
      if (*nnames >= SEM_MAX_NAMES) return false;
      snprintf(names[(*nnames)++], 128, "%s", nm);
    }
  }
  return true;
}

/* true bila `name` dipakai (kata utuh) di node `idx` — baris perubahan header
   sendiri dilewati (itu deklarasi yang memang baru). */
static bool nodeUsesName(DepCache *dc, int idx, const char *name, char **chg, int nchg) {
  if (isHeaderPath(dc->nodes[idx].path)) {
    size_t n = 0;
    const char *buf = semEnsureNodeText(dc, idx, &n);
    if (!buf) return true; /* tak terbaca: konservatif */
    size_t pos = 0;
    while (pos < n) {
      size_t end = pos;
      while (end < n && buf[end] != '\n') end++;
      size_t a = pos, len = end - pos;
      while (len && (buf[a] == ' ' || buf[a] == '\t' || buf[a] == '\r')) { a++; len--; }
      while (len && (buf[a + len - 1] == ' ' || buf[a + len - 1] == '\t' || buf[a + len - 1] == '\r')) len--;
      if (!lineInSetSpan(buf + a, len, chg, nchg) && hasWordSpan(buf + a, len, name)) return true;
      pos = end < n ? end + 1 : n;
    }
    return false;
  }

  size_t n = 0;
  char *buf = readWhole(dc->nodes[idx].path, &n);
  if (!buf) return true; /* tak terbaca: konservatif */
  char **lines = NULL;
  int nl = splitLines(buf, &lines);
  bool used = nl < 0;
  for (int i = 0; i < nl && !used; i++)
    if (hasWord(lines[i], name)) used = true;
  free(lines);
  free(buf);
  return used;
}

/* Inti: apakah perubahan header `idx` (vs salinan terakhir terkompilasi) aman
   untuk TU `root`? */
bool depsSemAcceptNode(DepCache *dc, int root, int idx) {
  if (!depsSemEnabled() || root < 0 || idx < 0 || root == idx) return false;
  if (!isHeaderPath(dc->nodes[idx].path) || dc->nodes[idx].mtime < 0) return false;

  char rel[64];
  copyRelFor(dc->nodes[idx].path, rel, sizeof(rel));
  char *oldb = semCopyRead(rel, NULL);
  if (!oldb) return false;
  char *newb = semReadNode(dc, idx, NULL);
  if (!newb) {
    free(oldb);
    return false;
  }
  char **oldl = NULL, **newl = NULL;
  int on = splitLines(oldb, &oldl), nn = splitLines(newb, &newl);
  bool ok = false;
  char **chg = NULL;
  int nchg = 0;
  char (*names)[128] = NULL;
  int nnames = 0;
  if (on < 0 || nn < 0) goto done;

  /* baris berubah = yang ada di salah satu sisi tapi tidak di sisi lain */
  chg = malloc((size_t)(on + nn + 1) * sizeof(char *));
  if (!chg) goto done;
  for (int i = 0; i < nn; i++)
    if (!inSet(oldl, on, newl[i])) chg[nchg++] = newl[i];
  for (int i = 0; i < on; i++)
    if (!inSet(newl, nn, oldl[i])) chg[nchg++] = oldl[i];
  if (nchg == 0) goto done; /* hash beda tapi himpunan baris sama: konservatif */

  names = malloc((size_t)SEM_MAX_NAMES * 128);
  if (!names) goto done;
  if (!collectNames(chg, nchg, names, &nnames)) goto done;

  /* telusuri subtree TU: setiap node tidak boleh memakai nama prototype yang berubah */
  int cap = dc->count + 256;
  bool *vis = calloc((size_t)cap, sizeof(bool));
  int *st = malloc((size_t)cap * sizeof(int));
  if (!vis || !st) {
    free(vis);
    free(st);
    goto done;
  }
  int sp = 0;
  st[sp++] = root;
  vis[root] = true;
  bool walk = true;
  while (sp > 0 && walk) {
    int cur = st[--sp];
    if (cur >= cap) {
      walk = false;
      break;
    }
    depParseNodeIfNeeded(dc, cur);
    for (int k = 0; k < nnames && walk; k++)
      if (nodeUsesName(dc, cur, names[k], chg, nchg)) walk = false;
    if (!walk) break;
    int nk = dc->nodes[cur].nkids;
    for (int j = 0; j < nk; j++) {
      int kid = dc->nodes[cur].kids[j];
      if (kid >= cap) {
        walk = false;
        break;
      }
      if (!vis[kid]) {
        vis[kid] = true;
        st[sp++] = kid;
      }
    }
  }
  free(vis);
  free(st);
  ok = walk;

done:
  free(chg);
  free(names);
  free(oldl);
  free(newl);
  free(oldb);
  free(newb);
  return ok;
}

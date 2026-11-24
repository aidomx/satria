/* eval.c — evaluator ekspresi & selektor DSL unit test (M4b).
   Padanan C dari bagian resolve_args/ev_* pada tests/unit/lib/dsl.sh.
   Nilai selalu string (seperti bash); angka hanya dipakai pembanding dan
   predikat hitung. Regex ERE kecil disertakan agar tidak butuh grep dan
   berperilaku sama di POSIX maupun Windows. */

#include "dsl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <glob.h>
#endif

/* ==================== util kecil ==================== */

static char *dupN(const char *s, size_t n) {
  char *p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

static char *dupS(const char *s) { return dupN(s, strlen(s)); }

static void appendf(char *out, size_t n, size_t *len, const char *s) {
  size_t l = strlen(s);
  if (*len + l >= n) l = n - 1 - *len;
  if ((long)l <= 0) return;
  memcpy(out + *len, s, l);
  *len += l;
  out[*len] = '\0';
}

int dslErr(DslCtx *c, int kind, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->errMsg, sizeof(c->errMsg), fmt, ap);
  va_end(ap);
  c->errKind = kind;
  return kind;
}

/* ==================== konteks & variabel ==================== */

void dslCtxInit(DslCtx *c, const char *work, const char *repo, const char *discovery,
                int jobs) {
  memset(c, 0, sizeof(*c));
  copyStr(c->work, sizeof(c->work), work ? work : "");
  copyStr(c->repo, sizeof(c->repo), repo ? repo : "");
  copyStr(c->discovery, sizeof(c->discovery), discovery ? discovery : "");
  c->jobs = jobs > 0 ? jobs : 2;
  c->fixture = "";
}

void dslCtxFree(DslCtx *c) {
  for (int i = 0; i < c->nvars; i++) {
    free(c->vars[i].name);
    free(c->vars[i].value);
  }
  free(c->vars);
  for (int i = 0; i < c->nruns; i++) {
    free(c->runs[i].label);
    free(c->runs[i].out);
  }
  free(c->runs);
  for (int i = 0; i < c->nmarks; i++) {
    free(c->marks[i].name);
    free(c->marks[i].path);
  }
  free(c->marks);
  memset(c, 0, sizeof(*c));
}

const char *dslVarGet(DslCtx *c, const char *name) {
  for (int i = 0; i < c->nvars; i++)
    if (strcmp(c->vars[i].name, name) == 0) return c->vars[i].value;
  return NULL;
}

void dslVarSetN(DslCtx *c, const char *name, const char *value, size_t n) {
  for (int i = 0; i < c->nvars; i++) {
    if (strcmp(c->vars[i].name, name) != 0) continue;
    char *nv = dupN(value ? value : "", n);
    if (!nv) return;
    free(c->vars[i].value);
    c->vars[i].value = nv;
    return;
  }
  if (c->nvars == c->capvars) {
    int nc = c->capvars ? c->capvars * 2 : 16;
    DslVar *grown = realloc(c->vars, (size_t)nc * sizeof(*grown));
    if (!grown) return;
    c->vars = grown;
    c->capvars = nc;
  }
  char *nm = dupS(name);
  char *nv = dupN(value ? value : "", n);
  if (!nm || !nv) {
    free(nm);
    free(nv);
    return;
  }
  c->vars[c->nvars].name = nm;
  c->vars[c->nvars].value = nv;
  c->nvars++;
}

void dslVarSet(DslCtx *c, const char *name, const char *value) {
  dslVarSetN(c, name, value ? value : "", value ? strlen(value) : 0);
}

static DslRun *runFind(DslCtx *c, const char *label) {
  for (int i = 0; i < c->nruns; i++)
    if (strcmp(c->runs[i].label, label) == 0) return &c->runs[i];
  return NULL;
}

static DslMark *markFind(DslCtx *c, const char *name) {
  for (int i = 0; i < c->nmarks; i++)
    if (strcmp(c->marks[i].name, name) == 0) return &c->marks[i];
  return NULL;
}

/* ==================== literal ==================== */

void dslUnesc(const char *s, char *out, size_t n) {
  size_t o = 0;
  for (const char *p = s; *p; p++) {
    char ch = *p;
    if (ch == '\\' && p[1]) {
      switch (p[1]) {
        case 'r': ch = '\r'; p++; break;
        case 'n': ch = '\n'; p++; break;
        case 't': ch = '\t'; p++; break;
        case '\\': ch = '\\'; p++; break;
        default: break;
      }
    }
    if (o + 1 < n) out[o++] = ch;
  }
  out[o] = '\0';
}

void dslUnq(char *s) {
  size_t n = strlen(s);
  if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
    memmove(s, s + 1, n - 2);
    s[n - 2] = '\0';
  }
}

bool dslTruthy(const char *s) {
  return s && *s && strcmp(s, "0") != 0 && strcmp(s, "false") != 0;
}

const char *dslWPath(DslCtx *c, const char *p, char *buf, size_t n) {
  if (p[0] == '/') {
    copyStr(buf, n, p);
    return buf;
  }
  size_t wl = strlen(c->work), pl = strlen(p);
  if (wl + 1 + pl < n) {
    memcpy(buf, c->work, wl);
    buf[wl] = '/';
    memcpy(buf + wl + 1, p, pl + 1);
  } else {
    copyStr(buf, n, c->work);
  }
  return buf;
}

char *dslReadFile(const char *path, size_t *lenOut) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  long sz = ftell(fp);
  if (sz < 0) {
    fclose(fp);
    return NULL;
  }
  rewind(fp);
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  buf[got] = '\0';
  if (lenOut) *lenOut = got;
  return buf;
}

void dslInterp(DslCtx *c, const char *s, char *out, size_t n) {
  size_t o = 0;
  out[0] = '\0';
  for (const char *p = s; *p;) {
    if (*p == '{') {
      const char *close = strchr(p + 1, '}');
      if (close && close > p + 1) {
        char name[128];
        size_t ln = (size_t)(close - p - 1);
        if (ln < sizeof(name)) {
          memcpy(name, p + 1, ln);
          name[ln] = '\0';
          const char *v = dslVarGet(c, name);
          if (v) {
            appendf(out, n, &o, v);
            p = close + 1;
            continue;
          }
        }
      }
    }
    char one[2] = {*p, '\0'};
    appendf(out, n, &o, one);
    p++;
  }
}

/* ==================== regex ERE mini ==================== */

enum { RX_CHAR = 1, RX_ANY, RX_CLASS, RX_BOL, RX_EOL, RX_SEQ, RX_ALT, RX_REP };

typedef struct Rx {
  int t;
  unsigned char ch;
  unsigned char cls[256];
  struct Rx *kid;
  struct Rx *next;
  int min, max; /* rep: max -1 = tak terbatas */
} Rx;

static Rx *rxNew(int t) {
  Rx *n = calloc(1, sizeof(*n));
  if (n) n->t = t;
  return n;
}

static void rxFree(Rx *n) {
  while (n) {
    Rx *next = n->next;
    rxFree(n->kid);
    free(n);
    n = next;
  }
}

static void rxClassAdd(Rx *n, unsigned char a, unsigned char b) {
  for (int i = a; i <= b; i++) n->cls[i] = 1;
}

static Rx *rxAtom(const char **pp);

/* Rentang {m,n}; cursor menunjuk sesudah '}'. */
static bool rxCount(const char **pp, int *min, int *max) {
  const char *p = *pp;
  if (*p != '{') return false;
  p++;
  if (!isdigit((unsigned char)*p)) return false;
  int a = 0;
  while (isdigit((unsigned char)*p)) a = a * 10 + (*p++ - '0');
  int b = a;
  if (*p == ',') {
    p++;
    if (*p == '}') {
      b = -1;
    } else {
      b = 0;
      while (isdigit((unsigned char)*p)) b = b * 10 + (*p++ - '0');
    }
  }
  if (*p != '}') return false;
  *pp = p + 1;
  *min = a;
  *max = b;
  return true;
}

static Rx *rxRep(Rx *atom, int min, int max) {
  Rx *n = rxNew(RX_REP);
  if (!n) {
    rxFree(atom);
    return NULL;
  }
  n->kid = atom;
  n->min = min;
  n->max = max;
  return n;
}

static Rx *rxAtom(const char **pp) {
  const char *p = *pp;
  Rx *n = NULL;
  if (*p == '(') {
    p++;
    /* alt sampai ')' */
    Rx *head = NULL, *tail = NULL;
    for (;;) {
      Rx *seq = rxNew(RX_SEQ);
      Rx *st = NULL, *sl = NULL;
      while (*p && *p != '|' && *p != ')') {
        Rx *a = rxAtom(&p);
        if (!a) {
          rxFree(seq);
          rxFree(head);
          return NULL;
        }
        /* suffix */
        for (;;) {
          if (*p == '*') {
            p++;
            a = rxRep(a, 0, -1);
          } else if (*p == '+') {
            p++;
            a = rxRep(a, 1, -1);
          } else if (*p == '?') {
            p++;
            a = rxRep(a, 0, 1);
          } else {
            int mn, mx;
            if (rxCount(&p, &mn, &mx))
              a = rxRep(a, mn, mx);
            else
              break;
          }
          if (!a) {
            rxFree(seq);
            rxFree(head);
            return NULL;
          }
        }
        if (!st) {
          st = sl = a;
        } else {
          sl->next = a;
          sl = a;
        }
      }
      seq->kid = st;
      if (!head) {
        head = tail = seq;
      } else {
        tail->next = seq;
        tail = seq;
      }
      if (*p == '|') {
        p++;
        continue;
      }
      break;
    }
    if (*p != ')') {
      rxFree(head);
      return NULL;
    }
    p++;
    if (head && head->next) {
      n = rxNew(RX_ALT);
      if (n) n->kid = head;
      else rxFree(head);
    } else if (head) {
      Rx *only = head->kid;
      head->kid = NULL;
      rxFree(head);
      n = only;
    } else {
      n = rxNew(RX_SEQ); /* grup kosong */
    }
  } else if (*p == '[') {
    p++;
    n = rxNew(RX_CLASS);
    if (!n) return NULL;
    bool neg = false;
    if (*p == '^') {
      neg = true;
      p++;
    }
    bool first = true;
    while (*p && (*p != ']' || first)) {
      unsigned char a = (unsigned char)*p++;
      if (a == '\\' && *p) a = (unsigned char)*p++;
      if (*p == '-' && p[1] && p[1] != ']') {
        p++;
        unsigned char b = (unsigned char)*p++;
        if (b == '\\' && *p) b = (unsigned char)*p++;
        rxClassAdd(n, a, b);
      } else {
        n->cls[a] = 1;
      }
      first = false;
    }
    if (*p != ']') {
      rxFree(n);
      return NULL;
    }
    p++;
    if (neg)
      for (int i = 0; i < 256; i++) n->cls[i] = !n->cls[i];
  } else if (*p == '.') {
    p++;
    n = rxNew(RX_ANY);
  } else if (*p == '^') {
    p++;
    n = rxNew(RX_BOL);
  } else if (*p == '$') {
    p++;
    n = rxNew(RX_EOL);
  } else if (*p == '\\' && p[1]) {
    p++;
    n = rxNew(RX_CHAR);
    if (n) n->ch = (unsigned char)*p++;
  } else if (*p && *p != '|' && *p != ')') {
    n = rxNew(RX_CHAR);
    if (n) n->ch = (unsigned char)*p++;
  }
  *pp = p;
  return n;
}

enum { I_CHAR = 1, I_ANY, I_CLASS, I_BOL, I_EOL, I_SPLIT, I_JMP, I_MATCH };

typedef struct {
  int op, x, y;
  unsigned char ch;
  unsigned char cls[256];
} Ins;

typedef struct {
  Ins *v;
  int n, cap;
} Prog;

static int emit(Prog *p, int op) {
  if (p->n == p->cap) {
    int nc = p->cap ? p->cap * 2 : 64;
    Ins *g = realloc(p->v, (size_t)nc * sizeof(*g));
    if (!g) return -1;
    p->v = g;
    p->cap = nc;
  }
  Ins *s = &p->v[p->n];
  memset(s, 0, sizeof(*s));
  s->op = op;
  return p->n++;
}

static void gen(Prog *p, const Rx *n);

static void genAlt(Prog *p, const Rx *k) {
  if (!k->next) {
    gen(p, k);
    return;
  }
  int sp = emit(p, I_SPLIT);
  int a = p->n;
  gen(p, k);
  int j = emit(p, I_JMP);
  int b = p->n;
  genAlt(p, k->next);
  int end = p->n;
  if (sp >= 0) {
    p->v[sp].x = a;
    p->v[sp].y = b;
  }
  if (j >= 0) p->v[j].x = end;
}

static void gen(Prog *p, const Rx *n) {
  if (!n) return;
  switch (n->t) {
    case RX_CHAR: {
      int i = emit(p, I_CHAR);
      if (i >= 0) p->v[i].ch = n->ch;
      break;
    }
    case RX_ANY: emit(p, I_ANY); break;
    case RX_CLASS: {
      int i = emit(p, I_CLASS);
      if (i >= 0) memcpy(p->v[i].cls, n->cls, 256);
      break;
    }
    case RX_BOL: emit(p, I_BOL); break;
    case RX_EOL: emit(p, I_EOL); break;
    case RX_SEQ:
      for (const Rx *k = n->kid; k; k = k->next) gen(p, k);
      break;
    case RX_ALT: genAlt(p, n->kid); break;
    case RX_REP: {
      int mn = n->min, mx = n->max;
      if (mn > 1000) mn = 1000;
      if (mx > 1000) mx = 1000;
      for (int i = 0; i < mn; i++) gen(p, n->kid);
      if (mx < 0) {
        int sp = emit(p, I_SPLIT);
        int body = p->n;
        gen(p, n->kid);
        int j = emit(p, I_JMP);
        int end = p->n;
        if (sp >= 0) {
          p->v[sp].x = body;
          p->v[sp].y = end;
        }
        if (j >= 0) p->v[j].x = sp;
      } else {
        for (int i = mn; i < mx; i++) {
          int sp = emit(p, I_SPLIT);
          int body = p->n;
          gen(p, n->kid);
          int end = p->n;
          if (sp >= 0) {
            p->v[sp].x = body;
            p->v[sp].y = end;
          }
        }
      }
      break;
    }
    default: break;
  }
}

/* DFS dengan memo (pc, offset) mencegah loop epsilon. */
static bool dfs(Prog *p, int pc, const char *s, const char *base, size_t len,
                unsigned char *seen) {
  size_t key = (size_t)pc * (len + 1) + (size_t)(s - base);
  if (seen[key]) return false;
  seen[key] = 1;
  const Ins *in = &p->v[pc];
  switch (in->op) {
    case I_CHAR: return *s && (unsigned char)*s == in->ch && dfs(p, pc + 1, s + 1, base, len, seen);
    case I_ANY: return *s != '\0' && dfs(p, pc + 1, s + 1, base, len, seen);
    case I_CLASS:
      return *s && in->cls[(unsigned char)*s] && dfs(p, pc + 1, s + 1, base, len, seen);
    case I_BOL:
      return s == base && dfs(p, pc + 1, s, base, len, seen);
    case I_EOL: return *s == '\0' && dfs(p, pc + 1, s, base, len, seen);
    case I_SPLIT:
      if (dfs(p, in->x, s, base, len, seen)) return true;
      return dfs(p, in->y, s, base, len, seen);
    case I_JMP: return dfs(p, in->x, s, base, len, seen);
    case I_MATCH: return true;
    default: return false;
  }
}

static Prog rxCompile(const char *pat) {
  Prog p = {0};
  const char *s = pat;
  Rx *head = NULL, *tail = NULL;
  for (;;) {
    Rx *seq = rxNew(RX_SEQ);
    Rx *st = NULL, *sl = NULL;
    while (*s && *s != '|') {
      Rx *a = rxAtom(&s);
      if (!a) break;
      for (;;) {
        if (*s == '*') {
          s++;
          a = rxRep(a, 0, -1);
        } else if (*s == '+') {
          s++;
          a = rxRep(a, 1, -1);
        } else if (*s == '?') {
          s++;
          a = rxRep(a, 0, 1);
        } else {
          int mn, mx;
          if (rxCount(&s, &mn, &mx))
            a = rxRep(a, mn, mx);
          else
            break;
        }
        if (!a) break;
      }
      if (!a) break;
      if (!st) {
        st = sl = a;
      } else {
        sl->next = a;
        sl = a;
      }
    }
    seq->kid = st;
    if (!head) {
      head = tail = seq;
    } else {
      tail->next = seq;
      tail = seq;
    }
    if (*s == '|') {
      s++;
      continue;
    }
    break;
  }
  if (head && head->next) {
    Rx *alt = rxNew(RX_ALT);
    if (alt) alt->kid = head;
    else rxFree(head);
    gen(&p, alt);
    rxFree(alt);
  } else if (head) {
    /* Satu alternatif: SEQ utuh (kid = rantai atom), bukan hanya atom pertama. */
    gen(&p, head);
    rxFree(head);
  }
  emit(&p, I_MATCH);
  return p;
}

bool dslRegexSearch(const char *pattern, const char *text) {
  if (!pattern) return false;
  Prog p = rxCompile(pattern);
  if (p.n == 0) {
    free(p.v);
    return false;
  }
  size_t len = strlen(text);
  unsigned char *seen = calloc((size_t)p.n * (len + 1) + 1, 1);
  if (!seen) {
    free(p.v);
    return false;
  }
  bool hit = false;
  size_t states = (size_t)p.n * (len + 1) + 1;
  for (size_t start = 0; start <= len && !hit; start++) {
    /* Status kunjungan harus bersih tiap posisi awal; sisa kunjungan dari
       start sebelumnya membuat pola cocok semu. */
    memset(seen, 0, states);
    hit = dfs(&p, 0, text + start, text, len, seen);
  }
  free(seen);
  free(p.v);
  return hit;
}

bool dslRegexText(const char *pattern, const char *text, size_t len) {
  size_t pos = 0;
  while (pos <= len) {
    size_t e = pos;
    while (e < len && text[e] != '\n') e++;
    char *line = dupN(text + pos, e - pos);
    if (!line) return false;
    bool hit = dslRegexSearch(pattern, line);
    free(line);
    if (hit) return true;
    if (e >= len) break;
    pos = e + 1;
  }
  return false;
}

int dslRegexCount(const char *pattern, const char *text, size_t len) {
  int n = 0;
  size_t pos = 0;
  while (pos <= len) {
    size_t e = pos;
    while (e < len && text[e] != '\n') e++;
    char *line = dupN(text + pos, e - pos);
    if (!line) return n;
    if (dslRegexSearch(pattern, line)) n++;
    free(line);
    if (e >= len) break;
    pos = e + 1;
  }
  return n;
}

/* ==================== selektor ==================== */

static bool strHas(const char *s, const char *sub) { return strstr(s, sub) != NULL; }

static char *readTsv(DslCtx *c) {
  if (!c->discovery[0]) return NULL;
  char path[SATRIA_MAX_PATH + 32];
  snprintf(path, sizeof(path), "%s/sources.tsv", c->discovery);
  return dslReadFile(path, NULL);
}

/* Kumpulkan path milik proyek (fallback: proyek pertama di sources.tsv).
   `wantOut` (opsional) menerima nama proyek yang akhirnya dipakai. Baris
   TSV tidak dimodifikasi: kolom nama dan path dipisah dengan memchr agar
   pass kedua masih melihat TAB. */
static bool projPaths(DslCtx *c, const char *proj, List *out, char *wantOut,
                      size_t wantN) {
  char *tsv = readTsv(c);
  if (!tsv) return false;
  bool found = false;
  char first[256] = "";
  size_t projLen = strlen(proj);
  for (char *line = tsv; *line;) {
    char *end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    char *tab = memchr(line, '\t', (size_t)(end - line));
    if (tab) {
      if (!first[0]) {
        size_t n = (size_t)(tab - line);
        if (n >= sizeof(first)) n = sizeof(first) - 1;
        memcpy(first, line, n);
        first[n] = '\0';
      }
      if ((size_t)(tab - line) == projLen && memcmp(line, proj, projLen) == 0)
        found = true;
    }
    line = *end ? end + 1 : end;
  }
  const char *want = found ? proj : first;
  if (!want[0]) {
    free(tsv);
    return false;
  }
  size_t wantLen = strlen(want);
  for (char *line = tsv; *line;) {
    char *end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    char *tab = memchr(line, '\t', (size_t)(end - line));
    if (tab && (size_t)(tab - line) == wantLen && memcmp(line, want, wantLen) == 0) {
      char *tmp = dupN(tab + 1, (size_t)(end - (tab + 1)));
      if (tmp) {
        listAdd(out, tmp);
        free(tmp);
      }
    }
    line = *end ? end + 1 : end;
  }
  free(tsv);
  if (wantOut && wantN) copyStr(wantOut, wantN, want);
  return out->count > 0;
}

static bool isEmbedded(const char *p) {
  const char *b = strrchr(p, '/');
  b = b ? b + 1 : p;
  return strcmp(b, "embedded.c") == 0;
}

static bool selSource(DslCtx *c, const char *proj, char *out, size_t n) {
  List l = {0};
  if (!projPaths(c, proj, &l, NULL, 0)) {
    listFree(&l);
    return false;
  }
  const char *best = NULL;
  int bestKey = 2;
  for (int i = 0; i < l.count; i++) {
    const char *p = l.items[i];
    if (isEmbedded(p)) continue;
    int key = (strncmp(p, "src/", 4) == 0 || strHas(p, "/src/")) ? 0 : 1;
    if (!best || key < bestKey || (key == bestKey && strcmp(p, best) < 0)) {
      best = p;
      bestKey = key;
    }
  }
  if (best) snprintf(out, n, "%s/%s", proj, best);
  listFree(&l);
  return best != NULL;
}

static bool selSources(DslCtx *c, const char *proj, char *out, size_t n) {
  List l = {0};
  if (!projPaths(c, proj, &l, NULL, 0)) {
    listFree(&l);
    return false;
  }
  size_t o = 0;
  bool first = true;
  for (int i = 0; i < l.count; i++) {
    if (isEmbedded(l.items[i])) continue;
    if (!first) appendf(out, n, &o, " ");
    appendf(out, n, &o, proj);
    appendf(out, n, &o, "/");
    appendf(out, n, &o, l.items[i]);
    first = false;
  }
  listFree(&l);
  return !first;
}

static bool selDir(DslCtx *c, const char *proj, char *out, size_t n) {
  List l = {0};
  char want[256] = "";
  bool ok = projPaths(c, proj, &l, want, sizeof(want));
  listFree(&l);
  if (!ok) return false;
  snprintf(out, n, "%s", want);
  return true;
}

/* Baris WsBuildfile: `<key><spasi>= / as<spasi><nilai>` (spasi di nilai dibuang). */
static bool buildfileValue(DslCtx *c, const char *key, const char *sep, char *out,
                           size_t n) {
  char path[SATRIA_MAX_PATH + 32];
  snprintf(path, sizeof(path), "%s/WsBuildfile", c->work);
  char *txt = dslReadFile(path, NULL);
  if (!txt) return false;
  bool hit = false;
  size_t klen = strlen(key);
  for (char *line = txt; *line && !hit;) {
    char *e = strchr(line, '\n');
    if (e) *e = '\0';
    if (strncmp(line, key, klen) == 0) {
      const char *p = line + klen;
      while (*p == ' ' || *p == '\t') p++;
      if (sep[0] == '=') {
        if (*p == '=') p++;
      } else if (strncmp(p, sep, strlen(sep)) == 0) {
        p += strlen(sep);
      } else {
        line = e ? e + 1 : line + strlen(line);
        continue;
      }
      while (*p == ' ' || *p == '\t') p++;
      size_t o = 0;
      for (; *p; p++)
        if (*p != ' ' && *p != '\t' && o + 1 < n) out[o++] = *p;
      out[o] = '\0';
      hit = true;
    }
    line = e ? e + 1 : line + strlen(line);
  }
  free(txt);
  return hit;
}

static bool selBinary(DslCtx *c, const char *proj, char *out, size_t n) {
  char key[512], alias[256] = "", name[256] = "";
  snprintf(key, sizeof(key), "%s.output", proj);
  if (buildfileValue(c, key, "as", alias, sizeof(alias)) && alias[0]) {
    snprintf(key, sizeof(key), "%s.binaryName", alias);
    buildfileValue(c, key, "=", name, sizeof(name));
  }
  if (!name[0]) snprintf(name, sizeof(name), "%s", proj);
  snprintf(out, n, "bin/%s", name);
  return true;
}

static bool selArchive(DslCtx *c, const char *proj, char *out, size_t n) {
  char path[SATRIA_MAX_PATH + 32];
  snprintf(path, sizeof(path), "%s/WsBuildfile", c->work);
  char *txt = dslReadFile(path, NULL);
  if (!txt) return false;
  char key[512], alias[256] = "", file[256] = "";
  /* <p>.embedded.<x> as <alias> → <alias>.file */
  for (char *line = txt; *line && !alias[0];) {
    char *e = strchr(line, '\n');
    if (e) *e = '\0';
    snprintf(key, sizeof(key), "%s.embedded.", proj);
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) == 0) {
      const char *p = line + kl;
      while (*p && *p != ' ' && *p != '\t') p++; /* ekstensi */
      while (*p == ' ' || *p == '\t') p++;
      if (strncmp(p, "as", 2) == 0) {
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        size_t o = 0;
        for (; *p && o + 1 < sizeof(alias); p++)
          if (*p != ' ' && *p != '\t') alias[o++] = *p;
        alias[o] = '\0';
      }
    }
    line = e ? e + 1 : line + strlen(line);
  }
  if (alias[0]) {
    snprintf(key, sizeof(key), "%s.file", alias);
    if (buildfileValue(c, key, "=", file, sizeof(file)) && file[0]) {
      snprintf(out, n, "%s", file);
      free(txt);
      return true;
    }
  }
  free(txt);
  char dir[256] = "", name[256] = "";
  snprintf(key, sizeof(key), "%s.archive.dir", proj);
  buildfileValue(c, key, "=", dir, sizeof(dir));
  snprintf(key, sizeof(key), "%s.archive.name", proj);
  buildfileValue(c, key, "=", name, sizeof(name));
  if (!dir[0] || !name[0]) return false;
  snprintf(out, n, "%s/%s.tar", dir, name);
  return true;
}

static bool selWs(DslCtx *c, const char *key, char *out, size_t n) {
  return buildfileValue(c, key, "=", out, n) && out[0];
}

static void replaceAll(const char *in, const char *needle, const char *repl, char *out,
                       size_t n) {
  size_t o = 0;
  out[0] = '\0';
  size_t nl = strlen(needle);
  const char *p = in;
  while (*p) {
    const char *hit = strstr(p, needle);
    if (!hit) {
      appendf(out, n, &o, p);
      break;
    }
    size_t pre = (size_t)(hit - p);
    char *tmp = dupN(p, pre);
    if (tmp) {
      appendf(out, n, &o, tmp);
      free(tmp);
    }
    appendf(out, n, &o, repl);
    p = hit + nl;
  }
}

int dslResolve(DslCtx *c, const char *raw, char *out, size_t outn) {
  if (c->errKind) return c->errKind;
  char cur[DSL_VAL], next[DSL_VAL];
  dslInterp(c, raw, cur, sizeof(cur));

  /* project(p).<attr> */
  for (;;) {
    char *p = strstr(cur, "project(");
    if (!p) break;
    char proj[128];
    size_t pl = 0;
    const char *q = p + 8;
    while ((isalnum((unsigned char)*q) || *q == '_') && pl + 1 < sizeof(proj))
      proj[pl++] = *q++;
    proj[pl] = '\0';
    if (!pl || *q != ')' || q[1] != '.') break;
    const char *a = q + 2;
    size_t al = 0;
    while (isalnum((unsigned char)a[al]) || a[al] == '_') al++;
    char attr[32];
    if (al >= sizeof(attr)) break;
    memcpy(attr, a, al);
    attr[al] = '\0';
    char val[DSL_VAL] = "";
    bool ok = true;
    if (strcmp(attr, "source") == 0) ok = selSource(c, proj, val, sizeof(val));
    else if (strcmp(attr, "sources") == 0) ok = selSources(c, proj, val, sizeof(val));
    else if (strcmp(attr, "binary") == 0) ok = selBinary(c, proj, val, sizeof(val));
    else if (strcmp(attr, "dir") == 0) ok = selDir(c, proj, val, sizeof(val));
    else if (strcmp(attr, "embedded") == 0) snprintf(val, sizeof(val), "build/%s/embedded.c", proj);
    else if (strcmp(attr, "stamp") == 0) snprintf(val, sizeof(val), "build/%s/embedded.stamp", proj);
    else if (strcmp(attr, "archive") == 0) ok = selArchive(c, proj, val, sizeof(val));
    else break;
    if (!ok) return dslErr(c, 1, "selektor project(%s).%s tak terpenuhi", proj, attr);
    char needle[256];
    snprintf(needle, sizeof(needle), "project(%s).%s", proj, attr);
    replaceAll(cur, needle, val, next, sizeof(next));
    copyStr(cur, sizeof(cur), next);
  }

  /* repo(rel) — tanpa precond: exists()/absent() yang memutuskan */
  for (;;) {
    char *p = strstr(cur, "repo(");
    if (!p) break;
    char *close = strchr(p + 5, ')');
    if (!close) break;
    char inner[DSL_VAL];
    size_t il = (size_t)(close - (p + 5));
    if (il >= sizeof(inner)) break;
    memcpy(inner, p + 5, il);
    inner[il] = '\0';
    char val[DSL_VAL * 2];
    snprintf(val, sizeof(val), "%s/%s", c->repo, inner);
    char needle[DSL_VAL * 2];
    snprintf(needle, sizeof(needle), "repo(%s)", inner);
    replaceAll(cur, needle, val, next, sizeof(next));
    copyStr(cur, sizeof(cur), next);
  }

  for (;;) {
    char *p = strstr(cur, "work(");
    if (!p) break;
    char *close = strchr(p + 5, ')');
    if (!close) break;
    char inner[DSL_VAL];
    size_t il = (size_t)(close - (p + 5));
    if (il >= sizeof(inner)) break;
    memcpy(inner, p + 5, il);
    inner[il] = '\0';
    char val[DSL_VAL * 2];
    snprintf(val, sizeof(val), "%s/%s", c->work, inner);
    char needle[DSL_VAL * 2];
    snprintf(needle, sizeof(needle), "work(%s)", inner);
    replaceAll(cur, needle, val, next, sizeof(next));
    copyStr(cur, sizeof(cur), next);
  }

  for (;;) {
    char *p = strstr(cur, "ws(");
    if (!p) break;
    char *close = strchr(p + 3, ')');
    if (!close) break;
    char inner[DSL_VAL];
    size_t il = (size_t)(close - (p + 3));
    if (il >= sizeof(inner)) break;
    memcpy(inner, p + 3, il);
    inner[il] = '\0';
    char val[DSL_VAL * 2];
    if (!selWs(c, inner, val, sizeof(val)))
      return dslErr(c, 1, "kunci WsBuildfile tidak ada: %s", inner);
    char needle[DSL_VAL * 2];
    snprintf(needle, sizeof(needle), "ws(%s)", inner);
    replaceAll(cur, needle, val, next, sizeof(next));
    copyStr(cur, sizeof(cur), next);
  }

  copyStr(out, outn, cur);
  return 0;
}

/* ==================== ekspresi ==================== */

static const char *skipsp(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static int scanTopChar(const char *s, char sep) {
  int depth = 0;
  bool q = false;
  for (int i = 0; s[i]; i++) {
    char c = s[i];
    if (q) {
      if (c == '"') q = false;
      continue;
    }
    if (c == '"') {
      q = true;
      continue;
    }
    if (c == '(') depth++;
    else if (c == ')') {
      if (depth > 0) depth--;
    } else if (c == sep && depth == 0) {
      return i;
    }
  }
  return -1;
}

static int scanTopWord(const char *s, const char *w) {
  int depth = 0;
  bool q = false;
  size_t wl = strlen(w);
  for (int i = 0; s[i]; i++) {
    char c = s[i];
    if (q) {
      if (c == '"') q = false;
      continue;
    }
    if (c == '"') {
      q = true;
      continue;
    }
    if (c == '(') depth++;
    else if (c == ')') {
      if (depth > 0) depth--;
    }
    if (depth == 0 && strncmp(s + i, w, wl) == 0) return i;
  }
  return -1;
}

static char *evRun(DslCtx *c, const char *s);
static char *evOr(DslCtx *c, const char *s);

static char *trimDup(const char *s, size_t n) {
  while (n && (*s == ' ' || *s == '\t')) {
    s++;
    n--;
  }
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
  return dupN(s, n);
}

static char *predEval(DslCtx *c, const char *name, const char *args);

static char *evPrimary(DslCtx *c, const char **ps) {
  const char *s = skipsp(*ps);
  if (!*s) {
    *ps = s;
    return dupS("");
  }
  if (*s == '(') {
    int depth = 0;
    int i = 0;
    for (; s[i]; i++) {
      if (s[i] == '(') depth++;
      if (s[i] == ')') {
        depth--;
        if (depth == 0) break;
      }
    }
    char *inner = dupN(s + 1, i > 1 ? (size_t)i - 1 : 0);
    *ps = s + i + (s[i] ? 1 : 0);
    if (!inner) return NULL;
    char *v = evRun(c, inner);
    free(inner);
    return v;
  }
  if (*s == '"') {
    const char *close = strchr(s + 1, '"');
    if (!close) close = s + strlen(s);
    char *raw = dupN(s + 1, (size_t)(close - s - 1));
    *ps = *close ? close + 1 : close;
    if (!raw) return NULL;
    char *v = malloc(DSL_VAL);
    if (v) dslUnesc(raw, v, DSL_VAL);
    free(raw);
    return v;
  }
  if (isalpha((unsigned char)*s) || *s == '_') {
    size_t nl = 0;
    while (isalnum((unsigned char)s[nl]) || s[nl] == '_') nl++;
    if (s[nl] == '(') {
      char name[64];
      if (nl < sizeof(name)) {
        memcpy(name, s, nl);
        name[nl] = '\0';
        int depth = 0, i = (int)nl;
        for (; s[i]; i++) {
          if (s[i] == '(') depth++;
          if (s[i] == ')') {
            depth--;
            if (depth == 0) break;
          }
        }
        char *args = dupN(s + nl + 1, (size_t)i - nl - 1);
        *ps = s + i + (s[i] ? 1 : 0);
        if (!args) return NULL;
        char *v = predEval(c, name, args);
        free(args);
        return v;
      }
    }
  }
  size_t bl = 0;
  while (s[bl] && s[bl] != ' ' && s[bl] != '\t') bl++;
  *ps = s + bl;
  return dupN(s, bl);
}

static bool isIntLit(const char *s, long long *out) {
  if (!*s) return false;
  char *end = NULL;
  long long v = strtoll(s, &end, 10);
  if (!end || *end) return false;
  *out = v;
  return true;
}

static char *evCmp(DslCtx *c, const char *s) {
  const char *p = s;
  char *lhs = evPrimary(c, &p);
  if (!lhs) return NULL;
  p = skipsp(p);
  static const char *ops[] = {"==", "!=", "<=", ">=", "!~", "<", ">", "~"};
  const char *op = NULL;
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    size_t l = strlen(ops[i]);
    if (strncmp(p, ops[i], l) == 0) {
      op = ops[i];
      p += l;
      break;
    }
  }
  if (!op) return lhs;
  char *rhs = evPrimary(c, &p);
  if (!rhs) {
    free(lhs);
    return NULL;
  }
  bool r = false;
  long long a, b;
  if (strcmp(op, "==") == 0)
    r = strcmp(lhs, rhs) == 0;
  else if (strcmp(op, "!=") == 0)
    r = strcmp(lhs, rhs) != 0;
  else if (strcmp(op, "~") == 0)
    r = dslRegexSearch(rhs, lhs);
  else if (strcmp(op, "!~") == 0)
    r = !dslRegexSearch(rhs, lhs);
  else if (isIntLit(lhs, &a) && isIntLit(rhs, &b)) {
    if (strcmp(op, "<") == 0) r = a < b;
    else if (strcmp(op, ">") == 0) r = a > b;
    else if (strcmp(op, "<=") == 0) r = a <= b;
    else if (strcmp(op, ">=") == 0) r = a >= b;
  } else {
    int cmp = strcmp(lhs, rhs);
    if (strcmp(op, "<") == 0) r = cmp < 0;
    else if (strcmp(op, ">") == 0) r = cmp > 0;
    else if (strcmp(op, "<=") == 0) r = cmp <= 0;
    else if (strcmp(op, ">=") == 0) r = cmp >= 0;
  }
  free(lhs);
  free(rhs);
  return dupS(r ? "1" : "0");
}

static char *evNot(DslCtx *c, const char *s) {
  const char *p = skipsp(s);
  if (strncmp(p, "not", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) {
    p = skipsp(p + 3);
    char *v = evCmp(c, p);
    if (!v) return NULL;
    bool t = dslTruthy(v);
    free(v);
    return dupS(t ? "0" : "1");
  }
  return evCmp(c, p);
}

static char *evAnd(DslCtx *c, const char *s) {
  int k = scanTopWord(s, " and ");
  if (k < 0) return evNot(c, s);
  char *l = dupN(s, (size_t)k);
  if (!l) return NULL;
  char *v = evAnd(c, l);
  free(l);
  if (!v) return NULL;
  if (!dslTruthy(v)) {
    free(v);
    return dupS("0");
  }
  free(v);
  return evAnd(c, skipsp(s + k + 5));
}

static char *evOr(DslCtx *c, const char *s) {
  int k = scanTopWord(s, " or ");
  if (k < 0) return evAnd(c, s);
  char *l = dupN(s, (size_t)k);
  if (!l) return NULL;
  char *v = evOr(c, l);
  free(l);
  if (!v) return NULL;
  if (dslTruthy(v)) {
    free(v);
    return dupS("1");
  }
  free(v);
  return evOr(c, skipsp(s + k + 4));
}

static char *evRun(DslCtx *c, const char *s) {
  int q = scanTopChar(s, '?');
  if (q < 0) return evOr(c, s);
  char *cond = dupN(s, (size_t)q);
  if (!cond) return NULL;
  char *v = evOr(c, cond);
  free(cond);
  if (!v) return NULL;
  const char *rest = s + q + 1;
  int col = scanTopChar(rest, ':');
  if (col < 0) {
    free(v);
    dslErr(c, 2, "ternary tanpa ':': %s", s);
    return NULL;
  }
  char *a = dupN(rest, (size_t)col);
  char *b = dupS(rest + col + 1);
  if (!a || !b) {
    free(v);
    free(a);
    free(b);
    return NULL;
  }
  char *res = dslTruthy(v) ? evOr(c, a) : evRun(c, b);
  free(v);
  free(a);
  free(b);
  return res;
}

int dslEval(DslCtx *c, const char *expr, char *out, size_t outn) {
  if (c->errKind) return c->errKind;
  char *v = evRun(c, expr);
  if (!v) {
    if (!c->errKind) dslErr(c, 2, "evaluasi gagal: %s", expr);
    out[0] = '\0';
    return c->errKind;
  }
  copyStr(out, outn, v);
  free(v);
  return 0;
}

/* ==================== predikat ==================== */

static char *readRunOut(DslCtx *c, const char *label) {
  DslRun *r = runFind(c, label);
  /* Run latar boleh dibaca selagi berjalan (polling watcher via @while). */
  if (!r || !r->out) return NULL;
  return dslReadFile(r->out, NULL);
}

static int countFilesRec(const char *dir) {
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  int n = files.count;
  for (int i = 0; i < dirs.count; i++) n += countFilesRec(dirs.items[i]);
  listFree(&dirs);
  listFree(&files);
  return n;
}

static char *predEval(DslCtx *c, const char *name, const char *args) {
  char a1[DSL_VAL] = "", a2[DSL_VAL] = "";
  int comma = scanTopChar(args, ',');
  if (comma >= 0) {
    char *l = trimDup(args, (size_t)comma);
    char *r = trimDup(args + comma + 1, strlen(args + comma + 1));
    if (l) copyStr(a1, sizeof(a1), l);
    if (r) copyStr(a2, sizeof(a2), r);
    free(l);
    free(r);
  } else {
    char *o = trimDup(args, strlen(args));
    if (o) copyStr(a1, sizeof(a1), o);
    free(o);
  }
  dslUnq(a1);
  dslUnq(a2);

  if (strcmp(name, "exists") == 0) {
    char path[SATRIA_MAX_PATH];
    dslWPath(c, a1, path, sizeof(path));
    return dupS((fsFileExists(path) || fsDirExists(path)) ? "1" : "0");
  }
  if (strcmp(name, "dirname") == 0) {
    char *s = dupS(a1);
    if (!s) return dupS("");
    char *sl = strrchr(s, '/');
    if (sl) *sl = '\0';
    return s;
  }
  if (strcmp(name, "contains") == 0) {
    char *txt = readRunOut(c, a1);
    bool hit = txt && strstr(txt, a2) != NULL;
    free(txt);
    return dupS(hit ? "1" : "0");
  }
  if (strcmp(name, "matches") == 0) {
    char *txt = readRunOut(c, a1);
    bool hit = txt && dslRegexText(a2, txt, strlen(txt));
    free(txt);
    return dupS(hit ? "1" : "0");
  }
  if (strcmp(name, "rc") == 0) {
    DslRun *r = runFind(c, a1);
    char buf[32];
    if (!r || !r->hasRc) return dupS("");
    snprintf(buf, sizeof(buf), "%d", r->rc);
    return dupS(buf);
  }
  if (strcmp(name, "lines") == 0 || strcmp(name, "size") == 0 ||
      strcmp(name, "count") == 0) {
    char path[SATRIA_MAX_PATH];
    dslWPath(c, a1, path, sizeof(path));
    size_t len = 0;
    char *txt = dslReadFile(path, &len);
    if (!txt) return dupS("0");
    char buf[32];
    if (strcmp(name, "size") == 0) {
      snprintf(buf, sizeof(buf), "%lld", fsFileSize(path));
    } else if (strcmp(name, "count") == 0) {
      snprintf(buf, sizeof(buf), "%d", dslRegexCount(a2, txt, len));
    } else {
      int n = 0;
      for (size_t i = 0; i < len; i++)
        if (txt[i] == '\n') n++;
      snprintf(buf, sizeof(buf), "%d", n);
    }
    free(txt);
    return dupS(buf);
  }
  if (strcmp(name, "changed") == 0 || strcmp(name, "unchanged") == 0) {
    DslMark *m = markFind(c, a1);
    if (!m || !m->valid) return dupS("0");
    int64_t cur = fsMTimeNs(m->path);
    bool changed = cur != m->ns;
    if (strcmp(name, "unchanged") == 0) changed = !changed;
    return dupS(changed ? "1" : "0");
  }
  if (strcmp(name, "same") == 0) {
    char p1[SATRIA_MAX_PATH], p2[SATRIA_MAX_PATH];
    dslWPath(c, a1, p1, sizeof(p1));
    dslWPath(c, a2, p2, sizeof(p2));
    size_t l1 = 0, l2 = 0;
    char *t1 = dslReadFile(p1, &l1);
    char *t2 = dslReadFile(p2, &l2);
    bool same = t1 && t2 && l1 == l2 && memcmp(t1, t2, l1) == 0;
    free(t1);
    free(t2);
    return dupS(same ? "1" : "0");
  }
  if (strcmp(name, "pid") == 0) {
    DslRun *r = runFind(c, a1);
    char buf[32];
    if (!r || r->pid <= 0) return dupS("");
    snprintf(buf, sizeof(buf), "%d", r->pid);
    return dupS(buf);
  }
  if (strcmp(name, "line") == 0) {
    char path[SATRIA_MAX_PATH];
    dslWPath(c, a1, path, sizeof(path));
    size_t len = 0;
    char *txt = dslReadFile(path, &len);
    int found = 0, n = 1;
    if (txt) {
      char *p = txt;
      while (*p) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char *one = dupN(p, l);
        if (one) {
          if (dslRegexSearch(a2, one)) {
            found = n;
            free(one);
            break;
          }
          free(one);
        }
        if (!e) break;
        p = e + 1;
        n++;
      }
    }
    free(txt);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", found);
    return dupS(buf);
  }
  if (strcmp(name, "files") == 0) {
    char path[SATRIA_MAX_PATH];
    dslWPath(c, a1, path, sizeof(path));
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", fsDirExists(path) ? countFilesRec(path) : 0);
    return dupS(buf);
  }
  if (strcmp(name, "glob") == 0) {
    int cnt = 0;
#ifndef _WIN32
    char full[SATRIA_MAX_PATH];
    dslWPath(c, a1, full, sizeof(full));
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob(full, 0, NULL, &g) == 0) cnt = (int)g.gl_pathc;
    globfree(&g);
#endif
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", cnt);
    return dupS(buf);
  }
  dslErr(c, 2, "predikat tak dikenal: %s(...)", name);
  return NULL;
}

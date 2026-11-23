/* parser.c — parser DSL `.test` (design/unit_test_dsl.md), padanan C dari
   scripts/lib/parser.sh. Parse-only; tidak mengeksekusi apa pun.

   Kesetaraan dengan versi bash dijaga pada tingkat byte: spasi/tab pada
   argumen dipertahankan, baris `#` di dalam heredoc bukan komentar DSL,
   penutup heredoc opsional di EOF, dan pesan kesalahan memakai nomor baris
   yang sama. Uji: tests/unit/test_parser.c + tests/unit/parser/parity.sh. */

#include "parser.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

/* ---- kosakata (design/unit_test_dsl.md) ---------------------------------- */
#define HDRS "suite fixture purpose requires keep"
#define SINGLES "include guard return set get find"
#define OBJS "work fixture file env time run stage text out rc mtime"

/* ---- tipe dasar ----------------------------------------------------------- */

typedef struct {
  const char *name; /* static: response/if/while/for */
  int line;
  bool seenElse;
} Block;

typedef struct {
  bool trace;
  bool fail;
  bool body;
  int line;
  char *heredoc; /* NULL bila tidak aktif */
  Block *blocks;
  int nblocks;
  int capblocks;
  FILE *out;
  FILE *err;
} Parser;

/* `[[:space:]]` pada locale C — dipakai untuk indentasi, pemisah pernyataan,
   dan bentuk @for. */
static bool isWs(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* IFS bawaan bash: spasi, tab, newline (dipakai `read`/`read -a`). */
static bool isIFS(unsigned char c) { return c == ' ' || c == '\t' || c == '\n'; }

static bool isIdentStart(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool isIdentChar(unsigned char c) {
  return isIdentStart(c) || (c >= '0' && c <= '9');
}

static bool isAksiChar(unsigned char c) { return isIdentChar(c) || c == '-'; }

static bool isFullIdent(const char *s) {
  if (!isIdentStart((unsigned char)*s)) return false;
  for (s++; *s; s++) {
    if (!isIdentChar((unsigned char)*s)) return false;
  }
  return true;
}

static char *dupStr(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = malloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

/* Anggota daftar kata dipisah satu spasi (kosakata di atas). */
static bool inList(const char *word, const char *list) {
  size_t n = strlen(word);
  const char *p = list;
  while (*p) {
    const char *sp = strchr(p, ' ');
    size_t len = sp ? (size_t)(sp - p) : strlen(p);
    if (len == n && memcmp(p, word, n) == 0) return true;
    if (!sp) break;
    p = sp + 1;
  }
  return false;
}

static const char *actionsFor(const char *obj) {
  if (strcmp(obj, "work") == 0) return "new clean";
  if (strcmp(obj, "fixture") == 0) return "copy";
  if (strcmp(obj, "file") == 0)
    return "write append touch delete copy exists absent same contains";
  if (strcmp(obj, "env") == 0) return "set unset";
  if (strcmp(obj, "time") == 0) return "tick mark";
  if (strcmp(obj, "run") == 0) return "build cmd exec bg stop wait tool";
  if (strcmp(obj, "stage") == 0) return "set log";
  if (strcmp(obj, "text") == 0) return "count aggregate";
  if (strcmp(obj, "out") == 0) return "contains not-contains matches not-matches equals";
  if (strcmp(obj, "rc") == 0) return "ok fail";
  if (strcmp(obj, "mtime") == 0) return "changed unchanged";
  return NULL;
}

/* ---- keluaran ------------------------------------------------------------- */

/* Satu event TSV: tag, nomor baris, lalu field apa adanya. */
static void emitFields(Parser *p, const char *tag, int line, const char *const *fields,
                       int nfields) {
  if (!p->trace) return;
  fprintf(p->out, "%s\t%d", tag, line);
  for (int i = 0; i < nfields; i++) {
    fputc('\t', p->out);
    fputs(fields[i], p->out);
  }
  fputc('\n', p->out);
}

static void errAtf(Parser *p, int line, const char *fmt, ...) {
  fprintf(p->err, "baris %d: ", line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(p->err, fmt, ap);
  va_end(ap);
  fputc('\n', p->err);
  p->fail = true;
}

static void errAt(Parser *p, int line, const char *msg) { errAtf(p, line, "%s", msg); }

/* ---- tumpukan blok -------------------------------------------------------- */

static bool pushBlock(Parser *p, const char *name, int line) {
  if (p->nblocks == p->capblocks) {
    int next = p->capblocks ? p->capblocks * 2 : 8;
    Block *nb = realloc(p->blocks, (size_t)next * sizeof(*nb));
    if (!nb) {
      errAt(p, p->line, "kehabisan memori");
      return false;
    }
    p->blocks = nb;
    p->capblocks = next;
  }
  p->blocks[p->nblocks].name = name;
  p->blocks[p->nblocks].line = line;
  p->blocks[p->nblocks].seenElse = false;
  p->nblocks++;
  return true;
}

static void dropBlock(Parser *p) {
  if (p->nblocks > 0) p->nblocks--;
}

/* ---- util string ---------------------------------------------------------- */

/* `read -r -a`: pecah `s` (dimutasi) atas IFS, simpan maksimal `max` pointer
   ke `out`, tetapi kembalikan jumlah kata sebenarnya. */
static int splitWords(char *s, char **out, int max) {
  int n = 0;
  char *p = s;
  for (;;) {
    while (*p && isIFS((unsigned char)*p)) p++;
    if (!*p) break;
    if (n < max) out[n] = p;
    n++;
    while (*p && !isIFS((unsigned char)*p)) p++;
    if (*p) *p++ = '\0';
  }
  return n;
}

/* `read -r d rest`: potong token pertama dan sisa yang sudah di-trim. */
static void splitDirective(char *l, char **dirOut, char **restOut) {
  char *p = l;
  while (*p && !isIFS((unsigned char)*p)) p++;
  char *rest;
  if (*p) {
    *p++ = '\0';
    while (*p && isIFS((unsigned char)*p)) p++;
    rest = p;
  } else {
    rest = p; /* menunjuk terminator */
  }
  char *end = rest + strlen(rest);
  while (end > rest && isIFS((unsigned char)end[-1])) *--end = '\0';
  *dirOut = l;
  *restOut = rest;
}

/* ---- direktif ------------------------------------------------------------- */

static void handleDirective(Parser *p, char *l) {
  char *d;
  char *rest;
  splitDirective(l, &d, &rest);
  const char *name = d[0] == '@' ? d + 1 : d;

  if (inList(name, HDRS)) {
    if (p->body) {
      errAt(p, p->line, "direktif header setelah badan");
      return;
    }
    if (strcmp(d, "@suite") == 0 || strcmp(d, "@fixture") == 0 ||
        strcmp(d, "@purpose") == 0) {
      if (!*rest) {
        errAtf(p, p->line, "%s memerlukan argumen", d);
        return;
      }
    } else if (strcmp(d, "@requires") == 0) {
      if (strcmp(rest, "cold") != 0 && strcmp(rest, "snap") != 0) {
        errAt(p, p->line, "bentuk: @requires cold|snap");
        return;
      }
    } else if (strcmp(d, "@keep") == 0) {
      if (*rest) {
        errAt(p, p->line, "@keep tidak menerima argumen");
        return;
      }
    }
    emitFields(p, "HDR", p->line, (const char *const[]){name, rest}, 2);
    return;
  }

  if (strcmp(d, "@end") == 0) {
    if (p->nblocks == 0) {
      errAt(p, p->line, "@end tanpa blok terbuka");
      return;
    }
    int i = p->nblocks - 1;
    emitFields(p, "ENDB", p->line, &p->blocks[i].name, 1);
    dropBlock(p);
    p->body = true;
    return;
  }

  if (strcmp(d, "@else") == 0) {
    if (p->nblocks == 0 || strcmp(p->blocks[p->nblocks - 1].name, "if") != 0) {
      errAt(p, p->line, "@else di luar @if");
      return;
    }
    Block *top = &p->blocks[p->nblocks - 1];
    if (top->seenElse) {
      errAt(p, p->line, "@else kedua pada @if yang sama");
      return;
    }
    top->seenElse = true;
    emitFields(p, "DIR", p->line, (const char *const[]){"else", ""}, 2);
    p->body = true;
    return;
  }

  if (inList(name, SINGLES)) {
    char *copy = dupStr(rest);
    if (!copy) {
      errAt(p, p->line, "kehabisan memori");
      return;
    }
    char *F[4] = {NULL, NULL, NULL, NULL};
    int n = splitWords(copy, F, 4);
    if (strcmp(d, "@include") == 0) {
      if (!*rest) {
        errAt(p, p->line, "bentuk: @include <path> [as <nama>]");
        free(copy);
        return;
      }
      if (n == 3) {
        if (strcmp(F[1], "as") != 0) {
          errAt(p, p->line, "bentuk: @include <path> [as <nama>]");
          free(copy);
          return;
        }
      } else if (n != 1) {
        errAt(p, p->line, "bentuk: @include <path> [as <nama>]");
        free(copy);
        return;
      }
    } else if (strcmp(d, "@guard") == 0) {
      if (!*rest) {
        errAt(p, p->line, "bentuk: @guard <ekspresi>");
        free(copy);
        return;
      }
    } else if (strcmp(d, "@return") == 0) {
      /* ekspresi opsional */
    } else if (strcmp(d, "@set") == 0) {
      if (n < 2) {
        errAt(p, p->line, "bentuk: @set <nama> <ekspresi>");
        free(copy);
        return;
      }
      if (!isFullIdent(F[0])) {
        errAtf(p, p->line, "nama variabel tidak sah: %s", F[0]);
        free(copy);
        return;
      }
    } else if (strcmp(d, "@get") == 0) {
      if (n != 1) {
        errAt(p, p->line, "bentuk: @get <nama>");
        free(copy);
        return;
      }
    } else if (strcmp(d, "@find") == 0) {
      if (n < 2) {
        errAt(p, p->line, "bentuk: @find <nama> <pola>");
        free(copy);
        return;
      }
      if (!isFullIdent(F[0])) {
        errAtf(p, p->line, "nama variabel tidak sah: %s", F[0]);
        free(copy);
        return;
      }
    }
    free(copy);
    emitFields(p, "DIR", p->line, (const char *const[]){name, rest}, 2);
    p->body = true;
    return;
  }

  if (strcmp(d, "@response") == 0) {
    char *copy = dupStr(rest);
    if (!copy) {
      errAt(p, p->line, "kehabisan memori");
      return;
    }
    char *W[1] = {NULL};
    int n = splitWords(copy, W, 1);
    free(copy);
    if (n > 1) {
      errAt(p, p->line, "bentuk: @response [label]");
      return;
    }
    if (!pushBlock(p, "response", p->line)) return;
    emitFields(p, "BLOCK", p->line, (const char *const[]){"response", rest}, 2);
    p->body = true;
    return;
  }

  if (strcmp(d, "@if") == 0 || strcmp(d, "@while") == 0) {
    if (!*rest) {
      errAtf(p, p->line, "bentuk: %s <ekspresi>", d);
      return;
    }
    const char *block = strcmp(d, "@if") == 0 ? "if" : "while";
    if (!pushBlock(p, block, p->line)) return;
    emitFields(p, "BLOCK", p->line, (const char *const[]){block, rest}, 2);
    p->body = true;
    return;
  }

  if (strcmp(d, "@for") == 0) {
    const char *q = rest;
    if (!isIdentStart((unsigned char)*q)) {
      errAt(p, p->line, "bentuk: @for <var> in <rentang|koleksi>");
      return;
    }
    q++;
    while (isIdentChar((unsigned char)*q)) q++;
    if (!isWs((unsigned char)*q)) {
      errAt(p, p->line, "bentuk: @for <var> in <rentang|koleksi>");
      return;
    }
    while (isWs((unsigned char)*q)) q++;
    if (q[0] != 'i' || q[1] != 'n') {
      errAt(p, p->line, "bentuk: @for <var> in <rentang|koleksi>");
      return;
    }
    q += 2;
    if (!isWs((unsigned char)*q)) {
      errAt(p, p->line, "bentuk: @for <var> in <rentang|koleksi>");
      return;
    }
    while (isWs((unsigned char)*q)) q++;
    if (!*q) {
      errAt(p, p->line, "bentuk: @for <var> in <rentang|koleksi>");
      return;
    }
    if (!pushBlock(p, "for", p->line)) return;
    emitFields(p, "BLOCK", p->line, (const char *const[]){"for", rest}, 2);
    p->body = true;
    return;
  }

  errAtf(p, p->line, "direktif tidak dikenal: %s", d);
}

/* ---- pernyataan objek.aksi ------------------------------------------------ */

/* Pecah `line` menjadi objek/aksi/argumen. False bila bentuk tak dipahami.
   `work` wajib dibebaskan pemanggil; pointer lain menunjuk ke dalamnya. */
static bool splitStatement(const char *line, char **work, char **obj, char **aksi,
                           char **rest) {
  char *w = dupStr(line);
  if (!w) return false;
  char *p = w;
  if (!isIdentStart((unsigned char)*p)) {
    free(w);
    return false;
  }
  while (isIdentChar((unsigned char)*p)) p++;
  if (*p != '.') {
    free(w);
    return false;
  }
  *p++ = '\0';
  char *o = w;
  char *a = p;
  if (!isIdentStart((unsigned char)*p)) {
    free(w);
    return false;
  }
  p++;
  while (isAksiChar((unsigned char)*p)) p++;
  char *r;
  if (*p == '\0') {
    r = p;
  } else if (isWs((unsigned char)*p)) {
    *p++ = '\0';
    while (isWs((unsigned char)*p)) p++;
    r = p;
  } else {
    free(w);
    return false;
  }
  *work = w;
  *obj = o;
  *aksi = a;
  *rest = r;
  return true;
}

static void handleStatement(Parser *p, const char *line) {
  char *work;
  char *obj;
  char *aksi;
  char *rest;
  if (!splitStatement(line, &work, &obj, &aksi, &rest)) {
    errAtf(p, p->line, "pernyataan tidak dipahami: %s", line);
    return;
  }

  if (!inList(obj, OBJS)) {
    errAtf(p, p->line, "objek tidak dikenal: %s", obj);
    free(work);
    return;
  }
  const char *list = actionsFor(obj);
  if (!list || !inList(aksi, list)) {
    errAtf(p, p->line, "aksi tidak dikenal: %s.%s", obj, aksi);
    free(work);
    return;
  }

  char *marker = NULL;
  if (strstr(rest, "<<") != NULL) {
    const char *mk = NULL;
    size_t mklen = 0;
    for (const char *q = rest; (q = strstr(q, "<<")) != NULL; q++) {
      if (isIdentStart((unsigned char)q[2])) {
        const char *s = q + 2;
        const char *e = s + 1;
        while (isIdentChar((unsigned char)*e)) e++;
        mk = s;
        mklen = (size_t)(e - s);
        break;
      }
    }
    if (!mk) {
      errAtf(p, p->line, "marker heredoc tidak sah di: %s", rest);
      free(work);
      return;
    }
    char *needle = malloc(mklen + 3);
    if (!needle) {
      errAt(p, p->line, "kehabisan memori");
      free(work);
      return;
    }
    memcpy(needle, "<<", 2);
    memcpy(needle + 2, mk, mklen);
    needle[mklen + 2] = '\0';
    const char *found = strstr(rest, needle);
    const char *after = found ? found + strlen(needle) : rest + strlen(rest);
    free(needle);
    if (strstr(after, "<<") != NULL) {
      errAt(p, p->line, "hanya satu heredoc per pernyataan");
      free(work);
      return;
    }
    if (strcmp(obj, "file") != 0 || strcmp(aksi, "write") != 0) {
      errAt(p, p->line, "heredoc hanya sah pada file.write");
      free(work);
      return;
    }
    marker = malloc(mklen + 1);
    if (!marker) {
      errAt(p, p->line, "kehabisan memori");
      free(work);
      return;
    }
    memcpy(marker, mk, mklen);
    marker[mklen] = '\0';
  } else if (strcmp(obj, "file") == 0 && strcmp(aksi, "write") == 0) {
    errAt(p, p->line, "file.write memerlukan heredoc: file.write <path> <<T");
    free(work);
    return;
  }

  emitFields(p, "STMT", p->line, (const char *const[]){obj, aksi, rest}, 3);
  if (marker) {
    emitFields(p, "OPEN", p->line, (const char *const[]){"heredoc", marker}, 2);
    free(p->heredoc);
    p->heredoc = marker;
  }
  p->body = true;
  free(work);
}

/* ---- pembaca baris -------------------------------------------------------- */

/* Baris berikutnya tanpa newline; -1 di EOF. Baris tanpa newline terakhir
   tetap diproses (sama dengan `while read … || [ -n "$raw" ]`). */
static long readLineDyn(FILE *f, char **buf, size_t *cap) {
  size_t len = 0;
  bool got = false;
  for (;;) {
    if (*cap < len + 2) {
      size_t next = *cap ? *cap * 2 : 128;
      while (next < len + 2) next *= 2;
      char *nb = realloc(*buf, next);
      if (!nb) return -1;
      *buf = nb;
      *cap = next;
    }
    if (!fgets(*buf + len, (int)(*cap - len), f)) break;
    got = true;
    len += strlen(*buf + len);
    if (len > 0 && (*buf)[len - 1] == '\n') {
      (*buf)[--len] = '\0';
      break;
    }
  }
  if (!got) return -1;
  return (long)len;
}

/* ---- API ------------------------------------------------------------------ */

int testParseDsl(FILE *in, bool trace, FILE *traceOut, FILE *errOut) {
  Parser p;
  memset(&p, 0, sizeof(p));
  p.trace = trace;
  p.out = traceOut ? traceOut : stdout;
  p.err = errOut ? errOut : stderr;

  char *buf = NULL;
  size_t cap = 0;
  while (readLineDyn(in, &buf, &cap) >= 0) {
    p.line++;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';

    if (p.heredoc) {
      if (strcmp(buf, p.heredoc) == 0) {
        emitFields(&p, "CLOSE", p.line, (const char *const[]){"heredoc", p.heredoc}, 2);
        free(p.heredoc);
        p.heredoc = NULL;
      } else {
        emitFields(&p, "TEXT", p.line, (const char *const[]){buf}, 1);
      }
      continue;
    }

    char *l = buf;
    while (isWs((unsigned char)*l)) l++;
    if (!*l) continue;
    if (*l == '#') {
      emitFields(&p, "CMT", p.line, (const char *const[]){l}, 1);
      continue;
    }
    if (*l == '@') {
      handleDirective(&p, l);
      continue;
    }
    handleStatement(&p, l);
  }

  /* Penutup heredoc opsional: tanpa penutup, isi berakhir di EOF. */
  if (p.heredoc) {
    emitFields(&p, "CLOSE", p.line, (const char *const[]){"heredoc", p.heredoc}, 2);
    free(p.heredoc);
    p.heredoc = NULL;
  }
  if (p.nblocks > 0) {
    Block *b = &p.blocks[p.nblocks - 1];
    errAtf(&p, b->line, "blok @%s (mulai baris ini) tanpa @end", b->name);
  }

  bool fail = p.fail;
  free(buf);
  free(p.blocks);
  return fail ? 1 : 0;
}

#ifdef _WIN32
#include <sys/stat.h>
#define PSTAT(path, st) _stat((path), (st))
#define PISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#else
#define PSTAT(path, st) stat((path), (st))
#define PISREG(m) S_ISREG(m)
#endif

static bool isRegularFile(const char *path) {
#ifdef _WIN32
  struct _stat st;
#else
  struct stat st;
#endif
  return PSTAT(path, &st) == 0 && PISREG(st.st_mode);
}

int testParseFile(const char *path, bool trace, FILE *traceOut, FILE *errOut) {
  FILE *err = errOut ? errOut : stderr;
  if (!isRegularFile(path)) {
    fprintf(err, "parser: berkas tidak ada: %s\n", path);
    return 2;
  }
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(err, "parser: berkas tidak ada: %s\n", path);
    return 2;
  }
  int rc = testParseDsl(fp, trace, traceOut, err);
  fclose(fp);
  return rc;
}

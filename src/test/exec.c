/* exec.c — eksekutor event DSL unit test (M4b). Padanan C dari
   tests/unit/lib/dsl.sh: aksi work/fixture/file/env/time/run/stage/text,
   blok @if/@for/@while/@response, @guard/@set/@get/@find/@include/@return,
   asersi, serta verdict PASS/FAIL/PRECONDITION dengan format yang sama.
   Proses `run.*` di-spawn lewat fork+exec (tanpa shell). */

#include "dsl.h"

#include "parser.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

int testExecMain(int argc, const char *const *argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr, "satria test <file>: eksekutor DSL hanya di POSIX.\n");
  return 2;
}

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

typedef struct {
  char *tag;
  int line;
  char *a, *b, *c;
} Ev;

typedef struct {
  Ev *v;
  int n, cap;
} EvList;

typedef struct {
  DslCtx *c;
  EvList *ev;
  int pass, fail;
  const char *resp;
  bool retHit;
  char retVal[DSL_VAL];
  char fixture[256];
  bool snap, keep;
  bool workspace;
  List works;
  const char *testName;
} Run;

static Run *g_run;

/* ==================== util ==================== */

static void evFree(EvList *l) {
  for (int i = 0; i < l->n; i++) {
    free(l->v[i].tag);
    free(l->v[i].a);
    free(l->v[i].b);
    free(l->v[i].c);
  }
  free(l->v);
  l->v = NULL;
  l->n = l->cap = 0;
}

static Ev *evPush(EvList *l) {
  if (l->n == l->cap) {
    int nc = l->cap ? l->cap * 2 : 64;
    Ev *g = realloc(l->v, (size_t)nc * sizeof(*g));
    if (!g) return NULL;
    l->v = g;
    l->cap = nc;
  }
  Ev *e = &l->v[l->n++];
  memset(e, 0, sizeof(*e));
  return e;
}

static void catStr(char *out, size_t n, size_t *len, const char *s) {
  size_t l = strlen(s);
  if (*len + l >= n) l = n - 1 - *len;
  if ((long)l <= 0) return;
  memcpy(out + *len, s, l);
  *len += l;
  out[*len] = '\0';
}

/* Bagi baris event pada TAB pertama; sisanya dipertahankan (TEXT bisa ber-TAB). */
static void evSplit(char *line, Ev *e) {
  char *t1 = strchr(line, '\t');
  if (!t1) {
    e->tag = strdup(line);
    return;
  }
  *t1 = '\0';
  e->tag = strdup(line);
  char *p = t1 + 1;
  char *t2 = strchr(p, '\t');
  e->line = atoi(p);
  if (!t2) return;
  *t2 = '\0';
  p = t2 + 1;
  char *t3 = strchr(p, '\t');
  if (!t3) {
    e->a = strdup(p);
    return;
  }
  *t3 = '\0';
  e->a = strdup(p);
  p = t3 + 1;
  char *t4 = strchr(p, '\t');
  if (!t4) {
    e->b = strdup(p);
    return;
  }
  *t4 = '\0';
  e->b = strdup(p);
  e->c = strdup(t4 + 1);
}

static bool loadEvents(const char *path, EvList *out) {
  FILE *tf = tmpfile();
  if (!tf) return false;
  int rc = testParseFile(path, true, tf, stderr);
  if (rc != 0) {
    fclose(tf);
    return false;
  }
  rewind(tf);
  char *line = NULL;
  size_t cap = 0;
  ssize_t got;
  while ((got = getline(&line, &cap, tf)) >= 0) {
    while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r')) line[--got] = '\0';
    if (!got) continue;
    Ev *e = evPush(out);
    if (!e) break;
    evSplit(line, e);
  }
  free(line);
  fclose(tf);
  return true;
}

static void cleanupWorks(void) {
  if (!g_run || g_run->keep) return;
  for (int i = 0; i < g_run->works.count; i++) fsRemoveTree(g_run->works.items[i]);
}

static void dieMsg(Run *r, const char *fmt, ...) {
  (void)r;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "FAIL dsl: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

static void precondition(Run *r, const char *msg) {
  (void)r;
  fprintf(stderr, "PRECONDITION %s\n", msg);
  exit(3);
}

static void pass(Run *r, const char *clause) {
  r->pass++;
  printf("PASS: %s\n", clause);
}

static void fail(Run *r, const char *clause, const char *got, const char *want) {
  r->fail++;
  printf("FAIL: %s (dapat %s, harap %s)\n", clause, got, want);
}

/* Split argumen pada spasi/tab (seperti `read -r -a`, tanpa proses kutip). */
/* Pisah argumen per spasi/tab dengan dukungan kutip tunggal/ganda: kutip
   dikupas dan spasi di dalamnya tidak memisah. Backslash dipertahankan apa
   adanya (mis. `\1` untuk sed), kecuali `\"`/`\'` di dalam kutip yang
   menjadi kutip literal. */
static int splitSpace(char *s, char ***out) {
  int cap = 8, n = 0;
  char **v = malloc((size_t)cap * sizeof(*v));
  if (!v) return 0;
  char *p = s;
  for (;;) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    if (n == cap) {
      int nc = cap * 2;
      char **g = realloc(v, (size_t)nc * sizeof(*g));
      if (!g) break;
      v = g;
      cap = nc;
    }
    char *w = p, *q = p;
    v[n++] = w;
    while (*p) {
      if (*p == '\'' || *p == '"') {
        char quote = *p++;
        while (*p && *p != quote) {
          if (*p == '\\' && p[1] == quote) p++;
          *q++ = *p++;
        }
        if (*p == quote) p++;
        continue;
      }
      if (*p == ' ' || *p == '\t') break;
      *q++ = *p++;
    }
    int sep = *p != '\0';
    if (sep) p++;
    *q = '\0';
  }
  *out = v;
  return n;
}

static char *firstToken(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *p = s;
  while (*p && *p != ' ' && *p != '\t') p++;
  if (*p) *p = '\0';
  return s;
}

static DslRun *runAdd(Run *r, const char *label) {
  DslCtx *c = r->c;
  DslRun *run = NULL;
  for (int i = 0; i < c->nruns; i++)
    if (strcmp(c->runs[i].label, label) == 0) run = &c->runs[i];
  if (!run) {
    if (c->nruns == c->capruns) {
      int nc = c->capruns ? c->capruns * 2 : 8;
      DslRun *g = realloc(c->runs, (size_t)nc * sizeof(*g));
      if (!g) return NULL;
      c->runs = g;
      c->capruns = nc;
    }
    run = &c->runs[c->nruns++];
    memset(run, 0, sizeof(*run));
    run->label = strdup(label);
  }
  return run;
}

/* ==================== proses ==================== */

static int spawnWait(char *const argv[], const char *cwd, const char *outPath, int *pidOut,
                     bool background) {
  int pid = fork();
  if (pid < 0) return 127;
  if (pid == 0) {
    if (cwd && *cwd && chdir(cwd) != 0) _exit(127);
    setenv("NO_COLOR", "1", 1);
    if (outPath) {
      int fd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
      }
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  if (pidOut) *pidOut = pid;
  if (background) return 0;
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return 1;
}

/* ==================== aksi ==================== */

static void writeFileHeredoc(Run *r, const char *args, int *ip) {
  DslCtx *c = r->c;
  char raw[DSL_VAL];
  copyStr(raw, sizeof(raw), args);
  char *mark = strstr(raw, " <<");
  bool heredoc = mark != NULL;
  if (mark) *mark = '\0';
  char *p = raw;
  while (*p == ' ' || *p == '\t') p++;
  size_t pl = strlen(p);
  while (pl && (p[pl - 1] == ' ' || p[pl - 1] == '\t')) p[--pl] = '\0';

  char full[SATRIA_MAX_PATH];
  dslWPath(c, p, full, sizeof(full));
  mkparent(full);

  if (!heredoc) {
    FILE *fp = fopen(full, "wb");
    if (fp) fclose(fp);
    return;
  }

  /* Lewati OPEN, kumpulkan TEXT, berhenti di CLOSE. */
  int i = *ip + 1;
  FILE *fp = fopen(full, "wb");
  if (fp) {
    while (i < r->ev->n && !(strcmp(r->ev->v[i].tag, "CLOSE") == 0)) {
      if (strcmp(r->ev->v[i].tag, "TEXT") == 0) {
        char val[DSL_VAL];
        dslInterp(c, r->ev->v[i].a ? r->ev->v[i].a : "", val, sizeof(val));
        fputs(val, fp);
        fputc('\n', fp);
      }
      i++;
    }
    fclose(fp);
  }
  *ip = i < r->ev->n ? i : r->ev->n - 1;
}

static void appendLine(const char *path, const char *text) {
  mkparent(path);
  FILE *fp = fopen(path, "ab");
  if (!fp) return;
  fputs(text, fp);
  fputc('\n', fp);
  fclose(fp);
}

static bool copyFileConvert(const char *src, const char *dst, const char *conv) {
  size_t len = 0;
  char *txt = dslReadFile(src, &len);
  if (!txt) return false;
  mkparent(dst);
  FILE *fp = fopen(dst, "wb");
  if (!fp) {
    free(txt);
    return false;
  }
  if (conv && strcmp(conv, "crlf") == 0) {
    for (size_t i = 0; i < len; i++) {
      if (txt[i] == '\n')
        fputc('\r', fp);
      fputc(txt[i], fp);
    }
  } else if (conv && strcmp(conv, "lf") == 0) {
    for (size_t i = 0; i < len; i++)
      if (txt[i] != '\r') fputc(txt[i], fp);
  } else {
    fwrite(txt, 1, len, fp);
  }
  fclose(fp);
  free(txt);
  return true;
}

static void tickWait(Run *r) {
  char path[SATRIA_MAX_PATH], buf[64];
  snprintf(buf, sizeof(buf), ".tick.%d", (int)getpid());
  dslWPath(r->c, buf, path, sizeof(path));
  FILE *fp = fopen(path, "wb");
  if (!fp) return;
  fclose(fp);
  int64_t a = fsMTimeNs(path);
  for (int i = 0; i < 200; i++) {
    fp = fopen(path, "ab");
    if (!fp) break;
    fputc('x', fp);
    fclose(fp);
    int64_t b = fsMTimeNs(path);
    if (b != a) break;
    procSleepMs(10);
  }
  fsRemoveFile(path);
}

/* ==================== deklarasi maju ==================== */

static void execRange(Run *r, int start, int end);
static int findEnd(EvList *l, int i);

static const char *assertLabel(Run *r) {
  return (r->resp && r->resp[0]) ? r->resp : "last";
}

static void evalOrPrecond(Run *r, const char *expr, char *out, size_t n) {
  /* Ekspresi juga melewati interpolasi + selektor (seperti ev_expr bash). */
  char resolved[DSL_VAL];
  int rr = dslResolve(r->c, expr, resolved, sizeof(resolved));
  if (rr == 1) precondition(r, r->c->errMsg);
  if (rr != 0) dieMsg(r, "resolve gagal: %s", expr);
  int rc = dslEval(r->c, resolved, out, n);
  if (rc == 1) precondition(r, r->c->errMsg);
  if (rc != 0) dieMsg(r, "evaluasi gagal: %s", expr);
}

static void resolveOrPrecond(Run *r, const char *raw, char *out, size_t n) {
  int rc = dslResolve(r->c, raw, out, n);
  if (rc == 1) precondition(r, r->c->errMsg);
  if (rc != 0) dieMsg(r, "resolve gagal: %s", raw);
}

/* ==================== asersi ==================== */

static void assertStmt(Run *r, const char *obj, const char *aksi, char *args,
                       const char *clause) {
  DslCtx *c = r->c;
  if (strcmp(obj, "rc") == 0) {
    const char *label = args[0] ? firstToken(args) : assertLabel(r);
    DslRun *run = NULL;
    for (int i = 0; i < c->nruns; i++)
      if (strcmp(c->runs[i].label, label) == 0) run = &c->runs[i];
    if (!run || !run->hasRc) {
      fail(r, clause, "tidak ada run berlabel", label);
      return;
    }
    if (strcmp(aksi, "ok") == 0) {
      if (run->rc == 0) pass(r, clause);
      else {
        char got[32];
        snprintf(got, sizeof(got), "%d", run->rc);
        fail(r, clause, got, "0");
      }
    } else {
      if (run->rc != 0) pass(r, clause);
      else {
        char got[32];
        snprintf(got, sizeof(got), "%d", run->rc);
        fail(r, clause, got, "bukan 0");
      }
    }
    return;
  }

  if (strcmp(obj, "out") == 0) {
    /* out.equals membandingkan dua label eksplisit, bukan label @response. */
    if (strcmp(aksi, "equals") == 0) {
      char tmp[DSL_VAL];
      copyStr(tmp, sizeof(tmp), args);
      char **tok = NULL;
      int n = splitSpace(tmp, &tok);
      DslRun *a = NULL;
      DslRun *b = NULL;
      for (int i = 0; i < c->nruns; i++) {
        if (n > 0 && strcmp(c->runs[i].label, tok[0]) == 0) a = &c->runs[i];
        if (n > 1 && strcmp(c->runs[i].label, tok[1]) == 0) b = &c->runs[i];
      }
      bool same = n >= 2 && a && b && a->out && b->out && fsFileExists(a->out) &&
                  fsFileExists(b->out);
      if (same) {
        size_t l1 = 0, l2 = 0;
        char *t1 = dslReadFile(a->out, &l1);
        char *t2 = dslReadFile(b->out, &l2);
        same = t1 && t2 && l1 == l2 && memcmp(t1, t2, l1) == 0;
        free(t1);
        free(t2);
      }
      free(tok);
      if (same) pass(r, clause);
      else fail(r, clause, "berbeda", "identik");
      return;
    }
    const char *label = assertLabel(r);
    DslRun *run = NULL;
    for (int i = 0; i < c->nruns; i++)
      if (strcmp(c->runs[i].label, label) == 0) run = &c->runs[i];
    /* Bentuk label eksplisit: `out.contains <label> <teks>`. Dipakai hanya
       bila label default tidak punya hasil, agar teks biasa tak salah tafsir. */
    char explicitLabel[256] = "";
    if (!run && args[0]) {
      char tmp[DSL_VAL];
      copyStr(tmp, sizeof(tmp), args);
      char *sp = strchr(tmp, ' ');
      if (sp) {
        *sp = '\0';
        for (int i = 0; i < c->nruns; i++) {
          if (strcmp(c->runs[i].label, tmp) == 0) {
            run = &c->runs[i];
            copyStr(explicitLabel, sizeof(explicitLabel), tmp);
            memmove(args, sp + 1, strlen(sp + 1) + 1);
            break;
          }
        }
      }
    }
    if (!run || !run->out || !fsFileExists(run->out)) {
      fail(r, clause, "tidak ada", explicitLabel[0] ? explicitLabel : "hasil run");
      return;
    }
    char pat[DSL_VAL];
    dslUnq(args);
    dslUnesc(args, pat, sizeof(pat));
    size_t len = 0;
    char *txt = dslReadFile(run->out, &len);
    bool hit = false;
    if (txt) {
      if (strcmp(aksi, "contains") == 0) hit = strstr(txt, pat) != NULL;
      else if (strcmp(aksi, "not-contains") == 0) hit = strstr(txt, pat) == NULL;
      else if (strcmp(aksi, "matches") == 0) hit = dslRegexText(pat, txt, len);
      else if (strcmp(aksi, "not-matches") == 0) hit = !dslRegexText(pat, txt, len);
    }
    free(txt);
    if (strcmp(aksi, "not-contains") == 0 || strcmp(aksi, "not-matches") == 0) {
      if (hit) pass(r, clause);
      else {
        char want[DSL_VAL + 16];
        snprintf(want, sizeof(want), "tiada %s", pat);
        fail(r, clause, "ada", want);
      }
      return;
    }
    if (hit) pass(r, clause);
    else
      fail(r, clause, strcmp(aksi, "matches") == 0 ? "tidak cocok" : "tidak ada", pat);
    return;
  }

  if (strcmp(obj, "file") == 0) {
    if (strcmp(aksi, "exists") == 0 || strcmp(aksi, "absent") == 0) {
      char path[SATRIA_MAX_PATH];
      dslWPath(c, args, path, sizeof(path));
      bool has = fsFileExists(path) || fsDirExists(path);
      if (strcmp(aksi, "exists") == 0) {
        if (has) pass(r, clause);
        else fail(r, clause, "absen", args);
      } else {
        if (!has) pass(r, clause);
        else fail(r, clause, "ada", args);
      }
      return;
    }
    if (strcmp(aksi, "same") == 0) {
      char tmp[DSL_VAL];
      copyStr(tmp, sizeof(tmp), args);
      char **tok = NULL;
      int n = splitSpace(tmp, &tok);
      bool same = false;
      if (n >= 2) {
        char p1[SATRIA_MAX_PATH], p2[SATRIA_MAX_PATH];
        dslWPath(c, tok[0], p1, sizeof(p1));
        dslWPath(c, tok[1], p2, sizeof(p2));
        size_t l1 = 0, l2 = 0;
        char *t1 = dslReadFile(p1, &l1);
        char *t2 = dslReadFile(p2, &l2);
        same = t1 && t2 && l1 == l2 && memcmp(t1, t2, l1) == 0;
        free(t1);
        free(t2);
      }
      free(tok);
      if (same) pass(r, clause);
      else fail(r, clause, "berbeda", "identik");
      return;
    }
    if (strcmp(aksi, "contains") == 0) {
      char tmp[DSL_VAL];
      copyStr(tmp, sizeof(tmp), args);
      char *sp = strchr(tmp, ' ');
      if (!sp) dieMsg(r, "bentuk: file.contains <path> <pola>");
      *sp = '\0';
      char *path = tmp;
      char pat[DSL_VAL];
      dslUnesc(sp + 1, pat, sizeof(pat));
      char full[SATRIA_MAX_PATH];
      dslWPath(c, path, full, sizeof(full));
      size_t len = 0;
      char *txt = dslReadFile(full, &len);
      bool hit = txt && dslRegexText(pat, txt, len);
      free(txt);
      if (hit) pass(r, clause);
      else fail(r, clause, "tidak ada", pat);
      return;
    }
    return;
  }

  if (strcmp(obj, "mtime") == 0) {
    DslMark *m = NULL;
    for (int i = 0; i < c->nmarks; i++)
      if (strcmp(c->marks[i].name, args) == 0) m = &c->marks[i];
    if (!m || !m->valid) {
      fail(r, clause, "rekaman tidak ada", "time.mark dulu");
      return;
    }
    int64_t cur = fsMTimeNs(m->path);
    bool changed = cur != m->ns;
    if (strcmp(aksi, "changed") == 0) {
      if (changed) pass(r, clause);
      else fail(r, clause, "sama", "berubah");
    } else {
      if (!changed) pass(r, clause);
      else fail(r, clause, "berubah", "sama");
    }
    return;
  }
}

/* ==================== run.* ==================== */

static void splitArgs(char *args, char ***out, int *n, char *label, size_t labeln) {
  char *tmp = strdup(args);
  int cnt = 0;
  char **tok = NULL;
  if (tmp) cnt = splitSpace(tmp, &tok);
  label[0] = '\0';
  if (cnt >= 2 && strcmp(tok[cnt - 2], "as") == 0) {
    copyStr(label, labeln, tok[cnt - 1]);
    cnt -= 2;
  }
  *out = tok;
  *n = cnt;
}

static void runStmt(Run *r, const char *aksi, char *args) {
  DslCtx *c = r->c;
  if (!fsFileExists(c->satria)) precondition(r, "binary satria tidak ada");

  char label[128];
  char **tok = NULL;
  int n = 0;
  splitArgs(args, &tok, &n, label, sizeof(label));
  if (!label[0]) copyStr(label, sizeof(label), "last");

  char outPath[SATRIA_MAX_PATH + 256];
  if (strcmp(aksi, "wait") == 0) {
    const char *wl = n > 0 ? tok[0] : "last";
    int lim = n > 1 ? atoi(tok[1]) : 120;
    DslRun *run = runAdd(r, wl);
    if (!run || !run->pid) dieMsg(r, "run.wait: tidak ada proses latar '%s'", wl);
    int st = 0;
    int waited = 0;
    bool done = false;
    while (!done) {
      pid_t got = waitpid(run->pid, &st, WNOHANG);
      if (got == run->pid) {
        done = true;
      } else if (got < 0 && errno != EINTR) {
        /* ECHILD: sudah di-reap pihak lain — anggap selesai. */
        st = 0;
        done = true;
      } else if (waited >= lim * 10) {
        dieMsg(r, "run.wait: lewat batas %ds: %s", lim, wl);
      } else {
        procSleepMs(100);
        waited++;
      }
    }
    run->rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    run->hasRc = true;
    run->pid = 0;
    free(tok);
    return;
  }
  if (strcmp(aksi, "stop") == 0) {
    const char *sl = n > 0 ? tok[0] : "last";
    DslRun *run = runAdd(r, sl);
    if (!run || !run->pid) dieMsg(r, "run.stop: tidak ada proses latar '%s'", sl);
    kill(run->pid, SIGINT);
    free(tok);
    return;
  }

  snprintf(outPath, sizeof(outPath), "%s/.out.%s.txt", c->work, label);
  DslRun *run = runAdd(r, label);
  if (!run) dieMsg(r, "kehabisan memori");
  free(run->out);
  run->out = strdup(outPath);
  run->hasRc = false;

  char *argv[512];
  int ac = 0;
  /* run.exec menjalankan program tes itu sendiri; build/cmd/bg adalah
     argumen untuk binary satria. */
  if (strcmp(aksi, "exec") != 0) argv[ac++] = c->satria;
  char jb[32];
  if (strcmp(aksi, "build") == 0) {
    snprintf(jb, sizeof(jb), "-j%d", c->jobs);
    argv[ac++] = jb;
    if (r->workspace) argv[ac++] = "-w";
  }

  if (strcmp(aksi, "tool") == 0) {
    if (n < 1) dieMsg(r, "run.tool butuh skrip");
    char script[SATRIA_MAX_PATH + 256];
    snprintf(script, sizeof(script), "%s/%s", c->repo, tok[0]);
    if (!fsFileExists(script)) dieMsg(r, "run.tool: skrip tidak ada: %s", tok[0]);
    char *shargv[512];
    int sc = 0;
    shargv[sc++] = "sh";
    shargv[sc++] = script;
    for (int i = 1; i < n; i++) shargv[sc++] = tok[i];
    shargv[sc] = NULL;
    run->rc = spawnWait(shargv, c->repo, outPath, NULL, false);
    run->hasRc = true;
    free(tok);
    return;
  }

  const char *cwd = c->work;
  int first = 0;
  if (strcmp(aksi, "exec") == 0) {
    if (n < 1) dieMsg(r, "run.exec butuh program");
    argv[ac++] = tok[0];
    first = 1;
  } else if (strcmp(aksi, "bg") == 0) {
    if (n < 1) dieMsg(r, "run.bg butuh argumen");
  } else if (strcmp(aksi, "cmd") == 0 && n < 1) {
    dieMsg(r, "run.cmd butuh argumen");
  }

  if (strcmp(aksi, "bg") != 0) {
    for (int i = first; i < n; i++) argv[ac++] = tok[i];
    argv[ac] = NULL;
    run->rc = spawnWait(argv, cwd, outPath, NULL, false);
    run->hasRc = true;
  } else {
    for (int i = first; i < n; i++) argv[ac++] = tok[i];
    argv[ac] = NULL;
    int pid = 0;
    spawnWait(argv, cwd, outPath, &pid, true);
    run->pid = pid;
  }
  free(tok);
}

/* ==================== fixture & discovery ==================== */

/* Salin isi fixture rekursif ke `dst`, menjaga struktur relatif terhadap
   root fixture (seperti `cp -a <fix>/. <dst>/`). Artefak build tidak ikut:
   cache/compile db basi menunjuk path lama dan membuat build pertama gagal. */
bool testCopyFixtureTree(const char *fixdir, const char *dst) {
  List stack = {0};
  listAdd(&stack, fixdir);
  size_t rootLen = strlen(fixdir);
  for (int i = 0; i < stack.count; i++) {
    List dirs = {0}, files = {0};
    const char *base = stack.items[i];
    fsListDir(base, &dirs, &files);
    for (int j = 0; j < files.count; j++) {
      char out[SATRIA_MAX_PATH + 256];
      snprintf(out, sizeof(out), "%s/%s", dst, files.items[j] + rootLen + 1);
      copyFileConvert(files.items[j], out, NULL);
    }
    for (int j = 0; j < dirs.count; j++) {
      char out[SATRIA_MAX_PATH + 256];
      snprintf(out, sizeof(out), "%s/%s", dst, dirs.items[j] + rootLen + 1);
      mkdirs(out);
      listAdd(&stack, dirs.items[j]);
    }
    listFree(&dirs);
    listFree(&files);
  }
  listFree(&stack);
  const char *artifacts[] = {"bin", "lib", "build", "modules", "dist",
                             ".satria", "compile_commands.json"};
  for (size_t i = 0; i < sizeof(artifacts) / sizeof(artifacts[0]); i++) {
    char p[SATRIA_MAX_PATH + 64];
    snprintf(p, sizeof(p), "%s/%s", dst, artifacts[i]);
    fsRemoveTree(p);
  }
  return true;
}

/* Ambil nilai string `"key": "..."` dari satu baris JSON. */
static bool jsonString(const char *line, const char *key, char *out, size_t n) {
  const char *p = strstr(line, key);
  if (!p) return false;
  p += strlen(key);
  while (*p == ' ' || *p == '\t') p++;
  if (*p != ':') return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return false;
  p++;
  size_t o = 0;
  while (*p && *p != '"' && o + 1 < n) {
    out[o++] = (*p == '\\') ? '/' : *p;
    p++;
  }
  out[o] = '\0';
  return o > 0;
}

/* compile_commands.json → sources.tsv (proyek<TAB>sumber<TAB>objek), sama
   dengan konversi awk runner: proyek = segmen pertama di bawah build/. */
bool testCompdbToTsv(const char *jsonPath, const char *tsvPath) {
  size_t len = 0;
  char *txt = dslReadFile(jsonPath, &len);
  if (!txt) return false;
  FILE *fp = fopen(tsvPath, "wb");
  if (!fp) {
    free(txt);
    return false;
  }
  char file[SATRIA_MAX_PATH * 2] = "";
  bool any = false;
  for (char *line = txt; *line;) {
    char *end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    char save = *end;
    *end = '\0';
    char val[SATRIA_MAX_PATH * 2];
    if (jsonString(line, "\"file\"", val, sizeof(val))) {
      copyStr(file, sizeof(file), val);
    } else if (jsonString(line, "\"output\"", val, sizeof(val)) && file[0]) {
      const char *b = strstr(val, "/build/");
      if (b) {
        const char *obj = b + 7;
        const char *slash = strchr(obj, '/');
        if (slash && slash > obj) {
          size_t pl = (size_t)(slash - obj);
          char pat[SATRIA_MAX_PATH + 8];
          snprintf(pat, sizeof(pat), "/%.*s/", (int)pl, obj);
          const char *hit = strstr(file, pat);
          const char *src = hit ? hit + strlen(pat) : NULL;
          if (!src) {
            const char *bs = strrchr(file, '/');
            src = bs ? bs + 1 : file;
          }
          fprintf(fp, "%.*s\t%s\t%s\n", (int)pl, obj, src, obj);
          any = true;
        }
      }
      file[0] = '\0';
    }
    *end = save;
    line = save ? end + 1 : end;
  }
  fclose(fp);
  free(txt);
  return any;
}

/* Bila UNIT_DISCOVERY tidak diisi, siapkan discovery sendiri dari fixture:
   salin fixture (tanpa artefak), minta `satria -w -g compdb`, lalu konversi.
   Dipanggil hanya bila tes memang memakai selektor project(). */
static bool autoDiscovery(Run *r) {
  DslCtx *c = r->c;
  if (c->discovery[0]) {
    char tsv[SATRIA_MAX_PATH + 32];
    snprintf(tsv, sizeof(tsv), "%s/sources.tsv", c->discovery);
    if (fsFileExists(tsv)) return true;
  }
  if (!r->fixture[0] || !fsFileExists(c->satria)) return false;
  char fixdir[SATRIA_MAX_PATH + 512];
  const char *fenv = getenv("UNIT_FIXTURE");
  if (fenv && *fenv)
    copyStr(fixdir, sizeof(fixdir), fenv);
  else
    snprintf(fixdir, sizeof(fixdir), "%s/tests/%s", c->repo, r->fixture);
  if (!fsDirExists(fixdir)) return false;

  char disc[SATRIA_MAX_PATH + 320];
  snprintf(disc, sizeof(disc), "%s/discovery.%s", c->work, r->testName);
  fsRemoveTree(disc);
  mkdirs(disc);
  testCopyFixtureTree(fixdir, disc);

  char logp[SATRIA_MAX_PATH + 360];
  snprintf(logp, sizeof(logp), "%s/discovery.log", disc);
  char jb[32];
  snprintf(jb, sizeof(jb), "-j%d", c->jobs);
  char *argv[] = {c->satria, jb, "-w", "-g", "compdb", NULL};
  int rc = spawnWait(argv, disc, logp, NULL, false);
  if (rc != 0) return false;

  char json[SATRIA_MAX_PATH + 360], tsv[SATRIA_MAX_PATH + 360];
  snprintf(json, sizeof(json), "%s/compile_commands.json", disc);
  snprintf(tsv, sizeof(tsv), "%s/sources.tsv", disc);
  if (!testCompdbToTsv(json, tsv)) return false;
  copyStr(c->discovery, sizeof(c->discovery), disc);
  return true;
}

/* ==================== statement ==================== */

static void stmtExec(Run *r, Ev *e, int *ip) {
  DslCtx *c = r->c;
  char args[DSL_VAL];
  resolveOrPrecond(r, e->c ? e->c : "", args, sizeof(args));

  char clause[DSL_VAL + 128];
  snprintf(clause, sizeof(clause), "%s.%s %s", e->a, e->b, e->c ? e->c : "");

  if (strcmp(e->a, "work") == 0) {
    if (strcmp(e->b, "new") == 0) {
      char name[256];
      if (args[0]) {
        copyStr(name, sizeof(name), args);
      } else {
        copyStr(name, sizeof(name), r->testName);
      }
      char path[SATRIA_MAX_PATH];
      snprintf(path, sizeof(path), "%s/%s", getenv("UNIT_TMP") ? getenv("UNIT_TMP") : "/tmp/satria-dsl", name);
      fsRemoveTree(path);
      mkdirs(path);
      listAdd(&r->works, path);
      copyStr(c->work, sizeof(c->work), path);
    } else if (strcmp(e->b, "clean") == 0) {
      if (r->snap) dieMsg(r, "work.clean hanya untuk kelas [cold]");
      const char *what = args;
      if (strcmp(what, "all") == 0) {
        const char *dirs[] = {"build", "bin", "lib", "modules", ".satria"};
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
          char p[SATRIA_MAX_PATH + 64];
          snprintf(p, sizeof(p), "%s/%s", c->work, dirs[i]);
          fsRemoveTree(p);
        }
      } else if (strcmp(what, "state") == 0) {
        char p[SATRIA_MAX_PATH + 64];
        snprintf(p, sizeof(p), "%s/.satria", c->work);
        fsRemoveTree(p);
      } else {
        dieMsg(r, "bentuk: work.clean all|state");
      }
    }
    return;
  }

  if (strcmp(e->a, "fixture") == 0 && strcmp(e->b, "copy") == 0) {
    if (!r->fixture[0]) dieMsg(r, "fixture.copy tanpa @fixture");
    char fixdir[SATRIA_MAX_PATH + 512];
    const char *fenv = getenv("UNIT_FIXTURE");
    if (fenv && *fenv)
      copyStr(fixdir, sizeof(fixdir), fenv);
    else
      snprintf(fixdir, sizeof(fixdir), "%s/tests/%s", c->repo, r->fixture);
    if (!fsDirExists(fixdir)) dieMsg(r, "gagal salin fixture %s", fixdir);
    testCopyFixtureTree(fixdir, c->work);
    return;
  }

  if (strcmp(e->a, "file") == 0) {
    if (strcmp(e->b, "write") == 0) {
      writeFileHeredoc(r, args, ip);
      return;
    }
    if (strcmp(e->b, "append") == 0) {
      char tmp[DSL_VAL];
      copyStr(tmp, sizeof(tmp), args);
      char *sp = strchr(tmp, ' ');
      if (!sp) dieMsg(r, "bentuk: file.append <path> <teks>");
      *sp = '\0';
      char text[DSL_VAL];
      dslUnq(sp + 1);
      dslUnesc(sp + 1, text, sizeof(text));
      char full[SATRIA_MAX_PATH];
      dslWPath(c, tmp, full, sizeof(full));
      appendLine(full, text);
      return;
    }
    if (strcmp(e->b, "touch") == 0) {
      char full[SATRIA_MAX_PATH];
      dslWPath(c, args, full, sizeof(full));
      FILE *fp = fopen(full, "ab");
      if (fp) fclose(fp);
      utime(full, NULL);
      return;
    }
    if (strcmp(e->b, "delete") == 0) {
      char full[SATRIA_MAX_PATH];
      dslWPath(c, args, full, sizeof(full));
      fsRemoveFile(full);
      return;
    }
    if (strcmp(e->b, "copy") == 0) {
      char tmp[DSL_VAL];
      copyStr(tmp, sizeof(tmp), args);
      char **tok = NULL;
      int n = splitSpace(tmp, &tok);
      if (n < 2) dieMsg(r, "bentuk: file.copy <src> <dst> [lf|crlf]");
      if (!fsFileExists(tok[0])) dieMsg(r, "file.copy: sumber tidak ada: %s", tok[0]);
      const char *conv = n > 2 ? tok[2] : "";
      if (conv[0] && strcmp(conv, "lf") != 0 && strcmp(conv, "crlf") != 0)
        dieMsg(r, "file.copy: konversi tidak dikenal: %s", conv);
      char dst[SATRIA_MAX_PATH];
      dslWPath(c, tok[1], dst, sizeof(dst));
      copyFileConvert(tok[0], dst, conv);
      free(tok);
      return;
    }
    assertStmt(r, e->a, e->b, args, clause);
    return;
  }

  if (strcmp(e->a, "env") == 0) {
    if (strcmp(e->b, "set") == 0) {
      char *eq = strchr(args, '=');
      if (eq) {
        *eq = '\0';
        setenv(args, eq + 1, 1);
      }
    } else if (strcmp(e->b, "unset") == 0) {
      unsetenv(args);
    }
    return;
  }

  if (strcmp(e->a, "time") == 0) {
    if (strcmp(e->b, "tick") == 0) {
      tickWait(r);
    } else if (strcmp(e->b, "mark") == 0) {
      char tmp[DSL_VAL];
      copyStr(tmp, sizeof(tmp), args);
      char *as = strstr(tmp, " as ");
      if (!as) return;
      *as = '\0';
      char *name = firstToken(as + 4);
      char full[SATRIA_MAX_PATH];
      dslWPath(c, tmp, full, sizeof(full));
      if (c->nmarks == c->capmarks) {
        int nc = c->capmarks ? c->capmarks * 2 : 8;
        DslMark *g = realloc(c->marks, (size_t)nc * sizeof(*g));
        if (!g) return;
        c->marks = g;
        c->capmarks = nc;
      }
      DslMark *m = &c->marks[c->nmarks++];
      memset(m, 0, sizeof(*m));
      m->name = strdup(name);
      m->path = strdup(full);
      m->ns = fsMTimeNs(full);
      m->valid = true;
    }
    return;
  }

  if (strcmp(e->a, "run") == 0) {
    runStmt(r, e->b, args);
    return;
  }

  if (strcmp(e->a, "stage") == 0) {
    char tmp[DSL_VAL];
    copyStr(tmp, sizeof(tmp), args);
    char *sp = strchr(tmp, ' ');
    if (!sp) return;
    *sp = '\0';
    char text[DSL_VAL];
    dslUnq(sp + 1);
    dslUnesc(sp + 1, text, sizeof(text));
    char full[SATRIA_MAX_PATH];
    dslWPath(c, tmp, full, sizeof(full));
    if (strcmp(e->b, "set") == 0) {
      mkparent(full);
      FILE *fp = fopen(full, "wb");
      if (fp) {
        fputs(text, fp);
        fputc('\n', fp);
        fclose(fp);
      }
    } else {
      appendLine(full, text);
    }
    return;
  }

  if (strcmp(e->a, "text") == 0) {
    if (strcmp(e->b, "count") == 0) {
      char tmp[DSL_VAL];
      copyStr(tmp, sizeof(tmp), args);
      char **tok = NULL;
      int n = splitSpace(tmp, &tok);
      if (n < 2) dieMsg(r, "bentuk: text.count <path> <pola> [as <nama>]");
      char full[SATRIA_MAX_PATH];
      dslWPath(c, tok[0], full, sizeof(full));
      size_t len = 0;
      char *txt = dslReadFile(full, &len);
      int cnt = txt ? dslRegexCount(tok[1], txt, len) : 0;
      free(txt);
      if (n >= 4 && strcmp(tok[2], "as") == 0) {
        char num[32];
        snprintf(num, sizeof(num), "%d", cnt);
        dslVarSet(c, tok[3], num);
      } else {
        printf("INFO: text.count %s = %d\n", tok[0], cnt);
      }
      free(tok);
      return;
    }
    if (strcmp(e->b, "aggregate") == 0) {
      char tmp[DSL_VAL];
      copyStr(tmp, sizeof(tmp), args);
      char **tok = NULL;
      int n = splitSpace(tmp, &tok);
      if (n != 2) dieMsg(r, "bentuk: text.aggregate <src> <dst>");
      char src[SATRIA_MAX_PATH], dst[SATRIA_MAX_PATH];
      dslWPath(c, tok[0], src, sizeof(src));
      dslWPath(c, tok[1], dst, sizeof(dst));
      size_t len = 0;
      char *txt = dslReadFile(src, &len);
      /* sort | uniq -c | sort -rn | head */
      char *lines[4096];
      int nl = 0;
      char *p = txt;
      while (txt && p && *p && nl < 4096) {
        char *e2 = strchr(p, '\n');
        if (e2) *e2 = '\0';
        lines[nl++] = p;
        p = e2 ? e2 + 1 : NULL;
      }
      for (int i = 0; i < nl; i++)
        for (int j = i + 1; j < nl; j++)
          if (strcmp(lines[i], lines[j]) > 0) {
            char *t = lines[i];
            lines[i] = lines[j];
            lines[j] = t;
          }
      FILE *fp = fopen(dst, "wb");
      if (fp) {
        int i = 0, out = 0;
        while (i < nl && out < 10) {
          int j = i;
          while (j < nl && strcmp(lines[i], lines[j]) == 0) j++;
          fprintf(fp, "%7d %s\n", j - i, lines[i]);
          i = j;
          out++;
        }
        fclose(fp);
      }
      free(txt);
      free(tok);
      return;
    }
    return;
  }

  if (strcmp(e->a, "rc") == 0 || strcmp(e->a, "out") == 0 || strcmp(e->a, "mtime") == 0) {
    assertStmt(r, e->a, e->b, args, clause);
    return;
  }
  dieMsg(r, "baris %d: %s.%s belum dieksekusi", e->line, e->a, e->b);
}

/* ==================== direktif ==================== */

static void execEvents(Run *r, EvList *l, int start, int end);

static void includeDirective(Run *r, const char *rest) {
  char tmp[DSL_VAL];
  copyStr(tmp, sizeof(tmp), rest);
  char **tok = NULL;
  int n = splitSpace(tmp, &tok);
  if (n < 1) dieMsg(r, "@include tanpa path");
  char ip[SATRIA_MAX_PATH + 128];
  snprintf(ip, sizeof(ip), "%s/tests/unit/%s", r->c->repo, tok[0]);
  if (!fsFileExists(ip)) snprintf(ip, sizeof(ip), "%s", tok[0]);
  if (!fsFileExists(ip)) dieMsg(r, "@include tidak ada: %s", tok[0]);
  const char *as = (n >= 3 && strcmp(tok[1], "as") == 0) ? tok[2] : NULL;

  EvList sub = {0};
  if (!loadEvents(ip, &sub)) dieMsg(r, "parse gagal: %s", ip);
  execEvents(r, &sub, 0, sub.n);
  evFree(&sub);
  if (as) {
    dslVarSet(r->c, as, r->retVal);
    r->retVal[0] = '\0';
  }
  r->retHit = false;
  free(tok);
}

/* Glob sederhana: * ? [] dan ** (melintasi '/'). */
static bool globMatch(const char *p, const char *t) {
  if (!*p) return !*t;
  if (p[0] == '*' && p[1] == '*') {
    const char *rest = p + 2;
    if (*rest == '/') rest++;
    for (const char *s = t;; s++) {
      if (globMatch(rest, s)) return true;
      if (!*s) break;
    }
    return false;
  }
  if (*p == '*') {
    for (const char *s = t;; s++) {
      if (globMatch(p + 1, s)) return true;
      if (!*s || *s == '/') break;
    }
    return false;
  }
  if (*p == '?') return *t && *t != '/' && globMatch(p + 1, t + 1);
  if (*p == '[') {
    const char *q = p + 1;
    bool neg = false;
    if (*q == '!') {
      neg = true;
      q++;
    }
    bool hit = false;
    while (*q && *q != ']') {
      if (q[1] == '-' && q[2] && q[2] != ']') {
        if (*t >= q[0] && *t <= q[2]) hit = true;
        q += 3;
      } else {
        if (*t == *q) hit = true;
        q++;
      }
    }
    if (*q != ']') return false;
    return *t && (hit != neg) && globMatch(q + 1, t + 1);
  }
  return *p == *t && globMatch(p + 1, t + 1);
}

static void collectGlob(Run *r, const char *dir, const char *rel, const char *pat,
                        List *out) {
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  for (int i = 0; i < files.count; i++) {
    const char *name = strrchr(files.items[i], '/');
    name = name ? name + 1 : files.items[i];
    char relp[SATRIA_MAX_PATH * 2];
    snprintf(relp, sizeof(relp), "%s%s", rel[0] ? rel : "", name);
    if (globMatch(pat, relp)) listAdd(out, relp);
  }
  for (int i = 0; i < dirs.count; i++) {
    const char *name = strrchr(dirs.items[i], '/');
    name = name ? name + 1 : dirs.items[i];
    char relp[SATRIA_MAX_PATH * 2];
    snprintf(relp, sizeof(relp), "%s%s", rel[0] ? rel : "", name);
    char nextrel[SATRIA_MAX_PATH * 2 + 2];
    snprintf(nextrel, sizeof(nextrel), "%s/", relp);
    collectGlob(r, dirs.items[i], nextrel, pat, out);
  }
  listFree(&dirs);
  listFree(&files);
}

static void dirExec(Run *r, Ev *e) {
  DslCtx *c = r->c;
  const char *d = e->a ? e->a : "";
  const char *rest = e->b ? e->b : "";
  if (strcmp(d, "return") == 0) {
    char val[DSL_VAL];
    if (*rest) evalOrPrecond(r, rest, val, sizeof(val));
    else val[0] = '\0';
    copyStr(r->retVal, sizeof(r->retVal), val);
    r->retHit = true;
    return;
  }
  if (strcmp(d, "guard") == 0) {
    char val[DSL_VAL];
    evalOrPrecond(r, rest, val, sizeof(val));
    if (!dslTruthy(val)) {
      char msg[DSL_VAL + 32];
      snprintf(msg, sizeof(msg), "@guard salah: %s", rest);
      precondition(r, msg);
    }
    return;
  }
  if (strcmp(d, "set") == 0) {
    char tmp[DSL_VAL];
    copyStr(tmp, sizeof(tmp), rest);
    char *sp = strchr(tmp, ' ');
    if (!sp) dieMsg(r, "bentuk: @set <nama> <ekspresi>");
    *sp = '\0';
    char val[DSL_VAL];
    evalOrPrecond(r, sp + 1, val, sizeof(val));
    dslVarSet(c, tmp, val);
    return;
  }
  if (strcmp(d, "get") == 0) {
    printf("INFO: %s = %s\n", rest, dslVarGet(c, rest) ? dslVarGet(c, rest) : "");
    return;
  }
  if (strcmp(d, "find") == 0) {
    char tmp[DSL_VAL];
    dslInterp(c, rest, tmp, sizeof(tmp));
    char *sp = strchr(tmp, ' ');
    if (!sp) dieMsg(r, "bentuk: @find <nama> <pola>");
    *sp = '\0';
    List found = {0};
    collectGlob(r, c->work, "", sp + 1, &found);
    char joined[DSL_VAL] = "";
    size_t o = 0;
    for (int i = 0; i < found.count; i++) {
      if (i) catStr(joined, sizeof(joined), &o, " ");
      catStr(joined, sizeof(joined), &o, found.items[i]);
    }
    dslVarSet(c, tmp, joined);
    listFree(&found);
    return;
  }
  if (strcmp(d, "include") == 0) {
    includeDirective(r, rest);
    return;
  }
  dieMsg(r, "baris %d: @%s belum dieksekusi", e->line, d);
}

/* ==================== blok ==================== */

static int findEnd(EvList *l, int i) {
  int depth = 0;
  for (int j = i + 1; j < l->n; j++) {
    if (strcmp(l->v[j].tag, "BLOCK") == 0) depth++;
    else if (strcmp(l->v[j].tag, "ENDB") == 0) {
      if (depth == 0) return j;
      depth--;
    }
  }
  return l->n;
}

static void runBlock(Run *r, Ev *e, int startIdx) {
  DslCtx *c = r->c;
  const char *kind = e->a ? e->a : "";
  const char *expr = e->b ? e->b : "";
  int endIdx = findEnd(r->ev, startIdx);

  if (strcmp(kind, "response") == 0) {
    const char *old = r->resp;
    r->resp = expr[0] ? expr : NULL;
    execRange(r, startIdx + 1, endIdx);
    r->resp = old;
    return;
  }
  if (strcmp(kind, "if") == 0) {
    char val[DSL_VAL];
    evalOrPrecond(r, expr, val, sizeof(val));
    int elseIdx = -1, depth = 0;
    for (int j = startIdx + 1; j < endIdx; j++) {
      if (strcmp(r->ev->v[j].tag, "BLOCK") == 0) depth++;
      else if (strcmp(r->ev->v[j].tag, "ENDB") == 0) depth--;
      else if (depth == 0 && strcmp(r->ev->v[j].tag, "DIR") == 0 &&
               strcmp(r->ev->v[j].a, "else") == 0) {
        elseIdx = j;
        break;
      }
    }
    if (dslTruthy(val)) {
      execRange(r, startIdx + 1, elseIdx >= 0 ? elseIdx : endIdx);
    } else if (elseIdx >= 0) {
      execRange(r, elseIdx + 1, endIdx);
    }
    return;
  }
  if (strcmp(kind, "while") == 0) {
    for (int n = 1; n <= 1000; n++) {
      if (n == 1000) dieMsg(r, "@while lebih dari 1000 iterasi: %s", expr);
      char val[DSL_VAL];
      evalOrPrecond(r, expr, val, sizeof(val));
      if (!dslTruthy(val)) break;
      execRange(r, startIdx + 1, endIdx);
      if (r->retHit) break;
    }
    return;
  }
  if (strcmp(kind, "for") == 0) {
    char tmp[DSL_VAL];
    copyStr(tmp, sizeof(tmp), expr);
    char *sp = strchr(tmp, ' ');
    if (!sp) dieMsg(r, "bentuk @for: @for <var> in <rentang|koleksi>");
    *sp = '\0';
    const char *var = tmp;
    char *rest = sp + 1;
    while (*rest == ' ') rest++;
    if (strncmp(rest, "in ", 3) != 0) dieMsg(r, "bentuk @for: @for <var> in <rentang|koleksi>");
    rest += 3;
    char spec[DSL_VAL];
    resolveOrPrecond(r, rest, spec, sizeof(spec));
    long long lo = 0, hi = 0;
    char *dots = strstr(spec, "..");
    bool isRange = false;
    if (dots && dots != spec && dots[2]) {
      char *end1 = NULL;
      lo = strtoll(spec, &end1, 10);
      if (end1 == dots) {
        char *end2 = NULL;
        hi = strtoll(dots + 2, &end2, 10);
        if (end2 && !*end2) isRange = true;
      }
    }
    if (isRange) {
      int step = lo <= hi ? 1 : -1;
      for (long long v = lo; step > 0 ? v <= hi : v >= hi; v += step) {
        char num[32];
        snprintf(num, sizeof(num), "%lld", v);
        dslVarSet(c, var, num);
        execRange(r, startIdx + 1, endIdx);
        if (r->retHit) break;
      }
    } else {
      char *save = NULL;
      for (char *tok = strtok_r(spec, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        dslVarSet(c, var, tok);
        execRange(r, startIdx + 1, endIdx);
        if (r->retHit) break;
      }
    }
    return;
  }
  dieMsg(r, "baris %d: @%s belum dieksekusi", e->line, kind);
}

static void execRange(Run *r, int start, int end) {
  for (int i = start; i < end; i++) {
    if (r->retHit) return;
    Ev *e = &r->ev->v[i];
    if (strcmp(e->tag, "CMT") == 0) {
      if (e->a) printf("%s\n", e->a);
      continue;
    }
    if (strcmp(e->tag, "HDR") == 0) {
      const char *d = e->a ? e->a : "";
      if (strcmp(d, "fixture") == 0) {
        copyStr(r->fixture, sizeof(r->fixture), e->b ? e->b : "");
      } else if (strcmp(d, "purpose") == 0) {
        printf("# @purpose %s\n", e->b ? e->b : "");
      } else if (strcmp(d, "requires") == 0) {
        const char *cls = getenv("UNIT_CLASS");
        if (cls && *cls && strcmp(cls, e->b ? e->b : "") != 0)
          dieMsg(r, "kelas tidak cocok: @requires %s, run %s", e->b, cls);
      } else if (strcmp(d, "keep") == 0) {
        r->keep = true;
        if (g_run) g_run->keep = true;
      }
      continue;
    }
    if (strcmp(e->tag, "DIR") == 0) {
      dirExec(r, e);
      continue;
    }
    if (strcmp(e->tag, "BLOCK") == 0) {
      runBlock(r, e, i);
      i = findEnd(r->ev, i);
      continue;
    }
    if (strcmp(e->tag, "STMT") == 0) {
      stmtExec(r, e, &i);
      continue;
    }
  }
}

static void execEvents(Run *r, EvList *l, int start, int end) {
  EvList *saved = r->ev;
  r->ev = l;
  execRange(r, start, end);
  r->ev = saved;
}

/* ==================== main ==================== */

static const char *envOr(const char *name, const char *def) {
  const char *v = getenv(name);
  return (v && *v) ? v : def;
}

static void baseName(const char *path, char *out, size_t n) {
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  copyStr(out, n, b);
  char *dot = strrchr(out, '.');
  if (dot) *dot = '\0';
}

int testExecMain(int argc, const char *const *argv) {
  if (argc < 1) {
    fprintf(stderr, "pemakaian: satria test <file.test>\n");
    return 2;
  }
  const char *file = argv[0];
  if (!fsFileExists(file)) {
    fprintf(stderr, "FAIL dsl: berkas tidak ada: %s\n", file);
    return 1;
  }

  EvList ev = {0};
  if (!loadEvents(file, &ev)) {
    fprintf(stderr, "FAIL dsl: parse gagal: %s\n", file);
    return 1;
  }

  DslCtx c;
  char cwd[SATRIA_MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) copyStr(cwd, sizeof(cwd), ".");
  const char *repo = envOr("UNIT_REPO", cwd);
  const char *discovery = envOr("UNIT_DISCOVERY", "");
  const char *tmp = envOr("UNIT_TMP", "/tmp/satria-dsl");
  int jobs = atoi(envOr("UNIT_JOBS", "2"));
  char work[SATRIA_MAX_PATH];
  mkdirs(tmp);
  dslCtxInit(&c, tmp, repo, discovery, jobs);

  /* Binary satria: env SATRIA, selain itu <repo>/bin/satria. */
  const char *sat = getenv("SATRIA");
  if (sat && *sat)
    copyStr(c.satria, sizeof(c.satria), sat);
  else
    snprintf(c.satria, sizeof(c.satria), "%s/bin/satria", repo);
  const char *fixEnv = getenv("UNIT_FIXTURE");

  char name[256];
  baseName(file, name, sizeof(name));

  Run r;
  memset(&r, 0, sizeof(r));
  r.c = &c;
  r.testName = name;
  const char *cls = getenv("UNIT_CLASS");
  r.snap = cls && strcmp(cls, "snap") == 0;
  g_run = &r;
  atexit(cleanupWorks);

  /* @suite dari HDR pertama. */
  char modes[256] = "workspace";
  for (int i = 0; i < ev.n; i++) {
    if (strcmp(ev.v[i].tag, "HDR") == 0 && strcmp(ev.v[i].a, "suite") == 0 && ev.v[i].b &&
        ev.v[i].b[0]) {
      copyStr(modes, sizeof(modes), ev.v[i].b);
      break;
    }
  }
  int nmode = 1;
  for (const char *p = modes; *p; p++)
    if (*p == ',') nmode++;

  /* Discovery mandiri: hanya bila tes memakai selektor project() dan runner
     tidak memasok UNIT_DISCOVERY. Fixture dibaca dari HDR @fixture. */
  bool needsDiscovery = false;
  char fixName[256] = "";
  for (int i = 0; i < ev.n; i++) {
    Ev *e = &ev.v[i];
    if (strcmp(e->tag, "HDR") == 0 && strcmp(e->a, "fixture") == 0 && e->b)
      copyStr(fixName, sizeof(fixName), e->b);
    const char *fields[] = {e->a, e->b, e->c};
    for (size_t k = 0; k < sizeof(fields) / sizeof(fields[0]); k++)
      if (fields[k] && strstr(fields[k], "project(")) needsDiscovery = true;
  }
  if (needsDiscovery) {
    copyStr(r.fixture, sizeof(r.fixture), fixName);
    if (!autoDiscovery(&r))
      fprintf(stderr, "PRECONDITION discovery tidak tersedia untuk fixture '%s'\n", fixName);
  }

  printf("== %s\n", name);
  for (char *m = modes; m && *m;) {
    char *comma = strchr(m, ',');
    if (comma) *comma = '\0';
    char mode[64];
    copyStr(mode, sizeof(mode), m);

    if (nmode > 1)
      snprintf(work, sizeof(work), "%s/%s.%s", tmp, name, mode);
    else
      snprintf(work, sizeof(work), "%s/%s", tmp, name);
    fsRemoveTree(work);
    mkdirs(work);
    listAdd(&r.works, work);
    copyStr(c.work, sizeof(c.work), work);
    r.fixture[0] = '\0';
    r.resp = NULL;
    r.retHit = false;
    r.retVal[0] = '\0';
    r.workspace = strcmp(mode, "workspace") == 0;
    execEvents(&r, &ev, 0, ev.n);
    if (r.retHit && strcmp(r.retVal, "fail") == 0) r.fail++;
    r.retHit = false;
    r.retVal[0] = '\0';
    (void)fixEnv;

    if (!comma) break;
    m = comma + 1;
  }

  printf("lulus: %d  gagal: %d\n", r.pass, r.fail);
  cleanupWorks();
  g_run = NULL;
  evFree(&ev);
  listFree(&r.works);
  dslCtxFree(&c);
  return r.fail == 0 ? 0 : 1;
}

#endif /* _WIN32 */

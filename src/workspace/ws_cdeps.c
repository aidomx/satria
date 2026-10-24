/* ws_cdeps.c — cdeps & header: folder header publik proyek, pemetaan daftar dan header workspace.
 * Bagian dari modul workspace (src/workspace/). Lihat ws_internal.h.
 */
#include "ws_internal.h"

static void wsProjectHeaderDirs(const WsProject *dep, List *out);
static int wsHeaderCmp(const void *a, const void *b);

/* ==================== cdeps ==================== */
/*
 * cdeps = <proyek lain>[, ...] — ketergantungan compile-time lintas proyek
 * dalam satu key (todos/7_10_2026.txt). Menggantikan penulisan manual:
 *     <n>.headers = <dep>/include      (folder header publik proyek)
 *     <n>.library.<os> = <dep>          (artifact library per-OS)
 * Dengan cdeps, WsConfig menerbitkan `library = <dep>/<libDir>/lib<name>.a`
 * dan `headers = <dep>/<dir>` (keduanya relatif root workspace) secara
 * internal. Siklus antar proyek sah — sama seperti library.<os>: dua fase
 * build workspace yang menyelesaikannya (fase library melewatkan artifact
 * path proyek lain).
 */
/* Folder header publik proyek `dep`: semua nilai `headers = ...` proyek
   itu (diprefix <root>/ — relatif root workspace), default <root>/include
   bila proyek tidak menulis headers sama sekali. */
static void wsProjectHeaderDirs(const WsProject *dep, List *out) {
  bool any = false;
  for (int j = 0; j < dep->lines.count; j++) {
    const char *ln = dep->lines.items[j];
    if (strncmp(ln, "headers = ", 10) != 0) continue;
    const char *v = ln + 10;
    char buf[WS_LINE_LEN];
    if (strlen(v) >= sizeof(buf)) continue;
    copyStr(buf, sizeof(buf), v);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item) {
        /* <root>/<item> — relatif root workspace; dibangun manual
           (memcpy) agar gcc -Wformat-truncation tidak menolak path. */
        char joined[MAX_PATH];
        size_t need = strlen(dep->root) + 1 + strlen(item) + 1;
        if (need > sizeof(joined)) continue;
        char *dst = joined;
        size_t rl = strlen(dep->root);
        memcpy(dst, dep->root, rl);
        dst += rl;
        *dst++ = '/';
        memcpy(dst, item, strlen(item));
        dst += strlen(item);
        *dst = '\0';
        listAdd(out, joined);
        any = true;
      }
      if (!comma) break;
      save = comma + 1;
    }
  }
  if (!any) {
    char joined[MAX_PATH];
    size_t need = strlen(dep->root) + strlen("/include") + 1;
    if (need <= sizeof(joined)) {
      char *dst = joined;
      size_t rl = strlen(dep->root);
      memcpy(dst, dep->root, rl);
      dst += rl;
      memcpy(dst, "/include", 8);
      dst += 8;
      *dst = '\0';
      listAdd(out, joined);
    }
  }
}
/* Peta daftar "a, b, c" per item -> out (dipisah ", ").
   headersForm: sintaks headers — "I." = root proyek, "I.x" = <root>/.x
   (semantik headerEntryDir lama terhadap root proyek). */
bool wsMapList(const char *root, const char *value, bool headersForm, char *out,
                      size_t n) {
  char buf[WS_LINE_LEN];
  if (!value || strlen(value) >= sizeof(buf)) return false;
  copyStr(buf, sizeof(buf), value);
  out[0] = '\0';
  bool first = true;
  for (char *save = buf;;) {
    char *comma = strchr(save, ',');
    if (comma) *comma = '\0';
    char *item = trim(save);
    char mapped[WS_LINE_LEN];
    mapped[0] = '\0';
    if (*item) {
      if (headersForm && item[0] == 'I' && item[1] == '.') {
        if (item[2] == '\0')
          snprintf(mapped, sizeof(mapped), "%s", root); /* "I." */
        else
          snprintf(mapped, sizeof(mapped), "%s/%s", root, item + 1); /* "<root>/.x" */
      } else if (strcmp(item, ".") == 0) {
        snprintf(mapped, sizeof(mapped), "%s", root);
      } else if (wsIsRelPath(item)) {
        if ((size_t)snprintf(mapped, sizeof(mapped), "%s/%s", root, item) >= sizeof(mapped))
          return false;
      } else {
        snprintf(mapped, sizeof(mapped), "%s", item); /* flag/abs */
      }
      if (!first && strlen(out) + 2 >= n) return false;
      if (!first) strcat(out, ", ");
      if (strlen(out) + strlen(mapped) + 1 >= n) return false;
      strcat(out, mapped);
      first = false;
    }
    if (!comma) break;
    save = comma + 1;
  }
  return true;
}
/* Normalize workspace header groups into a deterministic, duplicate-free
   include list. Parentheses group tracked roots; they do not change -I
   precedence based on their position in the declaration. */
static int wsHeaderCmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}
/* Grup `(owner, item, ...)` hanya berlaku untuk proyek `owner`. Di dalamnya,
   `I.` = <owner>/include dan item lain = path relatif root workspace. Entri di
   luar kurung berlaku untuk semua proyek (aturan bare di atas). */
/* `I.` di level workspace = include root; di level proyek = <proyek>/include.
   Hanya `I.` yang menjadi -I (headers). Item lain hanya dilacak (headers.internal):
   konsumen harus memanggilnya lewat path relatif, mis. "../../api/api.h".
   Grup `(owner, ...)` hanya berlaku untuk proyek `owner`; di dalamnya `I.` =
   <owner>/include dan item lain = path relatif root workspace. */
bool wsMapHeaders(const char *root, const char *value, bool global, const char *proj,
                         bool split, char *outPub, char *outInt, size_t n) {
  char buf[WS_LINE_LEN];
  if (!value || strlen(value) >= sizeof(buf)) return false;
  copyStr(buf, sizeof(buf), value);
  char *pub[256], *intl[256];
  int npub = 0, nint = 0;
  char *p = buf;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (!*p) break;
    if (*p == '(') {
      char *close = strchr(p, ')');
      if (!close) goto fail;
      *close = '\0';
      char *owner = NULL;
      for (char *q = p + 1;;) {
        char *c = strchr(q, ',');
        if (c) *c = '\0';
        char *t = trim(q);
        if (*t) {
          if (!owner) {
            owner = t;
          } else if (strcmp(owner, proj) == 0) {
            char mapped[WS_LINE_LEN];
            bool isPub = strcmp(t, "I.") == 0;
            if (isPub) {
              if ((size_t)snprintf(mapped, sizeof(mapped), "%s/include", root) >= sizeof(mapped))
                goto fail;
            } else if (wsIsRelPath(t)) {
              if ((size_t)snprintf(mapped, sizeof(mapped), "./%s", t) >= sizeof(mapped)) goto fail;
            } else {
              if (!wsMapList(root, t, true, mapped, sizeof(mapped))) goto fail;
            }
            if (isPub ? npub >= 256 : nint >= 256) goto fail;
            char *dup = malloc(strlen(mapped) + 1);
            if (!dup) goto fail;
            strcpy(dup, mapped);
            if (isPub) pub[npub++] = dup; else intl[nint++] = dup;
          }
        }
        if (!c) break;
        q = c + 1;
      }
      p = close + 1;
    } else {
      char *end = p;
      while (*end && *end != ',' && *end != '(') end++;
      char save = *end;
      *end = '\0';
      char *t = trim(p);
      if (*t) {
        char mapped[WS_LINE_LEN];
        bool isPub = strcmp(t, "I.") == 0 || !split;
        if (strcmp(t, "I.") == 0) {
          /* global: include root workspace. proyek: include milik proyek itu. */
          if (global) snprintf(mapped, sizeof(mapped), "include");
          else if ((size_t)snprintf(mapped, sizeof(mapped), "%s/include", root) >= sizeof(mapped))
            goto fail;
        } else if (global && wsIsRelPath(t)) {
          char candidate[WS_LINE_LEN];
          if (snprintf(candidate, sizeof(candidate), "%s/include", t) >= (int)sizeof(candidate))
            goto fail;
          if (fsDirExists(candidate)) copyStr(mapped, sizeof(mapped), candidate);
          else if (!wsMapList(".", t, true, mapped, sizeof(mapped))) goto fail;
        } else if (!wsMapList(root, t, true, mapped, sizeof(mapped))) {
          goto fail;
        }
        if (isPub ? npub >= 256 : nint >= 256) goto fail;
        char *dup = malloc(strlen(mapped) + 1);
        if (!dup) goto fail;
        strcpy(dup, mapped);
        if (isPub) pub[npub++] = dup; else intl[nint++] = dup;
      }
      *end = save;
      p = end;
      if (save == '\0') break;
    }
  }
  /* gabungkan ke string, tanpa duplikat, dalam urutan deterministik */
  char **lists[2] = {pub, intl};
  int counts[2] = {npub, nint};
  char *outs[2] = {outPub, outInt};
  for (int li = 0; li < 2; li++) {
    qsort(lists[li], (size_t)counts[li], sizeof(char *), wsHeaderCmp);
    outs[li][0] = '\0';
    for (int i = 0; i < counts[li]; i++) {
      if (i && strcmp(lists[li][i], lists[li][i - 1]) == 0) continue;
      size_t have = strlen(outs[li]), add = strlen(lists[li][i]);
      if (have + add + (have ? 2 : 0) + 1 > n) goto fail;
      if (have) strcat(outs[li], ", ");
      strcat(outs[li], lists[li][i]);
    }
  }
  for (int i = 0; i < npub; i++) free(pub[i]);
  for (int i = 0; i < nint; i++) free(intl[i]);
  return true;
fail:
  for (int i = 0; i < npub; i++) free(pub[i]);
  for (int i = 0; i < nint; i++) free(intl[i]);
  return false;
}
/* cdeps -> `library = <dep>/<libDir>/lib<name>.a` + `headers = <dep>/<dir>`
   (keduanya relatif root workspace; tanpa ../ — tidak ada chdir). */
void wsApplyCdeps(const WsModel *m, const WsProject *p, Config *c) {
  List deps = {0};
  bool found = false;
  for (int j = 0; j < p->lines.count; j++) {
    const char *ln = p->lines.items[j];
    if (strncmp(ln, "cdeps = ", 8) != 0) continue;
    found = true;
    const char *v = ln + 8;
    char buf[WS_LINE_LEN];
    if (strlen(v) >= sizeof(buf)) continue;
    copyStr(buf, sizeof(buf), v);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item) {
        if (strcmp(item, p->name) == 0) {
          fprintf(stderr, "satria: workspace: '%s' cdeps berisi dirinya sendiri\n", p->name);
        } else if (wsFindProject(m, item) < 0) {
          fprintf(stderr, "satria: workspace: '%s' cdeps unknown project '%s'\n", p->name, item);
        } else if (!wsListHas(&deps, item)) {
          listAdd(&deps, item);
        }
      }
      if (!comma) break;
      save = comma + 1;
    }
  }
  if (!found) {
    listFree(&deps);
    return;
  }
  for (int j = 0; j < deps.count; j++) {
    int di = wsFindProject(m, deps.items[j]);
    const WsProject *dep = &m->projects[di];
    char mapped[WS_LINE_LEN];
    if (wsLibraryValue(m, p, dep->name, mapped, sizeof(mapped)))
      configApplyLine(c, "library", mapped);
    List depHeaders = {0};
    wsProjectHeaderDirs(dep, &depHeaders);
    if (depHeaders.count > 0) {
      char joined[WS_LINE_LEN * 4];
      size_t off = 0;
      off += (size_t)snprintf(joined + off, sizeof(joined) - off, "%s", depHeaders.items[0]);
      for (int k = 1; k < depHeaders.count && off < sizeof(joined) - 2; k++)
        off += (size_t)snprintf(joined + off, sizeof(joined) - off, ", %s", depHeaders.items[k]);
      configApplyLine(c, "headers", joined);
    }
    listFree(&depHeaders);
  }
  listFree(&deps);
}

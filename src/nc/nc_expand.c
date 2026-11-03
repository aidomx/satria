/*
 * nc_expand — ekspansi template rule ninja & klasifikasi argumen compile.
 *
 * Dipindah apa adanya dari ninjaconv.c: ncExpand (substitusi $var/${var}
 * dengan parameter edge lalu variabel top-level), dan ncParseCompile
 * (klasifikasi token command hasil ekspansi jadi src/flags/includes/std).
 */
#include "nc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *ncExpand(const NcFile *f, const char *tpl, const char *in, const char *out,
               const char *flags, const char *incs, const char *defs,
               const char *depfile) {
  size_t cap = strlen(tpl) * 2 + 1024;
  char *res = malloc(cap);
  if (!res) return NULL;
  size_t w = 0;
  res[0] = '\0';

  for (const char *p = tpl; *p;) {
    if (*p == '$' && p[1] == '$') { /* $$ -> literal $ */
      if (w + 1 < cap) res[w++] = '$';
      p += 2;
      continue;
    }
    if (*p == '$') {
      char name[64];
      size_t nlen = 0;
      size_t skip; /* berapa karakter dari p yang dikonsumsi */
      if (p[1] == '{') { /* ${NAME} */
        const char *e = strchr(p + 2, '}');
        if (!e) break;
        nlen = (size_t)(e - (p + 2));
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, p + 2, nlen);
        name[nlen] = '\0';
        skip = 2 + nlen + 1; /* $ { NAME } */
      } else { /* $NAME */
        while (p[1 + nlen] && ((p[1 + nlen] >= 'A' && p[1 + nlen] <= 'Z') ||
                               (p[1 + nlen] >= 'a' && p[1 + nlen] <= 'z') ||
                               (p[1 + nlen] >= '0' && p[1 + nlen] <= '9') ||
                               p[1 + nlen] == '_')) {
          if (nlen < sizeof(name) - 1) name[nlen] = p[1 + nlen];
          nlen++;
        }
        name[nlen < sizeof(name) ? nlen : sizeof(name) - 1] = '\0';
        skip = 1 + nlen;
      }

      static const char *sNames[] = {"in_newline", "DEP_FILE", "INCLUDES",
                                     "DEFINES",     "FLAGS",    "out",
                                     "in",          NULL};
      const char *sVals[7];
      sVals[0] = in;
      sVals[1] = depfile ? depfile : "";
      sVals[2] = incs ? incs : "";
      sVals[3] = defs ? defs : "";
      sVals[4] = flags ? flags : "";
      sVals[5] = out;
      sVals[6] = in;

      const char *v = NULL;
      for (int k = 0; sNames[k]; k++)
        if (strcmp(name, sNames[k]) == 0) {
          v = sVals[k];
          break;
        }
      if (!v) v = ncVarGet(f, name);
      if (!v) v = "";

      size_t vl = strlen(v);
      if (vl >= cap - w) {
        free(res); /* jangan hasilkan command yang diam-diam terpotong */
        return NULL;
      }
      memcpy(res + w, v, vl);
      w += vl;
      p += skip;
      continue;
    }
    if (w + 1 >= cap) {
      free(res);
      return NULL;
    }
    res[w++] = *p++;
  }
  res[w] = '\0';
  return res;
}

/* ==================== klasifikasi argumen compile ==================== */

static void ncConsumeArg(NcCompile *cc, const char *a) {
  if (strncmp(a, "-I", 2) == 0 && a[2]) {
    /* tandai bahwa dir ini sudah relatif cwd — ncParseCompile tidak
       menjalankan ncFixPath kedua kali untuk include */
    char marked[SATRIA_MAX_PATH * 2];
    snprintf(marked, sizeof(marked), "\x01%s", a + 2);
    ncAdd(&cc->includes, marked);
    return;
  }
  if (strncmp(a, "-std=", 5) == 0) {
    if (!cc->std[0]) snprintf(cc->std, sizeof(cc->std), "%s", a + 5);
    return;
  }
  if (strncmp(a, "-l", 2) == 0 && a[2] && a[2] != '-') return; /* di link, bukan compile */
  if (strncmp(a, "-D", 2) == 0 || strncmp(a, "-O", 2) == 0 || strncmp(a, "-W", 2) == 0 ||
      strncmp(a, "-f", 2) == 0 || strcmp(a, "-g") == 0 || strcmp(a, "-pipe") == 0) {
    if (strncmp(a, "-Wno-", 5) != 0) ncAdd(&cc->flags, a);
    return;
  }
}

bool ncParseCompile(char *cmd, NcCompile *cc, const char *ninjaDir) {
  memset(cc, 0, sizeof(*cc));
  char *cur = cmd;
  char *tok;
  bool haveSrc = false, haveOut = false, isCompile = false;
  while ((tok = ncNextTok(&cur)) != NULL) {
    if (strcmp(tok, "-c") == 0) {
      isCompile = true;
    } else if (strcmp(tok, "-o") == 0) {
      char *next = ncNextTok(&cur);
      if (next) {
        ncFixPath(next, ninjaDir, cc->obj, sizeof(cc->obj));
        haveOut = true;
      }
      free(next);
    } else if (strncmp(tok, "-MF", 3) == 0 || strncmp(tok, "-MT", 3) == 0 ||
               strncmp(tok, "-MQ", 3) == 0) {
      /* dep-file flag yang membawa argumen (terpisah atau digabung); argumen
         hanya bila token berikutnya ada & bukan flag (DEP_FILE bisa kosong) */
      if (tok[3] == '\0' && *cur && *cur != '-') {
        char *next = ncNextTok(&cur);
        free(next);
      }
    } else if (tok[0] == '-' && tok[1] == 'M') {
      /* -MD/-MMD/-MP: dep-file flag tanpa argumen — abaikan */
    } else if (tok[0] == '-' && tok[1] != ' ') {
      char fixed[SATRIA_MAX_PATH * 2];
      ncFixIncludeTok(tok, ninjaDir, false, fixed, sizeof(fixed));
      ncConsumeArg(cc, fixed);
    } else if (tok[0] != '$') {
      /* path polos: kandidat source (.c) — posisi apapun, -c cukup ada di
         suatu tempat pada command */
      size_t tl = strlen(tok);
      if (tl > 2 && strcmp(tok + tl - 2, ".c") == 0) {
        ncFixPath(tok, ninjaDir, cc->src, sizeof(cc->src));
        haveSrc = true;
      }
    }
    free(tok);
  }

  /* -c biasanya muncul sebelum -o di template cmake; urutan bebas: terima
     bila keduanya ada dan obj berakhiran .o */
  if (!isCompile || !haveSrc || !haveOut) return false;
  size_t ol = strlen(cc->obj);
  if (ol < 2 || strcmp(cc->obj + ol - 2, ".o") != 0) return false;
  return true;
}

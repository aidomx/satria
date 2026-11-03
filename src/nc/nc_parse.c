/*
 * nc_parse — pembaca build.ninja untuk konverter satria.
 *
 * Dipindah apa adanya dari ninjaconv.c: path helper (normalisasi,
 * relativisasi terhadap cwd, fix token -I), pembaca baris logis
 * (continuation '$'+EOL dibuang, komentar '#' dipotong), dan parser
 * rule/edge/variabel (ncParseInto).
 */
#include "nc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../portability.h"

/* Rapikan "a/./b" -> "a/b" (versi ringan; deps.c punya versi lengkapnya). */
static void ncNormalize(char *p) {
  char first = p[0];
  char *w = p, *r = p;
  while (*r) {
    if (r[0] == '/' && r[1] == '/') {
      r++;
      continue;
    }
    if (r[0] == '/' && r[1] == '.' && (r[2] == '/' || r[2] == '\0')) {
      r += 2; /* buang "/." utuh — dulu kasus akhir-path menyisakan "." */
      continue;
    }
    *w++ = *r++;
  }
  *w = '\0';
  /* Path yang habis ternormalisasi (mis. "/.") tetap akar, jangan kosong. */
  if (w == p && first) {
    p[0] = '/';
    p[1] = '\0';
  }
}

/* Bila path absolut di bawah cwd, buang prefix cwd (jadikan relatif).
   Path absolut di luar cwd dibiarkan apa adanya. */
static void ncRelativize(char *path, size_t cap) {
  char cwd[SATRIA_MAX_PATH * 2];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  size_t cl = strlen(cwd);
  if (strncmp(path, cwd, cl) != 0) return;
  if (path[cl] == '\0') { /* persis cwd (mis. -I. dengan ninjaDir absolut) */
    path[0] = '.';
    path[1] = '\0';
    return;
  }
  if (path[cl] != '/') return;
  memmove(path, path + cl + 1, strlen(path + cl + 1) + 1);
  (void)cap;
}

void ncFixPath(const char *raw, const char *ninjaDir, char *out, size_t cap) {
  if (!raw || !*raw) {
    if (cap) out[0] = '\0';
    return;
  }
  char joined[SATRIA_MAX_PATH * 2];
  if (raw[0] == '/') {
    /* absolut: relatif-kan terhadap cwd bila berada di bawahnya */
    snprintf(joined, sizeof(joined), "%s", raw);
    ncNormalize(joined);
    ncRelativize(joined, sizeof(joined));
  } else if (raw[0] == '\'') {
    snprintf(out, cap, "%s", raw); /* bukan path — biarkan */
    return;
  } else {
    /* relatif terhadap dir build.ninja (cmake_ninja_workdir dsb.);
       jalur cross-dir (nbuild/...) dibuat absolut-itu-relevan dulu */
    if (strcmp(ninjaDir, ".") == 0) {
      snprintf(joined, sizeof(joined), "%s", raw);
    } else {
      snprintf(joined, sizeof(joined), "%s/%s", ninjaDir, raw);
      ncNormalize(joined);
      ncRelativize(joined, sizeof(joined));
    }
  }
  snprintf(out, cap, "%s", joined);
}

void ncFixIncludeTok(const char *tok, const char *ninjaDir, bool fixedIn, char *out,
                     size_t cap) {
  if (strncmp(tok, "-I", 2) == 0 && tok[2]) {
    if (fixedIn) {
      snprintf(out, cap, "%s", tok);
      return;
    }
    char dir[SATRIA_MAX_PATH * 2];
    ncFixPath(tok + 2, ninjaDir, dir, sizeof(dir));
    size_t len = strlen(dir);
    if (len > cap || cap - len < 3) {
      if (cap) out[0] = '\0';
      return;
    }
    out[0] = '-';
    out[1] = 'I';
    memcpy(out + 2, dir, len + 1);
    return;
  }
  snprintf(out, cap, "%s", tok);
}

/* Baca seluruh file, gabungkan continuation ninja ('$', EOL) -> baris logis
   dipisah '\n'. Return malloc; pemanggil yang free. */
static char *ncReadLogical(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz < 0 || sz > 32 * 1024 * 1024) {
    fclose(fp);
    return NULL;
  }
  char *raw = malloc((size_t)sz + 1);
  if (!raw) {
    fclose(fp);
    return NULL;
  }
  size_t got = fread(raw, 1, (size_t)sz, fp);
  fclose(fp);
  raw[got] = '\0';

  char *out = malloc(got + 2);
  if (!out) {
    free(raw);
    return NULL;
  }
  size_t w = 0;
  for (size_t i = 0; i < got; i++) {
    /* buang '#'-comment sampai EOL (ninja: # di awal token) */
    if (raw[i] == '#') {
      while (i < got && raw[i] != '\n')
        i++;
      if (i >= got) break;
    }
    if (raw[i] == '$' && i + 1 < got && raw[i + 1] == '\n') {
      i++; /* continuation: buang '$' + newline */
      continue;
    }
    if (raw[i] == '\r') continue;
    out[w++] = raw[i];
  }
  out[w] = '\0';
  free(raw);
  return out;
}

bool ncParseInto(NcFile *f, const char *path, const char *dirHint, int depth) {
  char *text = ncReadLogical(path);
  if (!text) return false;

  /* direktori file ini untuk resolve include & path relatif */
  char myDir[SATRIA_MAX_PATH];
  snprintf(myDir, sizeof(myDir), "%s", path);
  char *sl = strrchr(myDir, '/');
  if (sl)
    *sl = '\0';
  else
    snprintf(myDir, sizeof(myDir), "%s", dirHint && *dirHint ? dirHint : ".");

  char *save = NULL;
  NcRule *curRule = NULL;
  NcEdge *curEdge = NULL;

  for (char *line = ncTok(text, "\n", &save); line; line = ncTok(NULL, "\n", &save)) {
    if (line[0] == ' ' || line[0] == '\t') {
      /* variabel milik rule/edge terakhir */
      char *q = line;
      while (*q == ' ' || *q == '\t')
        q++;
      char *eq = strchr(q, '=');
      if (!eq) continue;
      *eq = '\0';
      char *key = q;
      char *val = eq + 1;
      while (*key == ' ' || *key == '\t')
        key++;
      char *ke = key + strlen(key);
      while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t'))
        ke--;
      *ke = '\0';
      while (*val == ' ' || *val == '\t')
        val++;

      if (curEdge) {
        char **dst = NULL;
        if (strcmp(key, "FLAGS") == 0)
          dst = &curEdge->flags;
        else if (strcmp(key, "INCLUDES") == 0)
          dst = &curEdge->incs;
        else if (strcmp(key, "DEFINES") == 0)
          dst = &curEdge->defs;
        else if (strcmp(key, "DEP_FILE") == 0)
          dst = &curEdge->depfile;
        if (dst) {
          free(*dst);
          *dst = ncStrndup(val, val + strlen(val));
        }
      } else if (curRule && strcmp(key, "command") == 0) {
        free(curRule->command);
        curRule->command = ncStrndup(val, val + strlen(val));
      }
      continue;
    }

    curEdge = NULL;
    curRule = NULL;

    if (strncmp(line, "include ", 8) == 0 && depth < 4) {
      char incPath[SATRIA_MAX_PATH * 2];
      ncFixPath(line + 8, myDir, incPath, sizeof(incPath));
      /* include dibaca relatif cwd hasil ncFixPath; bila file itu relatif
         dan tidak ada, coba lagi relatif direktori file induk */
      FILE *probe = fopen(incPath, "rb");
      if (!probe && line[8] != '/') {
        snprintf(incPath, sizeof(incPath), "%s/%s", myDir, line + 8);
        probe = fopen(incPath, "rb");
      }
      if (probe) fclose(probe);
      ncParseInto(f, incPath, myDir, depth + 1);
      continue;
    }
    if (strncmp(line, "rule ", 5) == 0 && f->nrules < NC_MAX_RULES) {
      NcRule *r = &f->rules[f->nrules++];
      r->name = ncStrndup(line + 5, line + strlen(line));
      r->command = NULL;
      curRule = r;
      continue;
    }
    if (strncmp(line, "build ", 6) == 0) {
      if (f->nedges == f->edgeCap) {
        int ncap = f->edgeCap ? f->edgeCap * 2 : 128;
        void *nn = realloc(f->edges, (size_t)ncap * sizeof(NcEdge));
        if (!nn) break;
        f->edges = nn;
        f->edgeCap = ncap;
      }
      /* "build OUT(|OUT2)*: RULE IN... || extra" */
      char *colon = strchr(line + 6, ':');
      if (!colon) continue;
      *colon = '\0';
      char *ruleAndIns = colon + 1;
      while (*ruleAndIns == ' ' || *ruleAndIns == '\t')
        ruleAndIns++; /* spasi setelah ':' — tanpa ini nama rule kosong */
      char *sp = strchr(ruleAndIns, ' ');
      char *ruleName = ruleAndIns;
      char *ins = NULL;
      if (sp) {
        *sp = '\0';
        ins = sp + 1;
      }
      /* buang pipe segmen dari input */
      if (ins) {
        char *pipe = strchr(ins, '|');
        if (pipe) *pipe = '\0';
      }
      char *outTok = line + 6;
      char *pipeOut = strchr(outTok, '|');
      if (pipeOut) *pipeOut = '\0';
      /* out bisa multi token: ambil token pertama */
      char *save2 = NULL;
      char *firstOut = ncTok(outTok, " \t", &save2);

      NcEdge *e = &f->edges[f->nedges++];
      memset(e, 0, sizeof(*e));
      e->out = firstOut ? ncStrndup(firstOut, firstOut + strlen(firstOut)) : NULL;
      e->rule = ncStrndup(ruleName, ruleName + strlen(ruleName));
      while (ins && (*ins == ' ' || *ins == '\t'))
        ins++;
      if (ins && *ins) {
        e->ins = ncStrndup(ins, ins + strlen(ins)); /* full list utk cek link */
        char *save3 = NULL;
        char *firstIn = ncTok(ins, " \t", &save3);
        e->in = firstIn ? ncStrndup(firstIn, firstIn + strlen(firstIn)) : NULL;
      }
      curEdge = e;
      continue;
    }

    /* variabel top-level: "name = value" (bukan rule/build/include) */
    if (!strncmp(line, "rule ", 5) || !strncmp(line, "build ", 6)) continue;
    char *eq = strchr(line, '=');
    if (eq && eq != line) {
      *eq = '\0';
      char *key = line;
      char *val = eq + 1;
      while (*key == ' ' || *key == '\t')
        key++;
      char *ke = key + strlen(key);
      while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t'))
        ke--;
      *ke = '\0';
      while (*val == ' ' || *val == '\t')
        val++;
      if (*key && *val) ncVarSet(f, key, val);
    }
    /* baris lain (default, pool, dsb.) diabaikan */
  }
  free(text);
  return true;
}

#include "use.h"

#include <string.h>

/*
 * use — deteksi baris `use` (implementasi). Sengaja satu titik kecil:
 * menambah varian `use` baru berarti menambah satu strcmp di sini dan satu
 * cabang di parser, tidak lebih.
 */

/* Argumen mentah setelah `use project` / `use alias` (boleh kosong). NULL bila
   baris bukan `use project`/`use alias`. Dipakai pemanggil untuk memperingatkan
   bahasa tak dikenal yang seharusnya tidak dibuang diam-diam. */
const char *useLineLangArg(const char *text) {
  if (!text) return NULL;
  const char *p;
  if (strncmp(text, "use project", 11) == 0)
    p = text + 11;
  else if (strncmp(text, "use alias", 9) == 0)
    p = text + 9;
  else
    return NULL;
  /* Pemisah wajib spasi/tab; "use projectx" bukan baris use. */
  if (*p != ' ' && *p != '\t') return NULL;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

/* Bahasa setelah `use project` / `use alias`: "c" | "cpp" | "c++".
   Mengembalikan "c", "cpp", atau "" bila tidak dideklarasikan / tak dikenal. */
const char *useLineLang(const char *text) {
  const char *p = useLineLangArg(text);
  if (!p) return "";
  size_t len = strlen(p);
  while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
  /* Perbandingan case-insensitive tanpa strncasecmp (tidak ada di MSVC). */
  char buf[8];
  if (len >= sizeof(buf)) return "";
  for (size_t i = 0; i < len; i++) {
    char ch = p[i];
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    buf[i] = ch;
  }
  buf[len] = '\0';
  if (strcmp(buf, "c") == 0) return "c";
  if (strcmp(buf, "cpp") == 0 || strcmp(buf, "c++") == 0) return "cpp";
  return "";
}

UseMode useLineMode(const char *text) {
  if (!text) return USE_NONE;
  if (strcmp(text, "use project") == 0 || strcmp(text, "use alias") == 0)
    return USE_PROJECT;
  if (useLineLang(text)[0]) return USE_PROJECT; /* use project c|cpp|c++ */
  if (strcmp(text, "use workspace") == 0) return USE_WORKSPACE;
  return USE_NONE;
}

bool useLineKnown(const char *text) { return useLineMode(text) != USE_NONE; }

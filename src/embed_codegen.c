/*
 * embed_codegen — codegen embedded.c/.h (array byte + tabel getEmbed).
 * Satu-satunya mekanisme embed (design/embedded.md §Regenerasi).
 * Dipisah dari embed.c (audit refactor: berkas kandidat >600 baris).
 */
#include "embed.h"
#include "cmds/cmds_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache/cache_store.h"
#include "compile.h"
#include "portability.h"
#include "util.h"
static void embedSymBase(const char *archivePath, char *out, size_t n) {
  snprintf(out, n, "_binary_");
  size_t k = strlen(out);
  for (const char *p = archivePath; *p && k < n - 1; p++, k++) {
    char ch = *p;
    bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    out[k] = alnum ? ch : '_';
  }
  out[k] = '\0';
}

/* ==================== codegen (design/embedded.md) ====================
   Satu-satunya mekanisme embed: arsip dibaca menjadi array byte di
   <outBuildDir>/embedded.c (dikompilasi sebagai source biasa) dan
   <outBuildDir>/embedded.h (struct Embedded + API getEmbed + macro kompat
   EMBED_<VAR>_*). Keduanya diregenerasi berbasis KONTEN: tulis .tmp,
   bandingkan dengan file lama, identik -> buang temp (mtime tetap),
   beda -> rename atomik — touch tanpa perubahan isi tidak pernah memicu
   compile ulang (touch-proof; design/embedded.md §Regenerasi). */

/* Tulis string sebagai literal C di file hasil generate (escape
   quote/backslash/karakter kontrol). */
static void embCStr(FILE *out, const char *s) {
  fputc('"', out);
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    if (*p == '"' || *p == '\\')
      fprintf(out, "\\%c", *p);
    else if (*p == '\n')
      fputs("\\n", out);
    else if (*p == '\t')
      fputs("\\t", out);
    else if (*p < 0x20 || *p == 0x7f)
      fprintf(out, "\\%03o", *p);
    else
      fputc(*p, out);
  }
  fputc('"', out);
}

/* Bandingkan dua file byte-per-byte (streaming 4KB); false bila salah satu
   tidak terbaca — dianggap BERBEDA supaya file digenerate ditulis ulang. */
static bool embFilesIdentical(const char *a, const char *b) {
  FILE *fa = fopen(a, "rb");
  if (!fa) return false;
  FILE *fb = fopen(b, "rb");
  if (!fb) {
    fclose(fa);
    return false;
  }
  bool same = true;
  unsigned char ba[4096], bb[4096];
  for (;;) {
    size_t na = fread(ba, 1, sizeof(ba), fa);
    size_t nb = fread(bb, 1, sizeof(bb), fb);
    if (na != nb || (na > 0 && memcmp(ba, bb, na) != 0)) {
      same = false;
      break;
    }
    if (na == 0) break;
  }
  fclose(fa);
  fclose(fb);
  return same;
}

/*
 * emitEmbeddedSource — tulis <outBuildDir>/embedded.c: array byte per entri
 * (dengan simbol kompat EMBED_<VAR>_SYMBOL yang lama), tabel `Embedded`, dan
 * definisi getEmbed()/getEmbedAt(). Arsip yang tidak terbaca = kegagalan
 * jelas (tanpa swallow). Regenerasi content-based: tulis <path>.tmp,
 * bandingkan dengan file lama; identik -> buang temp (mtime tetap), beda ->
 * rename atomik (fsRenameReplace).
 */
/* Tulis byte arsip sebagai `0x%02x,` (16 per baris) lewat buffer besar.
   Keluaran identik dengan loop fprintf per byte, jauh lebih cepat untuk arsip
   besar (fprintf per byte mendominasi biaya codegen). */
static void embWriteHexBytes(FILE *out, FILE *in, long long *total) {
  static const char hx[] = "0123456789abcdef";
  unsigned char in_buf[65536];
  char ob[1 << 16];
  size_t op = 0, r;
  int perLine = 0;
  while ((r = fread(in_buf, 1, sizeof(in_buf), in)) > 0) {
    for (size_t k = 0; k < r; k++) {
      if (op + 8 > sizeof(ob)) {
        fwrite(ob, 1, op, out);
        op = 0;
      }
      if (perLine == 0) ob[op++] = '\n';
      ob[op++] = '0';
      ob[op++] = 'x';
      ob[op++] = hx[in_buf[k] >> 4];
      ob[op++] = hx[in_buf[k] & 15];
      ob[op++] = ',';
      if (++perLine == 16) perLine = 0;
      (*total)++;
    }
  }
  fwrite(ob, 1, op, out);
}

/* Baris stamp satu entri: identitas + metadata + ukuran/mtime arsip. Dipakai
   untuk mendeteksi bahwa codegen tidak perlu diulang (lihat embStampSkip). */
static bool embStampLine(const EmbeddedEntry *e, char *line, size_t cap) {
  long long sz = fsFileSize(e->archivePath);
  int64_t mt = fsMTimeNs(e->archivePath);
  if (sz < 0 || mt < 0) return false;
  char sym[512];
  embedSymBase(e->archivePath, sym, sizeof(sym));
  const char *slash = strrchr(e->archivePath, '/');
  const char *name = slash ? slash + 1 : e->archivePath;
  const char *key = e->key[0] ? e->key : e->name;
  const char *type = e->type[0] ? e->type
                     : ((e->usePrebuilt || e->src[0]) ? "archive" : "asset");
  snprintf(line, cap, "e\t%s\t%lld\t%lld\t%s\t%s\t%s\t%s\t%s\n", e->archivePath, sz,
           (long long)mt, key, type, name, e->extract, sym);
  return true;
}

/* ---- Mode codegen ----
   "incbin": payload dimasukkan lewat assembler `.incbin` di blok __asm__ (GCC
   dan Clang). Biaya kompilasi nyaris nol dibanding array C (fprintf/teks C
   300 MB mendominasi); object tetap berisi byte payload yang sama.
   "carray": array uint8_t di C (MSVC, dan fallback bila diminta).
   Default: incbin untuk GCC/Clang, carray untuk MSVC. Override lewat env
   SATRIA_EMBED_MODE=carray|incbin (untuk pengujian/fallback). */
static bool embAbsPath(const char *rel, char *out, size_t cap) {
  char tmp[MAX_PATH * 2];
  size_t k = 0;
  for (const char *p = rel; *p && k < sizeof(tmp) - 1; p++) tmp[k++] = (*p == '\\') ? '/' : *p;
  tmp[k] = '\0';
  bool abs = tmp[0] == '/' || (tmp[0] && tmp[1] == ':');
  if (abs) {
    if (strlen(tmp) >= cap) return false;
    memcpy(out, tmp, strlen(tmp) + 1);
  } else {
    char cwd[MAX_PATH];
    if (!fsGetCwd(cwd, sizeof(cwd))) return false;
    const char *r = tmp;
    while (r[0] == '.' && r[1] == '/') r += 2;
    if (strlen(cwd) + 1 + strlen(r) >= cap) return false; /* tidak muat: fallback carray */
    memcpy(out, cwd, strlen(cwd));
    out[strlen(cwd)] = '/';
    memcpy(out + strlen(cwd) + 1, r, strlen(r) + 1);
  }
  for (const char *p = out; *p; p++)
    if (*p == '"' || *p == '\n' || *p == '\r') return false; /* tak bisa di string asm */
  return true;
}

static bool embUseIncbin(const Config *c) {
  const char *m = getenv("SATRIA_EMBED_MODE");
  if (compilerIsMSVC(c)) return false; /* MSVC tidak punya .incbin */
  if (m && strcmp(m, "carray") == 0) return false;
  for (int i = 0; i < c->embCount; i++) {
    const EmbeddedEntry *e = &c->emb[i];
    char abs[MAX_PATH * 2];
    if (e->enable && !embAbsPath(e->archivePath, abs, sizeof(abs))) return false;
  }
  return true;
}

/* Prelude generated file untuk mode incbin: makro label/section/lebar panjang. */
static const char *embIncbinPrelude =
    "#define SATRIA_STR2(x) #x\n"
    "#define SATRIA_STR(x) SATRIA_STR2(x)\n"
    "#define SATRIA_PFX SATRIA_STR(__USER_LABEL_PREFIX__)\n"
    "#if defined(__APPLE__)\n"
    "#define SATRIA_SECT \".section __TEXT,__const\\n\"\n"
    "#else\n"
    "#define SATRIA_SECT \".section .rodata\\n\"\n"
    "#endif\n"
    "#if __SIZEOF_LONG__ == 8\n"
    "#define SATRIA_LONG_DIR \".quad \"\n"
    "#else\n"
    "#define SATRIA_LONG_DIR \".long \"\n"
    "#endif\n\n";

/* Satu blok __asm__ untuk satu arsip: start/end/len dengan nama simbol SAMA
   seperti mode carray (header tetap valid). END = satu byte nol setelah payload
   (semantik kompat); LEN = selisih label di assembler, bukan konstanta C. */
/* FNV-1a 64 isi file (streaming). Dipakai di komentar embedded.c agar regenerasi
   berbasis KONTEN: touch/pemulihan arsip dengan isi sama tidak mengubah file. */
static bool embFileFnv64(const char *path, unsigned long long *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  unsigned long long h = 1469598103934665603ULL;
  unsigned char buf[65536];
  size_t r;
  while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
    for (size_t k = 0; k < r; k++) {
      h ^= buf[k];
      h *= 1099511628211ULL;
    }
  bool ok = !ferror(f);
  fclose(f);
  *out = h;
  return ok;
}

static void embWriteIncbinEntry(FILE *out, const char *sym, const char *absPath,
                                const char *relPath, long long sz, unsigned long long hash) {
  fprintf(out, "/* arsip %s size=%lld fnv1a64=%016llx: object dikompilasi ulang bila isi berubah */\n",
          relPath, sz, hash);
  fprintf(out, "__asm__(\n");
  fprintf(out, "  SATRIA_SECT\n");
  fprintf(out, "  \".balign 16\\n\"\n");
  fprintf(out, "  \".globl \" SATRIA_PFX \"%s_start\\n\"\n", sym);
  fprintf(out, "  SATRIA_PFX \"%s_start:\\n\"\n", sym);
  fprintf(out, "  \".incbin \\\"%s\\\"\\n\"\n", absPath);
  fprintf(out, "  SATRIA_PFX \"%s_pend:\\n\"\n", sym);
  fprintf(out, "  \".globl \" SATRIA_PFX \"%s_end\\n\"\n", sym);
  fprintf(out, "  SATRIA_PFX \"%s_end:\\n\"\n", sym);
  fprintf(out, "  \".byte 0\\n\"\n");
  fprintf(out, "  \".balign 8\\n\"\n");
  fprintf(out, "  \".globl \" SATRIA_PFX \"%s_len\\n\"\n", sym);
  fprintf(out, "  SATRIA_PFX \"%s_len:\\n\"\n", sym);
  fprintf(out, "  SATRIA_LONG_DIR SATRIA_PFX \"%s_pend - \" SATRIA_PFX \"%s_start\\n\"\n", sym, sym);
  fprintf(out, "  \".previous\\n\");\n\n");
}

/* Tulis stamp: versi, waktu mulai codegen (ns), lalu baris per entri. */
static bool embWriteStamp(const char *stampPath, int64_t genNs, const char *mode, const char *sig) {
  char tmpS[MAX_PATH + 48];
  snprintf(tmpS, sizeof(tmpS), "%s.tmp", stampPath);
  FILE *f = fopen(tmpS, "wb");
  if (!f) return false;
  fprintf(f, "satria-embed-stamp 2\ngen %lld\nmode %s\n", (long long)genNs, mode);
  fputs(sig, f);
  fclose(f);
  return fsRenameReplace(tmpS, stampPath);
}

/* Lewati codegen bila stamp cocok: embedded.c ada, metadata & ukuran/mtime
   arsip sama, dan setiap arsip ber-mtime LEBIH AWAL dari waktu mulai codegen
   sebelumnya (racy: arsip yang berubah dalam waktu yang sama dengan codegen
   tidak boleh dipercaya). */
static bool embStampSkip(const Config *c, const char *path, const char *stampPath, const char *mode) {
  if (!fsFileExists(path)) return false;
  FILE *sf = fopen(stampPath, "rb");
  if (!sf) return false;
  char head[64];
  long long gen = -1;
  char modeBuf[16] = "";
  if (!fgets(head, sizeof(head), sf) || strcmp(head, "satria-embed-stamp 2\n") != 0 ||
      fscanf(sf, "gen %lld\n", &gen) != 1 || fscanf(sf, "mode %15s\n", modeBuf) != 1 ||
      strcmp(modeBuf, mode) != 0) {
    fclose(sf);
    return false;
  }
  bool ok = true;
  char line[4096], cur[4096];
  for (int i = 0; i < c->embCount && ok; i++) {
    const EmbeddedEntry *e = &c->emb[i];
    if (!e->enable) continue;
    if (!embStampLine(e, cur, sizeof(cur))) { ok = false; break; }
    if (!fgets(line, sizeof(line), sf) || strcmp(line, cur) != 0) ok = false;
    /* racy: mtime arsip harus sebelum waktu mulai codegen yang di-stamp */
    if (ok && (long long)fsMTimeNs(e->archivePath) >= gen) ok = false;
  }
  if (ok && fgets(line, sizeof(line), sf)) ok = false; /* entri berlebih */
  fclose(sf);
  return ok;
}

bool emitEmbeddedSource(const Config *c, bool *changed) {
  if (changed) *changed = false;
  char path[MAX_PATH + 32], tmp[MAX_PATH + 40], stampPath[MAX_PATH + 40];
  snprintf(path, sizeof(path), "%s/embedded.c", c->outBuildDir);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  snprintf(stampPath, sizeof(stampPath), "%s/embedded.stamp", c->outBuildDir);
  mkparent(path);
  bool incbin = embUseIncbin(c);
  const char *mode = incbin ? "incbin" : "carray";
  if (embStampSkip(c, path, stampPath, mode)) return true; /* arsip & metadata tak berubah */
  FILE *out = fopen(tmp, "wb");
  if (!out) {
    fprintf(stderr, "satria: cannot write %s\n", tmp);
    return false;
  }
  fprintf(out, "/* Auto-generated by satria from Buildfile: embedded. Do not edit. */\n");
  fprintf(out, "/* Codegen: %s + tabel getEmbed() (design/embedded.md). */\n",
          incbin ? "payload via .incbin (GCC/Clang)" : "array byte");
  if (incbin) fputs(embIncbinPrelude, out);
  fprintf(out, "#include \"embedded.h\"\n\n#include <string.h>\n\n");
  fflush(out); /* mtime tmp = waktu mulai codegen (dipakai stamp/racy) */
  int64_t genNs = fsMTimeNs(tmp);
  char sigBuf[1 << 16] = "";
  size_t sigLen = 0;

  /* Metadata entri aktif untuk tabel (diulis setelah semua array). */
  struct {
    char key[EMBED_NAME_LEN];
    char type[32];
    char name[EMBED_NAME_LEN];
    const char *extract;
    size_t size;
    char sym[512];
  } meta[MAX_EMBEDDED];
  int n = 0;

  for (int i = 0; i < c->embCount && n < MAX_EMBEDDED; i++) {
    const EmbeddedEntry *e = &c->emb[i];
    if (!e->enable) continue;
    embedSymBase(e->archivePath, meta[n].sym, sizeof(meta[n].sym));

    /* stat dicatat SEBELUM dipakai: bila arsip berubah sesudahnya, stamp tidak
       cocok di run berikutnya dan codegen diulang (aman). */
    char sline[4096];
    if (embStampLine(e, sline, sizeof(sline)) && sigLen + strlen(sline) < sizeof(sigBuf)) {
      memcpy(sigBuf + sigLen, sline, strlen(sline) + 1);
      sigLen += strlen(sline);
    }

    long long total = 0;
    if (incbin) {
      long long sz = fsFileSize(e->archivePath);
      unsigned long long hash = 0;
      char absP[MAX_PATH * 2];
      if (sz < 0 || !embFileFnv64(e->archivePath, &hash) ||
          !embAbsPath(e->archivePath, absP, sizeof(absP))) {
        fprintf(stderr, "satria: %s: embedded file '%s' tidak terbaca\n", e->name,
                e->archivePath);
        fclose(out);
        remove(tmp);
        return false;
      }
      embWriteIncbinEntry(out, meta[n].sym, absP, e->archivePath, sz, hash);
      total = sz;
    } else {
      FILE *in = fopen(e->archivePath, "rb");
      if (!in) {
        fprintf(stderr, "satria: %s: embedded file '%s' tidak terbaca\n", e->name,
                e->archivePath);
        fclose(out);
        remove(tmp);
        return false;
      }
      fprintf(out, "const unsigned char %s_start[] = {", meta[n].sym);
      embWriteHexBytes(out, in, &total);
      fclose(in);
      fprintf(out, "\n};\n");
      /* Simbol kompat lama: END dummy (semantik one-past-the-end) + LEN. */
      fprintf(out, "const unsigned char %s_end[1] = { 0 };\n", meta[n].sym);
      fprintf(out, "const unsigned long %s_len = %lld;\n\n", meta[n].sym, total);
    }

    snprintf(meta[n].key, sizeof(meta[n].key), "%s", e->key[0] ? e->key : e->name);
    snprintf(meta[n].type, sizeof(meta[n].type), "%s",
             e->type[0] ? e->type
                        : ((e->usePrebuilt || e->src[0]) ? "archive" : "asset"));
    const char *slash = strrchr(e->archivePath, '/');
    snprintf(meta[n].name, sizeof(meta[n].name), "%s", slash ? slash + 1 : e->archivePath);
    meta[n].extract = e->extract;
    meta[n].size = (size_t)total;
    n++;
  }

  if (n > 0) {
    fprintf(out, incbin ? "static Embedded embed_table[] = {\n"
                        : "static const Embedded embed_table[] = {\n");
    for (int i = 0; i < n; i++) {
      fputs("  { ", out);
      embCStr(out, meta[i].key);
      fputs(", ", out);
      embCStr(out, meta[i].type);
      fputs(", ", out);
      embCStr(out, meta[i].name);
      fputs(", ", out);
      embCStr(out, meta[i].extract);
      fprintf(out, ", %llu, (const uint8_t *)%s_start },\n", (unsigned long long)meta[i].size,
              meta[i].sym);
    }
    fprintf(out, "};\n\n");
    if (incbin) {
      /* Ukuran dari simbol _len (dihitung assembler): tak ada ketidakcocokan
         dengan stat codegen bila arsip berubah di antara keduanya. */
      fprintf(out, "#if defined(__GNUC__)\n"
                   "__attribute__((constructor)) static void embSetSizes(void) {\n");
      for (int i = 0; i < n; i++)
        fprintf(out, "  embed_table[%d].size = (size_t)%s_len;\n", i, meta[i].sym);
      fprintf(out, "}\n#endif\n\n");
    }
    fprintf(out,
            "const Embedded *getEmbed(const char *key) {\n"
            "  size_t i;\n"
            "  if (!key) return NULL;\n"
            "  for (i = 0; i < sizeof(embed_table) / sizeof(embed_table[0]); i++)\n"
            "    if (strcmp(embed_table[i].key, key) == 0) return &embed_table[i];\n"
            "  return NULL;\n"
            "}\n\n"
            "const Embedded *getEmbedAt(size_t index) {\n"
            "  if (index >= sizeof(embed_table) / sizeof(embed_table[0])) return NULL;\n"
            "  return &embed_table[index];\n"
            "}\n");
  } else {
    /* Tanpa entri aktif: API tetap ada dan selalu NULL. */
    fprintf(out,
            "const Embedded *getEmbed(const char *key) { (void)key; return NULL; }\n"
            "const Embedded *getEmbedAt(size_t index) { (void)index; return NULL; }\n");
  }
  fclose(out);

  /* Stamp ditulis setelah codegen sukses (identik maupun baru). Gagal menulis
     stamp tidak fatal: run berikutnya hanya kehilangan skip. */
  bool stampOk = true;
  if (embFilesIdentical(tmp, path)) {
    remove(tmp); /* identik: buang temp, mtime file lama tidak berubah */
    stampOk = embWriteStamp(stampPath, genNs, mode, sigBuf);
    (void)stampOk;
    return true;
  }
  if (!fsRenameReplace(tmp, path)) {
    remove(tmp);
    fprintf(stderr, "satria: cannot write %s\n", path);
    return false;
  }
  if (changed) *changed = true;
  stampOk = embWriteStamp(stampPath, genNs, mode, sigBuf);
  (void)stampOk;
  return true;
}

/*
 * Terbitkan build/embedded.h: struct Embedded + API getEmbed (desain baru)
 * PLUS macro kompat lama EMBED_<N>_ARCHIVE_NAME/_EXTRACT_DIR/_SYMBOL/
 * _SYMBOL_END/_SYMBOL_LEN dengan deklarasi extern-nya (fase 1 migrasi,
 * design/embedded.md §Migrasi) — simbolnya menunjuk array hasil codegen di
 * embedded.c, jadi konsumen lama tanpa getEmbed tetap terbangun. Konten
 * dibandingkan dulu: file hanya ditulis ulang saat berubah; *changed memberi
 * tahu pemanggil bahwa konsumen perlu dikompilasi ulang.
 */
bool emitEmbeddedHeader(const Config *c, bool *changed) {
  static char body[16384];
  size_t used = 0;

  used += (size_t)snprintf(body + used, sizeof(body) - used,
                           "/* Auto-generated by satria from Buildfile: embedded. Do not edit. */\n"
                           "#ifndef RUPA_EMBEDDED_H\n#define RUPA_EMBEDDED_H\n\n"
                           "#include <stddef.h>\n#include <stdint.h>\n\n"
                           "/* Satu struct datar untuk semua tipe aset (design/embedded.md).\n"
                           "   Akses: getEmbed(\"<key>\") — NULL bila key tidak dikenal. */\n"
                           "typedef struct {\n"
                           "  const char *key;      /* kunci akses */\n"
                           "  const char *type;     /* \"archive\" | \"asset\" | label lain */\n"
                           "  const char *name;     /* nama file sumber */\n"
                           "  const char *extract;  /* tujuan ekstraksi, \"\" bila kosong */\n"
                           "  size_t size;          /* jumlah byte data */\n"
                           "  const uint8_t *data;  /* isi file (array di embedded.c) */\n"
                           "} Embedded;\n\n"
                           "const Embedded *getEmbed(const char *key);\n"
                           "const Embedded *getEmbedAt(size_t index); /* NULL di akhir iterasi */\n\n"
                           "/* --- kompat makro lama EMBED_<VAR>_* (design/embedded.md §Migrasi) --- */\n");

  for (int i = 0; i < c->embCount; i++) {
    const EmbeddedEntry *e = &c->emb[i];
    if (!e->enable) continue;

    const char *slash = strrchr(e->archivePath, '/');
    const char *archiveFileName = slash ? slash + 1 : e->archivePath;

    /* prefix macro dari embedded.<n>.variable bila diset, kalau tidak dari
       nama entri; di-uppercase */
    const char *baseName = e->variable[0] ? e->variable : e->name;
    char prefix[EMBED_NAME_LEN + 8] = {0};
    size_t k = 0;
    for (const char *p = baseName; *p && k < sizeof(prefix) - 1; p++, k++) {
      char ch = *p;
      if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
      bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
      prefix[k] = alnum ? ch : '_';
    }

    char sym[512];
    embedSymBase(e->archivePath, sym, sizeof(sym)); /* sym sudah termasuk _binary_ */

    char line[2048];

    used += (size_t)snprintf(body + used, sizeof(body) - used,
                             "/* ===== embedded entry: %s ===== */\n", e->name);

    snprintf(line, sizeof(line), "#define EMBED_%s_ARCHIVE_NAME \"%s\"\n", prefix, archiveFileName);
    used += (size_t)strlen(line);
    strcat(body, line);

    snprintf(line, sizeof(line), "#define EMBED_%s_EXTRACT_DIR \"%s\"\n", prefix, e->extract);
    used += (size_t)strlen(line);
    strcat(body, line);

    snprintf(line, sizeof(line), "#define EMBED_%s_SYMBOL %s_start\n", prefix, sym);
    used += (size_t)strlen(line);
    strcat(body, line);

    /* Satu jalur codegen (ld -r -b binary sudah dihapus): END adalah array
       dummy one-past-the-end, LEN datang dari simbol _len — semantik sama
       dengan fallback C-array lama. */
    snprintf(line, sizeof(line), "#define EMBED_%s_SYMBOL_END %s_end\n", prefix, sym);
    used += (size_t)strlen(line);
    strcat(body, line);

    snprintf(line, sizeof(line), "#define EMBED_%s_SYMBOL_LEN %s_len\n", prefix, sym);
    used += (size_t)strlen(line);
    strcat(body, line);

    snprintf(line, sizeof(line),
             "extern const unsigned char %s_start[];\n"
             "extern const unsigned char %s_end[];\n"
             "extern const unsigned long %s_len;\n\n",
             sym, sym, sym);
    used += (size_t)strlen(line);
    strcat(body, line);
  }

  used += (size_t)snprintf(body + used, sizeof(body) - used, "#endif /* RUPA_EMBEDDED_H */\n");

  char embeddedHeader[MAX_PATH + 32];
  snprintf(embeddedHeader, sizeof(embeddedHeader), "%s/embedded.h", c->outBuildDir);
  mkparent(embeddedHeader);

  /* lewati penulisan ulang bila tidak berubah agar mtime tetap stabil */
  static char existing[16384];
  FILE *rf = fopen(embeddedHeader, "rb");
  if (rf) {
    size_t n = fread(existing, 1, sizeof(existing) - 1, rf);
    fclose(rf);
    if (n == used && memcmp(existing, body, n) == 0) return true;
  }

  /* Mode BINER: agar isi hasil tulis (teks \n) identik dengan hasil baca
     biner — mode teks membuat \r\n di Windows dan embedded.h selalu
     dianggap berubah (recompile penuh tiap run). */
  FILE *hf = fopen(embeddedHeader, "wb");
  if (!hf) {
    fprintf(stderr, "satria: cannot write %s\n", embeddedHeader);
    return false;
  }
  fwrite(body, 1, used, hf);
  fclose(hf);
  if (changed) *changed = true;
  return true;
}


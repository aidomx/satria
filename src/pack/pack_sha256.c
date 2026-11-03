/*
 * pack_sha256 — SHA-256 murni C (FIPS 180-4) + penulis file checksum.
 *
 * Diimplementasikan sendiri agar checksum jalan di semua platform satria
 * (Windows/MSVC tidak punya sha256sum). Format file mengikuti coreutils:
 *   "<hex>  <basename>\n"   (dua spasi = mode biner, valid `sha256sum -c`)
 */
#include <stdio.h>
#include <string.h>

#include "../portability.h"
#include "pack_internal.h"

typedef struct {
  unsigned int h[8];
  unsigned long long len; /* total byte yang sudah di-update */
  unsigned char buf[64];
  size_t bufLen;
} Sha256;

static const unsigned int K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256Init(Sha256 *s) {
  static const unsigned int H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, H0, sizeof(H0));
  s->len = 0;
  s->bufLen = 0;
}

static void sha256Block(Sha256 *s, const unsigned char *p) {
  unsigned int w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
           ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    unsigned int s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    unsigned int s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  unsigned int a = s->h[0], b = s->h[1], cc = s->h[2], d = s->h[3];
  unsigned int e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    unsigned int S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
    unsigned int ch = (e & f) ^ (~e & g);
    unsigned int t1 = h + S1 + ch + K[i] + w[i];
    unsigned int S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
    unsigned int maj = (a & b) ^ (a & cc) ^ (b & cc);
    unsigned int t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += cc;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
}

static void sha256Update(Sha256 *s, const unsigned char *p, size_t n) {
  s->len += n;
  while (n > 0) {
    size_t take = 64 - s->bufLen;
    if (take > n) take = n;
    memcpy(s->buf + s->bufLen, p, take);
    s->bufLen += take;
    p += take;
    n -= take;
    if (s->bufLen == 64) {
      sha256Block(s, s->buf);
      s->bufLen = 0;
    }
  }
}

static void sha256Final(Sha256 *s, unsigned char out[32]) {
  unsigned long long bits = s->len * 8ULL;
  unsigned char pad = 0x80;
  sha256Update(s, &pad, 1);
  unsigned char zero = 0;
  while (s->bufLen != 56) sha256Update(s, &zero, 1);
  unsigned char lenBytes[8];
  for (int i = 0; i < 8; i++)
    lenBytes[i] = (unsigned char)(bits >> (56 - i * 8));
  sha256Update(s, lenBytes, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (unsigned char)(s->h[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
    out[i * 4 + 3] = (unsigned char)(s->h[i]);
  }
}

bool packWriteSha256(const char *path) {
  FILE *in = fopen(path, "rb");
  if (!in) return false;

  Sha256 s;
  sha256Init(&s);
  unsigned char buf[65536];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
    sha256Update(&s, buf, got);
  bool ok = feof(in) != 0;
  fclose(in);
  if (!ok) return false;

  unsigned char digest[32];
  sha256Final(&s, digest);

  char sumPath[SATRIA_MAX_PATH * 2 + 8];
  snprintf(sumPath, sizeof(sumPath), "%s.sha256", path);
  FILE *out = fopen(sumPath, "w");
  if (!out) return false;

  static const char hex[] = "0123456789abcdef";
  char hexDigest[65];
  for (int i = 0; i < 32; i++) {
    hexDigest[i * 2] = hex[digest[i] >> 4];
    hexDigest[i * 2 + 1] = hex[digest[i] & 0xf];
  }
  hexDigest[64] = '\0';

  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  fprintf(out, "%s  %s\n", hexDigest, base);
  return fclose(out) == 0;
}

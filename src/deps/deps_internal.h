#ifndef SATRIA_V0_1_0_DEPS_INTERNAL_H
#define SATRIA_V0_1_0_DEPS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../deps.h" /* Config, List, opaque DepCache */

/*
 * deps_internal — tipe & helper bersama modul deps/.
 *
 * Dipakai antar-file deps_core.c (pemindai #include & node cache),
 * deps_dotd.c (dependensi dari file .d compiler + cache edges), dan
 * deps_snapshot.c (snapshot hash lintas-run + API publik). BUKAN API
 * publik — publik tetap deps.h. Helper dulunya static di deps.c dan
 * di-prefix dep* supaya unik lintas translation unit.
 */

#define DEPS_MAX_NODES 8192
#define DOTD_MAX_LINE 16384
/* Cache terpusat (design/startup.md §3): semua proyek berbagi SATU file
   per jenis cache (WS_CACHES_DIR di cmds_internal.h); baris dikelompokkan
   per alamat proyek (cache_store). */
#define DEPS_CACHE_FILE ".satria/caches/deps.cache"

typedef struct {
  char *path;    /* path ternormalisasi relatif root */
  int64_t mtime; /* mtime ns; -1 bila tidak terbaca */
  uint64_t hash;
  int *kids; /* index node hasil resolusi #include */
  int nkids;
  unsigned mark; /* generasi kunjungan terakhir (deteksi siklus) */
  bool parsed;
  bool dloaded; /* edges dari file .d compiler: flat & lengkap — header anak
                   tidak perlu dibuka/dipindai saat verifikasi */
  /* Memo semantic-header selama satu proses build. Header yang sama dapat
     dikunjungi dari banyak TU; simpan isi saat ini dan hindari readWhole()
     berulang. Kunci validitas adalah mtime node yang sudah diperiksa. */
  char *semText;
  size_t semTextLen;
  int64_t semTextMtime;
  bool semTextLoaded;
  bool semRecorded;
  int64_t semRecordedMtime;
} DepNode;

struct DepCache {
  DepNode *nodes;
  int count, cap;
  unsigned gen;
  int semRoot; /* TU yang sedang diperiksa (SATRIA_HDR_SEM) */
  const Config *cfg;
  List incDirs;

  /* Hash table path -> index node (open addressing, kapasitas pangkat dua).
     Menggantikan pencarian linear O(n) per resolusi #include (dulu O(n^2)
     untuk ratusan file). Isi bucket = index node + 1; 0 = kosong. */
  int *hbucket;
  int hbcap;

  /* Snapshot hash+mtime lintas-run. Mtime dipakai sebagai shortcut:
     file dengan mtime sama seperti saat direkam TIDAK perlu dibaca ulang
     untuk di-hash — hanya mtime (1x stat) yang dibandingkan. */
  struct {
    char *path;
    int64_t mtime;
    uint64_t hash;
  } *snap;
  int snapCount, snapCap;
  bool snapLoaded;
  bool snapDirty;

  /* Hash table path -> index snapshot (isi = idx+1, 0 = kosong), memakai
     pathHash yang sama. Snapshot lookup/set per file per run tanpa ini
     adalah linear scan O(n) — untuk ratusan source berarti ribuan strcmp
     sia-sia tiap run, mahal di lingkungan proot. */
  int *sbucket;
  int sbcap;

  /* Cache edges dari run sebelumnya (entri ":edge:" di caches/deps.cache):
     bila mtime file .d masih sama, edges langsung dipakai tanpa membuka
     file .d — satu stat menggantikan open+read+fstat per source per run. */
  struct {
    char *src;
    int64_t dm;
    char **kids;
    int nkids;
    int expect; /* jumlah kids yang dijanjikan baris "!e" (-1 = format lama,
                   tak diketahui) — mendeteksi cache terpotong */
    bool used;  /* edge ini sudah dipasang ke node run ini */
  } *edges;
  int edgeCount, edgeCap;
  int *ebucket;
  int ebcap;
  bool edgeDirty;
  bool edgesLoaded;     /* section !e/!k sudah dicoba dimuat (sekali per run) */
  bool snapIncomplete;  /* ada file yang tak tertutup snapshot (deps.cache
                           hilang / file baru terlihat saat verifikasi) —
                           pass rekam wajib jalan setelah build sukses */
};

/* ==================== deps_core.c ==================== */

/* FNV-1a 64 atas isi file (streaming 4KB). 0 bila file tak terbaca. */
uint64_t depComputeFileHash(const char *path);

/* Rapikan "a/./b/../c" -> "a/c" (in-place) agar satu file = satu node. */
void depNormalizePath(char *p);

/* dir + '/' + name lalu dinormalisasi. */
void depJoinPath(char *out, size_t n, const char *dir, const char *name);

/* FNV-1a 64 untuk path (hash table). */
uint64_t depPathHash(const char *s);

int depNodeFind(const DepCache *dc, const char *path);
int depNodeAddM(DepCache *dc, const char *path, int64_t mtime);
int depNodeAdd(DepCache *dc, const char *path);
/* mtime node lewat memo stat proses (hanya hasil positif yang dimemo; lihat
   cmds_state.c). Negatif dan path yang bisa dibuat build selalu di-stat langsung. */
int64_t depNodeMTimeMemo(const char *path);
int depNodeGet(DepCache *dc, const char *path);

/* Tambah index kid ke node tanpa duplikat. */
void depKidAdd(DepNode *n, int kid);

/* Baca seluruh file ke buffer malloc (NUL-terminated); pemanggil yang free. */
char *depReadWhole(const char *path, size_t *outLen);

/* Pastikan node sudah punya edges (dari .d bila layak, else pemindai). */
void depParseNodeIfNeeded(DepCache *dc, int idx);

/* Pemeriksaan semantik header (default workspace; SATRIA_HDR_SEM=0 menonaktifkan). */
bool depsSemEnabled(void);
void depsSemRecordNode(DepCache *dc, int idx);
bool depsSemAcceptNode(DepCache *dc, int root, int idx);

/* DFS: mtime terbaru di subtree node; INT64_MAX bila ada dependensi hilang. */
int64_t depNewestUnder(DepCache *dc, int idx);

/* ==================== deps_dotd.c ==================== */

/* true bila path berakhiran .o / .obj (file object bukan dependensi source). */
bool depIsObjectPath(const char *p);

/* Bangun tabel hash edges dari seluruh entri yang ada. Dipanggil sekali
   setelah caches/deps.cache selesai dimuat. */
void depEdgesHashBuild(DepCache *dc);

/* ==================== deps_snapshot.c ==================== */

/* Muat .satria/caches/deps.cache sekali per run: snapshot hash + edges. */
void depSnapshotLoad(DepCache *dc);

#endif /* SATRIA_V0_1_0_DEPS_INTERNAL_H */

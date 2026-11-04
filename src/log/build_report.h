/* build_report — model record & renderer Build Report (design/build_report.md).
 *
 * Lapisan observasi murni: menerima hasil operasi yang SUDAH diputuskan mesin
 * build; tidak mengubah keputusan incremental/fingerprint/urutan fase.
 *
 * Alur: begin() -> (set toolchain/compdb/fingerprint, add artefak, catat
 * proyek + detail compile) -> finish(success) merender SATU laporan final
 * terurut (Header, Archive, Execution, Execution summary, Release, Summary).
 *
 * Detail compile boleh dicetak live selama build; catatan ringkas (header,
 * artefak, hitungan, ringkasan) direkam di sini dan dirender pada finish.
 * Kegagalan alokasi TIDAK menggagalkan build: record ditandai tak tersedia
 * dan finish() hanya melewatkan bagian yang kosong.
 */
#ifndef SATRIA_BUILD_REPORT_H
#define SATRIA_BUILD_REPORT_H

#include <stdbool.h>
#include <stddef.h>

/* Status artefak pada run yang dilaporkan (bukan sekadar keberadaan file). */
typedef enum {
  REP_BUILT = 0, /* archive/library/package dibuat/diupdate pada run ini */
  REP_CACHED,    /* dinyatakan valid & tidak dibangun ulang pada run ini */
  REP_LINKED     /* binary berhasil di-link pada run ini */
} RepArtStatus;

/* Status CompDB (eksplisit, dari return writer — bukan parse teks). */
typedef enum {
  REP_COMPDB_UNKNOWN = 0,
  REP_COMPDB_REFRESHED,
  REP_COMPDB_UNCHANGED,
  REP_COMPDB_RESTORED,
  REP_COMPDB_DISABLED,
  REP_COMPDB_SKIPPED,
  REP_COMPDB_FAILED
} RepCompdb;

/* Mulai report baru (reset model, catat start monotonic). Tanpa-opsi bila
   sudah aktif. Panggil SETELAH gerbang no-op. */
void buildReportBegin(void);
/* true bila report aktif (begin dipanggil & belum finish). */
bool buildReportActive(void);

/* Set toolchain efektif sekali per build (mis. "gcc -Wall ... -std=gnu11"). */
void buildReportSetToolchain(const char *cmd);
/* Set status CompDB (dan catatan pendukung, boleh NULL). */
void buildReportSetCompdb(RepCompdb st, const char *note);

/* Mode staging (child fork runner paralel): record artefak TIDAK dirender
   live & heading tidak dicetak — child mengekspor record lewat
   buildReportExport(), induk mengimpor (worklist order) lalu merender.
   Blok proyek (> Nama, compile line, hitungan) tetap dicetak live ke slot. */
void buildReportStaging(bool on);
/* Baris "Fingerprint : ..." pada header (opsional; NULL = tidak tampil). */
void buildReportSetFingerprint(const char *note);
/* Baris catatan tambahan pada header — teks utuh TANPA prefix "! " (mis.
   "Workspace never built — bade may be out of sync"), dirender sebagai
   `! <teks>` setelah baris Fingerprint. Panggil SETELAH buildReportBegin
   dan SEBELUM renderHeader (workspace: cakupan fingerprint saat run
   dimulai, lihat workspaceRun). NULL/kosong = tidak tampil. */
void buildReportSetHeaderNote(const char *text);

/* Render HEADER statis (Build Report + Toolchain/CompDB/Fingerprint) dan
   # Archive bila record arsip sudah diketahui (single-project: arsip dibangun
   sebelum header). # Execution DIBUKA LAZILY lewat buildReportBeginExecution /
   buildReportProject — pada workspace arsip proyek lain masih masuk setelah
   header, jadi heading tidak boleh tercetak di sini (design §3).
   Tanpa-opsi bila report tak aktif / sudah dirender. */
void buildReportRenderHeader(void);

/* Buka bagian # Execution (sekali). Bila header belum dirender, render dulu.
   Dipanggil buildReportProject (jalur serial/single) dan induk runner paralel
   sebelum mencetak isi slot proyek. Tanpa-opsi bila stading/aktif-inaktif. */
void buildReportBeginExecution(void);

/* Render baris artefak arsip yang belum tampil (buka # Archive sekali bila
   belum; setelah # Execution terbuka, baris telat dicetak apa adanya).
   Pemanggil: renderHeader, buildReportProject (sebelum > Nama), induk
   paralel setelah dump slot, buildReportFinish. Tanpa-opsi bila staging. */
void buildReportRenderArchives(void);

/* Staging antar fork (pola sama dengan stateBlob/verdict): tulis record
   artefak (archive/library/binary/release) ke file; induk mengimpor saat
   flush worklist dengan buildReportImport(). Best-effort: kegagalan tulis
   tidak menggagalkan build. */
bool buildReportExport(const char *path);
/* Gabungkan record dari file fragment (upsert per path), lalu hapus file. */
void buildReportImport(const char *path);

/* Tambah artefak. size < 0 atau hasSize=false -> baris tanpa ukuran
   (mis. checksum). Path dipakai apa adanya. */
void buildReportAddArchive(const char *path, RepArtStatus st, long long size);
void buildReportAddLibrary(const char *path, RepArtStatus st, long long size);
void buildReportAddBinary(const char *path, RepArtStatus st, long long size);
void buildReportAddRelease(const char *path, RepArtStatus st, long long size);

/* Blok proyek pada # Execution. upsert berdasarkan nama (bila sudah ada,
   proyek aktif dipindah ke entri itu — aman untuk dua pass workspace). */
void buildReportProject(const char *name);
/* Baris "* ..." pada blok proyek berjalan (mis. catatan embedded). */
void buildReportNote(const char *fmt, ...);
/* Baris detail compile "[i/total] TAG X.XXs src" pada proyek berjalan. */
void buildReportCompileLine(int idx, int total, const char *tag, double secs,
                            const char *src);
/* Baris "WARN <diag>" pada proyek berjalan (salinan diagnostic compiler). */
void buildReportWarning(const char *diag);
/* Hitungan proyek: "* N compiled, M skipped[, W warning]". */
void buildReportProjectCounts(int compiled, int skipped, int warnings);

/* Render laporan final. success=false -> Status: Failed (artefak sukses
   terdahulu tetap boleh tampil). Record TIDAK dibuang (dipakai kondensasi
   blob no-op) — lepas dengan buildReportRelease(). Tanpa-opsi bila tak aktif. */
void buildReportFinish(bool success);

/* Isi agregat untuk blob LAST BUILD SUMMARY (no-op) — SUMBER EKSPLISIT,
   bukan parsing teks log (design §9): nArch = arsip REP_BUILT ("created");
   nLibStatic/nLibShared = library REP_BUILT per ekstensi (.a/.so);
   nBin = REP_LINKED; nBinUp = REP_CACHED; nRel = release REP_BUILT
   ("created"); nRelUp = REP_CACHED — sidecar checksum (size < 0) tidak
   dihitung agar paket tidak dihitung ganda. Return false bila report tak
   aktif. */
bool buildReportAgg(int *nArch, int *nLibStatic, int *nLibShared, int *nBin,
                    int *nBinUp, int *nRel, int *nRelUp);

/* Bebaskan semua record. Aman dipanggil berkali-kali / setelah finish. */
void buildReportRelease(void);

#endif /* SATRIA_BUILD_REPORT_H */

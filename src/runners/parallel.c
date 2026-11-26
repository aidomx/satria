/* runners/parallel.c — Runner paralel batch workspace: penjadwal siap-dep (fork POSIX).
 * Lihat runners.h. Runner ini dipakai HANYA untuk child background.
 *
 * Kedua fase menjalankan SATU panggilan wsRunBatch atas seluruh worklist.
 * Anak diluncurkan SEGERA setelah dependensi depends_on-nya selesai
 * (dep-ready) — bukan menunggu seluruh level/antrean. Ini yang menutup gap
 * bench/ws.sh skenario hedit & libhdr (edit header pusat / header internal:
 * dua proyek sama-sama rebuild dengan kerja IDENTIK): penjadwal lama
 * serial-per-level, sehingga ar/so proyek pertama menghalangi compile proyek
 * kedua dan satria kalah 1.4-1.6x dari cninja. Profil SATRIA_PROFILE pada
 * -n 10: fase library 1944 ms serial (core 1029 + bade 915 termasuk rupamod)
 * -> ±1150 ms bila compile/tail saling tindih; ar 0.14 s + .so 0.34 s per
 * proyek tidak lagi berurut lintas proyek.
 *
 * Bagi jobs (anak yang mengompilasi):
 *   pj = jobs / (anak-compile berjalan + sisa anak-compile siap-launch),
 *   minimal 1. Anak kemasan (output.binary = false) berpj 1 dan TIDAK
 *   menghitung budget — arsip tak pernah memotong jatah proyek besar
 *   (pembagian statis penjadwal lama membuat rupaws cold -j4 +22%: proyek
 *   besar terjebak setengah jobs sementara arsip memegang jatah sia-sia).
 *   Bila budget habis, anak baru memakai porsi adil jobs/(berjalan+1):
 *   puncak gcc ~2x jobs hanya pada jendela tindihan singkat, sisa waktu
 *   <= jobs (penting: memori di proot/Termux jangan meledak -> force close).
 *
 * Anak fork menstaging tiga hal:
 *   - output   : file slot, dicetak induk urut worklist setelah selesai.
 *                Mode stream (stdout pipe/soket atau SATRIA_WS_PAR=1) TIDAK
 *                memakai slot: anak menulis langsung ke stdout induk
 *                (line-buffered) sehingga progres per-objek terlihat saat
 *                cold build — lihat runners.c runnerStreamOutput;
 *   - state    : stateBlob stage (digabung induk saat waitpid);
 *   - verdict  : file ws-verdict-*, berisi byte-object-verdict fase library
 *                (commands.c cmdsWsPhaseVerdict*). Verdict lama hanya di
 *                memori proses — hilang antar fork sehingga edit komentar
 *                (skenario leaf/bade) salah relink binary (bin 1, harap 0).
 *
 * Fallback & kegagalan: mkstemp/fork gagal -> proyek yang belum terluncur
 * dijalankan serial sesudah semua anak berjalan (urutan worklist = topo,
 * aman); anak gagal (rc != 0) -> berhenti meluncurkan proyek baru, anak
 * yang berjalan dikerikan — ekuivalen "batch berikutnya dilewati" perilaku
 * lama.
 *
 * Kebijakan pemilihan runner (runners.c, runnerWantParallel):
 *   - DEFAULT: PARALEL (fork) untuk batch n>1 proyek dengan jobs>1, baik
 *     foreground maupun child background. Satu panggilan link (cc/ld) berbiaya
 *     tetap ~0,25-0,37 dtk dan tidak bisa dipecah, jadi runner serial membuat
 *     link ar/so dan binary proyek-proyek berurutan (bench hedit/libhdr 2,9 ->
 *     1,8 dtk setelah paralel). Output urut worklist (slot) untuk berkas
 *     reguler; stdout pipe/soket memakai output live (lihat runners.c).
 *   - SATRIA_WS_PAR=1: paksa paralel + output live (stream).
 *   - SATRIA_WS_PAR=0: paksa serial (output proyek realtime, compile saja
 *     yang paralel di dalam proyek) — untuk pengujian/fallback.
 * Serial: fase library bertingkat & fase binary datar, urutan output tetap.
 */

#include "runners.h"
#include "../log/build_report.h" /* fragment record artefak antar fork */
#include "../cache/router.h" /* optsStoreReload: sinkron store opts.cache pasca fork */

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef _WIN32
/* ---------- verdict byte-object: staging antar anak fork ---------- */
/* Format ws-verdict-*: dua byte.
   b1 = objectsChanged (byte-object fase library — perilaku lama),
   b2 = "ada kompilasi" (audit 4/11 item 1): save-walk induk memakainya
        untuk memutuskan memo stat gerbang no-op masih valid untuk .o/.d.
   Diekspor anak fase library MAUPUN binary: pass binary bisa saja
   mengompilasi (recovery state) — OR di induk. */
static void wsVerdictChildExport(const char *project, const char *path) {
  bool changed = false, compiled = false;
  bool haveV = cmdsWsPhaseVerdictGet(project, &changed);
  bool haveC = cmdsWsPhaseCompiledGet(project, &compiled);
  if (!haveV && !haveC) return;
  FILE *f = fopen(path, "w");
  if (!f) return;
  fputc(haveV && changed ? '1' : '0', f);
  fputc(haveC && compiled ? '1' : '0', f);
  fclose(f);
}

static void wsVerdictParentImport(const char *project, const char *path, bool libOnly) {
  FILE *f = fopen(path, "r");
  if (!f) return;
  int c1 = fgetc(f), c2 = fgetc(f);
  fclose(f);
  remove(path);
  /* Verdict byte-object hanya milik fase library (dipakai pass binary lewat
     snapshot fork); byte fase binary tidak menimpanya. */
  if (libOnly && (c1 == '0' || c1 == '1')) cmdsWsPhaseVerdictSet(project, c1 == '1');
  if (c2 == '1') cmdsWsPhaseCompiledSet(project, true);
  /* Rencana dihitung sebelum batch. Begitu satu anak benar-benar menulis
     hasil, verdict hit untuk proyek lain bisa basi (mis. arsip embed yang
     baru dibuat) — buang di induk, bukan hanya di proses anak. */
  if (c1 == '1' || c2 == '1') wsPlanNoteBuilt(project);
}

/* ---------- penjadwal siap-dep (fork) ---------- */
typedef struct {
  pid_t pid;  /* > 0 bila ter-fork */
  int fd;     /* file slot output; -1 bila tidak ada */
  char tmpl[32];
  char stage[64];
  char verdict[64];
  char rep[64]; /* fragment record build report anak (A/L/B/R) */
  bool started, done, flushed, forked;
} WsSlot;

/* Semua dependensi proyek i sudah selesai? Dependensi di LUAR worklist
   (mis. proyek kemasan yang difilter dari fase binary) tidak menahan. */
static bool wsDepsDone(const WsModel *m, const WsWork *work, int n,
                       const WsSlot *slot, int i) {
  const List *deps = &m->depends[work[i].idx];
  for (int d = 0; d < deps->count; d++) {
    int di = wsFindProject(m, deps->items[d]);
    if (di < 0) continue; /* nama tak dikenal: sudah dilaporkan validate */
    int k = -1;
    for (int j = 0; j < n; j++)
      if (work[j].idx == di) {
        k = j;
        break;
      }
    if (k < 0) continue; /* di luar worklist: tak menahan */
    if (!slot[k].done) return false;
  }
  return true;
}

/* Dump slot + impor fragment report anak, mengikuti urutan flush (= urut
   worklist): heading # Execution + > Nama dicetak INDUK (bukan child), lalu
   fragment record artefak diimpor, isi slot (compile line + hitungan) dicetak,
   dan terakhir baris arsip yang belum tampil dirender sesudahnya — posisi
   identik dengan runner serial (arsip proyek tampil setelah blok proyeknya;
   lihat buildReportProject). */
static void wsSlotDump(const WsModel *m, const WsWork *work, WsSlot *s, int idx) {
  /* Heading `# Execution` + `> Nama` dicetak INDUK SEBELUM impor fragment:
     arsip proyek INI baru boleh dirender sesudah bloknya (urutan serial),
     sementara arsip proyek SEBELUMNYA yang masih pending ikut dirender di
     sini — persis perilaku buildReportProject pada runner serial. Child dalam
     staging tidak mencetak heading (lihat buildReportProject). */
  if (s->fd < 0) {
    /* Mode live: heading sudah dicetak ANAK fase library. Induk hanya perlu
       mencatat nama proyek di model report-nya supaya anak fase binary tidak
       mencetak "> Nama" kedua kali (anak mewarisi model induk). */
    if (!wsProjectArchiveOnly(&m->projects[work[idx].idx]))
      buildReportProjectNote(work[idx].name);
  } else if (!wsProjectArchiveOnly(&m->projects[work[idx].idx]) &&
             lseek(s->fd, 0, SEEK_END) > 0)
    buildReportProject(work[idx].name); /* slot fast-state-hit kosong -> tanpa heading */
  if (s->rep[0]) {
    buildReportImport(s->rep);
    s->rep[0] = '\0';
  }
  if (s->fd >= 0) {
    char buf[4096];
    ssize_t r;
    lseek(s->fd, 0, SEEK_SET);
    while ((r = read(s->fd, buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)r, stdout);
    close(s->fd);
    s->fd = -1;
    if (s->tmpl[0]) unlink(s->tmpl);
  }
  buildReportRenderArchives();
  fflush(stdout); /* output proyek langsung terlihat, termasuk saat stdout pipe */
  s->flushed = true;
}

/* Mode SATRIA_WS_PAR: cetak setiap slot yang selesai tanpa menunggu urutan
   worklist. Blok per proyek tetap utuh (satu slot = satu blok); hanya urutan
   antar-proyek yang mengikuti waktu selesai, seperti job paralel CMake/Ninja. */
static void wsFlushDone(const WsModel *m, const WsWork *work, WsSlot *slot, int n) {
  for (int i = 0; i < n; i++)
    if (slot[i].started && slot[i].done && !slot[i].flushed)
      wsSlotDump(m, work, &slot[i], i);
}

/* Cetak output slot yang selesai mengikuti urutan runner SERIAL (design
   kriteria pengujian workspace: "urutan proyek sama dengan worklist pada
   runner serial"): fase library per tingkat dependensi (batch-per-level),
   fase binary datar atas worklist. Slot berikutnya yang belum siap MENGHENTIKAN
   flush — output proyek tetap utuh dan deterministik, bukan urutan selesai
   anak. Peluncuran anak TIDAK berubah: penjadwal siap-dep tetap menindih
   proyek independen; hanya urutan CETAK yang disamakan. */
static void wsFlushPrefix(const WsModel *m, const WsWork *work, WsSlot *slot, int n,
                          bool libOnly) {
  int maxLevel = 0;
  if (libOnly)
    for (int i = 0; i < n; i++)
      if (work[i].level > maxLevel) maxLevel = work[i].level;
  for (int lv = 0; lv <= maxLevel; lv++) {
    for (int i = 0; i < n; i++) {
      if (libOnly && work[i].level != lv) continue;
      if (slot[i].flushed) continue;
      if (slot[i].started && slot[i].done) {
        wsSlotDump(m, work, &slot[i], i);
        continue;
      }
      return; /* urut serial: berhenti pada slot berikutnya yang belum siap */
    }
  }
}

int runnerParallel(const WsModel *m, const WsWork *work, int n, int jobs,
                         const char *cmd, const char *wsDir, bool libOnly, bool compileOnly) {
  (void)wsDir;
  const char *pre = libOnly ? "ws-libpass-pre" : "ws-binpass-pre";
  const char *post = libOnly ? "ws-libpass-post" : "ws-binpass-post";
  WsSlot *slot = (WsSlot *)calloc((size_t)n, sizeof(WsSlot));
  if (!slot) return runnerSerial(m, work, n, jobs, cmd, wsDir, libOnly, compileOnly);
  for (int i = 0; i < n; i++) {
    slot[i].pid = -1;
    slot[i].fd = -1;
  }

  int running = 0, compileRunning = 0, finished = 0, rc = 0, seq = 0;
  bool failed = false, degraded = false;
  const bool live = runnerStreamOutput();

  profMark(pre);
  fflush(NULL);       /* buffer stdout/stderr induk tidak boleh ikut ke child */
  stateBlobFlush();   /* anak mewarisi base state dari file */
  if (live) {
    /* Output live: header + `# Execution` dicetak INDUK sebelum fork pertama,
       sehingga setiap anak mewarisi keadaan "sudah dirender" dan hanya
       mencetak blok proyeknya sendiri (`> Nama`, compile line, hitungan).
       Tanpa ini, beberapa anak akan balapan mencetak header yang sama. */
    buildReportRenderHeader();
    buildReportBeginExecution();
    fflush(stdout);
  }

  while (finished < n) {
    /* ---- luncurkan semua proyek yang siap (dependensi selesai) ---- */
    if (!failed && !degraded) {
      int ready[WS_MAX_PROJECTS];
      int rn = 0;
      for (int i = 0; i < n; i++)
        if (!slot[i].started && wsDepsDone(m, work, n, slot, i)) ready[rn++] = i;
      for (int j = 0; j < rn && !failed && !degraded; j++) {
        int i = ready[j];
        bool arch = wsProjectArchiveOnly(&m->projects[work[i].idx]);
        int pj;
        if (arch) {
          pj = 1; /* proyek kemasan tak memakai pool compile: tak dihitung */
        } else {
          int cap = 0;
          for (int k = j; k < rn; k++)
            if (!wsProjectArchiveOnly(&m->projects[work[ready[k]].idx])) cap++;
          if (cap < 1) cap = 1;
          int share = jobs / (compileRunning + cap);
          pj = share > 0 ? share : 1;
        }
        char tmpl[32] = {0};
        int fd = -1;
        if (!live) {
          snprintf(tmpl, sizeof(tmpl), ".satria/ws-par-XXXXXX");
          fd = mkstemp(tmpl);
          if (fd < 0) {
            degraded = true; /* disk penuh dst: sisa -> serial setelah drain */
            break;
          }
        }
        WsSlot *s = &slot[i];
        snprintf(s->stage, sizeof(s->stage), ".satria/ws-state-%ld-%d.cache",
                 (long)getpid(), seq);
        snprintf(s->verdict, sizeof(s->verdict), ".satria/ws-verdict-%ld-%d",
                 (long)getpid(), seq);
        snprintf(s->rep, sizeof(s->rep), ".satria/ws-report-%ld-%d",
                 (long)getpid(), seq);
        seq++;
        snprintf(s->tmpl, sizeof(s->tmpl), "%s", tmpl);
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
          if (!live) {
            dup2(fd, STDOUT_FILENO);
            /* stderr TIDAK dialihkan ke slot: relay build.log
               (buildLogBegin) menempel di stderr induk lewat pipe — bila anak
               memindahkannya, diagnostik kompilasi tidak pernah sampai ke
               relay dan .satria/build.log tidak terbentuk (uji
               build_log_miniws). Keluaran proyek (stdout) tetap di slot agar
               dicetak urut worklist; stderr mengalir langsung (live ke
               terminal/ERRLOG) — tidak memengaruhi penggretan unit test. */
            if (fd > STDERR_FILENO) close(fd);
          } else {
            /* Pipa: stdout stdio penuh-buffer 4 KiB akan menahan ~20 baris
               progres. Line-buffer membuat satu baris = satu write (atomik
               < PIPE_BUF) sehingga progres benar-benar realtime. */
            setvbuf(stdout, NULL, _IOLBF, 0);
          }
          stateBlobForkChild(); /* buang warisan; tulisan anak ke stage sendiri */
          /* Memo stat induk TIDAK boleh diwarisi: ia memuat stamp file yang
             dibaca induk sebelum fork (mis. arsip embed saat gerbang no-op),
             sementara anak yang lebih dulu selesai bisa sudah menulis ulang
             file itu (rupamod -> modules/rupa_modules.tar). Anak bade lalu
             melihat arsip BASI, menganggap fast-state-nya valid, dan melewatkan
             rebuild embedded.c — pekerjaan tertinggal satu run (audit ws
             skenario mod/dep). Serial aman karena cmdsStatMemoForget di proses
             yang sama; di fork, tiap anak butuh memo bersih sendiri. */
          cmdsStatMemoReset();
          /* Verdict fast-state precompute induk juga basi begitu satu proyek
             selesai (mis. rupamod menulis ulang tar embed yang dibaca bade):
             anak lanjutan memvalidasi sendiri seperti runner serial, anak
             gelombang pertama tetap memakai verdict precompute. */
          if (finished > 0) wsPlanInvalidate();
          /* Staging: record artefak anak TIDAK dirender live (heading & baris
             arsip adalah tanggung jawab induk saat flush worklist) — anak
             cukup mengekspornya lewat file fragment di bawah. Mode live
             kebalikannya: anak yang mencetak heading bloknya sendiri. */
          buildReportStaging(!live);
          int crc = wsRunProject(m, work[i].name, cmd, pj, libOnly, compileOnly);
          fflush(NULL);
          stateBlobExport(s->stage);
          buildReportExport(s->rep);
          wsVerdictChildExport(work[i].name, s->verdict); /* kedua fase: byte compiled */
          /* Commit mutasi opts.cache anak SEBELUM _exit (atexit tidak jalan
             di _exit): record router/metadata proyek anak wajib sampai ke
             disk sebelum induk optsStoreReload. */
          optsStoreFlush();
          _exit(crc); /* bukan exit(): hindari atexit ganda setelah fork */
        }
        if (pid < 0) { /* fork gagal: slot ini -> serial setelah drain */
          if (fd >= 0) {
            close(fd);
            unlink(tmpl);
          }
          s->tmpl[0] = '\0';
          degraded = true;
          break;
        }
        s->pid = pid;
        s->fd = fd;
        s->started = true;
        s->forked = true;
        running++;
        if (!arch) compileRunning++;
      }
    }
    if (running == 0) break; /* tak ada anak untuk ditunggu */

    /* ---- tunggu SATU anak (siapa pun yang selesai dulu) ---- */
    int st = 0;
    pid_t pid;
    do {
      pid = waitpid(-1, &st, 0);
    } while (pid < 0 && errno == EINTR);
    if (pid < 0) break;
    int si = -1;
    for (int i = 0; i < n; i++)
      if (slot[i].forked && !slot[i].done && slot[i].pid == pid) {
        si = i;
        break;
      }
    if (si < 0) continue; /* bukan anak batch ini (tak terduga) */
    WsSlot *s = &slot[si];
    s->done = true;
    running--;
    if (!wsProjectArchiveOnly(&m->projects[work[si].idx])) compileRunning--;
    finished++;
    int crc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (crc != 0 && rc == 0) rc = crc;
    stateBlobImport(s->stage);
    wsVerdictParentImport(work[si].name, s->verdict, libOnly);
    if (runnerStreamOutput())
      wsFlushDone(m, work, slot, n);
    else
      wsFlushPrefix(m, work, slot, n, libOnly);
    if (rc != 0) failed = true; /* berhenti meluncurkan; kerikan sisa anak */
  }

  /* Output anak yang belum tercetak (mis. sesudah kegagalan): apa adanya. */
  for (int i = 0; i < n; i++)
    if (slot[i].forked && !slot[i].flushed) wsSlotDump(m, work, &slot[i], i);

  /* Sisa proyek yang belum diluncurkan (mkstemp/fork gagal): serial,
     urutan worklist = topo — dependensi sudah selesai atau mendahului. */
  if (!failed) {
    for (int i = 0; i < n && rc == 0; i++) {
      if (slot[i].started) continue;
      slot[i].started = true;
      slot[i].done = true;
      slot[i].flushed = true; /* output langsung ke stdout induk */
      profMark(pre);
      rc = wsRunProject(m, work[i].name, cmd, jobs, libOnly, compileOnly);
      profMark(post);
    }
  }
  /* Flush mutasi opts.cache induk (serial fallback / gate refresh yang
     belum ter-commit), lalu sinkron dengan tulisan anak: buka ulang store
     dari disk. Urutan wajib: flush dulu — commit induk TIDAK boleh menyusul
     reload (menimpa entri anak, kelas bug audit 31/10 §10). Serial fallback
     di atas (wsRunProject in-process) menulis lewat handle yang sama:
     flush + reload tetap konsisten (memori == disk). */
  optsStoreFlush();
  optsStoreReload();
  free(slot);
  profMark(post);
  return rc;
}
#else
/* Windows: fork tidak tersedia; jalankan serial (perilaku identik, tanpa paralel). */
int runnerParallel(const WsModel *m, const WsWork *batch, int n, int jobs,
                   const char *cmd, const char *wsDir, bool libOnly, bool compileOnly) {
  return runnerSerial(m, batch, n, jobs, cmd, wsDir, libOnly, compileOnly);
}
#endif /* !_WIN32 */


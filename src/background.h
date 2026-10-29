#pragma once

#include <stdbool.h>

/*
 * background — build di latar belakang + watcher (design/background.md).
 *
 * File (relatif root kerja setelah chdir -f; di-absolutkan saat dipakai supaya
 * fsSetCwd(c.root) di tengah build tidak mengacaukan path child):
 *   .satria/build.pid | .satria/workspace.pid   PID build berjalan
 *   .satria/background.log                      stdout+stderr child (ditimpa
 *                                               tiap build background baru)
 *   .satria/background.status                   "success" | "failed" (akhir run)
 *   .satria/background.progress                 "pct|aktivitas" (tulis atomik)
 *   .satria/background.startup                  baris fase startup (cold) —
 *                                               di-replay oleh `satria watch`
 *
 * Alur (Buildfile: background = true):
 *   induk  : bgBuildGate -> bgSpawn (fork/spawn child, tulis PID, keluar rc 0)
 *   child  : bgChildBegin -> build -> bgChildFinish (status + hapus PID)
 *   user   : satria watch [--full] -> bgWatch (startup replay + progress / log)
 */
typedef enum {
  BG_SINGLE = 0, /* satu proyek: pid .satria/build.pid */
  BG_WS = 1      /* workspace : pid .satria/workspace.pid */
} BgMode;

/*
 * Simpan argv utama untuk spawn child (salinan pointer; argv[0] berupa path
 * relatif dibuat absolut SEBELUM chdir -f). Juga membaca & membuang penanda
 * env SATRIA_BG_CHILD sehingga child build menandai dirinya lewat bgIsChild().
 * Panggil di awal satriaRun — SEBELUM bootstrapApply.
 */
void bgNoteArgv(int argc, const char *argv[]);

/* true bila proses ini adalah child background (diluncurkan bgSpawn). */
bool bgIsChild(void);

/*
 * Gerbang build (design §4.1): tolak bila build background lain masih hidup
 * — pesan + rc 2. Return -1 = lanjut build. Tidak berlaku untuk child sendiri.
 */
int bgBuildGate(BgMode mode);

/*
 * Gerbang clean (design §4.4): blok bila build background berjalan agar
 * artefak tidak dirusak di tengah jalan. Return -1 = lanjut clean.
 */
int bgCleanGate(BgMode mode);

/*
 * Spawn child background memakai argv tersimpan + env SATRIA_BG_CHILD,
 * arahkan output ke background.log, tulis PID. Return rc: 0 = berhasil
 * dimulai (induk keluar dengan "berhasil memulai"), >0 = gagal.
 */
int bgSpawn(BgMode mode);

/*
 * Single-project: bila Buildfile menyetel `background = true`, spawn child
 * dan return rc; selain itu -1 (lanjut build normal). Child sendiri: -1.
 * Startup TIDAK dicetak di terminal — lihat bgStartupRecord.
 */
int bgMaybeSpawnSingle(const char *buildfile);

/*
 * Simpan/buang rekaman startup untuk watcher, dipanggil SEBELUM spawn:
 * want = ada startup (cold) -> tulis .satria/background.startup dari buffer
 * startup.c; selainnya -> buang rekaman lama supaya watch tidak me-replay
 * blok basi. Pada spawn startup memang tidak pernah dicetak ke terminal.
 */
void bgStartupRecord(bool want);

/* Dipanggil CHILD: tulis PID, bersihkan status/progress lama, progres awal. */
void bgChildBegin(BgMode mode);

/* Dipanggil CHILD setelah build selesai: tulis status lalu hapus PID
   (urutan penting: status ada => run sudah final bagi watcher). */
void bgChildFinish(bool success);

/* Update progres build berjalan (no-op di luar child). fmt = aktivitas,
   mis. "Compiling: src/x.c" / "Linking: bin/name". Ditulis atomik. */
void bgProgress(int pct, const char *fmt, ...);

/*
 * `satria watch` (design §3B/§3C): replay blok startup run berjalan, pantau
 * sampai selesai, cetak ringkasan build + status, lalu keluar (0 = success,
 * 1 = failed). full = stream isi background.log secara live (tail -f)
 * alih-alih baris progress. Invokasi BARU setelah run selesai (atau tanpa
 * run) DITOLAK: tidak ada pekerjaan yang berjalan di background.
 */
int bgWatch(bool wsMode, bool full);

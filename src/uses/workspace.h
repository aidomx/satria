#ifndef SATRIA_V0_1_0_USES_WORKSPACE_H
#define SATRIA_V0_1_0_USES_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * workspace — mode multi-proyek (use workspace / WsBuildfile).
 *
 * Implementasi dipisah dari use.c supaya use.c tetap satu titik dispatcher
 * kecil; semua logika daftar proyek, topo sort depends_on, dan sintesis
 * Buildfile ada di workspace.c.
 */

/* Nama file workspace di root workspace. Sengaja tanpa ekstensi: akhiran `.ws`
   bertabrakan dengan Witcher Script dan membuat editor/alat lain salah paham. */
#define WORKSPACE_FILENAME "WsBuildfile"

/* Batas model workspace (dipakai workspace.c & profile workspace=). */
#define WS_MAX_PROJECTS 16
#define WS_MAX_RELEASES 8

/* Nama lama — masih diterima saat WsBuildfile tidak ada, supaya workspace
   lama tetap jalan tanpa rename (run memperingatkan agar segera rename).
   `Buildfile.ws` adalah nama utama sampai v0.6.0; `Buildfile.workspace`
   nama sebelum 3 Okt 2026. Keduanya akan dihapus setelah masa transisi. */
#define WORKSPACE_FILENAME_DEPRECATED "Buildfile.ws"
#define WORKSPACE_FILENAME_LEGACY "Buildfile.workspace"

/* Folder tempat Buildfile hasil sintesis disimpan (stabil, di root
   workspace) agar cache config & fast path tidak miss tiap run. */
#define WORKSPACE_SYNTH_DIR ".satria/workspace"

/*
 * Jalankan mode workspace untuk command `cmd` ("build", "clean", atau
 * "compdb" = tulis CompDB gabungan tanpa membangun).
 * only: NULL/"" = semua proyek sesuai urutan depends_on; selain itu hanya
 * proyek itu (+ dependency-nya). Build selektif TIDAK membuat paket release
 * kecuali releaseSel diberikan.
 * releaseSel: `-- key=value[,key=value...]` (mis. "version=0.6.0") — OVERRIDE
 * setting release untuk run ini; konfigurasi dasar tetap dari section
 * release.* di WsBuildfile dan section itu tidak wajib ada. `name=<nama>`
 * adalah nama paket release (bukan nama proyek) dan divalidasi sebagai nama;
 * `target=tar|deb` diterima sebagai sinonim lama `format=`. NULL/"" = build
 * seluruh workspace; release hanya jalan bila section release.* ada.
 * withCdeps: `--with-cdeps` — build selektif (`only`) ikut menarik provider
 * cdeps (ketergantungan compile/link) ke closure; tanpa selektor sifatnya
 * no-op. Default false: cdeps tidak pernah memperluas closure (kontrak lama).
 * Return code gaya main(): 0 sukses, 1 gagal, 130 interupsi.
 */
int workspaceRun(const char *cmd, int jobs, const char *only, const char *releaseSel,
                 bool withCdeps);

/*
 * workspaceEnumerate — baca+validasi WsBuildfile di `wsDir` (parse, topo
 * sort, validasi release) lalu ENUMErasikan saja: cekBuildfile menunjukkan
 * apakah Buildfile project `<wsDir>/<name>/Buildfile` atau file `.Buildfile`
 * hasil sintesis ada (dipakai `satria profile workspace=` untuk menghitung
 * proyek tanpa menjalankan build apa pun).
 * projectNames[count] diisi pointer ke buffer internal model (valid selama
 * panggilan; TIDAK perlu dibebaskan oleh pemanggil).
 * Return 0 sukses; 1 bila file tidak ada / parse / validasi gagal.
 */
int workspaceEnumerate(const char *wsDir, int *count, const char **projectNames,
                       int maxProjects);

/* true bila WsBuildfile (atau nama lama) ada di cwd. */
bool workspaceFileExists(void);

/*
 * wsMemberCwdDetect — true bila cwd berada DI DALAM direktori proyek anggota
 * sebuah workspace INDUK (WsBuildfile di parent; keanggotaan divalidasi
 * lewat model — file workspace di parent saja tidak cukup; design/
 * workspace_v1.md aturan 1 & 5). Mengisi wsRootOut (root workspace induk)
 * dan memberOut (nama proyek anggota) bila true. Cwd pada root workspace
 * sendiri selalu false — jalur mode workspace biasa yang menanganinya.
 */
bool wsMemberCwdDetect(char *wsRootOut, size_t wsRootN, char *memberOut, size_t memberN);

/*
 * wsStateFileExists — true bila cache fingerprint workspace
 * (.satria/caches/fingerprint.cache) ada. Dipakai bootstrap.c hanya untuk
 * status cold kasar di SATRIA_STATUS; keputusan no-op sebenarnya diambil
 * wsCacheNoopValid di workspaceRun (opts.cache -> fingerprint.cache).
 * workspace.state (gerbang legacy) sudah tidak ditulis.
 */
bool wsStateFileExists(void);

/* Nama file workspace yang terdeteksi di cwd — WORKSPACE_FILENAME dulu,
   lalu WORKSPACE_FILENAME_DEPRECATED, lalu WORKSPACE_FILENAME_LEGACY; NULL
   bila tak ada ketiganya. Nama terdepan menang bila beberapa ada. Dipakai
   supaya pesan error & sintesis menyebut nama file yang benar-benar
   dipakai. */
const char *workspaceFileName(void);

#endif /* SATRIA_V0_1_0_USES_WORKSPACE_H */

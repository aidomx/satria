#ifndef SATRIA_V0_1_0_COMMANDS_H
#define SATRIA_V0_1_0_COMMANDS_H

#include <stdbool.h>

void showHelp(void);

/*
 * buildfilePath: file konfigurasi yang dipakai (default "Buildfile";
 * dirujuk `satria -f <file>`; NULL diterima dan diperlakukan sebagai
 * "Buildfile"). cmdInit menolak menulis jika file itu sudah ada.
 */
int cmdInit(const char *buildfilePath);

/*
 * `satria init -w` — tulis template workspace (default "Buildfile.ws";
 * bila -f diberikan, path itulah yang dipakai). Menolak menulis jika
 * file sudah ada, sama seperti cmdInit.
 */
int cmdInitWorkspace(const char *buildfilePath);

/*
 * `satria init -p` — scaffold interaktif (implementasi di commands.c):
 *   | Project name? [Enter for random name]
 *   | language? [c, cpp] [Enter = c; cpp => src/main.cpp + std = c++17]
 *   | std? / compiler? / flags? [Enter = default konvensi]
 *   | Create include/? [y/N]
 *   | Uses workspace? [y/N]
 *     | Workspace projects? [Enter = app,tool]
 *     | depends_on? (format: <proyek> depends <proyek>) [Enter = none]
 *
 * Simbol `|` pada prompt berwarna cyan bila stdout terminal; otomatis
 * polos di pipe/CI atau saat NO_COLOR/SATRIA_NO_COLOR/TERM=dumb.
 * Nama selalu menjadi folder scaffold <nama>/: single-project berisi
 * Buildfile + src/ (+ include/ bila diminta); proyek workspace tidak
 * punya Buildfile sendiri (satria mensintesisnya dari Buildfile.ws).
 * Jawaban non-default ditulis eksplisit ke Buildfile; sisanya konvensi.
 * Pra-cek overwrite: satu saja file target sudah ada => init dibatalkan
 * tanpa menimpa apa pun. EOF di stdin => dibatalkan.
 */
int cmdInteractiveInit(void);

/* jobs <= 0 berarti serial (1); >= 2 memicu kompilasi paralel -jN. */
int cmdBuild(int jobs, const char *buildfilePath);

/*
 * cmdBuildEx — build dengan mode fase:
 *   libOnly = false : build penuh (compile + link binary + library).
 *   libOnly = true  : fase library workspace — compile + buildEmbedded +
 *                     kemas lib<name>.a/.so TANPA link binary. Binary proyek
 *                     yang saling memakai library lintas-proyek (mis. rupa
 *                     <-> ruka di workspace) baru bisa link setelah SEMUA
 *                     proyek menyelesaikan fase library, jadi fase ini
 *                     juga tidak menyertakan library ber-path proyek lain
 *                     pada link .so dan tidak merekam fast state (state
 *                     menyangkut target binary — milik fase normal).
 */
int cmdBuildEx(int jobs, const char *buildfilePath, bool libOnly);
int cmdClean(const char *buildfilePath);

/*
 * Verdict byte-object antar fase workspace (library pass -> binary pass):
 * true bila object proyek yang ditulis ulang BERUBAH byte-nya, false bila
 * identik (edit komentar dsb — pass binary tidak boleh relink hanya karena
 * mtime). Tabel in-memory per proses; jalur fork fase library
 * (src/workspace/ws_batch.c) menstaging verdict lewat file agar tetap
 * sampai ke pass binary di induk. project = nama proyek workspace (NULL /
 * mode single-project = abaikan).
 */
bool cmdsWsPhaseVerdictGet(const char *project, bool *changed);
void cmdsWsPhaseVerdictSet(const char *project, bool changed);

/*
 * cmdCompdbGenerate — `satria -g compdb`: hasilkan/
 * refresh compile_commands.json TANPA membangun apa pun. Jalur query-only:
 * loadConfig -> gather source -> fingerprint compdb -> cache hit / restore
 * blob / render. Tidak ada decide, kompilasi, link, fast-state — aman
 * dipanggil kapan pun (mis. sebelum build pertama, untuk editor/clangd).
 */
int cmdCompdbGenerate(const char *buildfilePath);

#endif /* SATRIA_V0_1_0_COMMANDS_H */

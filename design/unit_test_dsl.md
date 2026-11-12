# DSL unit test

Menulis unit test dengan bash/sh itu bukanlah perkara mudah, akar masalah:
- Sulit untuk dirawat
- Perlu pemahaman bash/sh script
- Perlu memahami perilaku satria
---
# Tujuan

Dengan memperlakukan unit test sebagai bagian dari dsl, itu akan mempermudah penulisan, pembacaan, dan dapat didokumentasikan serta pengguna bisa mencoba menulis unit testnya sendiri terhadap satria tanpa perlu memahami cara kerja internal dari satria.

Ukuran keberhasilan:
- Seluruh `tests/unit/common` (25 tes) dapat dinyatakan **tanpa satu pun baris
  bash/sh di sisi penulis tes** — dicapai bertahap lewat milestone parser.
- Syntax baru yang harus dipelajari penulis tes kecil: **8 objek aksi +
  3 objek hasil + 14 direktif** — dan objek bisa dibuka untuk keperluan lain
  tanpa menambah bentuk syntax.
- Kontrak runner (`scripts/unit.sh` + `scripts/lib/unit_runner.sh`) tidak
  berubah: verdict, klasifikasi, snapshot, dan discovery tetap seperti semula.

---

# File spesifik

`example.test` menjadi standard yang dibaca oleh shell script penguji yaitu `unit.sh`
(nama `example.t` pun sah).

Perincian:
- Format berkas: `tests/unit/common/*.test` (`.t` juga sah) — satu suite datar
  bersama `*.sh` (design/unit_test.md §1: fixture adalah input runner, bukan
  pemilik tes). Discovery runner: nama sama → `.test`/`.t` menang atas `.sh`
  (`.sh` jadi transisi selama migrasi per tes).
- Interpreter: `tests/unit/lib/dsl.sh` — membaca `.test` dan mengeksekusi
  klausanya; dipanggil `unit_dev.sh` untuk berkas `.test`, sama seperti ia
  memanggil `*.sh` hari ini. Runtime boleh tetap shell; yang dilarang bash
  adalah **isi berkas `.test`**.
- Parser: `scripts/lib/parser.sh` — modul **parse-only terisolasi**
  (tokenisasi + heredoc + tumpukan blok, tanpa eksekusi apa pun).
  `unit_dev.sh`/`unit.sh` TIDAK meng-consume-nya sampai M0 stabil, sehingga
  suite yang berjalan hari ini tak tersentuh; `dsl.sh` nanti tinggal
  mengeksekusi event hasil parse modul ini.
- Parser dibangun bertahap (milestone M0–M3, lihat #Parser bertahap); tiap
  milestone diuji konforman sebelum dipakai memigrasi tes.
- Env yang diwarisi runner tetap sama: `UNIT_REPO`, `UNIT_TMP`, `UNIT_BASE`,
  `UNIT_JOBS`, `UNIT_DISCOVERY` (sources.tsv hasil `-w -g compdb`, §6).
- `classes.conf` tetap wajib: `.test` juga harus terdaftar `[cold]`/`[snap]`.

---

# Prinsip rancangan

1. **Objek, bukan verba datar.** Perilaku dikelompokkan ke objek; satu bentuk
   pernyataan: `<objek>.<aksi> [argumen…]`. Objek baru menambah kosakata tanpa
   menambah bentuk syntax.
2. **Kendali aliran hanya di lapis `@`.** `@if @while @for` adalah direktif
   blok ber-`@end` — bukan shell. Tidak ada shell inline di berkas `.test`.
3. **Ekspresi menggantikan cabang.** Ternary `kondisi ? a : b` (dipakai lewat
   `@return`, di fragmen `@include` maupun badan utama) adalah alternatif
   if-else bercabang.
4. **Nama berkas tidak di-hardcode.** Selektor membaca `sources.tsv` (warisan
   `lib/layout.sh`): miniws `core/src/core.c`, rupaws `core/src/main.c` — satu
   tes untuk semua target.
5. **PRECONDITION bukan FAIL.** Prasyarat lewat `@guard` dan selektor berakhir
   `PRECONDITION` rc 0 (kategori PRE), bukan kegagalan satria.
6. **Grammar verdict runner tidak berubah.** Runtime menulis `PASS:` / `FAIL:` /
   `PRECONDITION` yang dihitung `unit_runner.sh` apa adanya, plus ringkasan
   `lulus: N gagal: M`.
7. **Kelas [snap] melarang penghapusan state.** `work.clean` ditolak runtime
   pada `[snap]` (aturan classes.conf ditegakkan, bukan hanya komentar).
8. **Kesalahan selalu bernomor baris.** `FAIL <nama>: <file>.test:<baris>: <pesan>`.
9. **Generalisasi hemat.** Objek/aksi/direktif baru hanya bila ≥ 2 tes
   membutuhkannya.

---

# Model eksekusi: persiapan → aksi → tanggapan

Tiga peran Given/When/Then dari draft awal kini diekspresikan lewat objek:
pernyataan sudah cukup jelas dari objeknya, dan asersi berkumpul di
`@response` — prefiks `_G/_W/_T` diganti sepenuhnya (keputusan desain 1).

| Peran | Diekspresikan lewat |
| --- | --- |
| Given (menata panggung) | `work`, `fixture`, `file`, `env`, `time`, `stage` |
| When (aksi yang diuji) | `run` (build/cmd/exec/bg/tool), mutasi `file` |
| Then (tanggapan) | blok `@response` + `@guard` |

Kaidah penulisan:
- Satu baris = satu pernyataan atau satu direktif, selalu berbentuk
  `<objek>.<aksi> [argumen…]` — termasuk asersi di dalam `@response` (tak ada
  bentuk spasi). Komentar `#` diteruskan ke log; baris kosong diabaikan.
- Blok direktif selalu ditutup `@end`; isi berkas memakai heredoc
  `file.write <path> <<T … T`: penutup `T` sendirian di barisnya, marker bebas
  asal unik (`T`, `EOF`), isi diteruskan apa adanya — baris `#` di dalam
  heredoc adalah isi, bukan komentar. Penutup opsional: tanpa penutup, isi
  berakhir di EOF (sah).
- Urutan baris = urutan eksekusi; `@response` dinilai segera.
- Label run: `as <nama>` di akhir pernyataan `run.*`; tanpa `as` → label
  `last` (ditimpa run berikutnya).

---

# DSL format

## Tiga lapis syntax

| Lapis | Isi | Bentuk |
| --- | --- | --- |
| L1 struktur | `@suite @fixture @purpose @requires @keep` | direktif header |
| L2 pernyataan | objek aksi + objek hasil, blok `@guard @response @include @set @get @find @if @while @for @return` | `<objek>.<aksi>`, blok `… @end` |
| L3 ekspresi | selektor, predikat, pembanding, ternary, interpolasi `{…}` | nilai di dalam argumen/kondisi |

## Direktif (L1 + kendali)

| Direktif | Makna | Catatan |
| --- | --- | --- |
| `@suite <mode,…>` | `workspace`, `project`, atau keduanya | Dua mode = badan dieksekusi dua kali, dir kerja berakhiran `.workspace`/`.project`; `run.build` menyesuaikan mode (`-w` di workspace) |
| `@fixture <nama>` | Salin `tests/<nama>` sebagai input | Tanpa `@fixture` = proyek authored lewat `file.write` |
| `@purpose "<judul>"` | Judul tes, dicetak di header log | |
| `@requires cold\|snap` | Deklarasi kelas; validasi silang `classes.conf` | Tidak cocok = run ditolak |
| `@keep` | Dir kerja disimpan saat FAIL (debug) | Default: dihapus otomatis (trap) |
| `@include <path> [as <nama>]` | Sisip klausa dari berkas lain (setup bersama); dengan `as` → nilai `@return` fragmen tersimpan sebagai `{<nama>}` | Bukan `source` shell — yang disisip klausa DSL, bukan kode bash |
| `@guard <ekspresi>` | Salah → `PRECONDITION <klausa>` rc 0 | Selain guard otomatis dari selektor |
| `@response [label] … @end` | Kumpulan asersi atas hasil run; tiap baris = satu verdict | Barisnya aksi objek hasil; label default `last` |
| `@if <ekspresi> [@else …] @end` | Cabang | Ekspresi L3 |
| `@while <ekspresi> @end` | Ulang selalu benar | Untuk menunggu: polling + `time.tick` |
| `@for <var> in <rentang\|koleksi> @end` | Rentang `1..30`, koleksi `project(p).sources` atau `{nama}` hasil `@find` | `{var}` diinterpolasi ke argumen & isi heredoc |
| `@set <nama> <ekspresi>` | Simpan nilai skalar; diakses lewat `{<nama>}` | Ternary boleh di ekspresinya |
| `@find <nama> <pola>` | Simpan koleksi path cocok pola glob (relatif dir kerja) | Dipakai `@for p in {nama}` |
| `@get <nama>` | Baca/catat nilai variabel ke log (tracing) | Akses dalam argumen tetap `{<nama>}` |
| `@return [ekspresi]` | Fleksibel: di fragmen `@include` = nilai balik; di badan utama = berhenti lebih awal | `@return fail` memaksa verdict FAIL; selain itu verdict dari penghitung |

## Objek aksi

| Objek.Aksi | M | Makna | Dipetakan dari `tests/unit/common` |
| --- | --- | --- | --- |
| `fixture.copy` | M0 | Salin fixture ke dir kerja (`cp -a`) | cache_noop, cdeps_group, crud_touch_delete, … |
| `work.new [nama]` | M0 | Dir kerja bersih `$UNIT_TMP/<nama>` + trap | boilerplate semua tes (default: nama berkas) |
| `work.clean all\|state` | M0 | Buang artefak — **hanya [cold]**; `all` = build bin lib modules .satria, `state` = .satria | selective_noop_full, cdeps_flag (all), build_log (state) |
| `file.write <path> <<T … T` | M0 | Tulis berkas via heredoc — penutup `T` opsional (tanpa penutup = berakhir di EOF); heredoc hanya sah di sini | report_binaries_uptodate, faststate_external_header |
| `file.append <path> <teks>` | M0 | Tambah baris — konfigurasi MAUPUN mutasi sumber | cache_noop (`flags = MMD, MP`), cdeps_group, workspace_selective_target |
| `file.touch <path>` | M0 | Ubah mtime saja → PATCH stamp tanpa rebuild | crud_touch_delete (update) |
| `file.delete <path>` | M0 | Hapus berkas/sumber | crud_touch_delete (delete) |
| `file.copy <src> <dst> [lf\|crlf]` | M1 | Salin + konversi akhiran baris | cmdhelp_crlf (`awk` ke CRLF) |
| `env.set <VAR>=<nilai>` / `env.unset` | M1 | Env untuk run berikut | NO_COLOR (umum), SATRIA_CACHE_NOOP=0, SATRIA_CACHE_TRACE |
| `time.tick` | M0 | Tunggu tick mtime (bukan `sleep 1`) | `wait_tick` |
| `time.mark <path> as <n>` | M1 | Rekam mtime berkas | embed_stamp_skip (`cp emb.orig` + stat) |
| `run.build [as <label>]` | M0 | Run satria mode ini (`-w` di workspace) + `-j$UNIT_JOBS` | `"$SAT" -j2 -w`, `"$SAT" -j2` |
| `run.cmd <args…> [as <label>]` | M0 | Panggilan satria verbatim | `version`, `-w -g compdb`, `help`, `trace` |
| `run.exec <bin> [args…] [as <label>]` | M1 | Jalankan program HASIL build | embed_getembed (`bin/bade`) |
| `run.bg <args…> [as <label>]` | M3 | Run di latar + rekam startup | background (`-w &`) |
| `run.stop [label]` / `run.wait [label] [batas]` | M3 | Kendali proses run | build_resume (Ctrl+C), background |
| `run.tool <skrip> [args…] [as <label>]` | M3 | Jalankan alat repo (bukan shell inline) | `scripts/gen_tracer_src.sh` |
| `stage.set <berkas> <isi>` / `stage.log <teks>` | M3 | Rekaman staging sintetis (run.pid, run.env, plan.tsv, timing) tanpa run nyata | watcher_staging |
| `text.count <path> <pola>` / `text.aggregate <path>` | M3 | Hitung baris cocok; agregasi `sort\|uniq -c\|sort -rn\|head` | trace_summary (pipeline manual) |

## Objek hasil (asersi — di dalam `@response`)

| Objek.Aksi | M | Makna | Dipetakan dari |
| --- | --- | --- | --- |
| `rc.ok\|rc.fail` | M0 | Kode keluar run berlabel | cdeps_key_reject, version_alias |
| `out.contains\|out.not-contains <teks>` | M0 | Substring stdout+stderr | "Nothing to build", "Cache valid", "Binaries   : 1 linked, 0 up-to-date", "(up-to-date)" |
| `out.matches <regex>` | M0 | grep -E | `"Binaries *:"` |
| `out.equals <A> <B>` | M0 | cmp keluaran dua label | version_alias (v1=v2), cdeps_group (compdb identik) |
| `file.exists\|file.absent <path>` | M0 | Keberadaan berkas | legacy_sweep, build_log, report_artifact |
| `file.same <a> <b>` | M0 | cmp dua berkas | trace_src_sync, regenerasi embedded.c |
| `file.contains <path> <pola>` | M0 | `grep -qE` isi berkas | cache_noop (format SATRIASRC2) |
| `mtime.changed\|mtime.unchanged <n>` | M1 | Bandingkan dengan `time.mark` | embed_stamp_skip |

Label artefak per run: `out.<label>`, `rc.<label>`; `NO_COLOR=1` diekspor
runtime sehingga `contains` aman dari SGR.

## Ekspresi (L3)

- **Selektor**: `project(p).source|sources|binary|dir|embedded|stamp|archive`
  (sources.tsv + micro-read Buildfile.ws, perilaku `lib/layout.sh` termasuk
  fallback proyek-pertama; `embedded.c` selalu di-skip) dan `repo(<rel>)`
  (berkas di UNIT_REPO, untuk cmdhelp/trace_src_sync), `work(<rel>)`
  (berkas/berkasar di WORK, mendukung path absolut), `ws(<kunci-Buildfile.ws>)`
  (nilai kunci workspace). Selektor yang tidak terurai di argumen →
  PRECONDITION otomatis.
- **Predikat**: `exists(<path>)`, `contains(<label>,"teks")`,
  `matches(<label>,<regex>)`, `rc(<label>)`, `lines(<path>)`,
  `count(<path>,<pola>)`, `size(<path>)` (byte, mengikuti `wc -c`),
  `changed(n)`, `unchanged(n)`, `same(a,b)`.
- **Pembanding**: `== != < > <= >= ~ !~`, `and or not`; ternary
  `<kondisi> ? <a> : <b>`.
- **Interpolasi**: `{expr}` di argumen dan isi heredoc (untuk `@for`);
- **Variabel**: `{nama}` — hasil `@set`, koleksi `@find`, atau `@include … as` —
  dipakai di argumen, isi heredoc, dan kondisi;
- **Tanpa aritmetika**: tidak ada math umum — DSL murni unit test, deklaratif
  dengan kemampuan logika terbatas (`@if`, `@while`, predikat); math akan
  mengubah DSL menjadi bahasa pemrograman, bukan bahasa uji.
  escape `\r \n \t` di literal.
- **Angka**: `lines`, `count` menghasilkan angka — cukup untuk asersi hitungan.

## Verdict & integrasi runner

- Setiap baris `@response` menulis `PASS: <klausa>` / `FAIL: <klausa> (dapat
  X, harap Y)`; ringkasan akhir `lulus: N gagal: M`; rc tes 1 bila gagal > 0 —
  pola `check()` yang sudah dihitung `unit_runner.sh`.
- Asersi gagal tidak menghentikan (penghitung); `@guard` salah →
  `PRECONDITION <pesan>` rc 0.
- `work.clean` pada [snap] → `FAIL <nama>: clean hanya untuk kelas [cold]`.
- Kesalahan parse menghentikan tes dengan nomor baris berkas `.test`.
- `unit.sh` men-discovery `common/*.test` bersama `common/*.sh`; snapshot,
  plan, timing, pause/stop/resume tidak tersentuh.

---

# Contoh terjemahan dari tests/unit/common

Ilustrasi rancangan (bukan migrasi final); teks asersi dikutip dari tes aslinya.

### report_binaries_uptodate.sh → report_binaries_uptodate.test

Proyek authored tanpa fixture (mode `project`), 41 baris bash → 20 baris:
```test
@suite project
@purpose "Ringkasan binaries: up-to-date tercatat, bukan 0 linked, 0 up-to-date"

file.write Buildfile <<T
use project
T

file.write src/main.c <<T
int main(void){return 0;}
T

run.build as cold
run.build as noop1
@response noop1
  out.contains "Binaries   : 1 linked, 0 up-to-date"
@end

file.append src/main.c "/* komentar */"    # object identik -> link dilewati
run.build as touch
@response touch
  out.contains "(up-to-date)"
@end

run.build as noop2
@response noop2
  out.contains "Binaries   : 0 linked, 1 up-to-date"
@end
```

### cache_noop.sh → cache_noop.test

Tes [snap]: tanpa `work.clean`, mulai dari snapshot; mutasi sumber cukup
`file.append`/`file.touch` — tidak ada verba `edit` terpisah:
```test
@suite workspace
@fixture miniws
@purpose "Cache no-op: sentuh tetap no-op; edit isi membangun"
@requires snap

file.append Buildfile.ws "flags = MMD, MP"   # cache butuh file .d (-MMD)
time.tick                                    # hindari racy mtime snapshot

run.build as noop1                           # laporan STATUS build terakhir
@response noop1
  out.contains "Nothing to build"
  out.contains "Cache valid"
@end

file.touch project(core).source              # mtime berubah, isi sama -> PATCH
run.build as noop2
@response noop2
  out.contains "Nothing to build"
@end

file.append project(core).source "/* edit */"  # isi berubah -> build ulang
run.build as warm
@response warm
  out.not-contains "Nothing to build"
@end
```

### cdeps_group.sh → cdeps_group.test

Query `compdb` sebagai pembanding + validasi pesan bernomor baris:
```test
@suite workspace
@fixture miniws
@purpose "Grup cdeps: gabungan/tumpang tindih setara compdb; salah bentuk ditolak"

fixture.copy
run.cmd -w -g compdb as base

file.append Buildfile.ws "cdeps = {a, b}"    # setara a.cdeps = b && b.cdeps = a
run.cmd -w -g compdb as grouped
@response grouped
  rc.ok
  out.equals base grouped
@end

file.append Buildfile.ws "cdeps = {solo}"    # grup satu anggota: ditolak
run.cmd -w -g compdb as solo
@response solo
  rc.fail
  out.contains "Buildfile.ws:"
  out.contains "cdeps"
@end
```

### embed_stamp_skip.sh → embed_stamp_skip.test

Kelas [cold] + asersi mtime via `time.mark`:
```test
@suite workspace
@fixture miniws
@purpose "Stamp arsip: isi sama dilewati; isi baru hanya regenerasi embedded.c"
@requires cold

work.clean all
run.build as cold
@response cold
  file.exists project(bade).stamp
@end

time.mark project(bade).embedded as m0
time.mark project(bade).binary as b0

run.build as noop                            # arsip tidak berubah
@response noop
  out.contains "Nothing to build"
  mtime.unchanged m0
  mtime.unchanged b0
@end

file.append project(bade).archive "isi arsip berubah"
run.build as regen
@response regen
  mtime.changed m0                           # embedded.c diregenerasi
  out.not-contains "Nothing to build"
@end
```

### Kendali aliran & fragmen (@for, @include, ternary, @if)

Build_resume (A/B) menunjukkan `@for` + interpolasi; fragmen `@include` +
`@return` ternary menggantikan if-else bercabang lintas tes:
```test
@suite project
@purpose "Build gagal melanjutkan dari kemajuan, bukan mengulang dari awal"

@for i in 1..30
  file.write src/f{i}.c <<T
int f{i}(void){return {i};}
T
@end

# sengaja gagal di satu sumber
file.write src/bad.c <<T
int bad(void){ return "x"; }
T

run.build as cold
@response cold
  rc.fail
@end

file.write src/bad.c <<T
int bad(void){ return 0; }
T

run.build as fix
@response fix
  rc.ok                                      # detail "hanya sumber gagal" mengikuti tes asli
@end
```
Fragmen bersama (nilai balik = ternary):
```test
# tests/unit/fragmen/toolchain.test
@return exists({repo}(bin/satria.exe)) ? cl : gcc
```
```test
@include fragmen/toolchain.test as tool      # {tool} = "cl" atau "gcc"
@if {tool} == cl
  # ekspektasi khusus MSVC
@else
  # ekspektasi gcc/clang
@end
```
Menunggu proses latar (`@while` + polling, fase M3):
```test
run.bg -w as bg
@while not exists(.satria/background.startup)
  time.tick
@end
run.cmd watch as w
@response w
  out.contains "summary"
@end
```

---

# Pemetaan cakupan tests/unit/common → milestone

| Tes | Kelas | @suite | M | Aksi kunci |
| --- | --- | --- | --- | --- |
| build_log | cold | workspace | M0 | `work.clean state`, `file.absent` |
| cache_noop | snap | workspace | M0 | contoh §terjemahan |
| cdeps_flag | cold | workspace | M0 | `rc fail` + pesan penyedia |
| cdeps_group | cold* | workspace | M0 | `run.cmd -w -g compdb`, `out.equals` |
| cdeps_key_reject | cold* | workspace | M0 | `rc fail`, pesan baris |
| crud_touch_delete | snap | project,workspace | M0 | `file.touch/delete`; dua mode |
| faststate_external_header | snap | project | M0 | `file.write` blok authored |
| legacy_sweep | snap | workspace | M0 | `file.absent` path lama + no-op |
| opts_sources_sync | snap | workspace | M0 | mutasi berkas cache + sync route |
| report_artifact | cold | workspace | M0 | `file.exists` + no-op display |
| report_binaries_uptodate | snap | project | M0 | contoh §terjemahan |
| router_noop | snap | workspace | M0 | `file.append` flags + "Cache valid" |
| selective_noop_full | cold | workspace | M0 | `work.clean all` + closure selektif |
| semantic_header_workspace | cold* | workspace | M0 | dua `work.new` (unused/consumed) |
| version_alias | snap | — | M0 | `run.cmd version/--version`, `out.equals` |
| workspace_selective_target | cold* | workspace | M0 | selektor core + bade |
| embed_getembed | cold | workspace | M1 | `run.exec` program hasil build |
| embed_stamp_skip | cold* | workspace | M1 | contoh §terjemahan (time.mark/mtime) |
| p_meta_noop | snap | workspace | M1 | `env.set SATRIA_CACHE_TRACE` + contains |
| cmdhelp_crlf | snap | — | M1 | `file.copy crlf` + escape `\r` |
| build_resume | cold | project | M2 | `@for 1..30`, interpolasi, fail/fix |
| background | cold | project,workspace | M3 | `run.bg`, `@while` polling, replay watch |
| watcher_staging | snap | — | M3 | `stage.set/log`, multi-watch, lock basi |
| trace_summary | cold* | workspace | M3 | `text.count/aggregate` vs `satria trace` |
| trace_src_sync | snap | — | M3 | `run.tool` + `file.same` + `repo()` |

`*` = klasifikasi "sementara cold" di classes.conf. Sampai milestone-nya
tercapai, tes itu tetap `.sh` (transisi, bukan pengecualian desain).

---

# Parser bertahap

| Milestone | Isi parser | Uji konforman sebelum migrasi |
| --- | --- | --- |
| M0 | Header + pernyataan objek dasar + `@response/@guard/@requires/@include/@set/@get/@find` (tanpa ekspresi) + selektor source/binary + verdict | contoh kecil tiap aksi M0 |
| M1 (✓) | Evaluator ekspresi (predikat, pembanding, ternary), variabel `@set/@get/@find`, `@return` badan utama, interpolasi `{}`, escape, selektor embed + `repo()`/`work()`/`ws()` | tabel ekspresi benar/salah — `tests/unit/dsl/` 8/8 |
| M2 | `@if/@else`, `@for` + rentang/koleksi, `@while` | loop kecil + kondisi bertingkat |
| M3 | `run.bg/stop/wait`, `stage`, `text`, `run.tool` | staging sintetis + siklus proses |

- Runtime M0 cukup parser baris (`read`); evaluator M1+ diekstrak jadi fungsi
  tersendiri agar mudah diuji tanpa menjalankan satria.
- Tiap milestone selesai = berkas parser-tests lulus, baru tes milik milestone
  dimigrasi ke `.test` dan `classes.conf` diperbarui.

**Isolasi parser** (sudah berjalan). Parser dikembangkan terpisah sebagai
`scripts/lib/parser.sh` — parse-only: tokenisasi per baris, mesin status
heredoc, tumpukan blok, validasi objek/aksi, kesalahan bernomor baris; tanpa
mengeksekusi apa pun. Selama stabilisasi `unit_dev.sh`/`unit.sh` TIDAK
meng-consume modul ini — suite `.sh` hari ini tak tersentuh. Konforman diuji
berdiri sendiri:
```
bash tests/unit/parser/run.sh            # positif + negatif (# expect:)
bash scripts/lib/parser.sh <file.test>   # jejak event TSV untuk inspeksi
```
Event jejak: `HDR DIR BLOCK ENDB STMT OPEN TEXT CLOSE CMT`, semua bernomor
baris. Status: `unit_runner.sh` mengonsumsi parser via `tests/unit/lib/dsl.sh`
(staging `$STAGE/lib/parser.sh`); `unit_dev.sh` tetap tidak menyentuh keduanya.

---

# Keputusan desain (terkunci)

1. **Peran Given/When/Then**: prefiks `_G/_W/_T` diganti sepenuhnya objek +
   `@response`; peran persiapan → aksi → tanggapan tetap sebagai konsep baca.
2. **Variabel disediakan**: `@set` (skalar), `@find` (koleksi glob),
   `@get` (baca/catat ke log); akses lewat `{nama}` di argumen, isi heredoc,
   dan kondisi.
3. **`@return` fleksibel**: nilai balik fragmen `@include` (ternary) dan
   berhenti lebih awal di badan utama; `@return fail` memaksa FAIL, selain itu
   verdict tetap dari penghitung.
4. **`run.tool` generik**: satu aksi untuk semua alat repo — bukan verba
   spesifik per alat.
5. **`@include` berparameter (makro)**: ditunda — kebutuhannya ditutupi
   variabel (kembali ke poin 2).
6. **Tanpa aritmetika umum**: DSL murni unit test — deklaratif dengan
   kemampuan logika terbatas (`@if`, `@while`, predikat); math akan mengubah
   DSL menjadi bahasa pemrograman, bukan bahasa uji.
7. **Ekstensi**: `*.test` sah, `*.t` juga sah — parser dan runner menerima
   keduanya.
8. **Heredoc fleksibel**: penutup opsional — tanpa penutup, isi berakhir di
   EOF (sah); marker bebas satu token; hanya pada `file.write`.
9. **`satria test` (parser berpindah ke satria)**: menunggu M0–M3 selesai dan
   stabil — sudah bagian dari rencana.

Tersisa terbuka (kecil): varian `<<-T` buang-indentasi; kapan `.t` dipakai
resmi untuk tes di `tests/unit/common`.

---

# Daftar implementasi

- [x] `scripts/lib/parser.sh` — parse-only: tokenisasi seluruh kosakata + heredoc + tumpukan blok
- [x] Isolasi parser: tidak di-consume `unit_dev.sh`; konforman mandiri `tests/unit/parser/run.sh`
- [x] M0: parser baris + header + objek dasar (work, fixture, file, time.tick, run.build/cmd) — `tests/unit/lib/dsl.sh`
- [x] M0: `@response/@guard/@requires/@include/@set/@get/@find/@keep` + verdict PASS/FAIL/PRECONDITION + ringkasan
- [x] M0: selektor `project(p).source/binary` dari sources.tsv + guard otomatis
- [x] M0: gate `work.clean` pada [snap]; kesalahan parse bernomor baris
- [x] M0: integrasi `unit.sh` (discovery `*.test`/`*.t` + `*.sh`, nama sama `.test`/`.t` menang; staging `parser.sh`; eksekusi via `dsl.sh` dengan env `UNIT_CLASS/UNIT_FIXTURE/UNIT_DISCOVERY/SATRIA`)
- [x] M0 pilot: report_binaries_uptodate.test (3/3) + cache_noop.test (24/24) lewat `unit.sh`; asersi cache yang butuh penulisan-ulang berkas (alamat nol, self-heal SATRIASRC1) menunggu M1
- [x] M1: evaluator ekspresi + ternary; variabel `@set/@get/@find`; `@return` badan utama; interpolasi `{}`; escape; `time.mark`/mtime; `run.exec`; `env.set`; `file.copy crlf`; `repo()`/`work()`/`ws()`; predikat `size()` — kit konforman `tests/unit/dsl/` 8/8
- [x] M1 pilot: embed_stamp_skip.test (8/8) + cmdhelp_crlf.test (7/7) + embed_getembed.test (24/24) lewat `unit.sh` miniws; `p_meta_noop` menunggu M2 (butuh penulisan-ulang cache via `sed -i`, tak terekspresikan DSL)
- [ ] M2: `@if/@else`, `@for` + interpolasi blok, `@while`
- [ ] M2 pilot: build_resume.test (+ fragmen toolchain.test)
- [ ] M3: `run.bg/stop/wait`, `stage`, `text.count/aggregate`, `run.tool`
- [ ] M3 pilot: background.test + watcher_staging.test + trace_summary.test + trace_src_sync.test
- [ ] Contoh acuan `example.test` untuk penulis tes baru

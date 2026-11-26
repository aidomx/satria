# Cold build (workspace dari nol)

## Domain flow

startup + model (`workspace.c`, `ws_startup.c`, `ws_parse.c`, `ws_config.c`)
  -> gerbang no-op (`ws_cache_noop.c` `wsCacheNoopValid`) — miss: belum ada state
  -> worklist (`ws_worklist.c`) — urut topo + level dependensi
  -> rencana fase library (`ws_plan.c` `wsPlanBegin`)
  -> batch (`ws_batch.c` -> `runners/`) — serial di TTY, paralel siap-dep untuk pipe/berkas
  -> per proyek (`ws_exec.c` `wsRunProject`)
       config (`wsBuildConfig`) -> walk sumber (`cmds/state.c` `configWalkSources`)
       -> pass compile (`cmds_pass.c` `cmdsBuildPassCompile`)
       -> library (`cmds_lib.c`) / arsip (`embed_archive.c`, `pack/`)
  -> rencana fase binary (`wsPlanBegin`) -> batch compile+link (`cmds_passfinish.c`)
  -> simpan state (`ws_cache_save.c`, `cmds/state.c`, `deps/`, `cache/artifact.c`)
  -> laporan (`log/build_report.c`, `log/report_blob.c`)

## Responsibilities

startup:
  Memilih mode workspace, memuat `WsBuildfile`, menyusun model proyek + graf
  `depends_on`/`cdeps`, lalu menghitung closure proyek `needed` (penuh atau
  `-w <nama>`). Tidak ada fingerprint yang bisa dipakai, jadi gerbang no-op
  dipastikan miss dan jalur penuh yang dijalankan.

gerbang:
  `wsCacheNoopValid` membaca `sources.cache` + `fingerprint.cache` +
  `artifact.cache`. Cold build tidak punya entri yang cocok, sehingga tidak
  ada early-exit. Walk pertama juga miss di `sources.cache` (trace
  `[walk] <dir> ext=... -> miss`), lalu record ditulis untuk run berikutnya.
  `extension` proyek (mis. `mod.extension = .rp`) hanya mempersempit walk
  freshness/record; isi arsip tetap ditentukan `pattern`/`exclude`.

worklist + rencana:
  Worklist memastikan produsen arsip/library berjalan sebelum konsumen
  (`rupamod -> bade`). Rencana hanya memprecompute verdict bila runner paralel;
  runner serial memvalidasi per proyek tepat sebelum eksekusi karena verdict
  di depan bisa basi saat dependensi membangun lebih dulu.

eksekusi:
  Fase library mengompilasi semua sumber dan membangun `.a` (`.so` ditunda ke
  fase binary agar link-nya menindih link binary). Proyek kemasan
  (`output.binary = false`) membangun arsip di fase ini, tanpa satu pun `.c`.
  Fase binary mengompilasi sisa proyek lalu me-link binary setelah SEMUA
  library tersedia (cdeps boleh siklik).

simpan:
  `sources.cache` menyimpan hasil discovery per direktori, `deps.cache`
  snapshot hash sumber + closure `.d`, `ws.state`/`lib.cache` stamp fase,
  `fingerprint.cache` + `artifact.cache` keputusan no-op, dan report blob
  untuk replay STATUS. State ditulis hanya setelah build sukses.

## Invarian

- Cold build tidak boleh mewarisi object/arsip/library lama: setiap output
  yang belum ada dikompilasi/dibangun, dan kegagalan tidak menyimpan state
  sukses.
- Urutan produsen->konsumen ditentukan worklist, bukan urutan baris proyek.
- Walk sumber satu direktori hanya sekali per run, dipakai lintas proyek.
- `extension` bukan filter isi arsip: `module.json`/`module` tetap masuk
  arsip selama tidak dikecualikan.
- Output TTY memakai runner serial (progres realtime); berkas/pipe memakai
  runner paralel dengan urutan worklist untuk log deterministik.

## Related tests

- ws_plan
- build_log
- report_artifact
- embed_getembed
- embed_stamp_skip
- selective_noop_full
- wsbuildfile_legacy

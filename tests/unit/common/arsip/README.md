# Arsip skrip bash yang sudah digantikan port `.test`

Berkas di sini adalah versi bash asli yang sudah diport penuh ke DSL unit test
(`tests/unit/common/*.test`) — nama sama `.test` menang atas `.sh` saat
discovery, jadi berkas `.sh` ini tidak lagi dieksekusi oleh `unit.sh`.

| Skrip | Pengganti | Milestone |
| --- | --- | --- |
| `cache_noop.sh` | `../cache_noop.test` | M0 |
| `report_binaries_uptodate.sh` | `../report_binaries_uptodate.test` | M0 |
| `cmdhelp_crlf.sh` | `../cmdhelp_crlf.test` | M1 |
| `embed_stamp_skip.sh` | `../embed_stamp_skip.test` | M1 |
| `embed_getembed.sh` | `../embed_getembed.test` | M1 |
| `trace_src_sync.sh` | `../trace_src_sync.test` | M3 |

`.sh` yang MASIH berjalan (port parsial atau fitur proses yang belum
terekspresikan DSL) tetap di `../`:

- `background.sh` — SIGSTOP/kalibrasi Ctrl+C; subset DSL: `background_watch.test`
- `build_resume.sh` — kasus D Ctrl+C + kalibrasi; A–C: `build_resume_abc.test`
- `trace_summary.sh` — paritas hitungan `top` (ekstraksi kunci); subset: `trace_summary_basic.test`
- `watcher_staging.sh` — proses-latar non-satria + sinyal; `stage.set/log` di kit dsl
- `p_meta_noop.sh` — butuh penulisan-ulang cache (`sed -i`), tak terekspresikan DSL

Lihat `design/unit_test_dsl.md` untuk spesifikasi DSL dan status milestone.

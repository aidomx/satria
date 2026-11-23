#pragma once
/* Watcher unit test native (M4d, design/unit_test_dsl.md) — padanan C dari
   scripts/lib/unit_watcher.sh. Memantau progres run di area staging
   (targets/<target>/log) tanpa menyentuh repo.

   `target` boleh kosong: satu-satunya run aktif dipilih otomatis.
   `optLog` boleh kosong: hanya stdout. `stage` adalah UNIT_STAGE. */

int testWatchMain(const char *target, const char *optLog, const char *stage);

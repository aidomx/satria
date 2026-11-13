#pragma once
/* satria test: jalankan unit test repo lewat scripts/unit.sh (delegasi, tanpa
   porting — parser/runner DSL masih bash; porting ke C = milestone M4 di
   design/unit_test_dsl.md).

   satria test <args unit.sh…>
   Semua argumen diteruskan verbatim, mis.:
     satria test bin/satria miniws [-b] [-j N] [-o log] [-t tes] [--clean]
   Root repo dicari dari direktori binary ini (…/bin → repo root = parent),
   sehingga `satria test` jalan dari direktori mana pun. Exit code unit.sh
   diteruskan sebagai exit code satria. */
int cmdTest(const char *const *args, int nargs, const char *argv0);

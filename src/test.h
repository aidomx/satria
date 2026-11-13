#pragma once
/* satria test: jalankan unit test repo lewat scripts/unit.sh (delegasi, tanpa
   porting — parser/runner DSL masih bash; porting ke C = milestone M4 di
   design/unit_test_dsl.md).

   satria test <binary> <target> [opsi…]
   Shorthand: bila argumen pertama bukan opsi, bukan kata mode unit_dev, dan
   bukan path binary, binary satria itu sendiri disisipkan:
     satria test miniws            == satria test bin/satria miniws
     satria test miniws -t cache_noop
   Path binary relatif-repo diabsolutkan (unit.sh me-resolve relatif cwd).
   Root repo dicari dari direktori binary ini (…/bin → repo root = parent),
   sehingga `satria test` jalan dari direktori mana pun. Exit code unit.sh
   diteruskan sebagai exit code satria. */
int cmdTest(const char *const *args, int nargs, const char *argv0);

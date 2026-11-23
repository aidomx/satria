#pragma once
/* Runner unit test native (M4c, design/unit_test_dsl.md) — padanan C dari
   scripts/lib/unit_runner.sh untuk jalur foreground: staging tests/unit,
   klasifikasi classes.conf, fixture+snapshot, discovery compdb, eksekusi
   tes .test/.t/.sh, log per tes, timing.tsv, dan ringkasan.
   `root` adalah root repo (pemilik tests/ dan scripts/); `bin` adalah path
   binary satria yang dipin ke staging dan dipakai menjalankan tes.
   CLI: `satria test --run <target> [-j N] [-s tes[,tes...]] [-o log] [--clean]`. */

int testRunnerMain(int argc, const char *const *argv, const char *root, const char *bin);

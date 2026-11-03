#ifndef SATRIA_V0_1_0_COMPDB_H
#define SATRIA_V0_1_0_COMPDB_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "util.h"
#include "log/build_report.h" /* RepCompdb: status return writeCompdb */

bool compdbEnabled(const Config *c);
/* Kembalikan status CompDB eksplisit (design/build_report.md §6) tanpa
   mengubah keputusan cache. Caller (jalur build) menyimpannya ke report. */
RepCompdb writeCompdb(const Config *c, List *srcs);

/*
 * writeCompdbMulti — merged workspace: beberapa (Config, srcs) dirender
 * berurutan (topo order proyek) menjadi SATU compile_commands.json di root
 * workspace. Skip-write bila fingerprint gabungan cocok cache — no-op
 * diam tanpa sentuh disk (design/workspace_v1.md §No-op dan CompDB).
 */
RepCompdb writeCompdbMulti(const Config *const *cfgs, List *const *srcs, int count);

/* Fingerprint konten merged (hash gabungan seluruh entri) — dipakai
   workspace_log untuk keputusan skip-write TANPA merender file. */
uint64_t compdbMultiFingerprint(const Config *const *cfgs, List *const *srcs, int count);

#endif /* SATRIA_V0_1_0_COMPDB_H */

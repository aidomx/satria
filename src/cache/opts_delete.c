/* Shared opts.cache implementation: single-project and workspace. */
#include "opts_internal.h"
#include "../cmds/cmds_internal.h"
#include "../portability.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void wsRouterDrop(void) {
  CsStore *st = csOpen(WS_ROUTER_FILE);
  if (!st) return;
  csDelete(st, csHash(WS_ROUTER_KEY), csHash(WS_ROUTER_NAME));
  csDelete(st, csHash(WS_ROUTER_KEY), csHash(WS_SOURCE_ROUTE));
  csCommit(st);
  csClose(st);
}

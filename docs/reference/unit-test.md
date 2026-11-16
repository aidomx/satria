# Unit Tests

How to run the satria test suite and how to write tests for it. The suite
lives in `tests/unit/`; the authoritative specs are
[`tests/unit/README.md`](https://github.com/aidomx/satria/blob/main/tests/unit/README.md)
and
[`design/unit_test_dsl.md`](https://github.com/aidomx/satria/blob/main/design/unit_test_dsl.md)
— this page is the working summary.

## Running

`satria test` is a thin delegation to `scripts/unit.sh` (not available on
Windows). If the first argument is not an option or a path to an existing
executable, the running satria binary is inserted automatically:

```bash
satria test miniws                # full sweep (cold + snap), binary = satria itself
satria test miniws -t cache_noop  # one test only
satria test bin/satria miniws -b  # explicit binary, baseline run
satria test miniws --clean        # remove the target's staging first
satria test complexws -j 2        # whole suite, 2 jobs
satria test -w                    # watcher: follow an active run (Ctrl-C to stop)
```

- `<target>` is a **fixture name** under `tests/` (`miniws`, `complexws`,
  `rupaws`, …) — not a path. The runner copies the fixture into
  `/tmp/satria-unit/targets/<target>/{fixture,snap,work,t}` and writes logs
  to `.../<target>/log/`.
- `satria test` works from any directory inside the repo (the repo root is
  derived from the running binary).
- `-w` only **watches progress** — it never starts a run. Status flows per
  test from the staging logs; the summary prints once when the run ends.
- The exit code of `scripts/unit.sh` becomes the exit code of satria.

## Suite layout

One flat suite; **fixtures are runner input, not test owners**. A test that
needs a specific structure declares it and ends as `PRECONDITION` when the
fixture does not satisfy it.

| Path | Content |
| --- | --- |
| `tests/unit/common/` | all `*.test`/`*.t` tests, run for every target |
| `tests/unit/lib/layout.sh` | fixture-aware helpers (project sources, binaries, archives, PRECONDITION) |
| `tests/unit/classes.conf` | classification of every test: `[cold]` or `[snap]` — mandatory; the runner rejects unlisted tests |

`[cold]` tests start from a clean state (build from scratch); `[snap]` tests
reuse the prepared snapshot (warmstart, no-op and state-mutation behavior).

## Writing a test — the `.t` DSL

Test files use the `.test` or `.t` extension — both are valid and
equivalent. The canonical reference file is
[`tests/unit/common/example.t`](https://github.com/aidomx/satria/blob/main/tests/unit/common/example.t);
read it before writing a new test.

The syntax has three layers:

| Layer | Content |
| --- | --- |
| L1 structure | `@suite`, `@fixture`, `@purpose`, `@requires`, `@keep` |
| L2 statements | action objects + result objects, control blocks (`@response`, `@guard`, `@include`, `@set`, `@get`, `@find`, `@if`, `@while`, `@for`, `@return`) |
| L3 expressions | selectors, predicates, comparisons, ternary, `{name}` interpolation |

Key directives:

- `@suite project` / `workspace` (or both — the body runs twice, with the
  work dir suffixed `.project` / `.workspace`; `run.build` adapts its mode)
- `@purpose "title"` — printed in the log header
- `@requires cold|snap` — must match `classes.conf` or the run is rejected
- `@keep` — keep the work directory on FAIL for debugging (removed by
  default)
- `@guard <expr>` — false ends the test as `PRECONDITION` (rc 0)

A minimal test:

```text
# minimal.test — builds the smallest project and checks the binary
@suite project
@purpose "smallest possible project builds and runs"

file.write Buildfile <<T
use project
T
file.write src/main.c <<T
#include <stdio.h>
int main(void){ puts("halo"); return 0; }
T

run.build as cold
@response cold
  rc.ok
  file.exists bin/minimal
@end

run.exec bin/minimal as app
@response app
  rc.ok
  out.contains "halo"
@end
```

Action objects (selection — full table in the design doc):

| Action | Meaning |
| --- | --- |
| `file.write <path> <<T … T` | write a file via heredoc (also `append`, `touch`, `delete`, `copy`) |
| `work.new [name]` | fresh work dir under `$UNIT_TMP` (default: test file name) |
| `fixture.copy` | copy the target fixture into the work dir |
| `env.set <VAR>=<value>` | environment for the next run |
| `time.tick` / `time.mark <path> as <n>` | mtime tick for cache invalidation / record a stamp |
| `run.build [as <label>]` | run satria in this suite's mode |
| `run.cmd <args…> [as <label>]` | verbatim satria invocation |
| `run.exec <bin> [args…] [as <label>]` | run a program produced by the build |

Result objects (assertions — one line = one verdict, inside `@response`):

| Assertion | Meaning |
| --- | --- |
| `rc.ok` / `rc.fail` | exit code of the labeled run |
| `out.contains` / `out.not-contains <text>` | substring of stdout+stderr |
| `out.matches <regex>` | grep -E over the output |
| `file.exists` / `file.absent <path>` | artifact presence |
| `file.contains <path> <pattern>` | grep -qE the file |
| `file.same <a> <b>` | byte-identical files |
| `mtime.changed` / `mtime.unchanged <n>` | compare against a `time.mark` |

Loops and logic work inside the body: `@for i in 1..3` (with `{i}`
interpolated into arguments and heredocs), `@find` + `@for` over a glob,
`@if`/`@else`, `@while` polling, `@set` with ternaries. Literal `\r`, `\n`,
`\t` escapes are expanded in values and patterns.

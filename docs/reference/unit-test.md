# Unit Tests

How to run the satria test suite and how to write tests for it. The suite
lives in `tests/unit/`; the authoritative specs are
[`tests/unit/README.md`](https://github.com/aidomx/satria/blob/main/tests/unit/README.md)
and
[`design/unit_test_dsl.md`](https://github.com/aidomx/satria/blob/main/design/unit_test_dsl.md)
— this page is the working summary.

## Running

`satria test` is native (M4d): the runner C runs in-process, imports the
`tests/unit` tree into the staging area, classifies tests from
`classes.conf`, prepares the fixture/snapshot, and executes the selected
tests. `scripts/unit.sh` is no longer involved. If the first argument is not
an option or a path to an existing executable, the running satria binary is
inserted automatically:

```bash
satria test miniws                  # full sweep (cold + snap), binary = satria itself
satria test miniws -s cache_noop    # one test (comma-separated for several)
satria test miniws -j 4             # whole suite, 4 jobs
satria test miniws -o log/unit.log  # extra log copy
satria test miniws --clean          # remove the target's staging first
satria test miniws -b               # background run (PID/log/Stop printed)
satria test -w                      # watch the only active run (Ctrl-C to stop)
satria test -w miniws               # watch one target
```

- `<target>` is a **fixture name** under `tests/` (`miniws`, `complexws`,
  `rupaws`, …) — not a path. The runner copies the fixture into
  `$UNIT_STAGE/targets/<target>/{fixture,snap,work,t}` (default
  `UNIT_STAGE=/tmp/satria-unit`) and writes logs to `.../<target>/log/`.
- `satria test` works from any directory inside the repo (the repo root is
  derived from the running binary or the cwd when it contains `tests/unit`).
- `-w` only **watches progress** — it never starts a run. Status flows per
  test from the staging logs; the summary prints once when the run ends.
  Multiple watchers may run at the same time (one lock per watcher under
  `$UNIT_STAGE/watch/<pid>`); stale locks are cleaned up.
- The exit code of the runner (or of the watched/controlled run) is the exit
  code of satria.

### Controlling a run

`stop`, `pause`, and `resume` are native and operate on the staging of the
named target:

```bash
satria test pause miniws    # stop before the next test; writes RUN_PAUSED=1
satria test stop miniws     # terminate the run tree; keeps plan/timing
satria test resume miniws   # run the remaining plan (plan.tsv − timing.tsv)
```

`pause` is graceful: the test currently running finishes, then the runner
stops without `RUN_ENDED` and prints the remaining count. `stop` kills the
run tree (TERM then KILL) and removes `run.pid`, `bg.pid`, and the snapshot
pid marker, leaving `plan.tsv`/`timing.tsv` intact so `resume` can continue.
`resume` refuses to start while a run is still active and uses the binary
recorded in `run.env` (`RUN_ORIGINAL`, falling back to `RUN_BINARY`).

## Suite layout

One flat suite; **fixtures are runner input, not test owners**. A test that
needs a specific structure declares it and ends as `PRECONDITION` when the
fixture does not satisfy it.

| Path | Content |
| --- | --- |
| `tests/unit/common/` | all `*.test` tests, run for every target |
| `tests/unit/common/arsip/` | original `.sh` scripts fully replaced by `.test` (reference only, never executed) |
| `tests/unit/lib/layout.sh` | fixture-aware helpers (project sources, binaries, archives, PRECONDITION) |
| `tests/unit/lib/meta_tools.py` | precise cache-state mutations for `.test` (`strip-meta`, `corrupt-same-second`) |
| `tests/unit/classes.conf` | classification of every test: `[cold]` or `[snap]` — mandatory; the runner rejects unlisted tests |

`[cold]` tests start from a clean state (build from scratch); `[snap]` tests
reuse the prepared snapshot (warmstart, no-op and state-mutation behavior).

## Writing a test — the `.test` DSL

Test files use the `.test` or `.t` extension — both are valid and equivalent
(`.test` wins when both exist). The canonical reference files are
[`tests/unit/common/example.test`](https://github.com/aidomx/satria/blob/main/tests/unit/common/example.test)
and its `.t` twin; read one before writing a new test.

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
| `file.write <path> <<T … T` | write a file via heredoc (`\t` becomes TAB; also `append`, `touch`, `delete`, `copy`) |
| `work.new [name]` | fresh work dir under `$UNIT_TMP` (default: test file name) |
| `fixture.copy` | copy the target fixture into the work dir |
| `env.set <VAR>=<value>` | environment for the next run (`{var}`/selectors expanded) |
| `time.tick` / `time.sleep <s>` | mtime tick for cache invalidation / wait |
| `time.stamp as <n>` | ISO-8601 UTC timestamp in a variable |
| `time.mark <path> as <n>` | record a file mtime (`mtime.changed/unchanged`) |
| `run.build [as <label>]` | run satria in this suite's mode |
| `run.cmd <args…> [as <label>]` | verbatim satria invocation |
| `run.exec <bin> [args…] [as <label>]` | run a program produced by the build |
| `run.bg <args…> [as <label>]` | satria in the background |
| `run.stop <label> [int\|term\|stop\|cont\|kill]` / `run.wait <label> [s]` | background control |
| `run.execbg <bin> [args…] [as <label>]` | arbitrary program in the background (e.g. `sleep` as a synthetic run PID) |
| `proc.signal <pid\|pidfile> <int\|term\|stop\|cont\|kill>` | signal a process by pid file |
| `text.count <path> <pattern> [as <n>]` | count matching lines |

Result objects (assertions — one line = one verdict, inside `@response`):

| Assertion | Meaning |
| --- | --- |
| `rc.ok` / `rc.fail` | exit code of the labeled run |
| `out.contains` / `out.not-contains <text>` | substring of stdout+stderr |
| `out.matches` / `out.not-matches <regex>` | grep -E over the output |
| `file.exists` / `file.absent <path>` | artifact presence |
| `file.contains <path> <pattern>` | grep -qE the file |
| `file.same <a> <b>` | byte-identical files |
| `mtime.changed` / `mtime.unchanged <n>` | compare against a `time.mark` |

Loops and logic work inside the body: `@for i in 1..3` (with `{i}`
interpolated into arguments and heredocs), `@find` + `@for` over a glob,
`@if`/`@else`, `@while` polling (`matches(<label>, …)` also reads a
still-running background run), `@set` with ternaries. Literal `\r`, `\n`,
`\t` escapes are expanded in values and patterns.

# Incremental Builds and Dependency Tracking

Incremental correctness is one of satria's important design areas.

## `.d` dependency tracking

GNU/Clang builds can emit dependency files with:

```text
MMD
MP
```

A dependency file associates an object with the headers that affected it. satria uses this information to decide which source files are stale.

If a `.d` file is unavailable, satria can fall back to scanning `#include` relationships.

## Content-aware freshness

satria also tracks content/fingerprint state. A timestamp change does not automatically imply that the compiler must run when the relevant content is unchanged.

This makes cases such as:

```bash
touch include/foo.h
satria
```

potentially become a no-op instead of recompiling every source that includes `foo.h`.

Typical output can distinguish a touched-but-unchanged dependency from a genuinely changed one.

## Semantic header awareness

In workspace mode, a changed header does not automatically recompile every
translation unit that includes it. satria compares the header against the copy
saved at the last compile: if every differing line is only a function
prototype — and the function names do not appear anywhere in the unit's
dependency subtree — the change is semantically unused for that unit and the
unit is skipped. Any struct/enum/typedef/macro/static/inline difference falls
back to normal invalidation. Override with `SATRIA_HDR_SEM=1` (force on in all
modes) or `SATRIA_HDR_SEM=0` (off); the default is workspace-only.

Header search paths are configurable per workspace and per project
(`headers = ...`), and `headers.internal = ...` tracks shared headers as build
inputs without passing them with `-I`.

## Why this matters

The objective is not to claim that satria is always faster than Ninja. The objective is predictable incremental behavior with low overhead across environments.

The benchmark methodology should therefore look at both elapsed time and actual object recompilation count.

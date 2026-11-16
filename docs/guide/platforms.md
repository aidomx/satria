# Platforms and Bootstrap

satria is intended to work across Linux, macOS, and Windows.

## Linux/macOS

The normal installer can fetch a prebuilt binary. A source bootstrap is available with:

```bash
./install.sh --dev
```

A local C compiler such as GCC or Clang is sufficient for development bootstrap.

## Windows

Windows supports MSVC (`cl.exe`) or MinGW (`gcc`) without requiring CMake:

```powershell
.\install.ps1 --dev
```

The project is designed to keep platform-specific details inside satria rather than requiring users to reproduce platform logic in every Buildfile.

Both toolchains are exercised in CI: bootstrap, self-host build (the second run
must be a no-op), and the CLI smoke checks. `satria test` is not available on
Windows — the runner is a POSIX shell script, so it needs a shell such as Git
Bash (the CI workflow does not run the unit suite on Windows).

## Termux and proot

Termux and Debian/proot-distro are useful environments for validating portability because filesystem and process behavior can differ from a conventional desktop Linux installation.

The practical goal is not to win every benchmark on every platform. A successful satria build that remains predictable in constrained or layered environments is itself an important quality signal.

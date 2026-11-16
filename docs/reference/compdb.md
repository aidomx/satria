# Compilation Database

satria can generate `compile_commands.json` for editor and language-server tooling.

Use:

```text
output as o
o.compileCommands = auto
```

Or write/refresh it without building:

```bash
satria -g compdb
```

The `auto` mode is intended to generate or update the database when needed while avoiding unnecessary rewrites when the relevant configuration has not changed.

The benchmark suite treats compilation-database generation as a separate scenario because it measures a different concern from normal compilation.

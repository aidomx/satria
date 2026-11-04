# bench/compare/audit.sh  audit pekerjaan nyata (untimed) per tool.
# Dimuat oleh bench/compare.sh (cmp_source). Bukan skrip mandiri.

# -------------------- untimed actual-work audit -----------------------------
# GCC shim records the actual compiler driver invocations. The shim delegates
# to the real compiler; no wrapper is present during measured benchmark runs.
# Compare source coverage, compile/link counts, and important command options.
# Each tool gets an isolated clean project state. Non-GCC toolchains are marked
# unsupported rather than silently claiming equivalence.
audit_work() {
  [ "$AUDIT" = 1 ] || return 0
  local k scen label auditbin realgcc log rc summary
  realgcc=$(command -v gcc)
  auditbin="$OUTDIR/.audit-bin"
  mkdir -p "$auditbin"
  cat >"$auditbin/gcc" <<'SHIM'
#!/usr/bin/env bash
printf '%s\0' "$@" >>"$SATRIA_AUDIT_LOG"
printf '\n' >>"$SATRIA_AUDIT_LOG"
exec "$SATRIA_REAL_GCC" "$@"
SHIM
  chmod +x "$auditbin/gcc"
  : >"$AUDITCSV"
  echo 'tool,scenario,status,compiles,links,unique_sources,details' >"$AUDITCSV"
  : >"$AUDITLOG"
  echo
  echo "${C_BLD}[Actual Work Audit · untimed]${C_RST}"
  local audit_failed=0
  for k in "${TOOLS[@]}"; do
    label=$(label_of "$k")
    # Only tools whose compiler can be redirected to gcc are auditable here.
    case "$k" in
      ninja|make|bear|satria|cmake|meson) ;;
      *) echo "$label,cold,UNSUPPORTED,0,0,0,compiler interception not verified" >>"$AUDITCSV"; continue ;;
    esac
    for scen in cold noop edit; do
      reset_all
      cp "$PROJ/src/t1.c.baseline" "$PROJ/src/t1.c"
      log="$OUTDIR/.audit-$k-$scen.argv"
      : >"$log"
      # Build outside the observed window for incremental scenarios.
      if [ "$scen" != cold ]; then
        ( cd "$PROJ" && PATH="$auditbin:$PATH" SATRIA_REAL_GCC="$realgcc" SATRIA_AUDIT_LOG="$log" build_full "$k" ) >>"$AUDITLOG" 2>&1 || {
          echo "$label,$scen,PRIME_FAILED,0,0,0,initial build failed" >>"$AUDITCSV"; continue;
        }
        : >"$log"
        if [ "$scen" = edit ]; then
          # A semantic source change, not a comment-only edit.
          sed -i -E 's/(return v[0-9]+ \+ )[0-9]+;/\199999;/' "$PROJ/src/t1.c"
        fi
      fi
      if [ "$scen" = cold ]; then
        ( cd "$PROJ" && PATH="$auditbin:$PATH" SATRIA_REAL_GCC="$realgcc" SATRIA_AUDIT_LOG="$log" build_full "$k" ) >>"$AUDITLOG" 2>&1
      else
        ( cd "$PROJ" && PATH="$auditbin:$PATH" SATRIA_REAL_GCC="$realgcc" SATRIA_AUDIT_LOG="$log" tool_cmd "$k" ) >>"$AUDITLOG" 2>&1
      fi
      rc=$?
      python3 - "$log" "$AUDITCSV" "$label" "$scen" "$rc" "$NSRC" <<'PY_AUDIT'
import csv, os, sys
path, out, tool, scenario, rc, nsrc = sys.argv[1:]
nsrc = int(nsrc)
raw = open(path, 'rb').read().split(b'\n')
cmds = [[v.decode(errors='replace') for v in line.split(b'\0') if v]
        for line in raw if line]

def output_of(cmd):
    try:
        return cmd[cmd.index('-o') + 1]
    except (ValueError, IndexError):
        return ''

def source_of(cmd):
    return [x for x in cmd if x.endswith('.c') and
            (x.startswith('src/') or '/src/' in x)]

def includes_project(cmd):
    paths = []
    for i, arg in enumerate(cmd):
        if arg == '-I' and i + 1 < len(cmd):
            paths.append(cmd[i + 1])
        elif arg.startswith('-I') and len(arg) > 2:
            paths.append(arg[2:])
        elif arg == '-isystem' and i + 1 < len(cmd):
            paths.append(cmd[i + 1])
    return any(os.path.normpath(p) == 'include' or
               os.path.normpath(p).endswith('/include') for p in paths)

compiles, links, badflags = [], [], []
for cmd in cmds:
    output = output_of(cmd)
    sources = source_of(cmd)
    if '-c' in cmd and sources:
        compiles.extend(sources)
        if '-O2' not in cmd or '-std=gnu11' not in cmd:
            badflags.append('compile flags')
        if not includes_project(cmd):
            badflags.append('include path')
    elif '-c' not in cmd and output and os.path.basename(output) == 'app':
        # Only the project executable counts. CMake's compiler checks
        # (cmTC_*, CMakeFiles/CMakeScratch) are configure work, not app links.
        links.append(cmd)

expected = nsrc + 1 if scenario == 'cold' else (1 if scenario == 'edit' else 0)
expected_links = 0 if scenario == 'noop' else 1
srcset = {os.path.basename(x) for x in compiles}
if scenario == 'cold':
    expected_sources = {'main.c'} | {f't{i}.c' for i in range(1, nsrc + 1)}
elif scenario == 'edit':
    expected_sources = {'t1.c'}
else:
    expected_sources = set()
issues = []
if int(rc):
    issues.append('build exit ' + rc)
if len(compiles) != expected:
    issues.append(f'compile {len(compiles)} != {expected}')
if len(links) != expected_links:
    issues.append(f'link {len(links)} != {expected_links}')
if srcset != expected_sources:
    issues.append('source coverage mismatch')
if badflags:
    issues.append('flags: ' + '/'.join(sorted(set(badflags))))
if any(len([x for x in cmd if x.endswith('.o')]) != nsrc + 1 for cmd in links):
    issues.append('link object inputs mismatch')
status = 'PASS' if not issues else 'FAIL'
with open(out, 'a', newline='') as f:
    csv.writer(f).writerow([tool, scenario, status, len(compiles),
                            len(links), len(srcset), '; '.join(issues)])
PY_AUDIT
      summary=$(tail -n 1 "$AUDITCSV")
      case "$summary" in *",PASS,"*) printf '  ✓ %-12s %-5s compile/link validated\n' "$label" "$scen" ;; *) audit_failed=1; printf '  ✗ %-12s %-5s %s\n' "$label" "$scen" "$(printf '%s' "$summary" | cut -d, -f7-)" ;; esac
    done
  done
  reset_all
  cp "$PROJ/src/t1.c.baseline" "$PROJ/src/t1.c"
  rm -rf "$auditbin" "$OUTDIR"/.audit-*.argv
  if [ "$audit_failed" -ne 0 ]; then
    echo "  ${C_RED}Audit gagal; benchmark tidak dijalankan. Periksa $AUDITCSV dan $AUDITLOG${C_RST}"
    return 1
  fi
  echo "  ${C_GRN}Audit lulus; benchmark dimulai.${C_RST}"
}

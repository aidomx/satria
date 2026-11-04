# bench/compare/workdir.sh  direktori kerja, path berkas hasil, dan header CSV.
# Dimuat oleh bench/compare.sh (cmp_source). Bukan skrip mandiri.

# ---------------------------- direktori kerja ------------------------------
if [ -z "$OUTDIR" ]; then OUTDIR="$PWD/sys.tmp"; fi
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)
PROJ="$OUTDIR/proj"
CSV="$OUTDIR/results.csv"
MARK="$OUTDIR/.mark"
ERRLOG="$OUTDIR/stderr.log"
AUDITCSV="$OUTDIR/work-audit.csv"
AUDITLOG="$OUTDIR/work-audit.log"
rm -f "$CSV" "$ERRLOG"
echo 'label,scenario,ms,rc,rebuilt,version' >"$CSV"
SYSCSV="$OUTDIR/syscalls.csv"

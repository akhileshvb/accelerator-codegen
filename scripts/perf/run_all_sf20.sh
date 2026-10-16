#!/usr/bin/env bash
# The final SF20 runs, sequentially: for the baseline tree and then the new tree, the read sweep and then QphH@20.
#   tmux new -d -s perf 'scripts/perf/run_all_sf20.sh [--force] <cores> <out_root>'      (tmux attach -t perf)
# Refuses to start when the machine is busy (1-min load >= 4) unless --force.  Everything is logged to <out_root>/all.log;
# results are under <out_root>/{baseline,new}/{read,qphh}.  A failing step does not stop the later ones.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/common.sh"
[ "${1:-}" = --force ] && { export PERF_FORCE=1; shift; }
[ $# -eq 2 ] || { sed -n 2,6p "$0"; exit 2; }
CORES=$1; OUT=$2
perf_require_quiet
mkdir -p "$OUT"
exec > >(tee -a "$OUT/all.log") 2>&1
failed=0
step() { echo "=== $* $(date +%T)"; "$@" || { echo "FAILED: $*"; failed=1; }; }
for tree in baseline new; do
  step "$HERE/run_sf20_read.sh" "$tree" "$CORES" "$OUT/$tree/read"
  step "$HERE/run_qphh.sh" "$tree" 20 "$CORES" "$OUT/$tree/qphh"
done
echo "=== done $(date +%T), failed=$failed"
exit $failed

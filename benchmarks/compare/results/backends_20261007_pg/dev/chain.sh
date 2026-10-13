#!/usr/bin/env bash
# Unattended: read-only sweep, concurrent RF sweep, summary, tarball (JSON + logs only).
#   tmux: bash benchmarks/compare/results/backends_20261007_pg/dev/chain.sh
set -u
source ~/accel_pg_env.sh
cd ~/accelerator-codegen
OUT=benchmarks/compare/results/backends_20261007_pg
stamp() { echo "=== chain: $* $(date -Is)" | tee -a "$OUT/chain.log"; }

stamp "read start"
bash benchmarks/compare/run_backends.sh read "$OUT"
stamp "read done (exit $?)"

stamp "rf start"
READERS=4 REPEATS=3 bash benchmarks/compare/run_backends.sh rf "$OUT"
stamp "rf done (exit $?)"

bash benchmarks/compare/run_backends.sh summary "$OUT" 2>&1 | tee "$OUT/summary.txt"
stamp "summary done"

# the worker's per-apply / per-build timings (accel apply / accel rebuild lines) and any server errors
docker logs --since "$(head -1 "$OUT/dev/calibrate_start_utc.txt" | sed 's/^/2026-10-07T/')Z" accel_pg_sf20 \
  > "$OUT/server_log.txt" 2>&1
kill "$(cat "$OUT/dev/memmon.pid")" 2>/dev/null

(cd benchmarks/compare/results &&
  find backends_20261007_pg -type f \( -name '*.json' -o -name '*.log' -o -name '*.txt' \) -print0 |
  tar --null -czf ~/backends_20261007_pg.tar.gz -T -)
stamp "tarball ~/backends_20261007_pg.tar.gz ($(du -h ~/backends_20261007_pg.tar.gz | cut -f1))"

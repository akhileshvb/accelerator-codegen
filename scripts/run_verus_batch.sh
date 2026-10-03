#!/usr/bin/env bash
# Run the Verus pipeline for several queries at once in this tree, one tmux window
# (and log, pipeline/logs/q<N>.log) per query.  They share the tree's build, proof
# library and playbook; benchmarks take turns machine-wide and only
# verus.optimization.max_concurrent pipelines optimize at once.
#
#   scripts/run_verus_batch.sh [--wait] [--resume] <query> ...
#     --wait     first wait until no Verus pipeline is running (e.g. an earlier batch)
#     --resume   continue each query's earlier run
#
# Watch: tmux attach -t verus-batch   (Ctrl-b w lists the windows)
# Usage per query: pipeline/orchestrator/.venv/bin/python -m accel_pipeline.verus.usage \
#                    pipeline/artifacts/tpch/q<N>/verus_tpch_q<N>_aux
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WAIT=0; RESUME=""
QUERIES=()
for a in "$@"; do
  case "$a" in
    --wait) WAIT=1 ;;
    --resume) RESUME="--resume" ;;
    *) QUERIES+=("$a") ;;
  esac
done
[[ ${#QUERIES[@]} -gt 0 ]] || { echo "usage: $0 [--wait] [--resume] <query> ..." >&2; exit 2; }

if [[ $WAIT == 1 ]]; then
  while pgrep -f "accel_pipeline.run_pipeline .*--mode verus" >/dev/null; do
    echo "$(date +%T) waiting for the running Verus pipelines to finish..."
    sleep 60
  done
fi

SESSION=verus-batch
mkdir -p "$ROOT/pipeline/logs"
tmux has-session -t "$SESSION" 2>/dev/null || tmux new-session -d -s "$SESSION" -n overview
for q in "${QUERIES[@]}"; do
  cmd="cd '$ROOT/pipeline/orchestrator' && .venv/bin/python -m accel_pipeline.run_pipeline --query $q \
--workload tpch_q${q}_aux --mode verus --models-config pipeline/config/models.codex.yaml $RESUME \
2>&1 | tee -a '$ROOT/pipeline/logs/q$q.log'; exec bash"
  tmux new-window -t "$SESSION" -n "q$q" "$cmd"
  sleep 5  # stagger the starts (preflight, first agent calls)
done
echo "launched ${#QUERIES[@]} pipelines in tmux session $SESSION: ${QUERIES[*]}"

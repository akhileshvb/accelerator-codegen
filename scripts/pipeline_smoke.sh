#!/usr/bin/env bash
# End-to-end smoke test of the accelerator pipeline with a scripted agent in place of a model (minutes, no model
# calls): a fresh clean tree, `run_pipeline --stage all`, every stage and gate asserted, nothing repaired.
#
#   scripts/pipeline_smoke.sh --workload tpch_q14_aux --engine duckdb [--rev HEAD] [--work DIR]
#
# Run it before launching real runs. See pipeline/orchestrator/tests/smoke/pipeline_smoke.py.
set -euo pipefail
export TMPDIR="${TMPDIR:-/dev/shm}"
exec python3 "$(dirname "${BASH_SOURCE[0]}")/../pipeline/orchestrator/tests/smoke/pipeline_smoke.py" "$@"

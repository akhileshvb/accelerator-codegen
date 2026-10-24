#!/usr/bin/env bash
# Real-model canary: one query end to end in a fresh clean tree with the models of a models config, then its
# TIMELINE.md (where the time and the money went). Read that before launching a big run.
#
#   scripts/canary.sh --workload <workload> --models models.yaml [--engine <engine>] [--rev HEAD] [--work DIR] [--probe-models]
#
# Costs about $1 and 20-30 minutes. See pipeline/orchestrator/tests/smoke/canary.py.
set -euo pipefail
export TMPDIR="${TMPDIR:-/dev/shm}"
exec python3 "$(dirname "${BASH_SOURCE[0]}")/../pipeline/orchestrator/tests/smoke/canary.py" "$@"

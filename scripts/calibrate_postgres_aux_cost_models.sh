#!/usr/bin/env bash
# Refresh per-query aux cost model JSON files used by Postgres and DuckDB runtimes.
#
# The calibrator measures DuckDB build/delta/fallback timings and writes
# config/<workload>_q<query>_aux_cost_model.json (TPC-H) or
# config/sec_edgar_q<query>_aux_cost_model.json (SEC). Postgres loads these
# automatically via ACCELERATOR_CODEGEN_CONFIG_DIR or the docker fallback path
# /tmp/accel-codegen-src/config/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ACCELERATOR_CODEGEN_CONFIG_DIR="${ACCELERATOR_CODEGEN_CONFIG_DIR:-${ROOT}/config}"

exec bash "${ROOT}/scripts/calibrate_shared_aux_cost_models.sh"

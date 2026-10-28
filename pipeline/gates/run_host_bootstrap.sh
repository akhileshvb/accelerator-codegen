#!/usr/bin/env bash
# Verify the engine's workload harnesses (made by host_bootstrap under backends/<engine>/tests/harness
# and bench): build every target through the engine host, then smoke each of them with --help. What they do on a
# query is the conformance check's (run_harness_conformance.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-0}"

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

ENGINE_JSON="${ROOT}/pipeline/artifacts/engines/${ENGINE_NAME}/host_bootstrap/host_bootstrap.json"
QUERY_JSON="${GATE_OUT}/host_bootstrap.json"
TARGETS=("${DIFF_BINARY}" "${MULTI_CONN_TARGET}" "${CROSS_ACCEL_TARGET}" "${HARNESS_BENCH_TARGET}" "${HARNESS_CALIBRATE_TARGET}")

finish() { # status reason log
  local rc=0 targets
  targets="$(printf '%s\n' "${TARGETS[@]}" | python3 -c "import json,sys; print(json.dumps(sys.stdin.read().split()))")"
  write_result "${ENGINE_JSON}" host_bootstrap "$@" "query:=${QUERY}" "targets:=${targets}" "smokes:=${SMOKES:-[]}" >/dev/null || rc=$?
  write_result "${QUERY_JSON}" host_bootstrap "$@" "query:=${QUERY}" "targets:=${targets}" "smokes:=${SMOKES:-[]}" || rc=$?
  exit "${rc}"
}

for target in "${TARGETS[@]}"; do
  [[ -n "${target}" ]] || finish fail "gate context lacks a harness target. ${HOST_HINT}" ""
done
for file in "${DIFF_SOURCE}" "backends/${ENGINE_NAME}/tests/harness.cmake"; do
  [[ -f "${ROOT}/${file}" ]] || finish fail "missing ${file}. ${HOST_HINT}" ""
done
grep -Eq 'accelerators/\*/q\*/[^/"]+/aux/(register|finalize)\.cpp' "${ROOT}/backends/${ENGINE_NAME}/tests/harness.cmake" ||
  finish fail "harness.cmake declares no module test for every benchmark: glob accelerators/*/q*/${ENGINE_NAME}/aux/finalize.cpp into test_<bench>_q<N>_aux (query_host_bootstrap.md)" ""

HARNESS_FILES=()
while IFS= read -r file; do HARNESS_FILES+=("${file}"); done < <(cd "${ROOT}" &&
  find "backends/${ENGINE_NAME}/tests/harness" "backends/${ENGINE_NAME}/bench" "backends/${ENGINE_NAME}/tests/harness.cmake" \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.cmake' \) 2>/dev/null | sort)
FILE_ARGS=()
for file in "${HARNESS_FILES[@]}"; do FILE_ARGS+=(--file "${file}"); done
if ! QUERY_FREE_LOG="$(cd "${ROOT}" && python3 scripts/check_backend_query_free.py "${FILE_ARGS[@]}" --bench-free 2>&1)"; then
  # which files still name one, with counts: a log tail shows only the last of them
  OFFENDERS="$(grep -oE '^[^: ]+:[0-9]+:' <<<"${QUERY_FREE_LOG}" | cut -d: -f1 | sort | uniq -c | sort -rn |
    awk '{printf "%s%s (%d)", (NR > 1 ? ", " : ""), $2, $1}')"
  finish fail "the harnesses must be workload-generic: no query or benchmark names, no query-number dispatch (the benchmark and query are inputs); still naming one: ${OFFENDERS}" "${QUERY_FREE_LOG}"
fi

REGISTERED_QUERY=""
for register in "${ROOT}"/accelerators/"${BENCHMARK_NAME}"/q*/"${ENGINE_NAME}"/aux/register.cpp; do
  [[ -f "${register}" ]] || continue
  REGISTERED_QUERY="$(sed -E 's#.*/q([0-9]+)/.*#\1#' <<<"${register}")"
  break
done
# The module test of the registered query (one target per query that has an aux module).
[[ -z "${REGISTERED_QUERY}" ]] || TARGETS+=("test_${BENCHMARK_NAME}_q${REGISTERED_QUERY}_aux")

if ! engine_ready "${TARGETS[@]}"; then
  finish fail "${ENGINE_FAIL}: fix the harness sources / harness.cmake" "${ENGINE_LOG}"
fi


SMOKE_LOG=""
SMOKES="[]"
smoke() { # target args...
  local target="$1"
  engine_run "$@"
  SMOKE_LOG+="[smoke ${*}] rc=${RC}"$'\n'"${LOG}"$'\n'
  SMOKES="$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); d.append({'target': sys.argv[2], 'args': sys.argv[4:], 'exit_code': int(sys.argv[3])}); print(json.dumps(d))" "${SMOKES}" "${target}" "${RC}" "${@:2}")"
  if [[ ${RC} -ne 0 ]]; then
    finish fail "smoke failed: ${target} ${*:2} (exit ${RC})" "${ENGINE_LOG}${SMOKE_LOG}"
  fi
}

for target in "${TARGETS[@]}"; do
  smoke "${target}" --help
done
finish pass "built and smoked ${TARGETS[*]}" "${ENGINE_LOG}${SMOKE_LOG}"

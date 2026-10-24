#!/usr/bin/env bash
# Verify the engine's workload harnesses (made by host_bootstrap under backends/<engine>/tests/harness
# and bench): build every target through the engine host, then smoke each of them. The differential
# runs one registered query when the engine has an accelerator, else every harness answers --help.
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
grep -Eq "aux/(register|finalize)\.cpp" "${ROOT}/backends/${ENGINE_NAME}/tests/harness.cmake" ||
  finish fail "harness.cmake declares no module test: glob accelerators/*/q*/${ENGINE_NAME}/aux/register.cpp (or finalize.cpp) into test_<bench>_q<N>_aux (query_host_bootstrap.md)" ""

HARNESS_FILES=()
while IFS= read -r file; do HARNESS_FILES+=("${file}"); done < <(cd "${ROOT}" &&
  find "backends/${ENGINE_NAME}/tests/harness" "backends/${ENGINE_NAME}/bench" "backends/${ENGINE_NAME}/tests/harness.cmake" \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.cmake' \) 2>/dev/null | sort)
FILE_ARGS=()
for file in "${HARNESS_FILES[@]}"; do FILE_ARGS+=(--file "${file}"); done
if ! QUERY_FREE_LOG="$(cd "${ROOT}" && python3 scripts/check_backend_query_free.py "${FILE_ARGS[@]}" 2>&1)"; then
  finish fail "the harnesses must be workload-generic (no query names or query-number dispatch)" "${QUERY_FREE_LOG}"
fi

if ! engine_ready "${TARGETS[@]}"; then
  finish fail "${ENGINE_FAIL}: fix the harness sources / harness.cmake" "${ENGINE_LOG}"
fi

REGISTERED_QUERY=""
for register in "${ROOT}"/accelerators/"${BENCHMARK_NAME}"/q*/"${ENGINE_NAME}"/aux/register.cpp; do
  [[ -f "${register}" ]] || continue
  REGISTERED_QUERY="$(sed -E 's#.*/q([0-9]+)/.*#\1#' <<<"${register}")"
  break
done

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

if [[ -n "${REGISTERED_QUERY}" ]]; then
  smoke "${DIFF_BINARY}" "${REGISTERED_QUERY}"
else
  smoke "${DIFF_BINARY}" --help
fi
for target in "${MULTI_CONN_TARGET}" "${CROSS_ACCEL_TARGET}" "${HARNESS_BENCH_TARGET}" "${HARNESS_CALIBRATE_TARGET}"; do
  smoke "${target}" --help
done

# The bench must measure the accelerator: run read-only once as is (reads hit the state) and once with
# every read forced to the fallback SQL (the substrate's ACCEL_AUX_FORCE_FALLBACK). The hit rate it prints
# (`accel_hit_rate`, bench_sweep_contract.md) must tell the two apart; a bench that reports the same rate
# both times assumes it (a constant) instead of counting.
BENCH_HIT_RATE=""
bench_read_only() { # [VAR=value ...]: a short read-only bench run; BENCH_HIT_RATE = the accel_hit_rate it printed
  local dir rc
  dir="$(mktemp -d)"
  (
    export ACCEL_AUX_WRITE_FREQUENCIES=0 ACCEL_BENCH_WARMUP=2 ACCEL_BENCH_QUERIES=5 ACCEL_BENCH_REF_SAMPLES=1 "$@"
    engine_run "${HARNESS_BENCH_TARGET}" --query "${REGISTERED_QUERY}"
    echo "${RC}" >"${dir}/rc"
    printf '%s' "${LOG}" >"${dir}/log"
  )
  rc="$(cat "${dir}/rc" 2>/dev/null || echo 1)"
  SMOKE_LOG+="[bench $*] rc=${rc}"$'\n'"$(cat "${dir}/log" 2>/dev/null)"$'\n'
  BENCH_HIT_RATE="$(python3 -c "
import csv, io, sys
lines = sys.stdin.read().splitlines()
start = next((i for i, l in enumerate(lines) if l.startswith('suite,')), None)
rows = list(csv.DictReader(io.StringIO(chr(10).join(lines[start:start + 2])))) if start is not None else []
print(rows[0].get('accel_hit_rate', '') if rows else '')" <"${dir}/log")"
  rm -rf "${dir}"
  [[ "${rc}" -eq 0 ]]
}
if [[ -n "${REGISTERED_QUERY}" ]]; then
  bench_read_only || finish fail "the bench failed on a read-only run: ${HARNESS_BENCH_TARGET} --query ${REGISTERED_QUERY}" "${ENGINE_LOG}${SMOKE_LOG}"
  HIT_RATE="${BENCH_HIT_RATE}"
  bench_read_only ACCEL_AUX_FORCE_FALLBACK=1 || finish fail "the bench failed with every read forced to the fallback (ACCEL_AUX_FORCE_FALLBACK=1)" "${ENGINE_LOG}${SMOKE_LOG}"
  FORCED_RATE="${BENCH_HIT_RATE}"
  verdict="$(python3 -c "
import sys
hit, forced = sys.argv[1:3]
try:
    hit, forced = float(hit), float(forced)
except ValueError:
    print('the bench printed no accel_hit_rate (bench_sweep_contract.md): hit=%r forced=%r' % (hit, forced)); sys.exit(0)
if hit - forced < 0.4:
    print('the bench reports hit rate %.2f with reads hitting and %.2f with every read forced to the fallback: it does not count hits' % (hit, forced))
" "${HIT_RATE}" "${FORCED_RATE}")"
  [[ -z "${verdict}" ]] || finish fail "${verdict}" "${ENGINE_LOG}${SMOKE_LOG}"
fi
finish pass "built and smoked ${TARGETS[*]}" "${ENGINE_LOG}${SMOKE_LOG}"

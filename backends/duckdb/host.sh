#!/usr/bin/env bash
# DuckDB host (embedded): the database is one file under ACCEL_ENGINE_STATE_DIR.
# Verbs: backends/{engine}/host contract (pipeline/prompts/engine_backend/host_contract.md).
# `run` replaces the literal argument {db} with the database path; a target that is not a file
# in the build directory runs as a PATH command (e.g. ctest). Both see the `env` file.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
: "${ACCEL_ENGINE_STATE_DIR:?}"
STATE="${ACCEL_ENGINE_STATE_DIR}"
DB="${STATE}/tpch.duckdb"
ENV_FILE="${STATE}/env"
BUILD="${ACCEL_BUILD_DIR:-${ROOT}/build-release}"
DUCKDB_ROOT="${ACCEL_DUCKDB_ROOT:-${ACCEL_PIPELINE_PRIMARY_REPO:-${ROOT}}/../duckdb-dbcapture}"
CLI="${ACCEL_DUCKDB_CLI:-${DUCKDB_ROOT}/build/release/duckdb}"

duckdb_cli() { "${CLI}" "$@"; }

load_env() { set -a; source "${ENV_FILE}"; set +a; }

# JSON array of the existing directories among the arguments (the `info` verb's mounts).
json_dirs() {
  local out="" d
  for d in "$@"; do
    [[ -n "${d}" && -d "${d}" ]] || continue
    d="$(cd "${d}" && pwd -P)"
    out+="${out:+, }\"${d}\""
  done
  printf '[%s]' "${out}"
}

cmd_info() {
  # mounts: the external checkouts the verbs need at runtime (absolute, existing); docker: they need the daemon
  printf '{"engine": "duckdb", "embedded": true, "verbs": ["info", "up", "down", "load", "build", "sql", "run", "slot"], "hit_contract": true, "bench_multi_point": true, "mounts": {"ro": %s, "rw": []}, "docker": false}\n' \
    "$(json_dirs "${DUCKDB_ROOT}")"
}

cmd_up() {
  mkdir -p "${STATE}/logs"
  printf 'ACCEL_DB=%s\nACCEL_TPCH_DB=%s\n' "${DB}" "${DB}" >"${ENV_FILE}"
}

cmd_down() { rm -rf "${STATE}"; }

cmd_load() { # schema.sql [constraints.sql]
  local schema="$1" constraints="${2:-}" tbl table
  : "${ACCEL_DATASET_DIR:?}"
  if [[ -f "${STATE}/loaded" && "$(cat "${STATE}/loaded")" == "${ACCEL_DATASET_DIR}" ]]; then
    return 0
  fi
  rm -f "${DB}" "${DB}.wal"
  {
    cat "${schema}"
    # in the schema's table order, so a foreign key's referenced table is loaded first
    for table in $(grep -oiE 'create table +[a-z_0-9]+' "${schema}" | awk '{print $3}'); do
      tbl="${ACCEL_DATASET_DIR}/${table}.tbl"
      [[ ! -f "${tbl}" ]] || echo "COPY ${table} FROM '${tbl}' (DELIMITER '|', HEADER false);"
    done
    [[ -z "${constraints}" ]] || cat "${constraints}"
  } | duckdb_cli "${DB}"
  echo "${ACCEL_DATASET_DIR}" >"${STATE}/loaded"
}

cmd_build() { # [target...]
  [[ -f "${BUILD}/CMakeCache.txt" ]] ||
    cmake -S "${ROOT}" -B "${BUILD}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DACCEL_CODEGEN_ENGINE=duckdb ${ACCEL_CMAKE_ARGS:-}
  cmake --build "${BUILD}" --target "${@:-accelerator_codegen}" -j"${ACCEL_BUILD_JOBS:-$(nproc)}"
}

cmd_sql() { # file | -
  local head tail
  head="$(mktemp)" tail="$(mktemp)"
  python3 -I "${HERE}/host/split_last.py" "${head}" "${tail}" < <(cat "${1:--}")
  duckdb_cli "${DB}" <"${head}" >/dev/null
  duckdb_cli -csv -nullvalue '' "${DB}" <"${tail}"
  rm -f "${head}" "${tail}"
}

cmd_run() { # target [args...]
  local target="$1" args=() arg
  shift
  for arg in "$@"; do args+=("${arg//\{db\}/${DB}}"); done
  [[ ! -f "${BUILD}/${target}" ]] || target="${BUILD}/${target}"
  load_env
  LD_LIBRARY_PATH="${DUCKDB_ROOT}/build/release/src:${BUILD}:${LD_LIBRARY_PATH:-}" "${target}" "${args[@]}"
}

cmd_slot() { # scenario seconds
  local scenario="$1" seconds="$2" q failed=0 work
  if [[ "${scenario}" != differential ]]; then
    cmd_run test_slot_gates_duckdb "${scenario}" "${seconds}"
    return
  fi
  for q in ${ACCEL_SLOT_DIFF_QUERIES:-$(seq 1 22)}; do
    if cmd_run test_duckdb_tpch_differential "${q}" >/dev/null 2>&1; then
      echo "PASS differential Q${q}"
    else
      echo "FAIL differential Q${q}"
      failed=1
    fi
  done
  work="$(mktemp -d)"
  cp "${DB}" "${work}/tpch.duckdb"
  if cmd_run test_shared_tpch_rf_differential "${work}/tpch.duckdb"; then
    echo "PASS rf differential"
  else
    echo "FAIL rf differential"
    failed=1
  fi
  rm -rf "${work}"
  return "${failed}"
}

verb="${1:-}"
[[ $# -gt 0 ]] && shift
case "${verb}" in
  info | up | down | build | sql | run | slot | load) "cmd_${verb}" "$@" ;;
  *) echo "usage: host.sh info|up|down|load|build|sql|run|slot" >&2; exit 2 ;;
esac

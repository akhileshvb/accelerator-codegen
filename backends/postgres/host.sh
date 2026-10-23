#!/usr/bin/env bash
# PostgreSQL host: one Docker container per instance, source/build mounted in, the
# extension built in the container. Verbs: pipeline/prompts/engine_backend/host_contract.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
HOST_DIR="${HERE}/host"
: "${ACCEL_ENGINE_INSTANCE:?}" "${ACCEL_ENGINE_STATE_DIR:?}"
STATE="${ACCEL_ENGINE_STATE_DIR}"
ENV_FILE="${STATE}/env"
NAME="accel_pg_${ACCEL_ENGINE_INSTANCE}"
DB="accel_${ACCEL_ENGINE_INSTANCE//[^a-zA-Z0-9]/_}"
IMAGE="${ACCEL_PG_IMAGE:-postgres:17}"
PASSWORD=postgres
DUCKDB_ROOT="${ACCEL_DUCKDB_ROOT:-${ROOT}/../duckdb-dbcapture}"
DUCKDB_SEED="${ACCEL_DUCKDB_SEED:-}"

psql_in() { docker exec -i -e PGOPTIONS="-c client_min_messages=warning" "${NAME}" psql -X -q -v ON_ERROR_STOP=1 -U postgres "$@"; }

ready() { docker exec "${NAME}" pg_isready -q -h 127.0.0.1 -U postgres 2>/dev/null; }

free_port() {
  local port
  for port in $(seq $((54100 + $(cksum <<<"${ACCEL_ENGINE_INSTANCE}" | cut -d' ' -f1) % 800)) 54999); do
    ss -ltn "sport = :${port}" | grep -q LISTEN || { echo "${port}"; return; }
  done
  return 1
}

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
  printf '{"engine": "postgres", "embedded": false, "verbs": ["info", "up", "down", "load", "build", "sql", "run", "slot"], "mounts": {"ro": %s, "rw": []}, "docker": true}\n' \
    "$(json_dirs "${DUCKDB_ROOT}" "${DUCKDB_SEED}")"
}

# The instance's database (empty until `load`): `build` registers the extension in it.
ensure_db() {
  psql_in -d postgres -tAc "select 1 from pg_database where datname = '${DB}'" | grep -q 1 ||
    psql_in -d postgres -c "CREATE DATABASE \"${DB}\"" >/dev/null
}

cmd_up() {
  mkdir -p "${STATE}/data" "${STATE}/logs" "${ACCEL_BUILD_DIR:?}"
  if ! docker ps --format '{{.Names}}' | grep -qx "${NAME}"; then
    docker rm -f "${NAME}" >/dev/null 2>&1 || true
    local port seed=()
    port="$(free_port)"
    [[ -z "${DUCKDB_SEED}" ]] || seed=(-v "${DUCKDB_SEED}:/accel-duckdb-seed:ro")
    docker run -d --name "${NAME}" \
      --shm-size="${ACCEL_PG_SHM_SIZE:-2g}" \
      -e POSTGRES_PASSWORD="${PASSWORD}" \
      -p "127.0.0.1:${port}:5432" \
      -v "${STATE}/data:/var/lib/postgresql/data" \
      -v "${ROOT}:/accel-src" \
      -v "${DUCKDB_ROOT}:/accel-duckdb:ro" \
      -v "${ACCEL_BUILD_DIR}:/accel-build" \
      "${seed[@]}" \
      "${IMAGE}" \
      -c shared_buffers="${ACCEL_PG_SHARED_BUFFERS:-256MB}" \
      -c work_mem=64MB -c maintenance_work_mem=256MB -c max_wal_size=2GB -c autovacuum=off \
      ${ACCEL_PG_EXTRA_ARGS:-} >/dev/null
    printf 'PGHOST=127.0.0.1\nPGPORT=%s\nPGUSER=postgres\nPGPASSWORD=%s\nTPCH_PG_DB=%s\nACCEL_PG_CONTAINER=%s\n' \
      "${port}" "${PASSWORD}" "${DB}" "${NAME}" >"${ENV_FILE}"
  fi
  local _
  for _ in $(seq 1 90); do
    if ready; then
      ensure_db
      return
    fi
    sleep 1
  done
  docker logs "${NAME}" 2>&1 | tail -40 >&2
  echo "timed out waiting for ${NAME}" >&2
  return 1
}

cmd_down() {
  docker rm -f "${NAME}" >/dev/null 2>&1 || true
  # The data directory is owned by the container's postgres user.
  [[ ! -d "${STATE}/data" ]] ||
    docker run --rm -v "${STATE}:/state" --entrypoint sh "${IMAGE}" -c 'rm -rf /state/data'
  rm -rf "${STATE}"
}

cmd_load() { # schema.sql [constraints.sql]
  local schema="$1" constraints="${2:-}" tbl table
  : "${ACCEL_DATASET_DIR:?}"
  ensure_db
  if [[ "$(cat "${STATE}/loaded" 2>/dev/null)" == "${ACCEL_DATASET_DIR}" ]]; then
    return 0
  fi
  psql_in -d "${DB}" -c 'DROP SCHEMA public CASCADE; CREATE SCHEMA public'
  sed -E -f "${HOST_DIR}/schema_to_pg.sed" "${schema}" | psql_in -d "${DB}" -f -
  for tbl in "${ACCEL_DATASET_DIR}"/*.tbl; do
    table="$(basename "${tbl}" .tbl)"
    # dbgen lines end with a delimiter: strip it.
    sed 's/|$//' "${tbl}" | psql_in -d "${DB}" -c "COPY ${table} FROM STDIN WITH (FORMAT text, DELIMITER '|')"
  done
  [[ -z "${constraints}" ]] || psql_in -d "${DB}" -f - <"${constraints}"
  psql_in -d "${DB}" -c 'ANALYZE'
  echo "${ACCEL_DATASET_DIR}" >"${STATE}/loaded"
}

cmd_build() { # [target...]
  docker exec \
    -e ACCEL_PG_TARGETS="${*}" \
    -e ACCEL_BUILD_JOBS="${ACCEL_BUILD_JOBS:-}" \
    -e ACCEL_HOST_UID="$(id -u)" -e ACCEL_HOST_GID="$(id -g)" \
    -e ACCEL_DUCKDB_SEED="${DUCKDB_SEED}" \
    "${NAME}" bash /accel-src/backends/postgres/host/container_build.sh
  # A fresh LOAD of the new .so needs fresh backends; register against the loaded schema.
  psql_in -d "${DB}" -f - <"${HOST_DIR}/register.sql" >/dev/null
  psql_in -d "${DB}" -f - <"${HOST_DIR}/capture_triggers.sql"
  local table
  for table in $(psql_in -d "${DB}" -tAc "select tablename from pg_tables where schemaname = 'public'"); do
    psql_in -d "${DB}" -c "SELECT create_accel_mutation_triggers('${table}', 'accel_codegen')" >/dev/null
  done
}

# Build targets a slot scenario needs when they are not built yet.
ensure_built() {
  local target missing=()
  for target in "$@"; do
    docker exec "${NAME}" test -x "/accel-build/accel/${target}" || missing+=("${target}")
  done
  [[ ${#missing[@]} -eq 0 ]] || cmd_build "${missing[@]}"
}

cmd_sql() { # file | -
  # SHOW_ALL_RESULTS=off: of a multi-statement script only the last result prints.
  psql_in -d "${DB}" --csv -v SHOW_ALL_RESULTS=off -c "$(cat "${1:--}")"
}

in_container() { # binary args...
  local binary="$1" var
  shift
  local args=() arg flags=()
  # The build directory is /accel-build/accel in the container.
  for arg in "$@"; do args+=("${arg//${ACCEL_BUILD_DIR}//accel-build/accel}"); done
  docker exec "${NAME}" test -f "/accel-build/accel/${binary}" && binary="/accel-build/accel/${binary}"
  while IFS= read -r var; do flags+=(-e "${var}"); done < <(
    env | grep -E '^(ACCEL_(PG_|SLOT_|FORCE_)|TPCH_|PGOPTIONS)' || true)
  docker exec "${flags[@]}" \
    -e ACCEL_PG_DOCKER=1 -e PGHOST=127.0.0.1 -e PGPORT=5432 -e PGUSER=postgres -e PGPASSWORD="${PASSWORD}" \
    -e TPCH_PG_DB="${DB}" -e ACCEL_ENGINE_NAME=postgres -e ACCEL_WORKLOAD_KIND=aux_state \
    -e ACCEL_PG_DIFF_KIND=aux -e ACCEL_SRC=/accel-src \
    -e LD_LIBRARY_PATH=/accel-build/accel:/accel-build/duckdb/src:/usr/lib/postgresql/17/lib \
    "${NAME}" "${binary}" "${args[@]}"
}

cmd_run() { # target|command [args...]: a built binary, else a PATH command (e.g. ctest), in the container
  in_container "$@"
}

# The shard-worker suite exits 0 for a scenario it does not know: require its PASS line.
shared_state_scenario() { # scenario seconds
  local log
  log="$(in_container test_postgres_shared_state "$1" "$2" 2>&1)" || { echo "${log}"; return 1; }
  echo "${log}"
  grep -q "^PASS $1\b" <<<"${log}" || { echo "scenario $1 did not report PASS"; return 1; }
}

differential() {
  local q mode rc=0 failed=0 log
  for mode in read write_maint; do
    for q in ${ACCEL_PG_DIFF_QUERIES:-$(seq 1 22)}; do
      if log="$(ACCEL_PG_DIFF_MODE="${mode}" in_container test_postgres_tpch_differential "${q}" 2>&1)" &&
        grep -qE 'PASS (read|write_maint)' <<<"${log}"; then
        echo "PASS ${mode} Q${q}"
      else
        echo "FAIL ${mode} Q${q}"; echo "${log}" | tail -30; failed=1
      fi
    done
  done
  return "${failed}"
}

# Which part of a scenario the suite does not cover:
#   hits_under_writes   only REPEATABLE READ readers
#   read_after_commit   not a read on another connection right after a slow commit
#   differential        write_maint stands in for an RF-chunk differential
#   own_writes, ddl_events   no read inside a writing transaction; no DROP/RENAME/recreate
#                            or DDL during a build: fail closed
cmd_slot() { # scenario seconds
  local scenario="$1" seconds="$2" rc=0 s
  case "${scenario}" in
    epoch_stress)
      ensure_built test_postgres_shared_state test_shm_ring test_shard_segment
      shared_state_scenario concurrent "${seconds}" || rc=1
      for s in test_shm_ring test_shard_segment; do in_container "${s}" || rc=1; done ;;
    hits_under_writes | restart | txn_edges | read_after_commit)
      ensure_built test_postgres_shared_state
      for s in $(case "${scenario}" in
        hits_under_writes) echo hits_under_writes ;;
        read_after_commit) echo read_after_own_commit hit_across_connections ;;
        txn_edges) echo abort_and_savepoint truncate_rebuild ;;
        restart) echo restart_worker kill_backend ;;
      esac); do shared_state_scenario "${s}" "${seconds}" || rc=1; done ;;
    differential)
      ensure_built test_postgres_tpch_differential
      differential || rc=1 ;;
    own_writes | ddl_events)
      echo "postgres: ${scenario} has no driver yet (needs a read inside a writing transaction / DDL racing a build)"
      rc=1 ;;
    *) echo "unknown scenario ${scenario}" >&2; return 2 ;;
  esac
  return "${rc}"
}

verb="${1:-}"
[[ $# -gt 0 ]] && shift
case "${verb}" in
  info | up | down | load | build | sql | run | slot) "cmd_${verb}" "$@" ;;
  *) echo "usage: host.sh info|up|down|load|build|sql|run|slot" >&2; exit 2 ;;
esac

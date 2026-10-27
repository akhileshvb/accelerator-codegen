#!/usr/bin/env bash
# MySQL host: one Docker container per instance, source/build mounted in, the audit
# plugin built in the container. Verbs: pipeline/prompts/engine_backend/host_contract.md.
# ACCEL_MYSQL_PLUGIN_VARIANT=test builds the plugin with the gate-test UDFs (default prod).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
HOST_DIR="${HERE}/host"
: "${ACCEL_ENGINE_INSTANCE:?}" "${ACCEL_ENGINE_STATE_DIR:?}"
STATE="${ACCEL_ENGINE_STATE_DIR}"
ENV_FILE="${STATE}/env"
NAME="accel_mysql_${ACCEL_ENGINE_INSTANCE}"
DB="accel_${ACCEL_ENGINE_INSTANCE//[^a-zA-Z0-9]/_}"
IMAGE="${ACCEL_MYSQL_IMAGE:-mysql:8.4}"
PASSWORD=mysql
VARIANT="${ACCEL_MYSQL_PLUGIN_VARIANT:-prod}"
DUCKDB_ROOT="${ACCEL_DUCKDB_ROOT:-${ACCEL_PIPELINE_PRIMARY_REPO:-${ROOT}}/../duckdb-dbcapture}"
DUCKDB_SEED="${ACCEL_DUCKDB_SEED:-}"

mysql_q() { docker exec -i "${NAME}" mysql -h 127.0.0.1 -P 3306 -uroot -p"${PASSWORD}" --local-infile=1 "$@" 2> >(grep -v 'Using a password' >&2); }

ready() { docker exec "${NAME}" mysqladmin ping -h 127.0.0.1 -uroot -p"${PASSWORD}" --silent >/dev/null 2>&1; }

wait_ready() {
  local _
  for _ in $(seq 1 120); do
    ready && return 0
    sleep 1
  done
  docker logs "${NAME}" 2>&1 | tail -40 >&2
  echo "timed out waiting for ${NAME}" >&2
  return 1
}

free_port() {
  local port
  for port in $(seq $((33100 + $(cksum <<<"${ACCEL_ENGINE_INSTANCE}" | cut -d' ' -f1) % 800)) 33999); do
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
  printf '{"engine": "mysql", "embedded": false, "verbs": ["info", "up", "down", "load", "build", "sql", "run", "slot"], "mounts": {"ro": %s, "rw": []}, "docker": true}\n' \
    "$(json_dirs "${DUCKDB_ROOT}" "${DUCKDB_SEED}")"
}

cmd_up() {
  mkdir -p "${STATE}/data" "${STATE}/logs" "${ACCEL_BUILD_DIR:?}"
  if ! docker ps --format '{{.Names}}' | grep -qx "${NAME}"; then
    docker rm -f "${NAME}" >/dev/null 2>&1 || true
    local port seed=()
    port="$(free_port)"
    [[ -z "${DUCKDB_SEED}" ]] || seed=(-v "${DUCKDB_SEED}:/accel-duckdb-seed:ro")
    docker run -d --name "${NAME}" \
      --shm-size="${ACCEL_MYSQL_SHM_SIZE:-2g}" \
      -e MYSQL_ROOT_PASSWORD="${PASSWORD}" -e MYSQL_DATABASE="${DB}" \
      -p "127.0.0.1:${port}:3306" \
      -v "${STATE}/data:/var/lib/mysql" \
      -v "${ROOT}:/accel-src" \
      -v "${DUCKDB_ROOT}:/accel-duckdb:ro" \
      -v "${ACCEL_BUILD_DIR}:/accel-build" \
      "${seed[@]}" \
      "${IMAGE}" --local-infile=1 ${ACCEL_MYSQL_EXTRA_ARGS:-} >/dev/null
    printf 'MYSQL_HOST=127.0.0.1\nMYSQL_TCP_PORT=%s\nMYSQL_USER=root\nMYSQL_PWD=%s\nTPCH_MYSQL_DB=%s\nACCEL_MYSQL_CONTAINER=%s\n' \
      "${port}" "${PASSWORD}" "${DB}" "${NAME}" >"${ENV_FILE}"
  fi
  wait_ready
}

cmd_down() {
  docker rm -f "${NAME}" >/dev/null 2>&1 || true
  # The data directory is owned by the container's mysql user.
  [[ ! -d "${STATE}/data" ]] ||
    docker run --rm -v "${STATE}:/state" --entrypoint sh "${IMAGE}" -c 'rm -rf /state/data'
  rm -rf "${STATE}"
}

cmd_load() { # schema.sql [constraints.sql]
  local schema="$1" constraints="${2:-}" tbl table
  : "${ACCEL_DATASET_DIR:?}"
  if [[ "$(cat "${STATE}/loaded" 2>/dev/null)" == "${ACCEL_DATASET_DIR}" ]]; then
    return 0
  fi
  mysql_q -e "DROP DATABASE IF EXISTS \`${DB}\`; CREATE DATABASE \`${DB}\`"
  mysql_q "${DB}" <"${schema}"
  # in the schema's table order, so a foreign key's referenced table is loaded first
  for table in $(grep -oiE 'create table +[a-z_0-9]+' "${schema}" | awk '{print $3}'); do
    tbl="${ACCEL_DATASET_DIR}/${table}.tbl"
    [[ -f "${tbl}" ]] || continue
    # dbgen lines end with a delimiter: strip it.
    sed 's/|$//' "${tbl}" | mysql_q "${DB}" -e \
      "LOAD DATA LOCAL INFILE '/dev/stdin' INTO TABLE ${table} FIELDS TERMINATED BY '|' LINES TERMINATED BY '\n'"
  done
  [[ -z "${constraints}" ]] || mysql_q "${DB}" <"${constraints}"
  echo "${ACCEL_DATASET_DIR}" >"${STATE}/loaded"
  # A reload recreated the database: the plugin's functions and triggers go back in.
  [[ ! -f "${STATE}/built" ]] || { registration_sql | mysql_q --force "${DB}" >/dev/null 2>&1 || true; }
  install_capture_triggers
}

# UDF registration: the backend's base plus every accelerator's functions.sql fragment.
registration_sql() {
  cat "${HOST_DIR}/register_base.sql"
  local fragment
  for fragment in $(printf '%s\n' "${ROOT}"/accelerators/*/q*/mysql/plugin/functions.sql | sort -V); do
    [[ -f "${fragment}" ]] && { echo; cat "${fragment}"; }
  done
  if [[ "${VARIANT}" == test ]]; then cat "${HOST_DIR}/register_test_udfs.sql"; else cat "${HOST_DIR}/unregister_test_udfs.sql"; fi
}

# Table ids the query modules use in Dependencies(), from the accelerators' id headers (kName = N).
table_id_args() {
  sed -nE 's/.*\bk([A-Z][A-Za-z]*) = ([0-9]+);.*/\1=\2/p' "${ROOT}"/accelerators/*/common/mysql_table_ids.hpp |
    while IFS='=' read -r name id; do echo "--table"; echo "${name,,}=${id}"; done
}

cmd_build() { # [target...]
  local test_udfs=OFF
  [[ "${VARIANT}" != test ]] || test_udfs=ON
  docker exec \
    -e ACCEL_MYSQL_TARGETS="${*}" -e ACCEL_MYSQL_TEST_UDFS="${test_udfs}" -e ACCEL_MYSQL_PLUGIN_VARIANT="${VARIANT}" \
    -e ACCEL_BUILD_JOBS="${ACCEL_BUILD_JOBS:-}" \
    -e ACCEL_HOST_UID="$(id -u)" -e ACCEL_HOST_GID="$(id -g)" \
    -e ACCEL_DUCKDB_SEED="${DUCKDB_SEED}" \
    "${NAME}" bash /accel-src/backends/mysql/host/container_build.sh
  # A restart makes mysqld load the new plugin .so.
  docker restart "${NAME}" >/dev/null
  wait_ready
  registration_sql | mysql_q --force "${DB}" 2>&1 | grep -v "already\|Using a password" || true
  touch "${STATE}/built"
  install_capture_triggers
}

# Capture triggers need the tables (load) and the plugin's UDFs (build): installed by
# whichever of the two runs second.
install_capture_triggers() {
  [[ -f "${STATE}/built" && -f "${STATE}/loaded" ]] || return 0
  local ids=()
  mapfile -t ids < <(table_id_args)
  ACCEL_MYSQL_DOCKER=1 ACCEL_MYSQL_CONTAINER="${NAME}" MYSQL_PWD="${PASSWORD}" \
    python3 -I "${HOST_DIR}/gen_capture_triggers.py" --db "${DB}" "${ids[@]}" | mysql_q "${DB}"
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
  local head tail
  head="$(mktemp)" tail="$(mktemp)"
  python3 -I "${HOST_DIR}/split_last.py" "${head}" "${tail}" < <(cat "${1:--}")
  mysql_q "${DB}" <"${head}" >/dev/null
  mysql_q "${DB}" --batch <"${tail}" | python3 -I -c '
import csv, sys
w = csv.writer(sys.stdout, lineterminator="\n")
for line in sys.stdin:
    w.writerow("" if f == "NULL" else f for f in line.rstrip("\n").split("\t"))'
  rm -f "${head}" "${tail}"
}

in_container() { # binary args...
  local binary="$1" var
  shift
  local args=() arg flags=()
  # The build directory is /accel-build/accel in the container.
  for arg in "$@"; do args+=("${arg//${ACCEL_BUILD_DIR}//accel-build/accel}"); done
  docker exec "${NAME}" test -f "/accel-build/accel/${binary}" && binary="/accel-build/accel/${binary}"
  while IFS= read -r var; do flags+=(-e "${var}"); done < <(
    env | grep -E '^(ACCEL_(MYSQL_|SLOT_|FORCE_|SESSION_|RACE_|MULTI_)|TPCH_)' || true)
  docker exec "${flags[@]}" \
    -e ACCEL_MYSQL_DOCKER=1 -e ACCEL_MYSQL_CONTAINER="${NAME}" \
    -e MYSQL_HOST=127.0.0.1 -e MYSQL_PORT=3306 -e MYSQL_USER=root -e MYSQL_PWD="${PASSWORD}" \
    -e TPCH_MYSQL_DB="${DB}" -e ACCEL_ENGINE_NAME=mysql -e ACCEL_WORKLOAD_KIND=aux_state -e ACCEL_SRC=/accel-src \
    -e LD_LIBRARY_PATH=/accel-build/accel:/accel-build/duckdb/src \
    "${NAME}" "${binary}" "${args[@]}"
}

cmd_run() { # target|command [args...]: a built binary, else a PATH command (e.g. ctest), in the container
  in_container "$@"
}

differential() {
  local q log failed=0
  for q in ${ACCEL_MYSQL_DIFF_QUERIES:-$(seq 1 22)}; do
    if log="$(ACCEL_MYSQL_DIFF_SKIP_WRITE_MAINT="${ACCEL_MYSQL_DIFF_SKIP_WRITE_MAINT:-1}" \
      in_container test_mysql_tpch_differential "${q}" 2>&1)" && grep -qE 'PASS (read|write_maint)' <<<"${log}"; then
      echo "PASS Q${q}"
    else
      echo "FAIL Q${q}"; echo "${log}" | tail -30; failed=1
    fi
  done
  return "${failed}"
}

# Driver coverage:
#   epoch_stress        multi-connection stress (commit epochs under N sessions)
#   hits_under_writes   build_race (exactness while a rebuild races writers) and the continuous
#                       phase of read_after_commit (reads concurrent with writes, bounded by the
#                       writes before/after, mostly answered from the state)
#   read_after_commit   sequential, slow-commit (forced rebuild) and continuous phases, any connection
#   txn_edges           session_isolation: other session's ROLLBACK/COMMIT, savepoint, statement and
#                       disconnect rollback each undo exactly that session's writes
#   differential        read differential; no RF differential
#   restart             77: the plugin lives inside mysqld
#   own_writes, ddl_events   fail closed: no harness reads inside a writing transaction (the plugin
#                       refuses it) or runs DROP/RENAME/recreate of a tracked table
cmd_slot() { # scenario seconds
  local scenario="$1" seconds="$2" rc=0
  case "${scenario}" in
    epoch_stress)
      ensure_built test_mysql_aux_multi_connection_stress
      ACCEL_MULTI_CONN_STRESS_SEC="${seconds}" in_container test_mysql_aux_multi_connection_stress || rc=1 ;;
    hits_under_writes)
      ensure_built test_mysql_aux_build_race test_mysql_aux_read_after_commit
      in_container test_mysql_aux_build_race || rc=1
      in_container test_mysql_aux_read_after_commit 10 1 2 3 "${seconds}" || rc=1 ;;
    read_after_commit)
      ensure_built test_mysql_aux_read_after_commit
      in_container test_mysql_aux_read_after_commit 100 2 2 3 "${seconds}" || rc=1 ;;
    txn_edges)
      ensure_built test_mysql_aux_session_isolation
      in_container test_mysql_aux_session_isolation || rc=1 ;;
    restart)
      echo "mysql has no separate accelerator process"; return 77 ;;
    differential)
      ensure_built test_mysql_tpch_differential
      differential || rc=1 ;;
    own_writes | ddl_events)
      echo "mysql: ${scenario} has no driver yet (no read inside a writing transaction / DDL of a tracked table)"
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

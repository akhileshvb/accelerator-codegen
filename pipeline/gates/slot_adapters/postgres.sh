# Slot gate adapter: Postgres (process per connection: shard worker per
# accelerator). Maps each scenario onto the shard-worker suite
# eval/tests/test_postgres_shared_state.cpp, run inside accel_pg_test where the
# server's processes are visible. Sourced by run_slot_gates.sh.
#
# Needs (the skeleton port, branch skel/pg-worker): that suite, scripts/
# run_postgres_tpch_diff_all22.sh and the shard-worker backend. Without them
# the gate fails closed.
#
# Not covered by the suite yet; a PostgresEngine driver for
# eval/harness/slot_gates.hpp (as DuckDB has) would cover them:
#   hits_under_writes   only REPEATABLE READ readers; READ COMMITTED needs a
#                       reader that brackets its answer between two quiet reads
#   read_after_commit   only the committing connection (read_after_own_commit);
#                       a read on another connection right after a slow commit
#   differential        no RF-chunk differential; mode write_maint (writes, then
#                       an exact rematch) stands in for it
#   own_writes          not run (77): the suite has no read inside a writing
#                       transaction (such a read must equal SQL with its rows)
#   ddl_events          not run (77): truncate_rebuild covers TRUNCATE only; DROP /
#                       RENAME / recreate of a tracked table and DDL during a build
#                       (the lock cycle of a build's table locks against DDL) are
#                       not exercised

# shellcheck source=../aux_gate_common.sh
source "${ROOT}/pipeline/gates/aux_gate_common.sh"

_pg_driver() { # scenario...
  local suite=eval/tests/test_postgres_shared_state.cpp scenario rc=0
  if [[ ! -f "${ROOT}/${suite}" ]]; then
    echo "missing ${suite}: merge the shard-worker port (skel/pg-worker)"
    return 1
  fi
  for scenario in "$@"; do
    run_postgres_docker_harness test_postgres_shared_state "${scenario}" || rc=1
    echo "${PG_DOCKER_LOG}"
    # The suite exits 0 for a scenario it does not know: require its PASS line.
    if ! grep -q "^PASS ${scenario}\b" <<<"${PG_DOCKER_LOG}"; then
      echo "scenario ${scenario} did not report PASS"
      rc=1
    fi
  done
  return "${rc}"
}

# The backend's own standalone tests (shared-memory ring, shard transport):
# built by backends/postgres/backend.cmake, no server needed, run with epoch_stress.
_pg_backend_unit_tests() {
  local binary rc=0
  for binary in test_shm_ring test_shard_segment; do
    run_postgres_docker_harness "${binary}" "" || rc=1
    echo "${PG_DOCKER_LOG}"
  done
  return "${rc}"
}

slot_adapter_run() { # scenario seconds
  case "$1" in
    epoch_stress)      _pg_driver concurrent && _pg_backend_unit_tests ;;
    hits_under_writes) _pg_driver hits_under_writes ;;
    read_after_commit) _pg_driver read_after_own_commit hit_across_connections ;;
    txn_edges)         _pg_driver abort_and_savepoint truncate_rebuild ;;
    restart)           _pg_driver restart_worker kill_backend ;;
    own_writes | ddl_events)
      echo "postgres: ${1} is not wired yet (needs a PostgresEngine driver for eval/harness/slot_gates.hpp)"
      return 77 ;;
    differential)
      local script=scripts/run_postgres_tpch_diff_all22.sh rc=0
      [[ -f "${ROOT}/${script}" ]] || { echo "missing ${script}"; return 1; }
      bash "${ROOT}/${script}" || rc=1
      ACCEL_PG_DIFF_MODE=write_maint bash "${ROOT}/${script}" || rc=1
      return "${rc}" ;;
    *) echo "unknown scenario $1"; return 1 ;;
  esac
}

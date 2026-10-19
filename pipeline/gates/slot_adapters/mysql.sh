# Slot gate adapter: MySQL (in-process plugin; counter under a commit mutex;
# readers at the newest epoch). Maps each scenario onto the MySQL harnesses run
# inside accel_mysql_test. Sourced by run_slot_gates.sh.
#
# Needs (the skeleton port, branch skel/mysql): the eval/tests/test_mysql_*
# harnesses, scripts/ensure_accel_mysql_test.sh, scripts/build_accel_mysql_plugin.sh
# and scripts/run_mysql_tpch_diff_all22.sh. Without them the gate fails closed.
#
# Not covered by the harnesses yet; a MysqlEngine driver for
# eval/harness/slot_gates.hpp would cover them:
#   hits_under_writes   exactness while a rebuild races writers (build_race), but
#                       no hit rate
#   read_after_commit   cross-session visibility (session_isolation), but not a
#                       read right after a slow commit (the stale-read case the
#                       per-commit epoch removes)
#   differential        read differential only; there is no RF differential
#   restart             N/A: the plugin lives inside mysqld
#   own_writes          not run (77): no harness reads inside a writing transaction
#                       (the plugin refuses such a read; the gate wants a SQL answer
#                       that includes the transaction's rows)
#   ddl_events          not run (77): test_mysql_aux_build_ddl_race covers trigger
#                       DDL racing a multi-lane build, not DROP / RENAME / recreate
#                       of a tracked table

_mysql_driver() { # binary args...
  local binary="$1"; shift
  local container="${ACCEL_MYSQL_CONTAINER:-accel_mysql_test}"
  if [[ ! -f "${ROOT}/eval/tests/${binary}.cpp" ]]; then
    echo "missing eval/tests/${binary}.cpp: merge the MySQL skeleton port (skel/mysql)"
    return 1
  fi
  bash "${ROOT}/scripts/ensure_accel_mysql_test.sh" || return 1
  ACCEL_MYSQL_NINJA_TARGETS="${binary}" bash "${ROOT}/scripts/build_accel_mysql_plugin.sh" || return 1
  docker exec \
    -e ACCEL_MYSQL_DOCKER=1 -e MYSQL_HOST=127.0.0.1 -e MYSQL_PORT=3306 \
    -e MYSQL_USER=root -e MYSQL_PWD="${MYSQL_PWD:-mysql}" \
    -e TPCH_MYSQL_DB="${TPCH_MYSQL_DB:-accel_mysql_test}" \
    -e ACCEL_MULTI_CONN_STRESS_SEC="${ACCEL_SLOT_GATE_SECONDS:-6}" \
    -e LD_LIBRARY_PATH=/accel-build/accel:/accel-build/duckdb/src \
    "${container}" "/accel-build/accel/${binary}" "$@"
}

slot_adapter_run() { # scenario seconds
  case "$1" in
    epoch_stress)      _mysql_driver test_mysql_aux_multi_connection_stress ;;
    hits_under_writes) _mysql_driver test_mysql_aux_build_race ;;
    read_after_commit) _mysql_driver test_mysql_aux_session_isolation ;;
    txn_edges)         _mysql_driver test_mysql_aux_session_isolation ;;
    restart)           echo "mysql has no separate accelerator process"; return 77 ;;
    own_writes | ddl_events)
      echo "mysql: ${1} is not wired yet (needs a MysqlEngine driver for eval/harness/slot_gates.hpp)"
      return 77 ;;
    differential)
      local script=scripts/run_mysql_tpch_diff_all22.sh
      [[ -f "${ROOT}/${script}" ]] || { echo "missing ${script}"; return 1; }
      bash "${ROOT}/${script}" ;;
    *) echo "unknown scenario $1"; return 1 ;;
  esac
}

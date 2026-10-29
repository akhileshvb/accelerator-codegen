#!/usr/bin/env bash
# Correctness tests of a benchmark's Verus kits (accel_<bench>_qN_vgen; default tpch) from the repo's own inputs:
# per kit, verus_kit_runner differential + snapshots on a private copy of the test database,
# with the kit's workload (accelerators/<bench>/qN/tests/workload: setup.sql, params.sql,
# txns/) and the declared schema's NOT NULL constraints (workloads/<bench>/schema_constraints.sql).
# validate (no accelerator) first checks the workload itself against the query
# (workloads/<bench>/qN.sql): setup runs, every tuple evaluates, every txn applies in sequence.
# Prints a summary table; exit 1 if any kit has a mismatch or a failed run.
#
#   scripts/run_verus_kit_tests.sh <build_dir> [query ...]      (default: all 22)
#
# Env:
#   BENCH        tpch    (null_double: NULL and DOUBLE cells, TEST_DB from
#                accelerators/null_double/q1/tests/make_test_db.sh)
#   TEST_DB      pristine test database, never modified (default ../accelerator-codegen-duckdb/tpch_test.duckdb;
#                scripts/create_tpch_test_db.py makes one from the current schema: BIGINT keys, which the
#                big-key transactions of the workloads (keys up to 2^62) need)
#   OUT          results dir (default <build_dir>/verus_kit_tests): qN_<mode>.{json,err,rc}, summary.txt
#   MODES        "validate differential snapshots"     STORE  ACCEL_AUX_STORE (default verus_global)
#   DIFF_EVERY   10 (all tuples checked every N txns)
#   SNAP_WRITES  200   SNAP_READERS 4   SNAP_SEED 1
#   KIT_JOBS     kits run in parallel (default 1; each run has its own database copy)
#   TIMEOUT      per run, seconds (default 900)
#   ACCEL_VERUS_SCHEMA_SQL  overrides the schema constraints
# A query with workloads/<bench>/qN.bounds.sql (eval/harness/float_bounds.hpp) rounds DOUBLE SUM / AVG in an
# engine-chosen order: its answers are accepted iff admissible, not only if equal to the reference.
# The kits must be built into <build_dir> (ACCEL_VERUS_LIB_DIR at configure time, then the
# verus_kit_runner target); see scripts/build_verus_crate.sh and scripts/build_verus_world.sh.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ $# -ge 1 ]] || { sed -n '2,20p' "$0" >&2; exit 2; }
BUILD="$(cd "$1" && pwd)"; shift
BENCH="${BENCH:-tpch}"
QUERIES="${*:-$(seq -s ' ' 1 22)}"
RUNNER="${RUNNER:-${BUILD}/verus_kit_runner}"
TEST_DB="${TEST_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_test.duckdb}"
OUT="${OUT:-${BUILD}/verus_kit_tests}"
MODES="${MODES:-validate differential snapshots}"
SCHEMA="${ACCEL_VERUS_SCHEMA_SQL:-${ROOT}/workloads/${BENCH}/schema_constraints.sql}"
KIT_JOBS="${KIT_JOBS:-1}"
TIMEOUT="${TIMEOUT:-900}"

[[ -x "${RUNNER}" ]] || { echo "no runner at ${RUNNER} (cmake --build ${BUILD} --target verus_kit_runner)" >&2; exit 2; }
[[ -f "${TEST_DB}" ]] || { echo "no test database at ${TEST_DB} (set TEST_DB)" >&2; exit 2; }
[[ -f "${SCHEMA}" ]] || { echo "no schema constraints at ${SCHEMA} (scripts/gen_verus_kit_params.py)" >&2; exit 2; }
mkdir -p "${OUT}/dbs"

run_one() {  # q mode
  local q=$1 mode=$2 spec="${ROOT}/accelerators/${BENCH}/q$1/tests/workload" db="${OUT}/dbs/q$1_$2.duckdb" args
  if [[ ! -d "${spec}/txns" ]]; then echo "missing" > "${OUT}/q${q}_${mode}.rc"; return; fi
  case ${mode} in
    validate)     args=("${db}" "${spec}" "${ROOT}/workloads/${BENCH}/q${q}.sql") ;;
    differential) args=("${db}" "${BENCH}_q${q}_vgen" "${spec}" "${DIFF_EVERY:-10}") ;;
    snapshots)    args=("${db}" "${BENCH}_q${q}_vgen" "${spec}" "${SNAP_WRITES:-200}" "${SNAP_READERS:-4}" "${SNAP_SEED:-1}") ;;
    *) echo "unknown mode ${mode}" >&2; return ;;
  esac
  rm -f "${db}" "${db}.wal"; cp "${TEST_DB}" "${db}"
  local bounds="${ROOT}/workloads/${BENCH}/q${q}.bounds.sql"; [[ -f "${bounds}" ]] || bounds=  # rounded DOUBLE aggregates
  VERUS_KIT_BOUNDS_SQL="${bounds}" ACCEL_AUX_STORE="${STORE:-verus_global}" ACCEL_VERUS_SCHEMA_SQL="${SCHEMA}" \
    timeout "${TIMEOUT}" "${RUNNER}" "${mode}" "${args[@]}" \
    > "${OUT}/q${q}_${mode}.json" 2> "${OUT}/q${q}_${mode}.err"
  echo $? > "${OUT}/q${q}_${mode}.rc"
  rm -f "${db}" "${db}.wal"
}

for q in ${QUERIES}; do
  for mode in ${MODES}; do
    rm -f "${OUT}/q${q}_${mode}".{json,err,rc}
    while [[ "$(jobs -rp | wc -l)" -ge "${KIT_JOBS}" ]]; do wait -n; done
    run_one "${q}" "${mode}" &
  done
done
wait
rmdir "${OUT}/dbs" 2>/dev/null

python3 -I - "${OUT}" "${QUERIES}" "${MODES}" <<'PY' | tee "${OUT}/summary.txt"
import json, os, sys
out, queries, modes = sys.argv[1], sys.argv[2].split(), sys.argv[3].split()
print("Verus kit tests  (mm = mismatches + order mismatches + reader failures; fb = other fallbacks (failures); pt = parameter outside its type, kd = proof-allowed kernel decline (neither fails);")
print("                  rfb = reader fallbacks; we = write errors; re = reference errors; chk = checks)")
print("              (validate: tuples with a nonempty answer / tuples; txn errors; setup/param errors)")
W = {"validate": 26}
print("q    " + "".join(f"| {m:<{W.get(m, 44)}}" for m in modes))
bad = 0
for q in queries:
    cells = []
    for m in modes:
        base = f"{out}/q{q}_{m}"
        rc = open(base + ".rc").read().strip() if os.path.exists(base + ".rc") else "?"
        if rc == "missing":
            cells.append("(no test workload)"); bad += 1; continue
        try:
            j = [json.loads(l) for l in open(base + ".json") if l.startswith("{")][-1]
        except Exception:
            err = open(base + ".err").read().strip().splitlines() if os.path.exists(base + ".err") else []
            cells.append(f"ERROR rc={rc}: {(err[-1] if err else '')[:80]}"); bad += 1; continue
        if m == "validate":
            errs = [e for e in j.get("errors", []) if not e.startswith("txn ")]
            bad += rc != "0"
            cells.append(f"{j.get('params_nonempty')}/{j.get('params')} te={j.get('txn_errors')} err={len(errs)}"
                         + ("" if rc == "0" else f" rc={rc}"))
            continue
        mm = j.get("mismatches", 0) + j.get("order_mismatches", 0) + j.get("reader_failures", 0)
        fb = j.get("executes", {}).get("fallback", 0)
        # answered by SQL by design, so reported but not failures: pt, a parameter outside its type
        # (NULL, too many digits); kd, a kernel decline, which its proof allows only outside the
        # envelope, for a zero divisor, or when the answer overflows.  Any other differential
        # fallback (host conversion fault, missing state) fails.
        kinds = j.get("fallback_kinds", {}) if m == "differential" else {}
        pt, kd = kinds.get("param_outside_type", 0), kinds.get("kernel_none", 0)
        bad += mm > 0 or rc != "0" or (m == "differential" and fb > pt + kd)
        cells.append(f"mm={mm} fb={fb - pt - kd}" + (f" pt={pt} kd={kd}" if m == "differential" else "") + f" rfb={j.get('reader_fallbacks', 0)} we={j.get('write_errors', 0)} "
                     f"re={j.get('reference_errors', 0)} chk={j.get('checks')}" + ("" if rc == "0" else f" rc={rc}"))
    print(f"q{q:<4}" + "".join(f"| {c:<{W.get(m, 44)}}" for m, c in zip(modes, cells)))
print(f"\n{'FAIL' if bad else 'PASS'}: {bad} failing run(s); details in {out}/qN_<mode>.json/.err")
sys.exit(1 if bad else 0)
PY

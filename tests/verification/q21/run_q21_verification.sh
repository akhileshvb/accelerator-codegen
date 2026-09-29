#!/usr/bin/env bash
# Q21 end-to-end verification gate (Stages 2-5, incl. live runtime) + Q1 regressions.
#
# Every mandatory gate below must pass for Q21 to be classified VERIFIED.
# Classification:
#   VERIFIED  every gate passes and RESIDUALS.txt lists no BLOCKING item
#   FAILED    a gate found a counterexample, or RESIDUALS.txt lists a BLOCKING
#             item with a concrete counterexample
#   UNKNOWN   a gate could not run / timed out / a proof obligation is open
#
# Usage: tests/verification/q21/run_q21_verification.sh [--skip-q1]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V="$ROOT/tests/verification"
FSQL="$V/formalsql"
DAFNY="${DAFNY_BIN:-$ROOT/tools/dafny/dafny}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
SKIP_Q1=0; [[ "${1:-}" == "--skip-q1" ]] && SKIP_Q1=1

declare -a NAMES STATUS
unknown=0; failed=0
run() {  # run <name> <cmd...>
  local name="$1"; shift
  echo "==> $name"
  if "$@" > "$WORK/$name.log" 2>&1; then
    NAMES+=("$name"); STATUS+=("PASS")
  else
    NAMES+=("$name"); STATUS+=("FAIL"); failed=1
    tail -20 "$WORK/$name.log"
  fi
}

# [1] Catalog / Logos pin: catalog base_sql hash == pinned hash == Rocq constant.
catalog_pin() {
  python3 - "$ROOT" <<'PY'
import hashlib, re, sys
from pathlib import Path
root = Path(sys.argv[1])
text = (root / "src/generated/catalog/postgres/query_catalog.cpp").read_text()
m = re.search(r'"tpch_q21",\s*/\* base_sql \*/\s*((?:"(?:\\.|[^"\\])*\"\s*)+),', text)
parts = re.findall(r'"((?:\\.|[^"\\])*)"', m.group(1))
sql = "".join(bytes(p, "utf-8").decode("unicode_escape") for p in parts).strip() + "\n"
d = hashlib.sha256(sql.encode()).hexdigest()
fs = root / "tests/verification/formalsql/q21"
pinned = (fs / "catalog_sql.sha256").read_text().strip()
rocq = re.search(r'catalog_sql_sha256 : string :=\s*"([0-9a-f]+)"', (fs / "Q21Catalog.v").read_text()).group(1)
committed = (fs / "catalog_base_sql.sql").read_text()
assert d == pinned == rocq, (d, pinned, rocq)
assert committed == sql, "committed catalog_base_sql.sql differs from catalog"
print("Q21 catalog pin OK", d)
PY
}
run catalog_pin catalog_pin

# [2] Stage 2: FormalSQL -> RelNode (all-outcomes) + Print Assumptions.
run stage2_compile bash "$FSQL/scripts/compile_q21_stage2.sh"
run stage2_print_assumptions bash "$FSQL/scripts/check_print_assumptions.sh" q21

# [3] RelNode spec export (Rocq DATA == dump == Dafny DATA) + interpreter pins.
run relnode_export bash "$V/q21/check_q21_relnode_export.sh"
run interpreter_pins bash "$FSQL/resultspec/check_spec_export.sh"

# [4] Dafny: semantic core, Aux/Inv, Build, Maintenance, Exec, ordering,
#     permutation invariance, runtime model (warnings are failures).
Q21_DFY=(Q21SqlCore Q21SqlAgg Q21Count Q21SetSum Q21Contrib Q21ModuleTwin Q21Sorted
         Q21Answer Q21Emit Q21Inv Q21Refine Q21Build Q21Delta Q21MaintNw Q21Maint
         Q21Perm Q21Runtime)
for f in "${Q21_DFY[@]}"; do
  run "dafny_$f" "$DAFNY" verify "$V/q21/dafny/$f.dfy"
done

# [5] No assumptions / axioms / disabled verification in the Q21 Dafny proof.
forbidden_scan() {
  ! grep -nE '(^|[^A-Za-z_])assume([^A-Za-z_]|$)|\{:axiom\}|\{:verify false\}|\{:extern|\bexpect\b' \
      "$V"/q21/dafny/*.dfy
}
run dafny_no_assume forbidden_scan

# [6] C++ differential (fixed module vs brute-force SQL; finite).
run cpp_differential bash "$V/q21/cpp/run_q21_aux_differential.sh"

# [7] C++ <-> Twin extract-diff (compiled Dafny twin vs generated C++; finite).
extract_diff() {
  local R="$ROOT"
  "$DAFNY" build --target py --no-verify --output "$WORK/twin" "$V/q21/dafny/Q21ModuleTwin.dfy" >/dev/null || return 1
  local SRC=("$R/src/stateful/core/aux_engine_services.cpp" "$R/src/stateful/core/aux_maintenance_policy.cpp"
             "$R/src/stateful/core/aux_maintenance_decision_log.cpp" "$R/src/stateful/maintenance/action_selector.cpp"
             "$R/src/stateful/state/invalidation_plan.cpp")
  g++ -std=c++20 -O2 -DQ21_HDR='"tpch/q21/postgres/aux/module.hpp"' -I"$R/include" -I"$R/src/generated" -I"$R/src" \
      "$V/q21/cpp/q21_twin_trace.cpp" "$R/src/generated/tpch/q21/postgres/aux/module.cpp" "${SRC[@]}" \
      -o "$WORK/q21_trace" || return 1
  "$WORK/q21_trace" "${Q21_EXTRACT_SEEDS:-1000}" 40 > "$WORK/trace.jsonl" || return 1
  python3 "$V/q21/cpp/q21_twin_replay.py" "$WORK/twin-py" < "$WORK/trace.jsonl"
}
run extract_diff extract_diff

# [8] Stage 5 live runtime (Postgres 17 container accel_pg_test): build and
#     install the extension from the current sources (no stale .so), recreate
#     accel_q21_rt, run the deterministic protocol repros (E1 multi-backend,
#     E2 savepoints, E3 build in dirty/aborted txn, E4 TRUNCATE; hit/fallback
#     asserted) and the concurrent randomized stress.  Unreachable server =>
#     UNKNOWN, never PASS.  Skip with Q21_SKIP_LIVE_PG=1 (=> UNKNOWN).
if [[ "${Q21_SKIP_LIVE_PG:-0}" == "1" ]] || \
   ! PGPASSWORD=postgres psql -h 127.0.0.1 -p "${PGPORT:-5434}" -U postgres -d postgres -tAc 'select 1' >/dev/null 2>&1; then
  echo "==> runtime_live: Postgres unavailable or skipped -> UNKNOWN"
  NAMES+=("runtime_live"); STATUS+=("UNKNOWN"); unknown=1
else
  run runtime_build bash "$ROOT/scripts/build_accel_postgres_extension.sh"
  run runtime_db_setup bash "$V/q21/runtime/setup_q21_rt_db.sh"
  run runtime_repro python3 "$V/q21/runtime/q21_runtime_repro.py"
  run runtime_stress python3 "$V/q21/runtime/q21_runtime_stress.py" "${Q21_STRESS_SECS:-60}" 4 1
fi

# [9] Q1 regressions.
if [[ "$SKIP_Q1" -eq 0 ]]; then
  run q1_compile_proof bash "$FSQL/scripts/compile_q1_proof.sh"
  run q1_compile_normalize_sound bash "$FSQL/scripts/compile_q1_normalize_sound.sh"
  run q1_print_assumptions bash "$FSQL/scripts/check_print_assumptions.sh" q1
  for f in generated/Q1AuxSpec Q1TwinSpecBridge Q1Finalize Q1ModuleTwin Q1Aux Q1TwinAuxRefinement \
           Q1TwinAuxClose Q1RuntimeAuxCorrect; do
    run "q1_dafny_$(basename "$f")" "$DAFNY" verify "$V/q1/dafny/$f.dfy"
  done
fi

echo
echo "================ Q21 gate summary ================"
for i in "${!NAMES[@]}"; do printf "  %-34s %s\n" "${NAMES[$i]}" "${STATUS[$i]}"; done
blocking="$(grep -E '^BLOCKING' "$V/q21/RESIDUALS.txt" || true)"
echo
if [[ "$failed" -ne 0 ]]; then
  echo "CLASSIFICATION: FAILED (a mandatory gate failed; see logs above)"; exit 1
elif [[ -n "$blocking" ]]; then
  echo "$blocking"
  echo "CLASSIFICATION: FAILED (blocking residual with a concrete counterexample; see q21/RESIDUALS.txt)"; exit 1
elif [[ "$unknown" -ne 0 ]]; then
  echo "CLASSIFICATION: UNKNOWN"; exit 2
else
  echo "CLASSIFICATION: VERIFIED"
fi

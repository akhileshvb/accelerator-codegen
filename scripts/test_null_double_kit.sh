#!/usr/bin/env bash
# NULL-able and DOUBLE cells through the Verus kit, end to end, on a cut of the SEC EDGAR
# `num` table (TPC-H has neither).  The query is workloads/null_double/q1.sql; sql2verus
# generates its spec, obligations, world, FFI, header and host glue into
# accelerators/null_double/q1/verus_kit/null_double_q1_vgen/; the kernel there (kernel.rs,
# f64bits.rs) is written by hand and verified against them, then built with its world.
# verus_kit_runner runs differential and snapshots (scripts/run_verus_kit_tests.sh,
# BENCH=null_double) over the workload in accelerators/null_double/q1/tests/workload,
# whose setup.sql and txns put NULLs and edge doubles into every cell the query reads.
#
#   scripts/test_null_double_kit.sh <build_dir>
#
# <build_dir>: a CMake build directory whose ACCEL_VERUS_LIB_DIR is writable (it gets the
# verified libraries and kits).  Env: STORE (ACCEL_AUX_STORE, default verus_global), ROWS
# (rows of num, default 5000), JOBS, and those of make_test_db.sh and the CMake configure
# (ACCEL_DUCKDB_ROOT).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ $# -eq 1 ]] || { sed -n '2,16p' "$0" >&2; exit 2; }
BUILD="$(cd "$1" && pwd)"
export ACCEL_VERUS_LIB_DIR="$(sed -n 's/^ACCEL_VERUS_LIB_DIR:PATH=//p' "${BUILD}/CMakeCache.txt")"
export TMPDIR="${TMPDIR:-/dev/shm}"
BENCH=null_double
KIT="${ROOT}/accelerators/${BENCH}/q1/verus_kit/${BENCH}_q1_vgen"

cargo build --release --quiet --manifest-path "${ROOT}/substrate/verus/sql2verus/Cargo.toml"
"${CARGO_TARGET_DIR:-${ROOT}/substrate/verus/sql2verus/target}/release/sql2verus" \
  --schema "${ROOT}/workloads/${BENCH}/schema.sql" --query "${ROOT}/workloads/${BENCH}/q1.sql" \
  --out "${KIT}/crate/src/gen_spec.rs" --obligations "${KIT}/crate/src/gen_obligations.rs" \
  --world "${KIT}/crate/src/gen_world.rs" --ffi "${KIT}/crate/src/gen_ffi.rs" \
  --ffi-header "${KIT}/${BENCH}_q1_vgen_verus.h" --ffi-prefix nulldoubleq1vgen \
  --host-glue "${KIT}/glue.cpp" --accel-id "${BENCH}_q1_vgen" --workload sec_edgar --query-id 100
python3 "${ROOT}/scripts/gen_verus_world.py" "${BENCH}" "${BENCH}_q1_vgen"
bash "${ROOT}/scripts/build_verus_crate.sh" "${KIT}/crate" "${BENCH}_q1_vgen" --rlib
bash "${ROOT}/scripts/build_verus_world.sh" "${BENCH}"
cmake -S "${ROOT}" -B "${BUILD}" > /dev/null
cmake --build "${BUILD}" --target verus_kit_runner -j "${JOBS:-8}"

DB="${BUILD}/${BENCH}_test.duckdb"
"${ROOT}/accelerators/${BENCH}/q1/tests/make_test_db.sh" "${DB}" "${ROWS:-5000}"
BENCH="${BENCH}" TEST_DB="${DB}" OUT="${BUILD}/${BENCH}_tests" \
  "${ROOT}/scripts/run_verus_kit_tests.sh" "${BUILD}" 1

// SCAFFOLDING
#pragma once

#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::harness {

// The argument lists a test or bench runs <workload> q<query_id> with: SQL literals in the $n order of the
// query template (e.g. "4000, 4999, 'EQ', 'USD'"), the `cpp` field of the generator's sets.
//
// Source: the JSON file $ACCEL_PARAM_FILE if set, else the workload's generator
//   python3 $ACCEL_SRC_DIR/eval/bench/<workload>/gen_params.py $ACCEL_PARAM_SETS $ACCEL_PARAM_SEED
// (sets default 8, seed default a fresh random one). Drawn once per process; prints
// "[params] <workload> q<N> seed=<s> sets=<k>" to stderr, so a run reproduces with ACCEL_PARAM_SEED=<s>.
const std::vector<std::string> &DrawArgumentLists(const std::string &workload, int query_id);

} // namespace accelerator_codegen::aux_state::harness

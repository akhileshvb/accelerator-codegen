#include "tpch/q15/mysql/aux/module.hpp"

#include <algorithm>
#include <limits>

namespace accelerator_codegen::aux_state::query {

// The winner derivation (max lookup, tie enumeration, directory join, ordering)
// lives directly on the read path; this file is kept as the lightweight
// per-query finalize anchor required by the module layout.

} // namespace accelerator_codegen::aux_state::query

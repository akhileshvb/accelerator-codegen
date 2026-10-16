#pragma once
// Moved to substrate/aux/core/partitioned_build.hpp.
#include "substrate/aux/core/partitioned_build.hpp"

namespace accelerator_codegen::aux_state::query {
using core::AppendBuckets;
using core::BuildPartitions;
using core::Buckets;
template <class Input> using OneRowBatch = core::SingleRowBatch<Input>;
} // namespace accelerator_codegen::aux_state::query

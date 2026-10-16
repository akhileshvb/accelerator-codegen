#pragma once
// Moved to substrate/aux/core/partitioned_build.hpp.
#include "substrate/aux/core/partitioned_build.hpp"

namespace accelerator_codegen::aux_state::query {
template <class T> using TypedRowBatch = core::SingleRowBatch<T>;
} // namespace accelerator_codegen::aux_state::query

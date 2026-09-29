#pragma once

#include <atomic>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// Global aux mutation epoch for TokenForSnapshot / PairInv.
// Starts at 1 so epoch filtering is active (epoch==0 disables the filter).
// Bumped at OnPreCommit (before Invalidate) and again before Commit TryPublish
// so concurrent readers that pinned an older epoch cannot observe Aux
// published after their pin (or mid-invalidate Valid stale state).
core::Version AuxMutationEpoch();
core::Version BumpAuxMutationEpoch();

} // namespace accelerator_codegen::aux_state::engine::postgres

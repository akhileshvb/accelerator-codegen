// GENERATED BACKEND
//
// The active accelerators' view of the tables they depend on, for the write
// path: what the capture trigger records and which accelerators a commit
// takes epochs of. Rebuilt whenever shared::Generation() changes.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <cstdint>
#include <set>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

struct WatchedTable {
  // Union of what the active accelerators on the table watch (0-based
  // physical attnums, ascending); `whole_row`: some accelerator watches all.
  bool whole_row = false;
  std::vector<int32_t> attnums;
};

// nullptr if no active accelerator depends on `table`.
const WatchedTable *WatchedBy(core::TableId table);
// Whether active accelerator `index` depends on `table`.
bool Depends(int index, core::TableId table);
// The active accelerators (ascending index) that depend on any of `tables`.
std::vector<int> AcceleratorsOn(const std::set<core::TableId> &tables);

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif

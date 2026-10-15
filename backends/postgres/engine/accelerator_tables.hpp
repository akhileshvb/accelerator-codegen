// GENERATED BACKEND
//
// The active accelerators' view of the tables they depend on, for the write
// path: what the capture trigger records and which accelerators a commit
// takes epochs of. Rebuilt whenever shared::Generation() changes.
//
// Tables are identified by relation oid here and in every batch; a module's
// own numbering (Dependencies()) is mapped to oids through the accelerator's
// Tables().
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <cstdint>
#include <set>
#include <vector>

#include "backends/postgres/api/accelerators.hpp"
#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/runtime/accelerator_runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

struct WatchedTable {
  // Union of what the active accelerators on the table watch (0-based
  // physical attnums, ascending); `whole_row`: some accelerator watches all.
  bool whole_row = false;
  std::vector<int32_t> attnums;
};

// The module's table numbers -> relation oids for `accelerator`, resolved in
// this session (raises an ERROR for a relation that does not exist).
runtime::TableMap HostTableMap(const PostgresAccelerator &accelerator);
// The oids of the tables active accelerator `index` depends on.
std::vector<core::TableId> HostTables(int index);

// nullptr if no active accelerator depends on `table`.
const WatchedTable *WatchedBy(core::TableId table);
// Whether active accelerator `index` depends on `table`.
bool Depends(int index, core::TableId table);
// The active accelerators (ascending index) that depend on any of `tables`.
std::vector<int> AcceleratorsOn(const std::set<core::TableId> &tables);

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif

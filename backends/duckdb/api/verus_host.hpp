// GENERATED BACKEND
#pragma once

// The DuckDB host of the Verus accelerators: everything that binds the Verus
// host kit (substrate/verus/kit/verus_kit.hpp: the generated glue, its
// KitModule) and the Verus store (ACCEL_AUX_STORE=verus / verus_global,
// substrate/aux/state/verus_aux_store.hpp) into the DuckDB runtime and its
// SQL table functions.  The kit's catalog entries are built in
// catalog/query_catalog.cpp (VerusKitCatalogEntry).

#include <memory>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "substrate/aux/state/verus_aux_store.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// The runtime binding of the Verus kit registered for `accelerator_id` (its
// KitModule over a host on `connection`), or null if no kit is registered.
std::unique_ptr<DuckdbRuntime::Binding> MakeVerusKitBinding(::duckdb::Connection &connection,
                                                            const std::string &accelerator_id);

// Whether `accelerator_id` is a Verus kit accelerator (verus_kit::Find).
bool IsVerusKitAccelerator(const std::string &accelerator_id);

// The accelerator's Verus store when ACCEL_AUX_STORE asks for it and the
// accelerator has one; else null (the runtime then uses its C++ store).
std::unique_ptr<VerusAuxStore> MakeRequestedVerusStore(const core::AcceleratorId &accelerator,
                                                       VerusAuxStore::EpochProvider epoch,
                                                       VerusAuxStore::FloorProvider floor);

// The SQL table function of one Verus kit accelerator: accel_<kit id>, its
// arguments typed by the glue's parameter kinds.
struct VerusKitTableFunction {
  const SharedQueryCatalogEntry *entry = nullptr;
  std::string name;
  ::duckdb::vector<::duckdb::LogicalType> argument_types;
};
// Every registered kit that has a catalog entry, in registration order.
std::vector<VerusKitTableFunction> VerusKitTableFunctions();

} // namespace accelerator_codegen::aux_state::engine::duckdb

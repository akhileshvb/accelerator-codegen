// SCAFFOLDING
#pragma once

#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/core/aux_types.hpp"
#include "accelerator_codegen/aux_state/engine/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/aux_state/engine/shared/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::harness {

namespace aux_engine = ::accelerator_codegen::aux_state::engine;

void CompareAuxRowsToBase(duckdb::Connection &connection,
                          const aux_engine::SharedQueryCatalogEntry &entry,
                          const std::vector<core::AuxRow> &accelerated);

void RequireSharedMatchesBase(duckdb::Connection &connection,
                              aux_engine::duckdb::DuckdbRuntime &runtime,
                              const aux_engine::SharedQueryCatalogEntry &entry);

void RunDifferentialCheck(const std::string &db_path,
                          const aux_engine::SharedQueryCatalogEntry &entry);

void RunIncrementalDifferentialCheck(const std::string &db_path,
                                     const aux_engine::SharedQueryCatalogEntry &entry);

} // namespace accelerator_codegen::aux_state::harness

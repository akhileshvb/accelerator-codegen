// SCAFFOLDING
#pragma once

#include <string>
#include <vector>

#include "catalog/query_catalog.hpp"
#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::harness {

namespace aux_engine = ::accelerator_codegen::aux_state::engine;

// The arguments a test or bench runs `entry` with: its default binding if it has one (one set),
// else the sets DrawArgumentLists draws for its workload and query, typed by the entry's signature.
std::vector<core::QueryArguments> ArgumentSets(const aux_engine::SharedQueryCatalogEntry &entry);

// A set as its SQL literal list ("4000, 4999, 'EQ', 'USD'"): for labels and calls.
std::string ArgumentLiterals(const core::QueryArguments &arguments);

} // namespace accelerator_codegen::aux_state::harness

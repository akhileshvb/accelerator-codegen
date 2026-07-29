#pragma once

#include <vector>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/tpch_q10_finalize.hpp"

namespace accelerator_codegen {

duckdb::unique_ptr<KernelInstance> CreateQ10AuxFastKernel(const Q10SideStores &side_stores,
                                                          std::vector<Q10RawSortKey> sort_keys);

} // namespace accelerator_codegen

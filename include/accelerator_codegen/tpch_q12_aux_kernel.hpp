#pragma once

#include <vector>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/tpch_q12_finalize.hpp"

namespace accelerator_codegen {

duckdb::unique_ptr<KernelInstance> CreateQ12AuxFastKernel(std::vector<Q12OutputRow> rows);

} // namespace accelerator_codegen

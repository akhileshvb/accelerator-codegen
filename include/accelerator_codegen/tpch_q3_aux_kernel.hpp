#pragma once

#include <vector>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/tpch_q3_finalize.hpp"

namespace accelerator_codegen {

duckdb::unique_ptr<KernelInstance> CreateQ3AuxFastKernel(std::vector<Q3OutputRow> rows);

} // namespace accelerator_codegen

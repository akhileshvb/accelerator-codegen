#pragma once

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/tpch_q6_finalize.hpp"

namespace accelerator_codegen {

duckdb::unique_ptr<KernelInstance> CreateQ6AuxFastKernel(Q6AccT revenue_raw);

} // namespace accelerator_codegen

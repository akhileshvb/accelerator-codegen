#pragma once

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"

namespace accelerator_codegen {

duckdb::unique_ptr<KernelInstance> CreateQ14AuxFastKernel(const Q14MonthTotals &totals);

} // namespace accelerator_codegen

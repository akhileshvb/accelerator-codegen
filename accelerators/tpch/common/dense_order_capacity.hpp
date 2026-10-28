#pragma once

#include "backends/duckdb/api/parallel/parallel_runtime.hpp"

namespace accelerator_codegen {

//! TPC-H o_orderkey spans ~4× orders cardinality (SF1: 1.5M rows → keys ≤ 6M).
inline int64_t EstimateTpchDenseOrderCapacity(duckdb::ClientContext &context,
                                              const duckdb::FunctionData &bind_data) {
	NativeParallelScanHandle handle;
	handle.Initialize(context, bind_data);
	const auto rows = handle.storage->GetTotalRows();
	const auto estimated = static_cast<int64_t>(rows) * 4 + 1;
	return estimated < 1 ? 1 : estimated;
}

} // namespace accelerator_codegen

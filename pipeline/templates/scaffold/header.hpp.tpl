#pragma once

#include <memory>

#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {

struct {{type_prefix}}Parameters final : public QueryParameters {
	duckdb::idx_t row_estimate = 0;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<{{type_prefix}}Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const {{type_prefix}}Parameters &>(other);
		return row_estimate == rhs.row_estimate;
	}
};

struct {{type_prefix}}ProfileCounters {
	duckdb::idx_t pre_rows = 0;
	duckdb::idx_t qualifying_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t flat_chunks = 0;
	duckdb::idx_t dict_chunks = 0;
	duckdb::idx_t generic_chunks = 0;
	duckdb::idx_t distinct_inserts = 0;
	duckdb::idx_t active_groups = 0;
	int64_t pre_scan_us = 0;
	int64_t pre_consume_us = 0;
	int64_t finalize_us = 0;
};

NativeScanSpec {{scan_spec_fn}}();

duckdb::unique_ptr<KernelInstance> {{kernel_fn}}(const {{type_prefix}}Parameters &params);
duckdb::unique_ptr<KernelInstance> {{profiling_kernel_fn}}(const {{type_prefix}}Parameters &params,
                                                           {{type_prefix}}ProfileCounters &counters);

duckdb::TableFunctionSet {{create_fn}}();

} // namespace accelerator_codegen

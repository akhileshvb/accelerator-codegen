#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "duckdb/common/common.hpp"

namespace accelerator_codegen {

struct Q13Parameters final : public QueryParameters {
	std::string comment_pattern;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q13Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q13Parameters &>(other);
		return comment_pattern == rhs.comment_pattern;
	}
};

struct Q13CustomerChunkLayout {
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 0;
};

struct Q13OrdersChunkLayout {
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 0;
	static constexpr duckdb::idx_t COMMENT_INDEX = 1;
};

enum class Q13InputId : duckdb::idx_t {
	CUSTOMER = 0,
	ORDERS = 1,
};

struct Q13ProfileCounters {
	duckdb::idx_t customer_rows_scanned = 0;
	duckdb::idx_t orders_rows_scanned = 0;
	duckdb::idx_t orders_rows_matched = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t customer_flat_fast_rows = 0;
	duckdb::idx_t orders_flat_fast_rows = 0;
	duckdb::idx_t orders_dict_fast_rows = 0;
	bool used_dense_state = false;
	bool used_two_part_like = false;
};

Q13Parameters MakeQ13Parameters(const std::string &comment_pattern);
NativeScanSpec CreateQ13CustomerScanSpec(const Q13Parameters &parameters);
NativeScanSpec CreateQ13OrdersScanSpec(const Q13Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ13Kernel(const Q13Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ13ProfilingKernel(const Q13Parameters &params, Q13ProfileCounters *profile);
duckdb::unique_ptr<KernelInstance> CreateQ13ParallelKernel(const Q13Parameters &params);

} // namespace accelerator_codegen

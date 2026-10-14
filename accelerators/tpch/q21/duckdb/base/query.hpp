#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen {

struct Q21Parameters final : public QueryParameters {
	std::string nation_name;
	int32_t target_nationkey = -1;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q21Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q21Parameters &>(other);
		return nation_name == rhs.nation_name && target_nationkey == rhs.target_nationkey;
	}
};

struct Q21SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 2;
};

struct Q21OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
};

struct Q21LineitemChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
	static constexpr duckdb::idx_t COMMITDATE_INDEX = 2;
	static constexpr duckdb::idx_t RECEIPTDATE_INDEX = 3;
};

enum class Q21InputId : duckdb::idx_t {
	SUPPLIER = 0,
	ORDERS = 1,
	LINEITEM = 2,
};

Q21Parameters MakeQ21Parameters(const std::string &nation_name);
NativeScanSpec CreateQ21SupplierScanSpec(const Q21Parameters &parameters);
NativeScanSpec CreateQ21OrdersScanSpec(const Q21Parameters &parameters);
NativeScanSpec CreateQ21LineitemScanSpec(const Q21Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ21Kernel(const Q21Parameters &params);

duckdb::unique_ptr<KernelInstance> CreateQ21ParallelKernel(const Q21Parameters &params);

} // namespace accelerator_codegen

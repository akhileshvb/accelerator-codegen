#pragma once

#include <cstdint>
#include <memory>

#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen {

struct Q15Parameters final : public QueryParameters {
	duckdb::date_t start_date;
	duckdb::date_t end_date;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q15Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q15Parameters &>(other);
		return start_date == rhs.start_date && end_date == rhs.end_date;
	}
};

struct Q15LineitemChunkLayout {
	static constexpr duckdb::idx_t SHIPDATE_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 2;
	static constexpr duckdb::idx_t DISCOUNT_INDEX = 3;
};

struct Q15SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
	static constexpr duckdb::idx_t ADDRESS_INDEX = 2;
	static constexpr duckdb::idx_t PHONE_INDEX = 3;
};

enum class Q15InputId : duckdb::idx_t {
	LINEITEM = 0,
	SUPPLIER = 1,
};

Q15Parameters MakeQ15Parameters(duckdb::date_t start_date);
NativeScanSpec CreateQ15LineitemScanSpec(const Q15Parameters &parameters);
NativeScanSpec CreateQ15SupplierScanSpec(const Q15Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ15Kernel(const Q15Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ15ParallelKernel(const Q15Parameters &params);

} // namespace accelerator_codegen

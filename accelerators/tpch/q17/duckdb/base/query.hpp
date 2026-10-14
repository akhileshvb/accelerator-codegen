#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"

namespace accelerator_codegen {

struct Q17Parameters final : public QueryParameters {
	std::string brand;
	std::string container;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q17Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q17Parameters &>(other);
		return brand == rhs.brand && container == rhs.container;
	}
};

struct Q17LineitemChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t QUANTITY_INDEX = 1;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 2;
};

struct Q17PartChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t BRAND_INDEX = 1;
	static constexpr duckdb::idx_t CONTAINER_INDEX = 2;
};

//! Input order: part is consumed first, then lineitem (single pass).
enum class Q17InputId : duckdb::idx_t {
	PART = 0,
	LINEITEM = 1,
};

Q17Parameters MakeQ17Parameters(const std::string &brand, const std::string &container);
NativeScanSpec CreateQ17PartScanSpec(const Q17Parameters &parameters);
NativeScanSpec CreateQ17LineitemScanSpec(const Q17Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ17Kernel(const Q17Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ17ParallelKernel(const Q17Parameters &params);

} // namespace accelerator_codegen

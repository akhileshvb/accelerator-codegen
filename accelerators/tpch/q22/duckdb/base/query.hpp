#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"

namespace accelerator_codegen {

struct Q22Parameters final : public QueryParameters {
	std::array<std::string, 7> country_codes {};

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q22Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q22Parameters &>(other);
		return country_codes == rhs.country_codes;
	}
};

struct Q22OrdersChunkLayout {
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 0;
};

struct Q22CustomerChunkLayout {
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 0;
	static constexpr duckdb::idx_t PHONE_INDEX = 1;
	static constexpr duckdb::idx_t ACCTBAL_INDEX = 2;
};

enum class Q22InputId : duckdb::idx_t {
	ORDERS = 0,
	CUSTOMER = 1,
};

Q22Parameters MakeQ22Parameters(const std::array<std::string, 7> &country_codes);
NativeScanSpec CreateQ22OrdersScanSpec(const Q22Parameters &parameters);
NativeScanSpec CreateQ22CustomerScanSpec(const Q22Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ22Kernel(const Q22Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ22ParallelKernel(const Q22Parameters &params);

} // namespace accelerator_codegen

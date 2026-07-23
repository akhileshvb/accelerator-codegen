#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
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

struct Q22ProfileCounters {
	duckdb::idx_t orders_rows_scanned = 0;
	duckdb::idx_t customer_rows_scanned = 0;
	duckdb::idx_t customer_rows_matched = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t orders_flat_fast_rows = 0;
	duckdb::idx_t orders_dict_fast_rows = 0;
	duckdb::idx_t customer_flat_fast_rows = 0;
	duckdb::idx_t customer_dict_fast_rows = 0;
	bool used_dense_state = false;
};

Q22Parameters MakeQ22Parameters(const std::array<std::string, 7> &country_codes);
NativeScanSpec CreateQ22OrdersScanSpec(const Q22Parameters &parameters);
NativeScanSpec CreateQ22CustomerScanSpec(const Q22Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ22Kernel(const Q22Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ22ProfilingKernel(const Q22Parameters &params, Q22ProfileCounters *profile);
duckdb::unique_ptr<KernelInstance> CreateQ22ParallelKernel(const Q22Parameters &params);

} // namespace accelerator_codegen

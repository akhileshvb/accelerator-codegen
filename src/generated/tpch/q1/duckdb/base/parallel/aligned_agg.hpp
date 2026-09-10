#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {

//! Worker-local Q1 aggregate state used by the parallel kernel aligned fast path.
struct Q1ParallelGroupState {
#if defined(__SIZEOF_INT128__)
	using AccT = __int128_t;
#else
	using AccT = duckdb::hugeint_t;
#endif
	AccT sum_qty = 0;
	AccT sum_base_price = 0;
	AccT sum_disc_price = 0;
	AccT sum_charge = 0;
	AccT sum_discount = 0;
	uint64_t count = 0;
	bool used = false;
};

struct Q1ParallelChunkLocalState {
	int64_t sum_qty = 0;
	int64_t sum_base_price = 0;
	int64_t sum_disc_price = 0;
	int64_t sum_charge = 0;
	int64_t sum_discount = 0;
	int64_t count = 0;
};

//! Per-worker cache for aligned fixed-slot aggregation (production Q1 fused path).
struct Q1ParallelAlignedWorkerCache {
	bool valid = false;
	duckdb::idx_t returnflag_dictionary_size = 0;
	duckdb::idx_t linestatus_dictionary_size = 0;
	const duckdb::string_t *returnflag_child = nullptr;
	const duckdb::string_t *linestatus_child = nullptr;
	const duckdb::ValidityMask *returnflag_validity = nullptr;
	const duckdb::ValidityMask *linestatus_validity = nullptr;
	bool returnflag_all_valid = false;
	bool linestatus_all_valid = false;
	std::vector<uint8_t> returnflag_chars;
	std::vector<uint8_t> linestatus_chars;
	std::vector<uint8_t> returnflag_seen;
	std::vector<uint8_t> linestatus_seen;
	std::vector<uint8_t> pair_to_slot;
	std::array<std::array<Q1ParallelChunkLocalState, 6>, 8> bank_states {};
};

//! Accumulate one already-scanned payload vector using the production aligned fixed-slot path.
//! Returns true when the aligned path handled the vector; false to fall back to scalar paths.
bool AccumulateQ1ParallelAlignedPayload(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec,
                                        duckdb::Vector &discount_vec, duckdb::Vector &tax_vec,
                                        duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                                        duckdb::SelectionVector &sel, duckdb::idx_t count, duckdb::idx_t scan_count,
                                        std::array<Q1ParallelGroupState, 6> &groups, std::vector<uint16_t> &used_keys,
                                        Q1ParallelAlignedWorkerCache &cache, duckdb::SelectionVector &composed_rf_sel,
                                        duckdb::SelectionVector &composed_ls_sel);

} // namespace accelerator_codegen

// Shared helpers for the stateless TPC-H Q10 accelerators (single-threaded and parallel).
//
// Everything here is per-query-call state: nothing is cached across calls.
//
//  * OrderCustomerIndex: the (date-filtered) orders as a bitmap over o_orderkey plus a per-word
//    prefix-popcount ("rank") array.  rank(orderkey) addresses a dense array holding a compact
//    customer id; compact ids are the rank of o_custkey in a second (small) bitmap.  Probing a
//    lineitem order key is therefore one bitmap word gather (AVX-512) and, only for the few hits,
//    one popcount.  No 6M-entry int64 array is ever materialised.
//  * Accumulator: per-compact-customer revenue (int128, exact) + "has lineitem" flag.
//  * TopK: bounded (20) candidate heap; string columns of c_* are copied only for rows that can
//    still enter the heap.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "prims/simd.hpp"
#include "tpch/q10/duckdb/base/query.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace q10 {

using acc_t = __int128_t;

inline constexpr std::size_t kResultLimit = 20;
inline constexpr int32_t kMaxNationSlots = 25;
//! Keys above this use hash-map lookups instead of bitmaps (never the case for TPC-H).
inline constexpr duckdb::idx_t kMaxFlagDictionary = 1024;
inline constexpr int64_t kMaxDenseKey = int64_t {1} << 28;

struct OrderPair {
	int64_t orderkey;
	int64_t custkey;
};

inline int PopCount64(uint64_t value) {
	return __builtin_popcountll(value);
}

class OrderCustomerIndex {
public:
	//! parts: per-worker (orderkey, custkey) lists.  Pairs with a negative key are ignored (a negative
	//! key can never match a lineitem/customer row through the old dense stores either).
	void Build(const std::vector<std::vector<OrderPair>> &parts) {
		int64_t max_ok = -1;
		int64_t max_ck = -1;
		for (const auto &part : parts) {
			for (const auto &pair : part) {
				if (pair.orderkey < 0 || pair.custkey < 0) {
					continue;
				}
				max_ok = std::max(max_ok, pair.orderkey);
				max_ck = std::max(max_ck, pair.custkey);
			}
		}
		dense_ = max_ok < kMaxDenseKey && max_ck < kMaxDenseKey;
		if (!dense_) {
			BuildSparse(parts);
			return;
		}
		max_ok_ = max_ok;
		max_ck_ = max_ck;
		order_limit_ = static_cast<uint64_t>(max_ok + 1);
		cust_limit_ = static_cast<uint64_t>(max_ck + 1);
		const size_t order_words = static_cast<size_t>(order_limit_ + 63) >> 6;
		const size_t cust_words = static_cast<size_t>(cust_limit_ + 63) >> 6;
		order_bits_.assign(order_words + 1, 0);
		cust_bits_.assign(cust_words + 1, 0);
		for (const auto &part : parts) {
			for (const auto &pair : part) {
				if (pair.orderkey < 0 || pair.custkey < 0) {
					continue;
				}
				order_bits_[static_cast<size_t>(pair.orderkey) >> 6] |= uint64_t {1} << (pair.orderkey & 63);
				cust_bits_[static_cast<size_t>(pair.custkey) >> 6] |= uint64_t {1} << (pair.custkey & 63);
			}
		}
		order_rank_.assign(order_words + 1, 0);
		uint32_t running = 0;
		for (size_t word = 0; word < order_words; word++) {
			order_rank_[word] = running;
			running += static_cast<uint32_t>(PopCount64(order_bits_[word]));
		}
		order_rank_[order_words] = running;
		cust_rank_.assign(cust_words + 1, 0);
		running = 0;
		cust_keys_.clear();
		for (size_t word = 0; word < cust_words; word++) {
			cust_rank_[word] = running;
			uint64_t bits = cust_bits_[word];
			running += static_cast<uint32_t>(PopCount64(bits));
			while (bits) {
				cust_keys_.push_back(static_cast<int64_t>(word * 64 + static_cast<size_t>(__builtin_ctzll(bits))));
				bits &= bits - 1;
			}
		}
		cust_rank_[cust_words] = running;
		dense_ids_.assign(order_rank_[order_words] + 1, -1);
		for (const auto &part : parts) {
			for (const auto &pair : part) {
				if (pair.orderkey < 0 || pair.custkey < 0) {
					continue;
				}
				const auto ow = static_cast<size_t>(pair.orderkey) >> 6;
				const auto ob = static_cast<unsigned>(pair.orderkey & 63);
				const auto pos = order_rank_[ow] + static_cast<uint32_t>(PopCount64(order_bits_[ow] & ((uint64_t {1} << ob) - 1)));
				dense_ids_[pos] = LookupCustomer(pair.custkey);
			}
		}
	}

	size_t CustomerCount() const {
		return cust_keys_.size();
	}

	int64_t CustomerKey(size_t id) const {
		return cust_keys_[id];
	}

	bool Empty() const {
		return cust_keys_.empty();
	}

	//! Compact customer id of the order's customer, or -1 when the order is not in the set.
	int32_t LookupOrder(int64_t orderkey) const {
		if (!dense_) {
			const auto it = sparse_orders_.find(orderkey);
			return it == sparse_orders_.end() ? -1 : it->second;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		if (key >= order_limit_) {
			return -1;
		}
		const auto word_index = key >> 6;
		const auto bit = static_cast<unsigned>(key & 63);
		const uint64_t word = order_bits_[word_index];
		if (((word >> bit) & 1) == 0) {
			return -1;
		}
		return dense_ids_[order_rank_[word_index] + static_cast<uint32_t>(PopCount64(word & ((uint64_t {1} << bit) - 1)))];
	}

	//! Compact id of a customer key (keys that have at least one qualifying order), or -1.
	int32_t LookupCustomer(int64_t custkey) const {
		if (!dense_) {
			const auto it = sparse_customers_.find(custkey);
			return it == sparse_customers_.end() ? -1 : it->second;
		}
		const auto key = static_cast<uint64_t>(custkey);
		if (key >= cust_limit_) {
			return -1;
		}
		const auto word_index = key >> 6;
		const auto bit = static_cast<unsigned>(key & 63);
		const uint64_t word = cust_bits_[word_index];
		if (((word >> bit) & 1) == 0) {
			return -1;
		}
		return static_cast<int32_t>(cust_rank_[word_index] + static_cast<uint32_t>(PopCount64(word & ((uint64_t {1} << bit) - 1))));
	}

	//! rows[0..m) <- ascending indexes i < n with keys[i] present in the order set.
	//! rows needs n + simd::kSelSlack entries.
	size_t Probe(const int64_t *ACCEL_RESTRICT keys, size_t n, uint32_t *ACCEL_RESTRICT rows) const {
		size_t m = 0;
		size_t i = 0;
		if (!dense_) {
			for (; i < n; i++) {
				if (LookupOrder(keys[i]) >= 0) {
					rows[m++] = static_cast<uint32_t>(i);
				}
			}
			return m;
		}
#if defined(ACCEL_SIMD_AVX512) && !defined(ACCEL_FORCE_SCALAR_PROBE)
		// 16 keys per step: 64-bit range check, then a 32-bit gather of the bitmap dwords.
		const __m512i limit = _mm512_set1_epi64(static_cast<long long>(order_limit_));
		const __m512i low5 = _mm512_set1_epi32(31);
		const __m512i one = _mm512_set1_epi32(1);
		const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
		const auto *base = reinterpret_cast<const int *>(order_bits_.data());
		for (; i + 16 <= n; i += 16) {
			const __m512i key_lo = _mm512_loadu_si512(keys + i);
			const __m512i key_hi = _mm512_loadu_si512(keys + i + 8);
			const __mmask16 in_range = static_cast<__mmask16>(_mm512_cmplt_epu64_mask(key_lo, limit) |
			                                                  (_mm512_cmplt_epu64_mask(key_hi, limit) << 8));
			const __m512i key = _mm512_inserti64x4(_mm512_castsi256_si512(_mm512_cvtepi64_epi32(key_lo)),
			                                       _mm512_cvtepi64_epi32(key_hi), 1);
			const __m512i words =
			    _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), in_range, _mm512_srli_epi32(key, 5), base, 4);
			const __m512i bits = _mm512_srlv_epi32(words, _mm512_and_si512(key, low5));
			const __mmask16 hit = in_range & _mm512_test_epi32_mask(bits, one);
			_mm512_storeu_si512(rows + m, _mm512_maskz_compress_epi32(
			                                  hit, _mm512_add_epi32(iota, _mm512_set1_epi32(static_cast<int>(i)))));
			m += static_cast<size_t>(PopCount64(hit));
		}
#endif
		for (; i < n; i++) {
			const auto key = static_cast<uint64_t>(keys[i]);
			const bool in_range = key < order_limit_;
			const uint64_t word = in_range ? order_bits_[key >> 6] : 0;
			rows[m] = static_cast<uint32_t>(i);
			m += static_cast<size_t>(in_range & ((word >> (key & 63)) & 1));
		}
		return m;
	}

	int64_t MinMaxOrder() const {
		return max_ok_;
	}

private:
	void BuildSparse(const std::vector<std::vector<OrderPair>> &parts) {
		std::vector<int64_t> custkeys;
		for (const auto &part : parts) {
			for (const auto &pair : part) {
				if (pair.orderkey >= 0 && pair.custkey >= 0) {
					custkeys.push_back(pair.custkey);
				}
			}
		}
		std::sort(custkeys.begin(), custkeys.end());
		custkeys.erase(std::unique(custkeys.begin(), custkeys.end()), custkeys.end());
		cust_keys_ = custkeys;
		for (size_t id = 0; id < custkeys.size(); id++) {
			sparse_customers_[custkeys[id]] = static_cast<int32_t>(id);
		}
		for (const auto &part : parts) {
			for (const auto &pair : part) {
				if (pair.orderkey >= 0 && pair.custkey >= 0) {
					sparse_orders_[pair.orderkey] = sparse_customers_[pair.custkey];
				}
			}
		}
	}

	bool dense_ = true;
	int64_t max_ok_ = -1;
	int64_t max_ck_ = -1;
	uint64_t order_limit_ = 0;
	uint64_t cust_limit_ = 0;
	std::vector<uint64_t> order_bits_;
	std::vector<uint32_t> order_rank_;
	std::vector<int32_t> dense_ids_;
	std::vector<uint64_t> cust_bits_;
	std::vector<uint32_t> cust_rank_;
	std::vector<int64_t> cust_keys_;
	std::unordered_map<int64_t, int32_t> sparse_orders_;
	std::unordered_map<int64_t, int32_t> sparse_customers_;
};

struct Accumulator {
	std::vector<acc_t> revenue;
	std::vector<uint8_t> touched;

	void Init(size_t customers) {
		revenue.assign(customers, 0);
		touched.assign(customers, 0);
	}

	void MergeFrom(const Accumulator &other) {
		for (size_t id = 0; id < revenue.size(); id++) {
			revenue[id] += other.revenue[id];
			touched[id] |= other.touched[id];
		}
	}
};

//! Adds extendedprice * (100 - discount) of the selected probe rows.  `rows[j]` are indexes into
//! `orderkeys`; extendedprice/discount are read at rows[j] (chunk layout) or at j (compact layout,
//! `compact_payload`).
inline void AccumulateHits(const OrderCustomerIndex &index, Accumulator &acc, const int64_t *orderkeys,
                           const int64_t *extendedprice, const int64_t *discount, const uint32_t *rows, size_t hits,
                           bool compact_payload) {
	for (size_t j = 0; j < hits; j++) {
		const auto id = index.LookupOrder(orderkeys[rows[j]]);
		const auto payload_row = compact_payload ? j : static_cast<size_t>(rows[j]);
		acc.revenue[static_cast<size_t>(id)] +=
		    static_cast<acc_t>(extendedprice[payload_row]) * static_cast<acc_t>(100 - discount[payload_row]);
		acc.touched[static_cast<size_t>(id)] = 1;
	}
}

inline duckdb::hugeint_t ToHugeint(acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

inline std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

struct ResultRow {
	int64_t custkey = 0;
	acc_t revenue = 0;
	int64_t acctbal_raw = 0;
	int32_t nationkey = -1;
	std::string name;
	std::string address;
	std::string phone;
	std::string comment;
};

inline bool Better(acc_t lhs_revenue, int64_t lhs_custkey, acc_t rhs_revenue, int64_t rhs_custkey) {
	if (lhs_revenue != rhs_revenue) {
		return lhs_revenue > rhs_revenue;
	}
	return lhs_custkey < rhs_custkey;
}

//! Optional admission floor: only rows at least as good as (revenue, custkey) can enter the result.
struct Floor {
	bool active = false;
	acc_t revenue = 0;
	int64_t custkey = 0;
};

class TopK {
public:
	void SetFloor(const Floor &floor) {
		floor_ = floor;
	}

	bool Admits(acc_t revenue, int64_t custkey) const {
		if (floor_.active && Better(floor_.revenue, floor_.custkey, revenue, custkey)) {
			return false;
		}
		return rows_.size() < kResultLimit || Better(revenue, custkey, rows_[worst_].revenue, rows_[worst_].custkey);
	}

	void Insert(ResultRow &&row) {
		if (rows_.size() < kResultLimit) {
			rows_.push_back(std::move(row));
		} else if (Better(row.revenue, row.custkey, rows_[worst_].revenue, rows_[worst_].custkey)) {
			rows_[worst_] = std::move(row);
		} else {
			return;
		}
		worst_ = 0;
		for (size_t i = 1; i < rows_.size(); i++) {
			if (Better(rows_[worst_].revenue, rows_[worst_].custkey, rows_[i].revenue, rows_[i].custkey)) {
				worst_ = i;
			}
		}
	}

	std::vector<ResultRow> &Rows() {
		return rows_;
	}

private:
	std::vector<ResultRow> rows_;
	size_t worst_ = 0;
	Floor floor_;
};

//! Merge candidate heaps and return the final ordered result (at most kResultLimit rows).
inline std::vector<ResultRow> FinishTopK(std::vector<TopK *> heaps) {
	std::vector<ResultRow> all;
	for (auto *heap : heaps) {
		for (auto &row : heap->Rows()) {
			all.push_back(std::move(row));
		}
	}
	std::sort(all.begin(), all.end(), [](const ResultRow &lhs, const ResultRow &rhs) {
		return Better(lhs.revenue, lhs.custkey, rhs.revenue, rhs.custkey);
	});
	if (all.size() > kResultLimit) {
		all.resize(kResultLimit);
	}
	return all;
}

struct CustomerScanInput {
	const OrderCustomerIndex *index = nullptr;
	const Accumulator *acc = nullptr;
	const std::array<std::string, kMaxNationSlots> *nation_names = nullptr;

	bool NationKnown(int32_t nationkey) const {
		return nationkey >= 0 && nationkey < kMaxNationSlots && !(*nation_names)[static_cast<size_t>(nationkey)].empty();
	}
};

//! The 20th best (revenue desc, custkey asc) among all customers with lineitem revenue.  Any customer worse
//! than this can only be in the result if one of the 20 better ones turns out to have no customer row / known
//! nation; callers verify that after the customer scan and rescan without the floor in that case.
inline Floor ComputeFloor(const OrderCustomerIndex &index, const Accumulator &acc) {
	struct Entry {
		acc_t revenue;
		int64_t custkey;
	};
	Entry best[kResultLimit];
	size_t count = 0;
	size_t worst = 0;
	const size_t customers = index.CustomerCount();
	for (size_t id = 0; id < customers; id++) {
		if (!acc.touched[id]) {
			continue;
		}
		const Entry entry {acc.revenue[id], index.CustomerKey(id)};
		if (count < kResultLimit) {
			best[count++] = entry;
		} else if (Better(entry.revenue, entry.custkey, best[worst].revenue, best[worst].custkey)) {
			best[worst] = entry;
		} else {
			continue;
		}
		if (count == kResultLimit) {
			worst = 0;
			for (size_t i = 1; i < count; i++) {
				if (Better(best[worst].revenue, best[worst].custkey, best[i].revenue, best[i].custkey)) {
					worst = i;
				}
			}
		}
	}
	Floor floor;
	if (count == kResultLimit) {
		floor.active = true;
		floor.revenue = best[worst].revenue;
		floor.custkey = best[worst].custkey;
	}
	return floor;
}

inline constexpr duckdb::idx_t kCustCustkey = 0;
inline constexpr duckdb::idx_t kCustName = 1;
inline constexpr duckdb::idx_t kCustAcctbal = 2;
inline constexpr duckdb::idx_t kCustNationkey = 3;
inline constexpr duckdb::idx_t kCustAddress = 4;
inline constexpr duckdb::idx_t kCustPhone = 5;
inline constexpr duckdb::idx_t kCustComment = 6;
inline constexpr duckdb::idx_t kCustColumnCount = 7;

//! Fused customer scan step for one vector: numeric columns first, string columns only for rows that
//! can still enter the top-k heap.
inline void ProcessCustomerVector(duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
                                  duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count,
                                  const CustomerScanInput &input, TopK &heap) {
	if (scan_state.GetColumnIds().size() != kCustColumnCount) {
		throw duckdb::InternalException("Q10 fused customer expected %llu projected columns, got %llu",
		                                static_cast<unsigned long long>(kCustColumnCount),
		                                static_cast<unsigned long long>(scan_state.GetColumnIds().size()));
	}
	static constexpr duckdb::idx_t kStringAndPayload[] = {kCustName, kCustAcctbal, kCustAddress, kCustPhone,
	                                                      kCustComment};
	duckdb::Vector custkey_vec(duckdb::LogicalType::BIGINT);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustCustkey, sel, count, custkey_vec,
	                                duckdb::UpdateScanType::STANDARD);
	duckdb::UnifiedVectorFormat custkey_format;
	custkey_vec.ToUnifiedFormat(count, custkey_format);
	const auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);

	struct Candidate {
		duckdb::idx_t position;
		duckdb::idx_t compact_row;
		int64_t custkey;
		int32_t nationkey;
		acc_t revenue;
	};
	Candidate candidates[STANDARD_VECTOR_SIZE];
	duckdb::idx_t candidate_count = 0;
	const bool identity = sel.data() == nullptr;
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto custkey_index = custkey_format.sel->get_index(row);
		if (!custkey_format.validity.RowIsValid(custkey_index)) {
			continue;
		}
		const auto id = input.index->LookupCustomer(custkeys[custkey_index]);
		if (id < 0 || !input.acc->touched[static_cast<size_t>(id)]) {
			continue;
		}
		const auto revenue = input.acc->revenue[static_cast<size_t>(id)];
		if (!heap.Admits(revenue, custkeys[custkey_index])) {
			continue;
		}
		candidates[candidate_count++] =
		    Candidate {identity ? row : sel.get_index(row), row, custkeys[custkey_index], -1, revenue};
	}

	if (candidate_count == 0) {
		row_group.SkipProjectedColumn(scan_state, kCustNationkey);
		for (const auto column : kStringAndPayload) {
			row_group.SkipProjectedColumn(scan_state, column);
		}
		return;
	}

	// Nation keys are only decoded for vectors that contain at least one candidate.
	duckdb::Vector nationkey_vec(duckdb::LogicalType::INTEGER);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustNationkey, sel, count, nationkey_vec,
	                                duckdb::UpdateScanType::STANDARD);
	duckdb::UnifiedVectorFormat nationkey_format;
	nationkey_vec.ToUnifiedFormat(count, nationkey_format);
	const auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	{
		duckdb::idx_t kept = 0;
		for (duckdb::idx_t i = 0; i < candidate_count; i++) {
			const auto nationkey_index = nationkey_format.sel->get_index(candidates[i].compact_row);
			if (!nationkey_format.validity.RowIsValid(nationkey_index) ||
			    !input.NationKnown(nationkeys[nationkey_index])) {
				continue;
			}
			candidates[i].nationkey = nationkeys[nationkey_index];
			candidates[kept++] = candidates[i];
		}
		candidate_count = kept;
	}
	if (candidate_count == 0) {
		for (const auto column : kStringAndPayload) {
			row_group.SkipProjectedColumn(scan_state, column);
		}
		return;
	}

	duckdb::SelectionVector candidate_sel(candidate_count);
	for (duckdb::idx_t i = 0; i < candidate_count; i++) {
		candidate_sel.set_index(i, candidates[i].position);
	}
	duckdb::Vector name_vec(duckdb::LogicalType::VARCHAR);
	duckdb::Vector acctbal_vec(duckdb::LogicalType::DECIMAL(15, 2));
	duckdb::Vector address_vec(duckdb::LogicalType::VARCHAR);
	duckdb::Vector phone_vec(duckdb::LogicalType::VARCHAR);
	duckdb::Vector comment_vec(duckdb::LogicalType::VARCHAR);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustName, candidate_sel, candidate_count, name_vec,
	                                duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustAcctbal, candidate_sel, candidate_count, acctbal_vec,
	                                duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustAddress, candidate_sel, candidate_count, address_vec,
	                                duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustPhone, candidate_sel, candidate_count, phone_vec,
	                                duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustComment, candidate_sel, candidate_count, comment_vec,
	                                duckdb::UpdateScanType::STANDARD);

	duckdb::UnifiedVectorFormat formats[5];
	name_vec.ToUnifiedFormat(candidate_count, formats[0]);
	acctbal_vec.ToUnifiedFormat(candidate_count, formats[1]);
	address_vec.ToUnifiedFormat(candidate_count, formats[2]);
	phone_vec.ToUnifiedFormat(candidate_count, formats[3]);
	comment_vec.ToUnifiedFormat(candidate_count, formats[4]);
	const auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[0]);
	const auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(formats[1]);
	const auto addresses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[2]);
	const auto phones = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[3]);
	const auto comments = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[4]);
	for (duckdb::idx_t i = 0; i < candidate_count; i++) {
		duckdb::idx_t idx[5];
		bool valid = true;
		for (int col = 0; col < 5; col++) {
			idx[col] = formats[col].sel->get_index(i);
			valid = valid && formats[col].validity.RowIsValid(idx[col]);
		}
		if (!valid || !heap.Admits(candidates[i].revenue, candidates[i].custkey)) {
			continue;
		}
		ResultRow row;
		row.custkey = candidates[i].custkey;
		row.revenue = candidates[i].revenue;
		row.acctbal_raw = acctbals[idx[1]];
		row.nationkey = candidates[i].nationkey;
		row.name = ReadString(names[idx[0]]);
		row.address = ReadString(addresses[idx[2]]);
		row.phone = ReadString(phones[idx[3]]);
		row.comment = ReadString(comments[idx[4]]);
		heap.Insert(std::move(row));
	}
}

//! Same logic for a materialised customer DataChunk (non-fused fallback path).
inline void ProcessCustomerChunk(duckdb::DataChunk &chunk, const CustomerScanInput &input, TopK &heap) {
	duckdb::UnifiedVectorFormat formats[kCustColumnCount];
	for (duckdb::idx_t col = 0; col < kCustColumnCount; col++) {
		chunk.data[col].ToUnifiedFormat(chunk.size(), formats[col]);
	}
	const auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(formats[kCustCustkey]);
	const auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(formats[kCustAcctbal]);
	const auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(formats[kCustNationkey]);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		duckdb::idx_t idx[kCustColumnCount];
		bool valid = true;
		for (duckdb::idx_t col = 0; col < kCustColumnCount; col++) {
			idx[col] = formats[col].sel->get_index(row);
			valid = valid && formats[col].validity.RowIsValid(idx[col]);
		}
		if (!valid) {
			continue;
		}
		const auto custkey = custkeys[idx[kCustCustkey]];
		const auto id = input.index->LookupCustomer(custkey);
		if (id < 0 || !input.acc->touched[static_cast<size_t>(id)]) {
			continue;
		}
		const auto nationkey = nationkeys[idx[kCustNationkey]];
		if (!input.NationKnown(nationkey)) {
			continue;
		}
		const auto revenue = input.acc->revenue[static_cast<size_t>(id)];
		if (!heap.Admits(revenue, custkey)) {
			continue;
		}
		ResultRow result;
		result.custkey = custkey;
		result.revenue = revenue;
		result.acctbal_raw = acctbals[idx[kCustAcctbal]];
		result.nationkey = nationkey;
		result.name = ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[kCustName])[idx[kCustName]]);
		result.address =
		    ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[kCustAddress])[idx[kCustAddress]]);
		result.phone =
		    ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[kCustPhone])[idx[kCustPhone]]);
		result.comment =
		    ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[kCustComment])[idx[kCustComment]]);
		heap.Insert(std::move(result));
	}
}

inline void ConsumeNationChunk(duckdb::DataChunk &chunk, std::array<std::string, kMaxNationSlots> &names) {
	duckdb::UnifiedVectorFormat nationkey_format;
	duckdb::UnifiedVectorFormat name_format;
	chunk.data[Q10NationChunkLayout::NATIONKEY_INDEX].ToUnifiedFormat(chunk.size(), nationkey_format);
	chunk.data[Q10NationChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto nationkey_index = nationkey_format.sel->get_index(row);
		const auto name_index = name_format.sel->get_index(row);
		if (!nationkey_format.validity.RowIsValid(nationkey_index) || !name_format.validity.RowIsValid(name_index)) {
			continue;
		}
		const auto nationkey = nationkeys[nationkey_index];
		if (nationkey < 0 || nationkey >= kMaxNationSlots) {
			continue;
		}
		names[static_cast<size_t>(nationkey)] =
		    ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index]);
	}
}

inline bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

inline void CollectOrdersChunk(duckdb::DataChunk &chunk, std::vector<OrderPair> &pairs) {
	auto &orderkey_vector = chunk.data[Q10OrdersChunkLayout::ORDERKEY_INDEX];
	auto &custkey_vector = chunk.data[Q10OrdersChunkLayout::CUSTKEY_INDEX];
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		const auto base = pairs.size();
		pairs.resize(base + chunk.size());
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			pairs[base + row] = OrderPair {orderkeys[row], custkeys[row]};
		}
		return;
	}
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat custkey_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto custkey_index = custkey_format.sel->get_index(row);
		if (!orderkey_format.validity.RowIsValid(orderkey_index) || !custkey_format.validity.RowIsValid(custkey_index)) {
			continue;
		}
		pairs.push_back(OrderPair {orderkeys[orderkey_index], custkeys[custkey_index]});
	}
}

//! rows[0..m) <- indexes of rows whose l_returnflag is exactly 'R' (NULL never matches).  rows needs n entries.
inline size_t SelectReturnFlagR(duckdb::Vector &vector, size_t n, uint32_t *rows) {
	if (vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
		// Dictionary-encoded flags (3 distinct values): classify each dictionary entry once, then map rows
		// through the selection vector.
		auto &child = duckdb::DictionaryVector::Child(vector);
		const auto dictionary_size = duckdb::DictionaryVector::DictionarySize(vector);
		const auto &dictionary_sel = duckdb::DictionaryVector::SelVector(vector);
		if (dictionary_size.IsValid() && dictionary_size.GetIndex() <= kMaxFlagDictionary && dictionary_sel.data() &&
		    child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
			uint32_t is_r[kMaxFlagDictionary + 16];
			const auto values = duckdb::FlatVector::GetData<duckdb::string_t>(child);
			const auto &validity = duckdb::FlatVector::Validity(child);
			const auto entries = dictionary_size.GetIndex();
			for (duckdb::idx_t i = 0; i < entries; i++) {
				is_r[i] = validity.RowIsValid(i) && values[i].GetSize() == 1 && values[i].GetData()[0] == 'R' ? 1u : 0u;
			}
			const uint32_t *ACCEL_RESTRICT codes = dictionary_sel.data();
			size_t m = 0;
			size_t row = 0;
#if defined(ACCEL_SIMD_AVX512) && !defined(ACCEL_FORCE_SCALAR_FLAG)
			const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
			for (; row + 16 <= n; row += 16) {
				const __m512i code = _mm512_loadu_si512(codes + row);
				const __m512i flag = _mm512_i32gather_epi32(code, is_r, 4);
				const __mmask16 hit = _mm512_test_epi32_mask(flag, flag);
				_mm512_storeu_si512(rows + m, _mm512_maskz_compress_epi32(hit, _mm512_add_epi32(iota, _mm512_set1_epi32(static_cast<int>(row)))));
				m += static_cast<size_t>(PopCount64(hit));
			}
#endif
			for (; row < n; row++) {
				rows[m] = static_cast<uint32_t>(row);
				m += is_r[codes[row]];
			}
			return m;
		}
	}
	duckdb::UnifiedVectorFormat format;
	vector.ToUnifiedFormat(n, format);
	const auto data = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(format);
	const bool all_valid = format.validity.AllValid();
	size_t m = 0;
	for (size_t row = 0; row < n; row++) {
		const auto index = format.sel->get_index(row);
		const duckdb::string_t &value = data[index];
		const bool match = value.GetSize() == 1 && value.GetData()[0] == 'R' && (all_valid || format.validity.RowIsValid(index));
		rows[m] = static_cast<uint32_t>(row);
		m += match ? 1 : 0;
	}
	return m;
}

//! Lineitem chunk (orderkey, extendedprice, discount, returnflag): applies the l_returnflag = 'R' predicate,
//! probes the order set and accumulates revenue per compact customer id.
inline void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const OrderCustomerIndex &index, Accumulator &acc) {
	const size_t n = chunk.size();
	uint32_t flagged[STANDARD_VECTOR_SIZE + simd::kSelSlack];
	const size_t flagged_count = SelectReturnFlagR(chunk.data[Q10LineitemChunkLayout::RETURNFLAG_INDEX], n, flagged);
	if (flagged_count == 0) {
		return;
	}
	auto &orderkey_vector = chunk.data[Q10LineitemChunkLayout::ORDERKEY_INDEX];
	auto &extendedprice_vector = chunk.data[Q10LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q10LineitemChunkLayout::DISCOUNT_INDEX];
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(extendedprice_vector) && IsFlatAllValid(discount_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
		int64_t keys[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		for (size_t k = 0; k < flagged_count; k++) {
			keys[k] = orderkeys[flagged[k]];
		}
		uint32_t hit_rows[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		const auto hits = index.Probe(keys, flagged_count, hit_rows);
		for (size_t j = 0; j < hits; j++) {
			hit_rows[j] = flagged[hit_rows[j]];
		}
		AccumulateHits(index, acc, orderkeys, extendedprices, discounts, hit_rows, hits, false);
		return;
	}
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	orderkey_vector.ToUnifiedFormat(n, orderkey_format);
	extendedprice_vector.ToUnifiedFormat(n, extendedprice_format);
	discount_vector.ToUnifiedFormat(n, discount_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
	for (size_t k = 0; k < flagged_count; k++) {
		const auto row = flagged[k];
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		if (!orderkey_format.validity.RowIsValid(orderkey_index) ||
		    !extendedprice_format.validity.RowIsValid(extendedprice_index) ||
		    !discount_format.validity.RowIsValid(discount_index)) {
			continue;
		}
		const auto id = index.LookupOrder(orderkeys[orderkey_index]);
		if (id < 0) {
			continue;
		}
		acc.revenue[static_cast<size_t>(id)] += static_cast<acc_t>(extendedprices[extendedprice_index]) *
		                                        static_cast<acc_t>(100 - discounts[discount_index]);
		acc.touched[static_cast<size_t>(id)] = 1;
	}
}

} // namespace q10
} // namespace accelerator_codegen

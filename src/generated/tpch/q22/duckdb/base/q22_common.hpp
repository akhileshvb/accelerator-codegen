#pragma once

// Query-local building blocks shared by the single-threaded and parallel Q22 kernels.
//
// Plan: one pass over customer (branch-free prefix match + append of the matching
// customers' (custkey, country code, acctbal) into compact arrays, plus the exact
// 128-bit sum/count of their positive balances), one pass over orders (customer-has-orders
// bit vector; per-chunk min/max via AVX-512 so the inner loop needs no bounds checks),
// then a tiny final pass over the matching customers.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/runtime/simd.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tpch/q22/duckdb/base/query.hpp"

namespace accelerator_codegen {
namespace q22 {

constexpr size_t kCountrySlotCount = 100;

#if defined(__SIZEOF_INT128__)
using acc_t = __int128_t;
inline duckdb::hugeint_t FromAccumulator(acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}
#else
#error "Q22 kernels require __int128"
#endif

inline bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

struct CountrySetup {
	std::array<uint8_t, kCountrySlotCount> allowed {};
	std::array<std::string, kCountrySlotCount> labels {};

	explicit CountrySetup(const Q22Parameters &params) {
		for (const auto &code : params.country_codes) {
			const int value = std::stoi(code);
			if (value >= 0 && value < static_cast<int>(kCountrySlotCount)) {
				allowed[static_cast<size_t>(value)] = 1;
				labels[static_cast<size_t>(value)] = code;
			}
		}
	}
};

//! Customers whose phone prefix is in the code set.
struct MatchedCustomers {
	std::vector<int64_t> custkey;
	std::vector<int64_t> acctbal;
	std::vector<uint8_t> code;
	acc_t positive_sum = 0;
	int64_t positive_count = 0;

	void Merge(const MatchedCustomers &other) {
		custkey.insert(custkey.end(), other.custkey.begin(), other.custkey.end());
		acctbal.insert(acctbal.end(), other.acctbal.begin(), other.acctbal.end());
		code.insert(code.end(), other.code.begin(), other.code.end());
		positive_sum += other.positive_sum;
		positive_count += other.positive_count;
	}
};

inline void ConsumeCustomerChunk(duckdb::DataChunk &chunk, const CountrySetup &setup, MatchedCustomers &out) {
	auto &custkey_vector = chunk.data[Q22CustomerChunkLayout::CUSTKEY_INDEX];
	auto &phone_vector = chunk.data[Q22CustomerChunkLayout::PHONE_INDEX];
	auto &acctbal_vector = chunk.data[Q22CustomerChunkLayout::ACCTBAL_INDEX];
	const size_t n = chunk.size();
	if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(phone_vector) && IsFlatAllValid(acctbal_vector)) {
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(phone_vector);
		auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vector);
		const size_t base = out.custkey.size();
		out.custkey.resize(base + n + 1);
		out.acctbal.resize(base + n + 1);
		out.code.resize(base + n + 1);
		int64_t *ACCEL_RESTRICT ok = out.custkey.data() + base;
		int64_t *ACCEL_RESTRICT ob = out.acctbal.data() + base;
		uint8_t *ACCEL_RESTRICT oc = out.code.data() + base;
		size_t count = 0;
		// Per-chunk partial sums stay in int64: |acctbal| < 10^15 (DECIMAL(15,2)) and n <= 2048 rows
		// per vector, so |sum| < 2.1e18 < 2^63; chunks are spilled into the 128-bit accumulator.
		int64_t chunk_sum = 0;
		int64_t chunk_pos = 0;
		const uint8_t *ACCEL_RESTRICT allowed = setup.allowed.data();
		for (size_t row = 0; row < n; row++) {
			const duckdb::string_t &phone = phones[row];
			const char *data = phone.GetData();
			const uint32_t size = phone.GetSize();
			const unsigned d0 = static_cast<unsigned char>(data[0]) - static_cast<unsigned>('0');
			const unsigned d1 = static_cast<unsigned char>(data[1]) - static_cast<unsigned>('0');
			const unsigned code = d0 * 10 + d1;
			// string_t always provides >= 4 readable inline bytes, so data[0..1] is safe even for short strings;
			// the size test keeps the semantics of "phone shorter than 2 chars never matches".
			const bool match = (size >= 2) & (d0 < 10) & (d1 < 10) & (allowed[code < kCountrySlotCount ? code : 0] != 0) &
			                   (code < kCountrySlotCount);
			ok[count] = custkeys[row];
			ob[count] = acctbals[row];
			oc[count] = static_cast<uint8_t>(code);
			const int64_t bal = acctbals[row];
			const bool positive = match & (bal > 0);
			chunk_sum += positive ? bal : 0;
			chunk_pos += positive ? 1 : 0;
			count += match ? 1 : 0;
		}
		out.custkey.resize(base + count);
		out.acctbal.resize(base + count);
		out.code.resize(base + count);
		out.positive_sum += chunk_sum;
		out.positive_count += chunk_pos;
		return;
	}
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat phone_format;
	duckdb::UnifiedVectorFormat acctbal_format;
	custkey_vector.ToUnifiedFormat(n, custkey_format);
	phone_vector.ToUnifiedFormat(n, phone_format);
	acctbal_vector.ToUnifiedFormat(n, acctbal_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto phones = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format);
	auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(acctbal_format);
	for (size_t row = 0; row < n; row++) {
		const auto ci = custkey_format.sel->get_index(row);
		const auto pi = phone_format.sel->get_index(row);
		const auto ai = acctbal_format.sel->get_index(row);
		if (!custkey_format.validity.RowIsValid(ci) || !phone_format.validity.RowIsValid(pi) ||
		    !acctbal_format.validity.RowIsValid(ai)) {
			continue;
		}
		const duckdb::string_t &phone = phones[pi];
		if (phone.GetSize() < 2) {
			continue;
		}
		const char *data = phone.GetData();
		if (data[0] < '0' || data[0] > '9' || data[1] < '0' || data[1] > '9') {
			continue;
		}
		const unsigned code = static_cast<unsigned>((data[0] - '0') * 10 + (data[1] - '0'));
		if (!setup.allowed[code]) {
			continue;
		}
		out.custkey.push_back(custkeys[ci]);
		out.acctbal.push_back(acctbals[ai]);
		out.code.push_back(static_cast<uint8_t>(code));
		if (acctbals[ai] > 0) {
			out.positive_sum += acctbals[ai];
			out.positive_count += 1;
		}
	}
}

//! Bit vector over customer keys (keys beyond ~1e9 or negative fall back to a hash set).
class HasOrders {
public:
	HasOrders() : words_((150001 >> 6) + 1, 0) {
	}

	void ConsumeChunk(duckdb::DataChunk &chunk) {
		auto &custkey_vector = chunk.data[Q22OrdersChunkLayout::CUSTKEY_INDEX];
		const size_t n = chunk.size();
		if (IsFlatAllValid(custkey_vector)) {
			MarkKeys(duckdb::FlatVector::GetData<int64_t>(custkey_vector), n);
			return;
		}
		duckdb::UnifiedVectorFormat format;
		custkey_vector.ToUnifiedFormat(n, format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(format);
		for (size_t row = 0; row < n; row++) {
			const auto index = format.sel->get_index(row);
			if (format.validity.RowIsValid(index)) {
				MarkSlow(custkeys[index]);
			}
		}
	}

	void MarkKeys(const int64_t *ACCEL_RESTRICT keys, size_t n) {
		if (n == 0) {
			return;
		}
		int64_t lo = keys[0];
		int64_t hi = keys[0];
		size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
		if (n >= 8) {
			__m512i vlo = _mm512_loadu_si512(keys);
			__m512i vhi = vlo;
			for (i = 8; i + 8 <= n; i += 8) {
				const __m512i v = _mm512_loadu_si512(keys + i);
				vlo = _mm512_min_epi64(vlo, v);
				vhi = _mm512_max_epi64(vhi, v);
			}
			lo = _mm512_reduce_min_epi64(vlo);
			hi = _mm512_reduce_max_epi64(vhi);
		}
#endif
		for (; i < n; i++) {
			lo = keys[i] < lo ? keys[i] : lo;
			hi = keys[i] > hi ? keys[i] : hi;
		}
		if (lo < 0 || hi > 1'000'000'000) {
			for (size_t j = 0; j < n; j++) {
				MarkSlow(keys[j]);
			}
			return;
		}
		const size_t need = (static_cast<size_t>(hi) >> 6) + 1;
		if (need > words_.size()) {
			words_.resize(need, 0);
		}
		uint64_t *ACCEL_RESTRICT words = words_.data();
		for (size_t j = 0; j < n; j++) {
			const uint64_t key = static_cast<uint64_t>(keys[j]);
			words[key >> 6] |= uint64_t(1) << (key & 63);
		}
	}

	bool Has(int64_t custkey) const {
		if (custkey < 0 || custkey > 1'000'000'000) {
			return overflow_.find(custkey) != overflow_.end();
		}
		const auto key = static_cast<uint64_t>(custkey);
		const auto w = key >> 6;
		if (w >= words_.size()) {
			return false;
		}
		return (words_[w] >> (key & 63)) & 1;
	}

	void MergeFrom(const HasOrders &other) {
		if (other.words_.size() > words_.size()) {
			words_.resize(other.words_.size(), 0);
		}
		for (size_t i = 0; i < other.words_.size(); i++) {
			words_[i] |= other.words_[i];
		}
		overflow_.insert(other.overflow_.begin(), other.overflow_.end());
	}

private:
	void MarkSlow(int64_t custkey) {
		if (custkey < 0 || custkey > 1'000'000'000) {
			overflow_.insert(custkey);
			return;
		}
		const auto key = static_cast<uint64_t>(custkey);
		const auto w = key >> 6;
		if (w >= words_.size()) {
			words_.resize(static_cast<size_t>(w) + 1, 0);
		}
		words_[w] |= uint64_t(1) << (key & 63);
	}

	std::vector<uint64_t> words_;
	std::unordered_set<int64_t> overflow_;
};

struct OutputRow {
	std::string country_code;
	int64_t customer_count = 0;
	duckdb::hugeint_t total_balance;
};

//! Final pass: customers with balance > avg(positive balance) and no orders, grouped by country code.
inline std::vector<OutputRow> BuildOutput(const CountrySetup &setup, const MatchedCustomers &matched,
                                          const HasOrders &has_orders) {
	std::array<int64_t, kCountrySlotCount> counts {};
	std::array<acc_t, kCountrySlotCount> totals {};
	if (matched.positive_count > 0) {
		const acc_t sum = matched.positive_sum;
		const acc_t cnt = matched.positive_count;
		for (size_t i = 0; i < matched.custkey.size(); i++) {
			const acc_t lhs = static_cast<acc_t>(matched.acctbal[i]) * cnt;
			if (!(lhs > sum)) {
				continue;
			}
			if (has_orders.Has(matched.custkey[i])) {
				continue;
			}
			counts[matched.code[i]] += 1;
			totals[matched.code[i]] += matched.acctbal[i];
		}
	}
	std::vector<OutputRow> rows;
	for (size_t code = 0; code < kCountrySlotCount; code++) {
		if (counts[code] == 0) {
			continue;
		}
		OutputRow row;
		row.country_code = setup.labels[code];
		row.customer_count = counts[code];
		row.total_balance = FromAccumulator(totals[code]);
		rows.push_back(std::move(row));
	}
	std::sort(rows.begin(), rows.end(),
	          [](const OutputRow &lhs, const OutputRow &rhs) { return lhs.country_code < rhs.country_code; });
	return rows;
}

} // namespace q22
} // namespace accelerator_codegen

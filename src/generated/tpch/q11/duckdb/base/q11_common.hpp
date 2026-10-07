// Shared low-level helpers for the Q11 single-threaded and parallel kernels.
// Everything here is per-call state (no statics, no caches).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/runtime/simd.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen {
namespace q11 {

using acc_t = __int128_t;

inline duckdb::hugeint_t ToHugeint(acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

//! Dense qualifying-supplier byte map keyed by s_suppkey (TPC-H keys are dense).
struct SupplierMap {
	std::vector<uint8_t> flags; // size() >= capacity + 8 so 4-byte gathers never leave the buffer
	size_t capacity = 0;
	bool any = false;

	SupplierMap() {
		Resize(10001);
	}
	void Resize(size_t cap) {
		flags.resize(cap + 8, 0);
		capacity = cap;
	}
	void Mark(int64_t suppkey) {
		if (suppkey < 0) {
			return;
		}
		const size_t key = static_cast<size_t>(suppkey);
		if (key >= capacity) {
			Resize(std::max(key + 1, capacity * 2));
		}
		flags[key] = 1;
		any = true;
	}
	bool Qualifies(int64_t suppkey) const {
		return static_cast<uint64_t>(suppkey) < capacity && flags[static_cast<size_t>(suppkey)] != 0;
	}
};

//! sel <- rows i in [0,n) whose suppkeys[i] qualifies.  sel needs n + kSelSlack entries.
inline size_t SelectQualifying(const SupplierMap &map, const int64_t *ACCEL_RESTRICT suppkeys, size_t n,
                               uint32_t *ACCEL_RESTRICT sel) {
	size_t count = 0;
	size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vcap = _mm512_set1_epi64(static_cast<long long>(map.capacity));
	const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const uint8_t *base = map.flags.data();
	for (; i + 8 <= n; i += 8) {
		const __m512i keys = _mm512_loadu_si512(suppkeys + i);
		const __mmask8 inrange = _mm512_cmplt_epu64_mask(keys, vcap);
		const __m256i got = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), inrange, keys, base, 1);
		const __mmask8 hit = _mm256_mask_test_epi32_mask(inrange, got, _mm256_set1_epi32(0xFF));
		const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
		_mm256_mask_compressstoreu_epi32(sel + count, hit, idx);
		count += static_cast<size_t>(__builtin_popcount(hit));
	}
#endif
	for (; i < n; i++) {
		sel[count] = static_cast<uint32_t>(i);
		count += map.Qualifies(suppkeys[i]);
	}
	return count;
}

//! Exact 128-bit running sum that stays in an int64 lane except on (rare) overflow.
struct WideSum {
	int64_t lo = 0;
	acc_t spill = 0;
	void MulAdd(int64_t cost, int64_t qty) {
		int64_t prod;
		if (ACCEL_UNLIKELY(__builtin_mul_overflow(cost, qty, &prod))) {
			spill += static_cast<acc_t>(cost) * static_cast<acc_t>(qty);
			return;
		}
		int64_t next;
		if (ACCEL_UNLIKELY(__builtin_add_overflow(lo, prod, &next))) {
			spill += static_cast<acc_t>(lo) + static_cast<acc_t>(prod);
			lo = 0;
			return;
		}
		lo = next;
	}
	void AddProduct(int64_t prod) {
		int64_t next;
		if (ACCEL_UNLIKELY(__builtin_add_overflow(lo, prod, &next))) {
			spill += static_cast<acc_t>(lo) + static_cast<acc_t>(prod);
			lo = 0;
			return;
		}
		lo = next;
	}
	acc_t Get() const {
		return static_cast<acc_t>(lo) + spill;
	}
};

//! Per-part accumulator array: int64 lanes with an overflow side map.
struct PartSums {
	std::vector<int64_t> lo;
	std::unordered_map<int64_t, acc_t> spill;

	void EnsureCapacity(size_t n) {
		if (n > lo.size()) {
			lo.resize(n, 0);
		}
	}
	void MulAdd(size_t idx, int64_t cost, int64_t qty) {
		int64_t prod;
		if (ACCEL_UNLIKELY(__builtin_mul_overflow(cost, qty, &prod))) {
			spill[static_cast<int64_t>(idx)] += static_cast<acc_t>(cost) * static_cast<acc_t>(qty);
			return;
		}
		AddProduct(idx, prod);
	}
	void AddProduct(size_t idx, int64_t prod) {
		int64_t next;
		if (ACCEL_UNLIKELY(__builtin_add_overflow(lo[idx], prod, &next))) {
			spill[static_cast<int64_t>(idx)] += static_cast<acc_t>(lo[idx]) + static_cast<acc_t>(prod);
			lo[idx] = 0;
			return;
		}
		lo[idx] = next;
	}
	acc_t Get(size_t idx) const {
		acc_t v = lo[idx];
		if (ACCEL_UNLIKELY(!spill.empty())) {
			auto it = spill.find(static_cast<int64_t>(idx));
			if (it != spill.end()) {
				v += it->second;
			}
		}
		return v;
	}
};

inline duckdb::Value ComputeThreshold(acc_t total, double fraction) {
	const auto total_decimal = duckdb::Value::DECIMAL(ToHugeint(total), 38, 2);
	const auto total_as_double = total_decimal.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
	return duckdb::Value::DOUBLE(total_as_double * fraction);
}

//! Decides `part_value > threshold` (DECIMAL(38,2) vs DOUBLE) exactly as the reference does,
//! using a cheap floating-point decision when the outcome is unambiguous and falling back to the
//! generic Value comparison otherwise.
class ThresholdTest {
public:
	ThresholdTest(acc_t total, double fraction) : value_(ComputeThreshold(total, fraction)) {
		const double thr = value_.GetValue<double>();
		const double raw = thr * 100.0;
		const double margin = std::fabs(raw) * 1e-9 + 1e-3;
		hi_ = raw + margin;
		lo_ = raw - margin;
	}
	bool Passes(acc_t part) const {
		const double pv = static_cast<double>(part);
		if (pv > hi_) {
			return true;
		}
		if (pv < lo_) {
			return false;
		}
		return duckdb::Value::DECIMAL(ToHugeint(part), 38, 2) > value_;
	}

private:
	duckdb::Value value_;
	double hi_ = 0;
	double lo_ = 0;
};

struct OutputRow {
	int64_t partkey = 0;
	acc_t value = 0;
};

inline bool BetterOutputRow(const OutputRow &lhs, const OutputRow &rhs) {
	if (lhs.value != rhs.value) {
		return lhs.value > rhs.value;
	}
	return lhs.partkey < rhs.partkey;
}

} // namespace q11
} // namespace accelerator_codegen

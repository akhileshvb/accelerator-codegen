// TPC-H Q4 (stateless PostgreSQL): parameters, scan specs and the SIMD
// semi-join probe shared by the single-threaded and parallel kernels.
//
// Plan: orders of the 3-month window (~1/28 of the table, filtered inside the
// scan) are indexed by orderkey in an open-addressing table; the lineitem scan
// keeps the rows with commitdate < receiptdate (AVX-512 compare + compress of
// the keys) and marks the matching orders (EXISTS).  Counts per priority.
#pragma once

#include "backends/postgres/api/stateless/pg_stateless.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace accelerator_codegen::pg_stateless::q4 {

inline constexpr const char *kSqlArgs = "date";
inline constexpr const char *kSqlReturns = "TABLE (o_orderpriority text, order_count bigint)";

struct Params {
	int32_t start;    // o_orderdate >= start
	int32_t end_excl; // o_orderdate < start + 3 months
};

inline Params Bind(const Args &args) {
	Params p;
	p.start = args.Date(0);
	int y;
	unsigned m, d;
	CivilFromDays(p.start, y, m, d);
	int total = y * 12 + static_cast<int>(m) - 1 + 3;
	const int ny = total / 12;
	const unsigned nm = static_cast<unsigned>(total % 12) + 1;
	static const unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	unsigned dim = kDays[nm - 1];
	if (nm == 2 && ((ny % 4 == 0 && ny % 100 != 0) || ny % 400 == 0)) {
		dim = 29;
	}
	p.end_excl = DaysFromCivil(ny, nm, std::min(d, dim)); // PostgreSQL clamps the day of month
	return p;
}

enum : int { kOOrderdate = 0, kOOrderkey = 1, kOPriority = 2 };
enum : int { kLOrderkey = 0, kLCommit = 1, kLReceipt = 2 };

inline ScanSpec OrdersSpec(const Params &p) {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_orderdate", ColType::Date}, {"o_orderkey", ColType::Int64}, {"o_orderpriority", ColType::Text}};
	s.filters = {{kOOrderdate, FilterOp::Between, p.start, int64_t(p.end_excl) - 1}};
	return s;
}

inline ScanSpec LineitemSpec(const std::vector<int64_t> &order_keys) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_orderkey", ColType::Int64}, {"l_commitdate", ColType::Date}, {"l_receiptdate", ColType::Date}};
	// Probe lineitem by the window's order keys (index bitmap scan when the key set is small).
	FilterSpec f {kLOrderkey, FilterOp::IntIn};
	f.ints = order_keys;
	s.filters.push_back(std::move(f));
	return s;
}

//! orderkey -> dense index (insertion order); open addressing, linear probing.
class KeyIndex {
public:
	void Reserve(std::size_t n) {
		std::size_t cap = 16;
		while (cap < n * 2) {
			cap <<= 1;
		}
		slots_.assign(cap, Slot {0, kEmpty, 0});
		shift_ = 64 - __builtin_ctzll(cap);
		mask_ = cap - 1;
		count_ = 0;
	}
	//! Insert (keys are unique); returns the new index.
	uint32_t Insert(int64_t key) {
		std::size_t s = Home(key);
		while (slots_[s].idx != kEmpty) {
			s = (s + 1) & mask_;
		}
		slots_[s] = Slot {key, count_, 0};
		return count_++;
	}
	std::size_t Home(int64_t key) const {
		return (static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ull) >> shift_;
	}
	const void *Addr(int64_t key) const {
		return &slots_[Home(key)];
	}
	//! index of key or kEmpty
	uint32_t Find(int64_t key) const {
		std::size_t s = Home(key);
		for (;;) {
			const Slot &sl = slots_[s];
			if (sl.key == key && sl.idx != kEmpty) {
				return sl.idx;
			}
			if (sl.idx == kEmpty) {
				return kEmpty;
			}
			s = (s + 1) & mask_;
		}
	}
	static constexpr uint32_t kEmpty = 0xFFFFFFFFu;

private:
	struct Slot {
		int64_t key;
		uint32_t idx;
		uint32_t pad;
	};
	std::vector<Slot> slots_;
	int shift_ = 60;
	std::size_t mask_ = 15;
	uint32_t count_ = 0;
};

//! Local priority dictionary (distinct strings in first-seen order).
struct PrioDict {
	std::vector<std::string> names;
	uint8_t Id(std::string_view v) {
		for (std::size_t i = 0; i < names.size(); i++) {
			if (names[i] == v) {
				return static_cast<uint8_t>(i);
			}
		}
		if (names.size() >= 255) {
			throw KernelError("q4: too many distinct o_orderpriority values");
		}
		names.emplace_back(v);
		return static_cast<uint8_t>(names.size() - 1);
	}
};

//! Mark orders (seen[idx] = 1) that have a lineitem in the batch with commit < receipt.
inline void MarkExists(const Batch &b, const KeyIndex &index, uint8_t *ACCEL_RESTRICT seen) {
	const int64_t *ACCEL_RESTRICT keys = b.I64(kLOrderkey);
	const int32_t *ACCEL_RESTRICT commit = b.I32(kLCommit);
	const int32_t *ACCEL_RESTRICT receipt = b.I32(kLReceipt);
	const std::size_t n = b.count;
	// Qualifying keys are compressed into a small buffer and probed with a prefetch pipeline.
	constexpr std::size_t kChunk = 256;
	alignas(64) int64_t cand[kChunk + 32];
	std::size_t i = 0;
	while (i < n) {
		const std::size_t end = std::min(n, i + kChunk);
		std::size_t m = 0;
#if defined(ACCEL_SIMD_AVX512)
		for (; i + 16 <= end; i += 16) {
			const __m512i c = _mm512_loadu_si512(commit + i);
			const __m512i r = _mm512_loadu_si512(receipt + i);
			const unsigned mask = _mm512_cmplt_epi32_mask(c, r);
			_mm512_mask_compressstoreu_epi64(cand + m, static_cast<__mmask8>(mask & 0xFF),
			                                 _mm512_loadu_si512(keys + i));
			m += static_cast<std::size_t>(__builtin_popcount(mask & 0xFF));
			_mm512_mask_compressstoreu_epi64(cand + m, static_cast<__mmask8>(mask >> 8),
			                                 _mm512_loadu_si512(keys + i + 8));
			m += static_cast<std::size_t>(__builtin_popcount(mask >> 8));
		}
#endif
		for (; i < end; i++) {
			cand[m] = keys[i];
			m += commit[i] < receipt[i];
		}
		constexpr std::size_t kDist = 8;
		for (std::size_t j = 0; j < std::min(m, kDist); j++) {
			ACCEL_PREFETCH(index.Addr(cand[j]));
		}
		for (std::size_t j = 0; j < m; j++) {
			if (j + kDist < m) {
				ACCEL_PREFETCH(index.Addr(cand[j + kDist]));
			}
			const uint32_t idx = index.Find(cand[j]);
			if (idx != KeyIndex::kEmpty) {
				seen[idx] = 1;
			}
		}
	}
}

inline void Emit(const std::vector<std::string> &names, const std::vector<int64_t> &counts, ResultWriter &out) {
	std::vector<std::size_t> order;
	for (std::size_t i = 0; i < names.size(); i++) {
		if (counts[i] > 0) {
			order.push_back(i);
		}
	}
	std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return names[a] < names[b]; });
	for (std::size_t i : order) {
		out.Text(names[i]);
		out.Int(counts[i]);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q4

// TPC-H Q9 (stateless PostgreSQL): shared code for both kernels.
//
// part (LIKE '%color%') -> dense partkey byte table; partsupp rows of those
// parts -> flat open-addressing map (partkey,suppkey)->cost; orders -> dense
// orderkey -> 1-byte year code; supplier -> dense suppkey -> nation code.
// Lineitem probe: SIMD gather of the part table selects the ~5% matching rows,
// then map / order / supplier lookups and an exact int128 per (nation,year) sum.
#pragma once

#include "accelerator_codegen/runtime/postgres_stateless/pg_stateless.hpp"
#include "accelerator_codegen/runtime/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace accelerator_codegen::pg_stateless::q9 {

inline constexpr const char *kSqlArgs = "text";
inline constexpr const char *kSqlReturns = "TABLE (nation text, o_year bigint, sum_profit numeric)";

//! Byte table indexed by key; absent = 0.  Keys in [0, size) live in a dense
//! array (padded so a 4-byte gather at any in-range key stays inside the
//! allocation); anything else (huge / negative keys) goes to a small overflow map.
struct ByteTable {
	static constexpr uint64_t kDenseMax = uint64_t(1) << 26;
	std::vector<uint8_t> v = std::vector<uint8_t>(8, 0);
	uint64_t size = 0; // dense keys < size
	struct Ov {
		int64_t key;
		int64_t val;
		bool operator<(const Ov &o) const {
			return key < o.key;
		}
	};
	std::vector<Ov> over; // sorted by key after Finish()
	void Reserve(uint64_t n) {
		if (n > size) {
			const uint64_t cap = std::min<uint64_t>(kDenseMax, std::max<uint64_t>(n, size + size / 2));
			v.resize(cap + 8, 0);
			size = cap;
		}
	}
	void Set(int64_t key, uint8_t val) {
		const uint64_t k = static_cast<uint64_t>(key);
		if (k >= kDenseMax) {
			over.push_back(Ov {key, val});
			return;
		}
		if (k >= size) {
			Reserve(k + 1);
		}
		v[static_cast<std::size_t>(k)] = val;
	}
	//! Sort the overflow entries (call after the last Set, before lookups).
	void Finish() {
		std::sort(over.begin(), over.end());
	}
	uint8_t Get(int64_t key) const {
		const uint64_t k = static_cast<uint64_t>(key);
		if (ACCEL_LIKELY(k < size)) {
			return v[static_cast<std::size_t>(k)];
		}
		if (over.empty() || k < kDenseMax) {
			return 0;
		}
		auto it = std::lower_bound(over.begin(), over.end(), Ov {key, 0});
		return (it != over.end() && it->key == key) ? static_cast<uint8_t>(it->val) : 0;
	}
	void Write(ByteWriter &w) const {
		w.PutVector(v);
		w.PutVector(over);
	}
	void Read(ByteReader &r) {
		v = r.GetVector<uint8_t>();
		size = v.size() - 8;
		over = r.GetVector<Ov>();
	}
};

//! sel <- { i : table.Get(keys[i]) != 0 } (ascending).  sel needs n + 16 entries.
inline std::size_t SelectNonZero(const int64_t *ACCEL_RESTRICT keys, std::size_t n, const ByteTable &tab,
                                 uint32_t *ACCEL_RESTRICT sel) {
	const uint8_t *ACCEL_RESTRICT table = tab.v.data();
	const uint64_t size = tab.size;
	std::size_t count = 0, i = 0;
#if defined(ACCEL_SIMD_AVX512)
	if (tab.over.empty()) {
		const __m512i vsize = _mm512_set1_epi64(static_cast<long long>(size));
		const __m256i low_byte = _mm256_set1_epi32(0xFF);
		const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
		for (; i + 8 <= n; i += 8) {
			const __m512i k = _mm512_loadu_si512(keys + i);
			const __mmask8 inr = _mm512_cmplt_epu64_mask(k, vsize);
			const __m256i w = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), inr, k, table, 1);
			const __m256i b = _mm256_and_si256(w, low_byte);
			const __mmask8 hit = _mm256_test_epi32_mask(b, b);
			const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
			_mm256_mask_compressstoreu_epi32(sel + count, hit, idx);
			count += static_cast<std::size_t>(__builtin_popcount(hit));
		}
	}
#endif
	for (; i < n; i++) {
		sel[count] = static_cast<uint32_t>(i);
		count += tab.Get(keys[i]) != 0 ? 1 : 0;
	}
	return count;
}

//! date + interval '1 year' (Feb 29 -> Feb 28).
inline int32_t PlusOneYear(int32_t d) {
	int y;
	unsigned m, dd;
	CivilFromDays(d, y, m, dd);
	if (m == 2 && dd == 29) {
		dd = 28;
	}
	return DaysFromCivil(y + 1, m, dd);
}

inline constexpr int kYearBase = 1990; // year code = year - kYearBase, 1..15
inline constexpr int kMaxNations = 32;
inline constexpr int kMaxYears = 16;

inline uint8_t YearCode(int32_t days) {
	int y;
	unsigned m, d;
	CivilFromDays(days, y, m, d);
	if (y <= kYearBase || y >= kYearBase + kMaxYears) {
		throw KernelError("order year outside the supported range 1991..2004");
	}
	return static_cast<uint8_t>(y - kYearBase);
}

//! Flat open-addressing map (partkey, suppkey) -> supplycost (scaled by 100).
struct PsMap {
	struct Entry {
		int64_t pk, sk, cost;
	};
	std::vector<Entry> t;
	uint64_t mask = 0;
	static uint64_t Hash(int64_t pk, int64_t sk) {
		uint64_t h = static_cast<uint64_t>(pk) * 0x9E3779B97F4A7C15ULL ^ (static_cast<uint64_t>(sk) * 0xC2B2AE3D27D4EB4FULL);
		return h ^ (h >> 29);
	}
	void Init(std::size_t n) {
		std::size_t cap = 16;
		while (cap < 2 * n) {
			cap <<= 1;
		}
		t.assign(cap, Entry {-1, -1, 0});
		mask = cap - 1;
	}
	void Insert(int64_t pk, int64_t sk, int64_t cost) {
		uint64_t i = Hash(pk, sk) & mask;
		while (t[i].pk != -1 && !(t[i].pk == pk && t[i].sk == sk)) {
			i = (i + 1) & mask;
		}
		t[i] = Entry {pk, sk, cost};
	}
	const Entry *Find(int64_t pk, int64_t sk) const {
		uint64_t i = Hash(pk, sk) & mask;
		while (t[i].pk != -1) {
			if (t[i].pk == pk && t[i].sk == sk) {
				return &t[i];
			}
			i = (i + 1) & mask;
		}
		return nullptr;
	}
};

struct Dims {
	ByteTable part;                  // partkey -> 1 if p_name LIKE '%color%'
	ByteTable supp;                  // suppkey -> nationkey + 1
	std::vector<std::string> nation; // nationkey -> name ("" if absent)
};

inline Dims BuildDims(const std::string &color) {
	Dims d;
	{
		ScanSpec s;
		s.table = "part";
		s.columns = {{"p_partkey", ColType::Int64}, {"p_name", ColType::Text}};
		ScanTable(s, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				if (b.Str(1)[i].View().find(color) != std::string_view::npos) {
					d.part.Set(b.I64(0)[i], 1);
				}
			}
		});
	}
	{
		ScanSpec s;
		s.table = "nation";
		s.columns = {{"n_nationkey", ColType::Int64}, {"n_name", ColType::Text}};
		ScanTable(s, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				const int64_t k = b.I64(0)[i];
				if (k < 0 || k >= kMaxNations - 1) {
					throw KernelError("nation key outside the supported range");
				}
				if (d.nation.size() <= static_cast<std::size_t>(k)) {
					d.nation.resize(static_cast<std::size_t>(k) + 1);
				}
				d.nation[static_cast<std::size_t>(k)] = std::string(b.Str(1)[i].View());
			}
		});
	}
	{
		ScanSpec s;
		s.table = "supplier";
		s.columns = {{"s_suppkey", ColType::Int64}, {"s_nationkey", ColType::Int64}};
		ScanTable(s, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				const int64_t nk = b.I64(1)[i];
				if (nk < 0 || nk >= kMaxNations - 1) {
					throw KernelError("nation key outside the supported range");
				}
				d.supp.Set(b.I64(0)[i], static_cast<uint8_t>(nk + 1));
			}
		});
	}
	d.part.Finish();
	d.supp.Finish();
	return d;
}

inline ScanSpec PartsuppSpec() {
	ScanSpec s;
	s.table = "partsupp";
	s.columns = {{"ps_partkey", ColType::Int64}, {"ps_suppkey", ColType::Int64}, {"ps_supplycost", ColType::Decimal, 2}};
	return s;
}

inline ScanSpec OrdersSpec() {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_orderkey", ColType::Int64}, {"o_orderdate", ColType::Date}};
	return s;
}

inline ScanSpec LineitemSpec() {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_partkey", ColType::Int64},
	             {"l_orderkey", ColType::Int64},
	             {"l_suppkey", ColType::Int64},
	             {"l_quantity", ColType::Decimal, 2},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2}};
	return s;
}

//! Partsupp batch -> rows whose part matched.
template <class Fn>
inline void PartsuppBatch(const Batch &b, const ByteTable &part, Fn &&emit) {
	for (std::size_t i = 0; i < b.count; i++) {
		if (part.Get(b.I64(0)[i])) {
			emit(b.I64(0)[i], b.I64(1)[i], b.I64(2)[i]);
		}
	}
}

struct Acc {
	__int128 sum[kMaxNations][kMaxYears] = {};
	int64_t cnt[kMaxNations][kMaxYears] = {};
};

inline void Probe(const Batch &b, const ByteTable &part, const ByteTable &supp, const ByteTable &ord, const PsMap &ps,
                  Acc &acc) {
	uint32_t sel[kBatchRows + 16];
	const int64_t *pk = b.I64(0);
	const int64_t *ok = b.I64(1);
	const int64_t *sk = b.I64(2);
	const int64_t *qty = b.I64(3);
	const int64_t *price = b.I64(4);
	const int64_t *disc = b.I64(5);
	const std::size_t m = SelectNonZero(pk, b.count, part, sel);
	for (std::size_t j = 0; j < m; j++) {
		const std::size_t i = sel[j];
		const auto *e = ps.Find(pk[i], sk[i]);
		const unsigned nat = supp.Get(sk[i]);
		const unsigned yr = ord.Get(ok[i]);
		if (e == nullptr || nat == 0 || yr == 0) {
			continue;
		}
		const int64_t amount = price[i] * (100 - disc[i]) - e->cost * qty[i];
		acc.sum[nat - 1][yr] += amount;
		acc.cnt[nat - 1][yr]++;
	}
}

inline void Emit(const Dims &d, const Acc &acc, ResultWriter &out) {
	struct Row {
		const std::string *nation;
		int year;
		__int128 sum;
	};
	std::vector<Row> rows;
	for (int n = 0; n < kMaxNations; n++) {
		for (int y = 1; y < kMaxYears; y++) {
			if (acc.cnt[n][y] > 0 && static_cast<std::size_t>(n) < d.nation.size()) {
				rows.push_back({&d.nation[static_cast<std::size_t>(n)], y + kYearBase, acc.sum[n][y]});
			}
		}
	}
	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
		if (*a.nation != *b.nation) {
			return *a.nation < *b.nation;
		}
		return a.year > b.year;
	});
	for (const auto &r : rows) {
		out.Text(*r.nation);
		out.Int(r.year);
		out.Decimal(r.sum, 4);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q9

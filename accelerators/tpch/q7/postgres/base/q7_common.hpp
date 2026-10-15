// TPC-H Q7 (base PostgreSQL): shared code for both kernels.
//
// Supplier and customer nations are reduced to a 2-bit mask (bit0: nation is
// the first argument, bit1: the second).  Orders -> dense table orderkey ->
// customer mask (only non-zero masks stored).  Lineitem (shipdate range pushed
// into the scan): SIMD gather of the order table, then the supplier mask must
// pair with the customer mask; revenue accumulates per (direction, year).
#pragma once

#include "backends/postgres/api/base/pg_base.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace accelerator_codegen::pg_base::q7 {

inline constexpr const char *kSqlArgs = "text, text";
inline constexpr const char *kSqlReturns =
    "TABLE (supp_nation text, cust_nation text, l_year bigint, revenue numeric)";

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

struct Params {
	std::string n1, n2;
};

inline Params Bind(const Args &args) {
	return Params {args.Text(0), args.Text(1)};
}

struct Dims {
	ByteTable supp; // suppkey -> mask
	ByteTable cust; // custkey -> mask
	bool any = false;
};

inline Dims BuildDims(const Params &p) {
	Dims d;
	int64_t k1 = -1, k2 = -1;
	{
		ScanSpec s;
		s.table = "nation";
		s.columns = {{"n_nationkey", ColType::Int64}, {"n_name", ColType::Text}};
		ScanTable(s, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				const auto nm = b.Str(1)[i].View();
				if (nm == p.n1) {
					k1 = b.I64(0)[i];
				}
				if (nm == p.n2) {
					k2 = b.I64(0)[i];
				}
			}
		});
	}
	if (k1 < 0 || k2 < 0) {
		return d; // a nation that does not exist matches nothing
	}
	d.any = true;
	auto mask_of = [&](int64_t nk) -> uint8_t { return static_cast<uint8_t>((nk == k1 ? 1 : 0) | (nk == k2 ? 2 : 0)); };
	for (int t = 0; t < 2; t++) {
		ScanSpec s;
		s.table = t == 0 ? "supplier" : "customer";
		s.columns = {{t == 0 ? "s_suppkey" : "c_custkey", ColType::Int64},
		             {t == 0 ? "s_nationkey" : "c_nationkey", ColType::Int64}};
		// nationkey in {k1, k2}: a Between on [min,max] is a cheap pre-filter.
		s.filters = {{1, FilterOp::Between, std::min(k1, k2), std::max(k1, k2)}};
		ByteTable &tab = t == 0 ? d.supp : d.cust;
		ScanTable(s, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				const uint8_t m = mask_of(b.I64(1)[i]);
				if (m) {
					tab.Set(b.I64(0)[i], m);
				}
			}
		});
	}
	d.supp.Finish();
	d.cust.Finish();
	return d;
}

inline ScanSpec OrdersSpec() {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_custkey", ColType::Int64}, {"o_orderkey", ColType::Int64}};
	return s;
}

template <class Fn>
inline void OrdersBatch(const Batch &b, const ByteTable &cust, Fn &&emit) {
	const int64_t *ck = b.I64(0);
	const int64_t *ok = b.I64(1);
	for (std::size_t i = 0; i < b.count; i++) {
		const uint8_t m = cust.Get(ck[i]);
		if (m) {
			emit(ok[i], m);
		}
	}
}

inline int32_t Jan1996() {
	return DaysFromCivil(1996, 1, 1);
}

inline ScanSpec LineitemSpec() {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_shipdate", ColType::Date},
	             {"l_orderkey", ColType::Int64},
	             {"l_suppkey", ColType::Int64},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2}};
	s.filters = {{0, FilterOp::Between, DaysFromCivil(1995, 1, 1), DaysFromCivil(1996, 12, 31)}};
	return s;
}

//! rev[direction][year 0=1995,1=1996]
struct Acc {
	__int128 rev[2][2] = {};
	int64_t cnt[2][2] = {};
};

inline void Probe(const Batch &b, const ByteTable &ord, const ByteTable &supp, int32_t split, Acc &acc) {
	uint32_t sel[kBatchRows + 16];
	const int32_t *sd = b.I32(0);
	const int64_t *ok = b.I64(1);
	const int64_t *sk = b.I64(2);
	const int64_t *price = b.I64(3);
	const int64_t *disc = b.I64(4);
	const std::size_t m = SelectNonZero(ok, b.count, ord, sel);
	for (std::size_t j = 0; j < m; j++) {
		const std::size_t i = sel[j];
		const unsigned cm = ord.Get(ok[i]);
		const unsigned sm = supp.Get(sk[i]);
		int dir;
		if ((sm & 1) && (cm & 2)) {
			dir = 0;
		} else if ((sm & 2) && (cm & 1)) {
			dir = 1;
		} else {
			continue;
		}
		const int yr = sd[i] >= split ? 1 : 0;
		acc.rev[dir][yr] += static_cast<__int128>(price[i]) * (100 - disc[i]);
		acc.cnt[dir][yr]++;
	}
}

inline void Emit(const Params &p, const Acc &acc, ResultWriter &out) {
	// direction 0: (n1, n2), direction 1: (n2, n1); equal names collapse to dir 0 only.
	struct Row {
		const std::string *s, *c;
		int year;
		__int128 rev;
	};
	std::vector<Row> rows;
	for (int dir = 0; dir < 2; dir++) {
		for (int y = 0; y < 2; y++) {
			if (acc.cnt[dir][y] > 0) {
				rows.push_back({dir == 0 ? &p.n1 : &p.n2, dir == 0 ? &p.n2 : &p.n1, 1995 + y, acc.rev[dir][y]});
			}
		}
	}
	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
		if (*a.s != *b.s) {
			return *a.s < *b.s;
		}
		if (*a.c != *b.c) {
			return *a.c < *b.c;
		}
		return a.year < b.year;
	});
	for (const auto &r : rows) {
		out.Text(*r.s);
		out.Text(*r.c);
		out.Int(r.year);
		out.Decimal(r.rev, 4);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_base::q7

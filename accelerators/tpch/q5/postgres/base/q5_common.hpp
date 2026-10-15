// TPC-H Q5 (base PostgreSQL): shared parameters, dimension builds and the
// lineitem probe used by the single-threaded and parallel kernels.
//
// Plan: region -> nations of the region; supplier / customer -> dense byte
// tables (0 = not in region, else 1 + index of the nation in the region list);
// orders (date range) -> dense table orderkey -> customer nation code;
// lineitem probe: SIMD gather of the order table selects candidate rows, then
// the supplier nation code must equal the order's customer nation code.
#pragma once

#include "backends/postgres/api/base/pg_base.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace accelerator_codegen::pg_base::q5 {

inline constexpr const char *kSqlArgs = "text, date";
inline constexpr const char *kSqlReturns = "TABLE (n_name text, revenue numeric)";

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
	std::string region;
	int32_t start;
	int32_t end_excl;
};

inline Params Bind(const Args &args) {
	Params p;
	p.region = args.Text(0);
	p.start = args.Date(1);
	p.end_excl = PlusOneYear(p.start);
	return p;
}

//! Small dimension tables (built serially in the calling backend).
struct Dims {
	std::vector<std::string> names; // names[code-1]
	ByteTable supp;                 // suppkey -> code
	ByteTable cust;                 // custkey -> code
};

inline Dims BuildDims(const Params &p) {
	Dims d;
	int64_t region_key = -1;
	{
		ScanSpec s;
		s.table = "region";
		s.columns = {{"r_regionkey", ColType::Int64}, {"r_name", ColType::Text}};
		s.filters = {{1, FilterOp::TextEq, 0, 0, {p.region}}};
		ScanTable(s, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				region_key = b.I64(0)[i];
			}
		});
	}
	if (region_key < 0) {
		return d;
	}
	std::vector<int64_t> nation_keys;
	{
		ScanSpec s;
		s.table = "nation";
		s.columns = {{"n_nationkey", ColType::Int64}, {"n_name", ColType::Text}, {"n_regionkey", ColType::Int64}};
		s.filters = {{2, FilterOp::Between, region_key, region_key}};
		ScanTable(s, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				nation_keys.push_back(b.I64(0)[i]);
				d.names.emplace_back(b.Str(1)[i].View());
			}
		});
	}
	if (nation_keys.size() > 200) {
		throw KernelError("too many nations");
	}
	auto code_of = [&](int64_t nk) -> uint8_t {
		for (std::size_t j = 0; j < nation_keys.size(); j++) {
			if (nation_keys[j] == nk) {
				return static_cast<uint8_t>(j + 1);
			}
		}
		return 0;
	};
	{
		ScanSpec s;
		s.table = "supplier";
		s.columns = {{"s_suppkey", ColType::Int64}, {"s_nationkey", ColType::Int64}};
		ScanTable(s, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				const uint8_t c = code_of(b.I64(1)[i]);
				if (c) {
					d.supp.Set(b.I64(0)[i], c);
				}
			}
		});
	}
	{
		ScanSpec s;
		s.table = "customer";
		s.columns = {{"c_custkey", ColType::Int64}, {"c_nationkey", ColType::Int64}};
		ScanTable(s, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				const uint8_t c = code_of(b.I64(1)[i]);
				if (c) {
					d.cust.Set(b.I64(0)[i], c);
				}
			}
		});
	}
	d.supp.Finish();
	d.cust.Finish();
	return d;
}

inline ScanSpec OrdersSpec(const Params &p) {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_orderdate", ColType::Date}, {"o_custkey", ColType::Int64}, {"o_orderkey", ColType::Int64}};
	s.filters = {{0, FilterOp::Between, p.start, p.end_excl - 1}};
	return s;
}

//! Orders batch -> (orderkey, code) of orders whose customer is in the region.
template <class Fn>
inline void OrdersBatch(const Batch &b, const ByteTable &cust, Fn &&emit) {
	const int64_t *ck = b.I64(1);
	const int64_t *ok = b.I64(2);
	for (std::size_t i = 0; i < b.count; i++) {
		const uint8_t c = cust.Get(ck[i]);
		if (c) {
			emit(ok[i], c);
		}
	}
}

inline ScanSpec LineitemSpec(const std::vector<int64_t> &order_keys) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_orderkey", ColType::Int64},
	             {"l_suppkey", ColType::Int64},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2}};
	// Probe lineitem by the qualifying orders' keys (index bitmap scan when the key set is small).
	FilterSpec f {0, FilterOp::IntIn};
	f.ints = order_keys;
	s.filters.push_back(std::move(f));
	return s;
}

struct Acc {
	__int128 rev[256] = {};
	int64_t cnt[256] = {};
};

inline void Probe(const Batch &b, const ByteTable &ord, const ByteTable &supp, Acc &acc) {
	uint32_t sel[kBatchRows + 16];
	const int64_t *sk = b.I64(1);
	const int64_t *price = b.I64(2);
	const int64_t *disc = b.I64(3);
	const int64_t *ok = b.I64(0);
	const std::size_t m = SelectNonZero(ok, b.count, ord, sel);
	for (std::size_t j = 0; j < m; j++) {
		const std::size_t i = sel[j];
		const uint8_t oc = ord.Get(ok[i]);
		if (supp.Get(sk[i]) == oc) {
			acc.rev[oc] += static_cast<__int128>(price[i]) * (100 - disc[i]);
			acc.cnt[oc]++;
		}
	}
}

inline void Emit(const Dims &d, const Acc &acc, ResultWriter &out) {
	struct Row {
		const std::string *name;
		__int128 rev;
	};
	std::vector<Row> rows;
	for (std::size_t j = 0; j < d.names.size(); j++) {
		if (acc.cnt[j + 1] > 0) {
			rows.push_back({&d.names[j], acc.rev[j + 1]});
		}
	}
	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
		if (a.rev != b.rev) {
			return a.rev > b.rev;
		}
		return *a.name < *b.name;
	});
	for (const auto &r : rows) {
		out.Text(*r.name);
		out.Decimal(r.rev, 4);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_base::q5

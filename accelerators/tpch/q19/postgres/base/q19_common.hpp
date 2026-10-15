// TPC-H Q19 (base PostgreSQL): shared code for both kernels.
//
// part -> dense partkey byte table holding a 3-bit mask of the OR-ed groups
// the part satisfies (brand, container set, size range).  Lineitem: the common
// predicates (l_shipinstruct, l_shipmode) and the union of the quantity ranges
// are pushed into the scan; surviving rows are filtered by a SIMD gather of the
// part mask and a per-group quantity test; revenue is summed exactly.
#pragma once

#include "backends/postgres/api/base/pg_base.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace accelerator_codegen::pg_base::q19 {

inline constexpr const char *kSqlArgs = "text, bigint, text, bigint, text, bigint";
inline constexpr const char *kSqlReturns = "TABLE (revenue numeric)";

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
	std::string brand[3];
	int64_t qlo[3]; // l_quantity >= qlo (scale 2)
	int64_t qhi[3]; // l_quantity <= qhi (scale 2)
};

inline Params Bind(const Args &args) {
	Params p;
	for (int g = 0; g < 3; g++) {
		p.brand[g] = args.Text(2 * g);
		const int64_t q = args.Int(2 * g + 1);
		p.qlo[g] = q * 100;
		p.qhi[g] = (q + 10) * 100;
	}
	return p;
}

inline constexpr const char *kContainers[3][4] = {{"SM CASE", "SM BOX", "SM PACK", "SM PKG"},
                                                  {"MED BAG", "MED BOX", "MED PKG", "MED PACK"},
                                                  {"LG CASE", "LG BOX", "LG PACK", "LG PKG"}};
inline constexpr int kMaxSize[3] = {5, 10, 15};

//! Mask of groups a part satisfies.
inline uint8_t PartMask(const Params &p, std::string_view brand, std::string_view container, int32_t size) {
	uint8_t m = 0;
	for (int g = 0; g < 3; g++) {
		if (brand == p.brand[g] && size >= 1 && size <= kMaxSize[g]) {
			for (const char *c : kContainers[g]) {
				if (container == c) {
					m |= static_cast<uint8_t>(1u << g);
					break;
				}
			}
		}
	}
	return m;
}

inline ScanSpec PartSpec(const Params &p) {
	ScanSpec s;
	s.table = "part";
	s.columns = {{"p_brand", ColType::Text},
	             {"p_partkey", ColType::Int64},
	             {"p_container", ColType::Text},
	             {"p_size", ColType::Int32}};
	s.filters = {{3, FilterOp::Between, 1, kMaxSize[2]},
	             {0, FilterOp::TextIn, 0, 0, {p.brand[0], p.brand[1], p.brand[2]}}};
	return s;
}

struct PartHit {
	int64_t key;
	int64_t mask;
};

inline void PartConsume(const Batch &b, const Params &p, std::vector<PartHit> &hits) {
	for (std::size_t i = 0; i < b.count; i++) {
		const uint8_t m = PartMask(p, b.Str(0)[i].View(), b.Str(2)[i].View(), b.I32(3)[i]);
		if (m) {
			hits.push_back({b.I64(1)[i], m});
		}
	}
}

inline ByteTable MakeTable(const std::vector<PartHit> &hits, std::vector<int64_t> &keys) {
	ByteTable t;
	for (const auto &h : hits) {
		t.Set(h.key, static_cast<uint8_t>(h.mask));
		keys.push_back(h.key);
	}
	t.Finish();
	return t;
}

inline ByteTable BuildPartTable(const Params &p, std::vector<int64_t> &keys) {
	std::vector<PartHit> hits;
	ScanTable(PartSpec(p), [&](const Batch &b) { PartConsume(b, p, hits); });
	return MakeTable(hits, keys);
}

inline ScanSpec LineitemSpec(const Params &p, const std::vector<int64_t> &keys) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_shipinstruct", ColType::Text},
	             {"l_shipmode", ColType::Text},
	             {"l_quantity", ColType::Decimal, 2},
	             {"l_partkey", ColType::Int64},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2}};
	const int64_t lo = std::min({p.qlo[0], p.qlo[1], p.qlo[2]});
	const int64_t hi = std::max({p.qhi[0], p.qhi[1], p.qhi[2]});
	// Probe by the qualifying parts first (index-driven via idx_lineitem_partkey).
	FilterSpec pf {3, FilterOp::IntIn};
	pf.ints = keys;
	s.filters.push_back(std::move(pf));
	s.filters.push_back({0, FilterOp::TextEq, 0, 0, {"DELIVER IN PERSON"}});
	s.filters.push_back({1, FilterOp::TextIn, 0, 0, {"AIR", "AIR REG"}});
	s.filters.push_back({2, FilterOp::Between, lo, hi});
	return s;
}

struct Agg {
	__int128 sum = 0;
	int64_t cnt = 0;
};

inline void Probe(const Batch &b, const Params &p, const ByteTable &part, Agg &agg) {
	uint32_t sel[kBatchRows + 16];
	const int64_t *qty = b.I64(2);
	const int64_t *pk = b.I64(3);
	const int64_t *price = b.I64(4);
	const int64_t *disc = b.I64(5);
	const std::size_t m = SelectNonZero(pk, b.count, part, sel);
	__int128 sum = 0;
	int64_t cnt = 0;
	for (std::size_t j = 0; j < m; j++) {
		const std::size_t i = sel[j];
		const unsigned mask = part.Get(pk[i]);
		const int64_t q = qty[i];
		bool ok = false;
		for (int g = 0; g < 3; g++) {
			ok |= ((mask >> g) & 1u) && q >= p.qlo[g] && q <= p.qhi[g];
		}
		if (ok) {
			sum += static_cast<__int128>(price[i]) * (100 - disc[i]);
			cnt++;
		}
	}
	agg.sum += sum;
	agg.cnt += cnt;
}

inline void Emit(const Agg &agg, ResultWriter &out) {
	if (agg.cnt > 0) {
		out.Decimal(agg.sum, 4);
	} else {
		out.Null();
	}
	out.EndRow();
}

} // namespace accelerator_codegen::pg_base::q19

// TPC-H Q8 (stateless PostgreSQL): parameters, scan specs and the kernel
// pieces shared by the single-threaded and parallel kernels.
//
// Plan: tiny region/nation scans give the set of customer nations and the
// target nation; part (p_type suffix), supplier (target nation) and customer
// (region) become key bitmaps; orders in the two-year window whose customer is
// in the region go into an open-addressing hash orderkey -> year slot.  The
// lineitem scan tests partkey bits with an AVX-512 gather + compress-store,
// probes the order hash (hash/prefetch pass then probe pass) and sums volume
// into int64 per-year totals (total / target nation).
#pragma once

#include "backends/postgres/api/stateless/pg_stateless.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace accelerator_codegen::pg_stateless::q8 {

inline constexpr const char *kSqlArgs = "text, text, text";
inline constexpr const char *kSqlReturns = "TABLE (o_year bigint, mkt_share numeric)";

struct Params {
	std::string nation, region, type_suffix;
};

inline Params Bind(const Args &args) {
	return Params {args.Text(0), args.Text(1), args.Text(2)};
}

inline int32_t WindowStart() {
	return DaysFromCivil(1995, 1, 1);
}
inline int32_t WindowEnd() { // inclusive
	return DaysFromCivil(1996, 12, 31);
}
inline int32_t Year1996() {
	return DaysFromCivil(1996, 1, 1);
}

inline ScanSpec RegionSpec(const Params &p) {
	ScanSpec s;
	s.table = "region";
	s.columns = {{"r_regionkey", ColType::Int32}, {"r_name", ColType::Text}};
	FilterSpec f;
	f.column = 1;
	f.op = FilterOp::TextEq;
	f.strings = {p.region};
	s.filters = {f};
	return s;
}
inline ScanSpec NationSpec() {
	ScanSpec s;
	s.table = "nation";
	s.columns = {{"n_nationkey", ColType::Int32}, {"n_name", ColType::Text}, {"n_regionkey", ColType::Int32}};
	return s;
}
inline ScanSpec PartSpec() {
	ScanSpec s;
	s.table = "part";
	s.columns = {{"p_partkey", ColType::Int64}, {"p_type", ColType::Text}};
	return s;
}
inline ScanSpec SupplierSpec() {
	ScanSpec s;
	s.table = "supplier";
	s.columns = {{"s_suppkey", ColType::Int64}, {"s_nationkey", ColType::Int32}};
	return s;
}
inline ScanSpec CustomerSpec() {
	ScanSpec s;
	s.table = "customer";
	s.columns = {{"c_custkey", ColType::Int64}, {"c_nationkey", ColType::Int32}};
	return s;
}
inline ScanSpec OrdersSpec() {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_orderdate", ColType::Date}, {"o_orderkey", ColType::Int64}, {"o_custkey", ColType::Int64}};
	s.filters = {{0, FilterOp::Between, WindowStart(), WindowEnd()}};
	return s;
}
enum : int { kLiPart = 0, kLiSupp = 1, kLiOrder = 2, kLiPrice = 3, kLiDisc = 4 };
inline ScanSpec LineitemSpec() {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_partkey", ColType::Int64},
	             {"l_suppkey", ColType::Int64},
	             {"l_orderkey", ColType::Int64},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2}};
	return s;
}

//! Nation facts: customer-region membership by nationkey and the target nation.
struct NationInfo {
	std::vector<uint8_t> in_region; // by nationkey
	int32_t target = -1;
	bool InRegion(int32_t nk) const {
		return nk >= 0 && std::size_t(nk) < in_region.size() && in_region[nk];
	}
};

//! Region + nation scans (serial; tiny).
inline NationInfo LoadNations(const Params &p) {
	int32_t regionkey = -1;
	bool have_region = false;
	ScanTable(RegionSpec(p), [&](const Batch &b) {
		if (b.count) {
			regionkey = b.I32(0)[0];
			have_region = true;
		}
	});
	NationInfo ni;
	ScanTable(NationSpec(), [&](const Batch &b) {
		for (std::size_t i = 0; i < b.count; i++) {
			const int32_t nk = b.I32(0)[i];
			if (nk < 0) {
				continue;
			}
			if (ni.in_region.size() <= std::size_t(nk)) {
				ni.in_region.resize(nk + 1, 0);
			}
			if (have_region && b.I32(2)[i] == regionkey) {
				ni.in_region[nk] = 1;
			}
			if (b.Str(1)[i].View() == p.nation) {
				ni.target = nk;
			}
		}
	});
	return ni;
}

//! Key bitmap over [0, Limit()) with a guaranteed-zero last bit (so clamped
//! probes are safe) plus a sorted overflow list for keys outside that range
//! (the test database contains a few keys far above the dense range).
struct Bitmap {
	std::vector<uint64_t> words;
	std::vector<int64_t> overflow;
	static Bitmap Make(const std::vector<int64_t> &keys) {
		const int64_t cap = int64_t(64) * std::max<std::size_t>(keys.size(), 65536);
		int64_t mx = 0;
		for (int64_t k : keys) {
			if (k < cap) {
				mx = std::max(mx, k);
			}
		}
		Bitmap b;
		b.words.assign(std::size_t(mx + 1) / 64 + 1, 0);
		const uint64_t limit = b.Limit();
		for (int64_t k : keys) {
			if (static_cast<uint64_t>(k) < limit) {
				b.words[k >> 6] |= uint64_t(1) << (k & 63);
			} else {
				b.overflow.push_back(k);
			}
		}
		std::sort(b.overflow.begin(), b.overflow.end());
		return b;
	}
	uint64_t Limit() const {
		return words.size() * 64 - 1;
	}
	bool Test(int64_t k) const {
		const uint64_t u = static_cast<uint64_t>(k);
		if (u < Limit()) {
			return (words[u >> 6] >> (u & 63)) & 1;
		}
		return !overflow.empty() && std::binary_search(overflow.begin(), overflow.end(), k);
	}
};

struct Slot {
	int64_t key;
	int64_t val;
};
inline constexpr int64_t kEmpty = INT64_MIN;
inline constexpr uint64_t kHashMul = 0x9E3779B97F4A7C15ULL;

struct Hash {
	std::vector<Slot> slots;
	uint64_t mask = 0;
	int shift = 64;
	void Init(std::size_t n) {
		int bits = 4;
		while ((std::size_t(1) << bits) < n * 5 / 2) {
			bits++;
		}
		slots.assign(std::size_t(1) << bits, Slot {kEmpty, 0});
		mask = (uint64_t(1) << bits) - 1;
		shift = 64 - bits;
	}
	std::size_t Home(int64_t k) const {
		return (static_cast<uint64_t>(k) * kHashMul) >> shift;
	}
	void Insert(int64_t k, int64_t v) {
		std::size_t s = Home(k);
		while (slots[s].key != kEmpty && slots[s].key != k) {
			s = (s + 1) & mask;
		}
		slots[s].key = k;
		slots[s].val = v;
	}
};

inline void PutBitmap(ByteWriter &w, const Bitmap &b) {
	w.PutVector(b.words);
	w.PutVector(b.overflow);
}
inline Bitmap GetBitmap(ByteReader &r) {
	Bitmap b;
	b.words = r.GetVector<uint64_t>();
	b.overflow = r.GetVector<int64_t>();
	return b;
}

//! Dimension scan consumers (used in the single process and in the round-1 task).
inline void CollectParts(const Batch &b, const std::string &suffix, std::vector<int64_t> &keys) {
	for (std::size_t i = 0; i < b.count; i++) {
		const std::string_view t = b.Str(1)[i].View();
		if (t.size() >= suffix.size() && t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0) {
			keys.push_back(b.I64(0)[i]);
		}
	}
}
inline void CollectSuppliers(const Batch &b, int32_t target, std::vector<int64_t> &keys) {
	for (std::size_t i = 0; i < b.count; i++) {
		if (b.I32(1)[i] == target) {
			keys.push_back(b.I64(0)[i]);
		}
	}
}
inline void CollectCustomers(const Batch &b, const NationInfo &ni, std::vector<int64_t> &keys) {
	for (std::size_t i = 0; i < b.count; i++) {
		if (ni.InRegion(b.I32(1)[i])) {
			keys.push_back(b.I64(0)[i]);
		}
	}
}
//! Orders rows of the window: orderkey, custkey, year slot (0 = 1995, 1 = 1996).
struct OrderRows {
	std::vector<int64_t> orderkey, custkey;
	std::vector<uint8_t> year;
	void Add(const Batch &b) {
		const int32_t y96 = Year1996();
		for (std::size_t i = 0; i < b.count; i++) {
			orderkey.push_back(b.I64(1)[i]);
			custkey.push_back(b.I64(2)[i]);
			year.push_back(b.I32(0)[i] >= y96 ? 1 : 0);
		}
	}
};

//! Keep only orders whose customer is in `cust`.
inline void FilterOrders(const OrderRows &in, const Bitmap &cust, std::vector<int64_t> &okey,
                         std::vector<uint8_t> &oyear) {
	for (std::size_t i = 0; i < in.orderkey.size(); i++) {
		if (cust.Test(in.custkey[i])) {
			okey.push_back(in.orderkey[i]);
			oyear.push_back(in.year[i]);
		}
	}
}

//! Lineitem aggregation: per year slot total volume, target-nation volume, rows (volume at scale 4).
struct Agg {
	int64_t total[2] = {0, 0};
	int64_t target[2] = {0, 0};
	int64_t rows[2] = {0, 0};
};

struct Prober {
	Bitmap part, supp;
	Hash orders;
	Agg agg;

	void Init(Bitmap part_p, Bitmap supp_p, const std::vector<int64_t> &okey, const std::vector<uint8_t> &oyear) {
		part = std::move(part_p);
		supp = std::move(supp_p);
		orders.Init(okey.size() + 1);
		for (std::size_t i = 0; i < okey.size(); i++) {
			orders.Insert(okey[i], oyear[i]);
		}
	}

	void Consume(const Batch &b) {
		const std::size_t n = b.count;
		const int64_t *ACCEL_RESTRICT pk = b.I64(kLiPart);
		const int64_t *ACCEL_RESTRICT sk = b.I64(kLiSupp);
		const int64_t *ACCEL_RESTRICT ok = b.I64(kLiOrder);
		const int64_t *ACCEL_RESTRICT price = b.I64(kLiPrice);
		const int64_t *ACCEL_RESTRICT disc = b.I64(kLiDisc);
		alignas(64) uint32_t sel[kBatchRows + simd::kSelSlack];
		alignas(64) uint32_t home[kBatchRows + simd::kSelSlack];
		std::size_t m = 0, i = 0;
		const uint64_t limit = part.Limit();
		const uint64_t *pw = part.words.data();
#if defined(ACCEL_SIMD_AVX512)
		{
			const __m512i vlim = _mm512_set1_epi64(static_cast<long long>(limit));
			const __m512i one = _mm512_set1_epi64(1);
			const __m512i c63 = _mm512_set1_epi64(63);
			__m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
			const __m256i eight = _mm256_set1_epi32(8);
			for (; i + 8 <= n; i += 8) {
				const __m512i raw = _mm512_loadu_si512(pk + i);
				const __m512i k = _mm512_min_epu64(raw, vlim);
				const __m512i w = _mm512_i64gather_epi64(_mm512_srli_epi64(k, 6), pw, 8);
				const __m512i bit = _mm512_and_si512(_mm512_srlv_epi64(w, _mm512_and_si512(k, c63)), one);
				const __mmask8 mk = _mm512_test_epi64_mask(bit, bit) | _mm512_cmpge_epu64_mask(raw, vlim);
				_mm256_mask_compressstoreu_epi32(sel + m, mk, _mm256_add_epi32(iota, _mm256_set1_epi32(int(i))));
				m += static_cast<std::size_t>(__builtin_popcount(mk));
			}
			(void)eight;
		}
#endif
		for (; i < n; i++) {
			const uint64_t c = std::min<uint64_t>(static_cast<uint64_t>(pk[i]), limit);
			sel[m] = static_cast<uint32_t>(i);
			m += ((pw[c >> 6] >> (c & 63)) & 1) | (static_cast<uint64_t>(pk[i]) >= limit);
		}
		{
			// Keys outside the dense range were kept as candidates: confirm them.
			std::size_t w = 0;
			for (std::size_t j = 0; j < m; j++) {
				const uint32_t r = sel[j];
				sel[w] = r;
				w += (static_cast<uint64_t>(pk[r]) < limit) || part.Test(pk[r]);
			}
			m = w;
		}
		const Slot *ACCEL_RESTRICT sl = orders.slots.data();
		for (std::size_t j = 0; j < m; j++) {
			home[j] = static_cast<uint32_t>(orders.Home(ok[sel[j]]));
			ACCEL_PREFETCH(sl + home[j]);
		}
		for (std::size_t j = 0; j < m; j++) {
			const uint32_t r = sel[j];
			const int64_t k = ok[r];
			std::size_t s = home[j];
			while (sl[s].key != k && sl[s].key != kEmpty) {
				s = (s + 1) & orders.mask;
			}
			if (sl[s].key != k) {
				continue;
			}
			const int y = static_cast<int>(sl[s].val);
			const int64_t vol = price[r] * (100 - disc[r]);
			agg.total[y] += vol;
			agg.target[y] += supp.Test(sk[r]) ? vol : 0;
			agg.rows[y]++;
		}
	}
};

inline void Emit(const Agg &agg, ResultWriter &out) {
	for (int y = 0; y < 2; y++) {
		if (agg.rows[y] == 0) {
			continue;
		}
		out.Int(1995 + y);
		if (agg.total[y] == 0) {
			out.Null();
		} else {
			// share = target / total at scale 18, rounded half away from zero.
			const __int128 num = static_cast<__int128>(agg.target[y]) * 1000000000000000000LL;
			const __int128 den = agg.total[y];
			const bool neg = (num < 0) != (den < 0);
			const __int128 an = num < 0 ? -num : num, ad = den < 0 ? -den : den;
			__int128 q = (an + ad / 2) / ad;
			out.Decimal(neg ? -q : q, 18);
		}
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q8

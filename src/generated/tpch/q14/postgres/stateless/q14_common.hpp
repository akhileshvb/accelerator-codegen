// TPC-H Q14 (stateless PostgreSQL): promo revenue share.
// Part classification (absent / non-promo / promo) is a dense byte array indexed
// by p_partkey (sorted-vector fallback for huge / sparse key domains); lineitem
// batches are probed with AVX-512 gathers and masked int64 lane sums.
#pragma once

#include "accelerator_codegen/runtime/postgres_stateless/pg_stateless.hpp"
#include "accelerator_codegen/runtime/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace accelerator_codegen::pg_stateless::q14 {

inline constexpr const char *kSqlArgs = "date";
inline constexpr const char *kSqlReturns = "TABLE (promo_revenue double precision)";

struct Params {
	int32_t start;
	int32_t end_excl;
};

inline Params Bind(const Args &args) {
	Params p;
	p.start = args.Date(0);
	int y;
	unsigned m, d;
	CivilFromDays(p.start, y, m, d);
	// date + interval '1 month': clamp the day to the end of the target month.
	int ny = y;
	unsigned nm = m + 1;
	if (nm == 13) {
		nm = 1;
		ny++;
	}
	static constexpr unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	unsigned dim = kDays[nm - 1];
	if (nm == 2 && ((ny % 4 == 0 && ny % 100 != 0) || ny % 400 == 0)) {
		dim = 29;
	}
	p.end_excl = DaysFromCivil(ny, nm, d > dim ? dim : d);
	return p;
}

enum : int { kPartkey = 0, kPrice = 1, kDiscount = 2 };

inline ScanSpec LineitemSpec(const Params &p) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_partkey", ColType::Int64},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2},
	             {"l_shipdate", ColType::Date}};
	s.filters = {{3, FilterOp::Between, p.start, static_cast<int64_t>(p.end_excl) - 1}};
	return s;
}

inline ScanSpec PartSpec() {
	ScanSpec s;
	s.table = "part";
	s.columns = {{"p_partkey", ColType::Int64}, {"p_type", ColType::Text}};
	return s;
}

//! Part classification: dense[key] in {0 absent, 1 non-promo, 2 promo} (padded
//! by 8 bytes for 32-bit gathers) or, for sparse domains, sorted (key, class).
struct SparseEnt {
	int64_t key;
	uint8_t cls;
	bool operator<(const SparseEnt &o) const {
		return key < o.key;
	}
};

struct PartClass {
	std::vector<uint8_t> dense;
	std::vector<SparseEnt> sparse;
	bool is_dense = true;
	uint64_t dense_size = 0;
};

inline constexpr int64_t kMaxDense = int64_t(1) << 26;

inline PartClass BuildPartClass() {
	std::vector<SparseEnt> rows;
	int64_t maxkey = -1;
	bool neg = false;
	ScanTable(PartSpec(), [&](const Batch &b) {
		const int64_t *k = b.I64(0);
		const StringRef *t = b.Str(1);
		for (std::size_t i = 0; i < b.count; i++) {
			const std::string_view v = t[i].View();
			const bool promo = v.size() >= 5 && std::memcmp(v.data(), "PROMO", 5) == 0;
			rows.push_back({k[i], static_cast<uint8_t>(promo ? 2 : 1)});
			maxkey = std::max(maxkey, k[i]);
			neg |= k[i] < 0;
		}
	});
	PartClass pc;
	if (!neg && maxkey < kMaxDense) {
		pc.is_dense = true;
		pc.dense_size = static_cast<uint64_t>(maxkey + 1);
		pc.dense.assign(pc.dense_size + 8, 0);
		for (const auto &r : rows) {
			pc.dense[r.key] = r.cls;
		}
	} else {
		pc.is_dense = false;
		pc.sparse = std::move(rows);
		std::sort(pc.sparse.begin(), pc.sparse.end());
		pc.sparse.erase(std::unique(pc.sparse.begin(), pc.sparse.end(),
		                            [](const SparseEnt &a, const SparseEnt &b) { return a.key == b.key; }),
		                pc.sparse.end());
	}
	return pc;
}

inline void Put(ByteWriter &w, const PartClass &pc) {
	w.Put<uint8_t>(pc.is_dense);
	w.Put<uint64_t>(pc.dense_size);
	w.PutVector(pc.dense);
	w.PutVector(pc.sparse);
}

inline PartClass Get(ByteReader &r) {
	PartClass pc;
	pc.is_dense = r.Get<uint8_t>() != 0;
	pc.dense_size = r.Get<uint64_t>();
	pc.dense = r.GetVector<uint8_t>();
	pc.sparse = r.GetVector<SparseEnt>();
	return pc;
}

struct Agg {
	__int128 total = 0;
	__int128 promo = 0;
	bool any = false;
};

inline uint8_t Lookup(const PartClass &pc, int64_t key) {
	if (pc.is_dense) {
		return (static_cast<uint64_t>(key) < pc.dense_size) ? pc.dense[key] : 0;
	}
	auto it = std::lower_bound(pc.sparse.begin(), pc.sparse.end(), SparseEnt{key, 0});
	return (it != pc.sparse.end() && it->key == key) ? it->cls : 0;
}

inline void Accumulate(const Batch &b, const PartClass &pc, Agg &agg) {
	const int64_t *ACCEL_RESTRICT key = b.I64(kPartkey);
	const int64_t *ACCEL_RESTRICT price = b.I64(kPrice);
	const int64_t *ACCEL_RESTRICT disc = b.I64(kDiscount);
	const std::size_t n = b.count;
	bool small = pc.is_dense;
	for (std::size_t i = 0; i < n; i++) {
		small &= (price[i] < (int64_t(1) << 31)) & (price[i] > -(int64_t(1) << 31)) & (disc[i] < (1 << 20)) &
		         (disc[i] > -(1 << 20));
	}
	if (!small) {
		for (std::size_t i = 0; i < n; i++) {
			const uint8_t c = Lookup(pc, key[i]);
			if (c) {
				const __int128 rev = static_cast<__int128>(price[i]) * (100 - disc[i]);
				agg.total += rev;
				if (c == 2) {
					agg.promo += rev;
				}
				agg.any = true;
			}
		}
		return;
	}
	int64_t total = 0, promo = 0;
	std::size_t i = 0;
	bool any = false;
#if defined(ACCEL_SIMD_AVX512) && defined(__AVX512DQ__)
	__m512i atotal = _mm512_setzero_si512(), apromo = atotal;
	const __m512i v100 = _mm512_set1_epi64(100);
	const __m512i vsize = _mm512_set1_epi64(static_cast<int64_t>(pc.dense_size));
	const int *base = reinterpret_cast<const int *>(pc.dense.data());
	uint32_t present_count = 0;
	for (; i + 8 <= n; i += 8) {
		const __m512i k = _mm512_loadu_si512(key + i);
		const __mmask8 inr = _mm512_cmplt_epu64_mask(k, vsize);
		const __m256i cls32 = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), inr, k, base, 1);
		const __m256i cls = _mm256_and_si256(cls32, _mm256_set1_epi32(0xFF));
		const __mmask8 present = _mm256_cmpneq_epi32_mask(cls, _mm256_setzero_si256()) & inr;
		const __mmask8 isp = _mm256_cmpeq_epi32_mask(cls, _mm256_set1_epi32(2)) & inr;
		const __m512i rev = _mm512_mullo_epi64(_mm512_loadu_si512(price + i),
		                                       _mm512_sub_epi64(v100, _mm512_loadu_si512(disc + i)));
		atotal = _mm512_mask_add_epi64(atotal, present, atotal, rev);
		apromo = _mm512_mask_add_epi64(apromo, isp, apromo, rev);
		present_count += static_cast<uint32_t>(__builtin_popcount(present));
	}
	total = _mm512_reduce_add_epi64(atotal);
	promo = _mm512_reduce_add_epi64(apromo);
	any = present_count != 0;
#endif
	for (; i < n; i++) {
		const uint8_t c = (static_cast<uint64_t>(key[i]) < pc.dense_size) ? pc.dense[key[i]] : 0;
		const int64_t rev = price[i] * (100 - disc[i]);
		total += (c != 0) ? rev : 0;
		promo += (c == 2) ? rev : 0;
		any |= c != 0;
	}
	agg.total += total;
	agg.promo += promo;
	agg.any |= any;
}

inline void Emit(const Agg &agg, ResultWriter &out) {
	if (!agg.any) {
		out.Null();
	} else if (agg.total == 0) {
		throw KernelError("division by zero");
	} else {
		out.Double(100.0 * static_cast<double>(agg.promo) / static_cast<double>(agg.total));
	}
	out.EndRow();
}

} // namespace accelerator_codegen::pg_stateless::q14

// TPC-H Q6 (base PostgreSQL): parameters, scan spec and the SIMD
// revenue accumulation shared by the single-threaded and parallel kernels.
#pragma once

#include "backends/postgres/api/base/pg_base.hpp"
#include "prims/simd.hpp"

#include <cstdint>
#include <limits>

namespace accelerator_codegen::pg_base::q6 {

inline constexpr const char *kSqlArgs = "date, double precision, double precision";
inline constexpr const char *kSqlReturns = "TABLE (revenue numeric)";

struct Params {
	int32_t start;    // shipdate >= start
	int32_t end_excl; // shipdate < start + 1 year
	int64_t disc_lo;  // discount between disc-0.01 and disc+0.01 (scale 2)
	int64_t disc_hi;
	int64_t qty_excl; // quantity < qty (scale 2)
};

inline Params Bind(const Args &args) {
	Params p;
	p.start = args.Date(0);
	int y;
	unsigned m, d;
	CivilFromDays(p.start, y, m, d);
	// date + interval '1 year' (Feb 29 -> Feb 28 like PostgreSQL).
	unsigned dd = d;
	if (m == 2 && d == 29) {
		dd = 28;
	}
	p.end_excl = DaysFromCivil(y + 1, m, dd);
	const int64_t disc = args.Decimal(1, 2);
	p.disc_lo = disc - 1;
	p.disc_hi = disc + 1;
	p.qty_excl = args.Decimal(2, 2);
	return p;
}

enum : int { kShipdate = 0, kDiscount = 1, kQuantity = 2, kExtendedprice = 3 };

inline ScanSpec Spec(const Params &p) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_shipdate", ColType::Date},
	             {"l_discount", ColType::Decimal, 2},
	             {"l_quantity", ColType::Decimal, 2},
	             {"l_extendedprice", ColType::Decimal, 2}};
	s.filters = {{kShipdate, FilterOp::Between, p.start, p.end_excl - 1},
	             {kDiscount, FilterOp::Between, p.disc_lo, p.disc_hi},
	             {kQuantity, FilterOp::Between, std::numeric_limits<int64_t>::min(), p.qty_excl - 1}};
	return s;
}

struct Agg {
	__int128 sum = 0;
	bool any = false;
};

//! Batches arrive pre-filtered by the scan.  Per batch, |price*discount| <
//! 2^31 * 100 fits easily, and 2048 such products fit in int64 lanes, so the
//! products are summed in 64-bit SIMD lanes and spilled to __int128 per batch.
inline void Accumulate(const Batch &b, Agg &agg) {
	const int64_t *ACCEL_RESTRICT price = b.I64(kExtendedprice);
	const int64_t *ACCEL_RESTRICT disc = b.I64(kDiscount);
	const std::size_t n = b.count;
	bool small = true;
	for (std::size_t i = 0; i < n; i++) {
		small &= (price[i] < (int64_t(1) << 40)) & (price[i] > -(int64_t(1) << 40)) & (disc[i] < (1 << 20)) &
		         (disc[i] > -(1 << 20));
	}
	if (!small) {
		for (std::size_t i = 0; i < n; i++) {
			agg.sum += static_cast<__int128>(price[i]) * disc[i];
		}
		agg.any |= n != 0;
		return;
	}
	int64_t total = 0;
	std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512) && defined(__AVX512DQ__)
	__m512i acc = _mm512_setzero_si512();
	for (; i + 8 <= n; i += 8) {
		acc = _mm512_add_epi64(acc, _mm512_mullo_epi64(_mm512_loadu_si512(price + i), _mm512_loadu_si512(disc + i)));
	}
	total = _mm512_reduce_add_epi64(acc);
#elif defined(ACCEL_SIMD_AVX2)
	// |values| < 2^40 and < 2^20: _mm256_mul_epi32 is not enough, use scalar pairs.
#endif
	for (; i < n; i++) {
		total += price[i] * disc[i];
	}
	agg.sum += total;
	agg.any |= n != 0;
}

inline void Emit(const Agg &agg, ResultWriter &out) {
	if (agg.any) {
		out.Decimal(agg.sum, 4);
	} else {
		out.Null();
	}
	out.EndRow();
}

} // namespace accelerator_codegen::pg_base::q6

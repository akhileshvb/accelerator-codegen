// Shared Q1 aggregation core (single-threaded kernel and parallel workers).
//
// Stateless: an Aggregator lives for one query call (one kernel instance / one
// worker) and owns only its scratch buffers and group states.  Fast path: the
// group id is a (return-flag, line-status) pair of single characters mapped to
// a small slot (<= 8 distinct values each, i.e. <= 64 slots); numeric inputs are
// range-checked so that per-block int64 partial sums cannot overflow, and the
// per-slot sums are done with AVX-512 masked adds.  Anything else (NULLs,
// multi-byte / unexpected group values, out-of-range decimals, > 8 distinct
// values) goes through an exact scalar path with a generic string-keyed map.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "prims/simd.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tpch/q1/duckdb/base/query.hpp"

#if defined(ACCEL_SIMD_AVX512) && defined(__AVX512DQ__) && defined(__AVX512BW__)
#define ACCEL_Q1_AVX512 1
#endif

namespace accelerator_codegen {
namespace q1 {

#if defined(__SIZEOF_INT128__)
using Wide = __int128_t;
inline duckdb::hugeint_t ToHugeint(Wide v) {
	return duckdb::hugeint_t(static_cast<int64_t>(v >> 64), static_cast<uint64_t>(v));
}
inline void WideAdd(Wide &acc, int64_t v) {
	acc += static_cast<Wide>(v);
}
inline void WideAddWide(Wide &acc, const Wide &v) {
	acc += v;
}
inline Wide WideProduct3(int64_t a, int64_t b, int64_t c) {
	return static_cast<Wide>(a) * static_cast<Wide>(b) * static_cast<Wide>(c);
}
#else
using Wide = duckdb::hugeint_t;
inline duckdb::hugeint_t ToHugeint(Wide v) {
	return v;
}
inline void WideAdd(Wide &acc, int64_t v) {
	if (!duckdb::Hugeint::TryAddInPlace(acc, duckdb::Hugeint::Convert(v))) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q1 aggregate");
	}
}
inline void WideAddWide(Wide &acc, const Wide &v) {
	if (!duckdb::Hugeint::TryAddInPlace(acc, v)) {
		throw duckdb::OutOfRangeException("Overflow while merging Q1 aggregates");
	}
}
inline Wide WideProduct3(int64_t a, int64_t b, int64_t c) {
	duckdb::hugeint_t result = duckdb::Hugeint::Convert(a);
	if (!duckdb::Hugeint::TryMultiply(result, duckdb::Hugeint::Convert(b), result) ||
	    !duckdb::Hugeint::TryMultiply(result, duckdb::Hugeint::Convert(c), result)) {
		throw duckdb::OutOfRangeException("Overflow while computing Q1 charge");
	}
	return result;
}
#endif

struct Group {
	Wide sum_qty {};
	Wide sum_base_price {};
	Wide sum_disc_price {};
	Wide sum_charge {};
	Wide sum_discount {};
	uint64_t count = 0;

	void Merge(const Group &o) {
		WideAddWide(sum_qty, o.sum_qty);
		WideAddWide(sum_base_price, o.sum_base_price);
		WideAddWide(sum_disc_price, o.sum_disc_price);
		WideAddWide(sum_charge, o.sum_charge);
		WideAddWide(sum_discount, o.sum_discount);
		count += o.count;
	}
};

//! Group key; NULLs sort last (matches ORDER BY l_returnflag, l_linestatus), strings compare bytewise.
struct GroupKey {
	bool rf_null = false;
	bool ls_null = false;
	std::string rf;
	std::string ls;

	bool operator<(const GroupKey &o) const {
		if (rf_null != o.rf_null) {
			return !rf_null;
		}
		if (rf != o.rf) {
			return rf < o.rf;
		}
		if (ls_null != o.ls_null) {
			return !ls_null;
		}
		return ls < o.ls;
	}
};

using GroupMap = std::map<GroupKey, Group>;

struct Columns {
	duckdb::Vector *quantity;
	duckdb::Vector *extendedprice;
	duckdb::Vector *discount;
	duckdb::Vector *tax;
	duckdb::Vector *returnflag;
	duckdb::Vector *linestatus;
	//! Optional predicate l_shipdate <= cutoff evaluated by the aggregator (nullptr: rows are pre-filtered).
	duckdb::Vector *shipdate = nullptr;
	int32_t cutoff = 0;
};

class Aggregator {
public:
	static constexpr size_t kBlock = 2048;
	static constexpr int kMaxDistinct = 8;
	static constexpr uint8_t kNoSlot = 0xFF;

	Aggregator()
	    : idx_buf_(8 * kBlock + simd::kSelSlack), num_buf_(4 * kBlock + 16), code_buf_(3 * kBlock + 64) {
		std::memset(rf_map_, -1, sizeof(rf_map_));
		std::memset(ls_map_, -1, sizeof(ls_map_));
	}

	//! Aggregate rows rsel[0..n) (or 0..n-1 when rsel == nullptr) of vectors holding scan_count rows.
	void Consume(const Columns &cols, duckdb::idx_t scan_count, const uint32_t *rsel, duckdb::idx_t n) {
		if (n == 0) {
			return;
		}
		if (n > kBlock) {
			if (rsel) {
				for (duckdb::idx_t off = 0; off < n; off += kBlock) {
					Consume(cols, scan_count, rsel + off, std::min<duckdb::idx_t>(kBlock, n - off));
				}
				return;
			}
			ConsumeGeneric(cols, scan_count, rsel, n);
			return;
		}
		if (!ConsumeFast(cols, scan_count, rsel, n)) {
			ConsumeGeneric(cols, scan_count, rsel, n);
		}
	}

	//! Add this aggregator's groups into dst.
	void MergeInto(GroupMap &dst) const {
		for (int slot = 0; slot < 64; slot++) {
			const auto &g = fast_[slot];
			if (g.count == 0) {
				continue;
			}
			GroupKey key;
			key.rf.assign(1, static_cast<char>(rf_chars_[slot >> 3]));
			key.ls.assign(1, static_cast<char>(ls_chars_[slot & 7]));
			auto it = dst.find(key);
			if (it == dst.end()) {
				dst.emplace(std::move(key), g);
			} else {
				it->second.Merge(g);
			}
		}
		for (const auto &entry : generic_) {
			auto it = dst.find(entry.first);
			if (it == dst.end()) {
				dst.emplace(entry.first, entry.second);
			} else {
				it->second.Merge(entry.second);
			}
		}
	}

private:
	// ------------------------------------------------------------------ fast path
	static constexpr int64_t kMaxMag = int64_t(1) << 24;    // |quantity|, |extendedprice| bound
	static constexpr int64_t kMaxSmall = 255;                // |discount|, |tax| bound

	//! A column as (data, validity, optional dictionary/selection indexes); flat vectors avoid ToUnifiedFormat.
	struct View {
		duckdb::UnifiedVectorFormat fmt;
		const uint8_t *data = nullptr;
		const duckdb::ValidityMask *validity = nullptr;
		const uint32_t *sel = nullptr;

		void Init(duckdb::Vector &vec, duckdb::idx_t scan_count) {
			if (vec.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
				data = reinterpret_cast<const uint8_t *>(duckdb::FlatVector::GetData(vec));
				validity = &duckdb::FlatVector::Validity(vec);
				sel = nullptr;
				return;
			}
			if (vec.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
				// Dictionary over a flat child: borrow its buffers directly (no UnifiedVectorFormat refcounts).
				auto &child = duckdb::DictionaryVector::Child(vec);
				const auto *dict_sel = duckdb::DictionaryVector::SelVector(vec).data();
				if (dict_sel && child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
					data = reinterpret_cast<const uint8_t *>(duckdb::FlatVector::GetData(child));
					validity = &duckdb::FlatVector::Validity(child);
					sel = dict_sel;
					return;
				}
			}
			vec.ToUnifiedFormat(scan_count, fmt);
			data = fmt.data;
			validity = &fmt.validity;
			sel = fmt.sel ? fmt.sel->data() : nullptr;
		}
	};

	bool ConsumeFast(const Columns &cols, duckdb::idx_t scan_count, const uint32_t *rsel, duckdb::idx_t n) {
		duckdb::Vector *vecs[7] = {cols.quantity,   cols.extendedprice, cols.discount,  cols.tax,
		                           cols.returnflag, cols.linestatus,    cols.shipdate};
		const int ncols = cols.shipdate ? 7 : 6;
		View view[7];
		for (int c = 0; c < ncols; c++) {
			view[c].Init(*vecs[c], scan_count);
			// Numeric/date columns must be fully valid; the string columns are validity-checked per referenced
			// entry in ResolveCodes (dictionary entry 0 is the NULL slot, so the mask is rarely AllValid).
			if ((c < 4 || c == 6) && !view[c].validity->AllValid()) {
				return false;
			}
		}
		// Effective per-column row indexes (nullptr == identity 0..n-1).
		const uint32_t *idx[7];
		const uint32_t *last_cs = nullptr;
		const uint32_t *last_res = nullptr;
		for (int c = 0; c < ncols; c++) {
			const uint32_t *cs = view[c].sel;
			if (!cs) {
				idx[c] = rsel;
			} else if (!rsel) {
				idx[c] = cs;
			} else if (cs == last_cs) {
				idx[c] = last_res;
			} else {
				uint32_t *out = idx_buf_.data() + static_cast<size_t>(c) * kBlock;
				for (duckdb::idx_t i = 0; i < n; i++) {
					out[i] = cs[rsel[i]];
				}
				idx[c] = out;
				last_cs = cs;
				last_res = out;
			}
		}
		// Numeric columns -> contiguous int64.
		const int64_t *num[4];
		for (int c = 0; c < 4; c++) {
			const auto *data = reinterpret_cast<const int64_t *>(view[c].data);
			if (!idx[c]) {
				num[c] = data;
			} else {
				int64_t *out = num_buf_.data() + static_cast<size_t>(c) * kBlock;
				Gather(data, idx[c], n, out);
				num[c] = out;
			}
		}
		if (!InRange(num, n)) {
			return false;
		}
		// Group codes.
		uint8_t *rfc = code_buf_.data();
		uint8_t *lsc = rfc + kBlock;
		uint8_t *slot = lsc + kBlock;
		if (!ResolveCodes(reinterpret_cast<const duckdb::string_t *>(view[4].data), *view[4].validity, idx[4], n,
		                  rf_map_, rf_chars_, n_rf_, rfc) ||
		    !ResolveCodes(reinterpret_cast<const duckdb::string_t *>(view[5].data), *view[5].validity, idx[5], n,
		                  ls_map_, ls_chars_, n_ls_, lsc)) {
			return false;
		}
		for (duckdb::idx_t i = 0; i < n; i++) {
			slot[i] = static_cast<uint8_t>((rfc[i] << 3) | lsc[i]);
		}
		size_t excluded = 0;
		if (cols.shipdate) {
			const auto *dates = reinterpret_cast<const int32_t *>(view[6].data);
			if (idx[6]) {
				auto *out = reinterpret_cast<int32_t *>(idx_buf_.data() + 7 * kBlock);
				for (duckdb::idx_t i = 0; i < n; i++) {
					out[i] = dates[idx[6][i]];
				}
				dates = out;
			}
			excluded = MaskByDate(dates, n, cols.cutoff, slot);
		}
		AggregateBlock(slot, num[0], num[1], num[2], num[3], n, n - excluded);
		return true;
	}

	//! slot[i] = kNoSlot where dates[i] > cutoff; returns the number of masked rows.
	static size_t MaskByDate(const int32_t *ACCEL_RESTRICT dates, size_t n, int32_t cutoff,
	                         uint8_t *ACCEL_RESTRICT slot) {
		size_t excluded = 0;
		size_t i = 0;
#if defined(ACCEL_Q1_AVX512)
		const __m512i vc = _mm512_set1_epi32(cutoff);
		const __m128i none = _mm_set1_epi8(static_cast<char>(kNoSlot));
		for (; i + 16 <= n; i += 16) {
			const __mmask16 m = _mm512_cmpgt_epi32_mask(_mm512_loadu_si512(dates + i), vc);
			const __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i *>(slot + i));
			_mm_storeu_si128(reinterpret_cast<__m128i *>(slot + i), _mm_mask_mov_epi8(s, m, none));
			excluded += static_cast<size_t>(__builtin_popcount(m));
		}
#endif
		for (; i < n; i++) {
			const bool out = dates[i] > cutoff;
			slot[i] = out ? kNoSlot : slot[i];
			excluded += out;
		}
		return excluded;
	}

	static void Gather(const int64_t *ACCEL_RESTRICT data, const uint32_t *ACCEL_RESTRICT idx, size_t n,
	                   int64_t *ACCEL_RESTRICT out) {
		size_t i = 0;
#if defined(ACCEL_Q1_AVX512)
		for (; i + 8 <= n; i += 8) {
			const __m256i vi = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(idx + i));
			_mm512_storeu_si512(out + i, _mm512_i32gather_epi64(vi, data, 8));
		}
#endif
		for (; i < n; i++) {
			out[i] = data[idx[i]];
		}
	}

	//! quantity/price within +-2^24 and discount/tax within +-255 => per-block int64 sums cannot overflow.
	static bool InRange(const int64_t *const *num, size_t n) {
		size_t i = 0;
		uint64_t bad = 0;
#if defined(ACCEL_Q1_AVX512)
		const __m512i bias_big = _mm512_set1_epi64(kMaxMag);
		const __m512i bias_small = _mm512_set1_epi64(kMaxSmall);
		__m512i acc_big = _mm512_setzero_si512();
		__m512i acc_small = _mm512_setzero_si512();
		for (; i + 8 <= n; i += 8) {
			acc_big = _mm512_or_si512(
			    acc_big, _mm512_add_epi64(_mm512_loadu_si512(num[0] + i), bias_big));
			acc_big = _mm512_or_si512(
			    acc_big, _mm512_add_epi64(_mm512_loadu_si512(num[1] + i), bias_big));
			acc_small = _mm512_or_si512(
			    acc_small, _mm512_add_epi64(_mm512_loadu_si512(num[2] + i), bias_small));
			acc_small = _mm512_or_si512(
			    acc_small, _mm512_add_epi64(_mm512_loadu_si512(num[3] + i), bias_small));
		}
		bad |= static_cast<uint64_t>(_mm512_reduce_or_epi64(acc_big)) >> 25;
		bad |= static_cast<uint64_t>(_mm512_reduce_or_epi64(acc_small)) >> 9;
#endif
		for (; i < n; i++) {
			bad |= (static_cast<uint64_t>(num[0][i] + kMaxMag) >> 25) | (static_cast<uint64_t>(num[1][i] + kMaxMag) >> 25) |
			       (static_cast<uint64_t>(num[2][i] + kMaxSmall) >> 9) |
			       (static_cast<uint64_t>(num[3][i] + kMaxSmall) >> 9);
		}
		return bad == 0;
	}

	//! Map single-character strings to small codes (<= kMaxDistinct per column). false => needs generic path.
	static bool ResolveCodes(const duckdb::string_t *ACCEL_RESTRICT data, const duckdb::ValidityMask &validity,
	                         const uint32_t *idx, size_t n, int8_t *map, uint8_t *chars, int &n_chars,
	                         uint8_t *out) {
		const bool all_valid = validity.AllValid();
		const auto code_of = [&](const duckdb::string_t &s) -> int {
			if (s.GetSize() != 1) {
				return -1;
			}
			const uint8_t ch = static_cast<uint8_t>(s.GetData()[0]);
			int8_t code = map[ch];
			if (code < 0) {
				if (n_chars >= kMaxDistinct) {
					return -1;
				}
				code = static_cast<int8_t>(n_chars);
				chars[n_chars++] = ch;
				map[ch] = code;
			}
			return code;
		};
		if (!idx) {
			// Flat vector: one string_t per row.
			for (size_t i = 0; i < n; i++) {
				if (!all_valid && !validity.RowIsValid(i)) {
					return false;
				}
				const int c = code_of(data[i]);
				if (c < 0) {
					return false;
				}
				out[i] = static_cast<uint8_t>(c);
			}
			return true;
		}
		uint32_t max_idx = 0;
		for (size_t i = 0; i < n; i++) {
			max_idx = std::max(max_idx, idx[i]);
		}
		if (max_idx < 16) {
			alignas(64) uint32_t tab[16] = {};
			for (uint32_t e = 0; e <= max_idx; e++) {
				if (!all_valid && !validity.RowIsValid(e)) {
					// An invalid dictionary entry is fine as long as no row references it.
					uint32_t hit = 0;
					for (size_t i = 0; i < n; i++) {
						hit |= static_cast<uint32_t>(idx[i] == e);
					}
					if (hit) {
						return false;
					}
					continue;
				}
				const int c = code_of(data[e]);
				if (c < 0) {
					return false;
				}
				tab[e] = static_cast<uint32_t>(c);
			}
			size_t i = 0;
#if defined(ACCEL_Q1_AVX512)
			const __m512i vtab = _mm512_load_si512(tab);
			for (; i + 16 <= n; i += 16) {
				const __m512i v = _mm512_permutexvar_epi32(_mm512_loadu_si512(idx + i), vtab);
				_mm_storeu_si128(reinterpret_cast<__m128i *>(out + i), _mm512_cvtepi32_epi8(v));
			}
#endif
			for (; i < n; i++) {
				out[i] = static_cast<uint8_t>(tab[idx[i]]);
			}
			return true;
		}
		if (max_idx < 4096) {
			uint8_t lazy[4096];
			std::memset(lazy, 0xFF, max_idx + 1);
			for (size_t i = 0; i < n; i++) {
				uint8_t c = lazy[idx[i]];
				if (c == 0xFF) {
					if (!all_valid && !validity.RowIsValid(idx[i])) {
						return false;
					}
					const int r = code_of(data[idx[i]]);
					if (r < 0) {
						return false;
					}
					c = static_cast<uint8_t>(r);
					lazy[idx[i]] = c;
				}
				out[i] = c;
			}
			return true;
		}
		for (size_t i = 0; i < n; i++) {
			if (!all_valid && !validity.RowIsValid(idx[i])) {
				return false;
			}
			const int c = code_of(data[idx[i]]);
			if (c < 0) {
				return false;
			}
			out[i] = static_cast<uint8_t>(c);
		}
		return true;
	}

#if defined(ACCEL_Q1_AVX512)
	//! Sums of cands[0..G) over rows of the block.  out[g] = {qty, price, discount, disc_price, charge}.
	template <int G>
	static void RunCands(const uint8_t *ACCEL_RESTRICT slot, const int64_t *ACCEL_RESTRICT q,
	                     const int64_t *ACCEL_RESTRICT p, const int64_t *ACCEL_RESTRICT d,
	                     const int64_t *ACCEL_RESTRICT t, size_t n, const uint8_t *cands, int64_t (*out)[5],
	                     uint64_t *cnt) {
		__m512i aq[G], ap[G], ad[G], adp[G], ach[G], cv[G];
		uint64_t c[G];
		for (int g = 0; g < G; g++) {
			aq[g] = ap[g] = ad[g] = adp[g] = ach[g] = _mm512_setzero_si512();
			cv[g] = _mm512_set1_epi64(cands[g]);
			c[g] = 0;
		}
		const __m512i k100 = _mm512_set1_epi64(100);
		size_t i = 0;
		for (; i + 8 <= n; i += 8) {
			const __m512i vq = _mm512_loadu_si512(q + i);
			const __m512i vp = _mm512_loadu_si512(p + i);
			const __m512i vd = _mm512_loadu_si512(d + i);
			const __m512i vt = _mm512_loadu_si512(t + i);
			const __m512i omd = _mm512_sub_epi64(k100, vd);
			const __m512i opt = _mm512_add_epi64(k100, vt);
			// All factors fit in 32 bits (range-checked): 32x32->64 multiplies.
			const __m512i dp = _mm512_mul_epi32(vp, omd);
			const __m512i ch = _mm512_mul_epi32(vp, _mm512_mul_epi32(omd, opt));
			const __m512i s = _mm512_cvtepu8_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(slot + i)));
#pragma GCC unroll 6
			for (int g = 0; g < G; g++) {
				const __mmask8 m = _mm512_cmpeq_epi64_mask(s, cv[g]);
				aq[g] = _mm512_mask_add_epi64(aq[g], m, aq[g], vq);
				ap[g] = _mm512_mask_add_epi64(ap[g], m, ap[g], vp);
				ad[g] = _mm512_mask_add_epi64(ad[g], m, ad[g], vd);
				adp[g] = _mm512_mask_add_epi64(adp[g], m, adp[g], dp);
				ach[g] = _mm512_mask_add_epi64(ach[g], m, ach[g], ch);
				c[g] += static_cast<uint64_t>(__builtin_popcount(m));
			}
		}
		if (i < n) {
			const __mmask8 k = static_cast<__mmask8>((1u << (n - i)) - 1);
			const __m512i vq = _mm512_maskz_loadu_epi64(k, q + i);
			const __m512i vp = _mm512_maskz_loadu_epi64(k, p + i);
			const __m512i vd = _mm512_maskz_loadu_epi64(k, d + i);
			const __m512i vt = _mm512_maskz_loadu_epi64(k, t + i);
			const __m512i omd = _mm512_sub_epi64(k100, vd);
			const __m512i opt = _mm512_add_epi64(k100, vt);
			const __m512i dp = _mm512_mul_epi32(vp, omd);
			const __m512i ch = _mm512_mul_epi32(vp, _mm512_mul_epi32(omd, opt));
			const __m512i s = _mm512_cvtepu8_epi64(_mm_maskz_loadu_epi8(k, slot + i));
			for (int g = 0; g < G; g++) {
				const __mmask8 m = _mm512_cmpeq_epi64_mask(s, cv[g]) & k;
				aq[g] = _mm512_mask_add_epi64(aq[g], m, aq[g], vq);
				ap[g] = _mm512_mask_add_epi64(ap[g], m, ap[g], vp);
				ad[g] = _mm512_mask_add_epi64(ad[g], m, ad[g], vd);
				adp[g] = _mm512_mask_add_epi64(adp[g], m, adp[g], dp);
				ach[g] = _mm512_mask_add_epi64(ach[g], m, ach[g], ch);
				c[g] += static_cast<uint64_t>(__builtin_popcount(m));
			}
		}
		for (int g = 0; g < G; g++) {
			out[g][0] = _mm512_reduce_add_epi64(aq[g]);
			out[g][1] = _mm512_reduce_add_epi64(ap[g]);
			out[g][2] = _mm512_reduce_add_epi64(ad[g]);
			out[g][3] = _mm512_reduce_add_epi64(adp[g]);
			out[g][4] = _mm512_reduce_add_epi64(ach[g]);
			cnt[g] = c[g];
		}
	}

	static void RunCandsDispatch(int g, const uint8_t *slot, const int64_t *q, const int64_t *p, const int64_t *d,
	                             const int64_t *t, size_t n, const uint8_t *cands, int64_t (*out)[5], uint64_t *cnt) {
		switch (g) {
		case 1: RunCands<1>(slot, q, p, d, t, n, cands, out, cnt); break;
		case 2: RunCands<2>(slot, q, p, d, t, n, cands, out, cnt); break;
		case 3: RunCands<3>(slot, q, p, d, t, n, cands, out, cnt); break;
		case 4: RunCands<4>(slot, q, p, d, t, n, cands, out, cnt); break;
		case 5: RunCands<5>(slot, q, p, d, t, n, cands, out, cnt); break;
		default: RunCands<6>(slot, q, p, d, t, n, cands, out, cnt); break;
		}
	}

	//! Aggregate the rows whose slot is in cands[0..nc) into fast_; returns the number of rows covered.
	uint64_t RunSlots(const uint8_t *slot, const int64_t *q, const int64_t *p, const int64_t *d, const int64_t *t,
	                  size_t n, const uint8_t *cands, int nc) {
		uint64_t covered = 0;
		for (int base = 0; base < nc; base += 6) {
			const int g = std::min(6, nc - base);
			int64_t out[6][5];
			uint64_t cnt[6];
			RunCandsDispatch(g, slot, q, p, d, t, n, cands + base, out, cnt);
			for (int j = 0; j < g; j++) {
				if (cnt[j] == 0) {
					continue;
				}
				covered += cnt[j];
				const int s = cands[base + j];
				auto &grp = fast_[s];
				WideAdd(grp.sum_qty, out[j][0]);
				WideAdd(grp.sum_base_price, out[j][1]);
				WideAdd(grp.sum_discount, out[j][2]);
				WideAdd(grp.sum_disc_price, out[j][3]);
				WideAdd(grp.sum_charge, out[j][4]);
				grp.count += cnt[j];
				seen_ |= uint64_t(1) << s;
			}
		}
		return covered;
	}

	void AggregateBlock(const uint8_t *slot, const int64_t *q, const int64_t *p, const int64_t *d, const int64_t *t,
	                    size_t n, size_t expected) {
		uint8_t cands[64];
		int nc = 0;
		for (uint64_t m = seen_; m; m &= m - 1) {
			cands[nc++] = static_cast<uint8_t>(__builtin_ctzll(m));
		}
		uint64_t covered = nc ? RunSlots(slot, q, p, d, t, n, cands, nc) : 0;
		if (covered < expected) {
			nc = 0;
			for (int rf = 0; rf < n_rf_; rf++) {
				for (int ls = 0; ls < n_ls_; ls++) {
					const int s = rf * 8 + ls;
					if (!(seen_ & (uint64_t(1) << s))) {
						cands[nc++] = static_cast<uint8_t>(s);
					}
				}
			}
			covered += RunSlots(slot, q, p, d, t, n, cands, nc);
		}
		if (covered != expected) {
			throw duckdb::InternalException("Q1 aggregation lost rows");
		}
	}
#else
	void AggregateBlock(const uint8_t *slot, const int64_t *q, const int64_t *p, const int64_t *d, const int64_t *t,
	                    size_t n, size_t) {
		int64_t acc[64][5] = {};
		uint32_t cnt[64] = {};
		for (size_t i = 0; i < n; i++) {
			const int s = slot[i];
			if (s == kNoSlot) {
				continue;
			}
			const int64_t omd = 100 - d[i];
			const int64_t dp = p[i] * omd;
			acc[s][0] += q[i];
			acc[s][1] += p[i];
			acc[s][2] += d[i];
			acc[s][3] += dp;
			acc[s][4] += dp * (100 + t[i]);
			cnt[s]++;
		}
		for (int s = 0; s < 64; s++) {
			if (cnt[s] == 0) {
				continue;
			}
			auto &grp = fast_[s];
			WideAdd(grp.sum_qty, acc[s][0]);
			WideAdd(grp.sum_base_price, acc[s][1]);
			WideAdd(grp.sum_discount, acc[s][2]);
			WideAdd(grp.sum_disc_price, acc[s][3]);
			WideAdd(grp.sum_charge, acc[s][4]);
			grp.count += cnt[s];
		}
	}
#endif

	// --------------------------------------------------------------- generic path
	//! Exact, slow: any layout, NULL group keys, any string values, 128-bit arithmetic.  Rows with a NULL
	//! numeric input are skipped (the TPC-H columns are NOT NULL).
	void ConsumeGeneric(const Columns &cols, duckdb::idx_t scan_count, const uint32_t *rsel, duckdb::idx_t n) {
		duckdb::Vector *vecs[6] = {cols.quantity, cols.extendedprice, cols.discount,
		                           cols.tax,      cols.returnflag,    cols.linestatus};
		duckdb::UnifiedVectorFormat fmt[6];
		for (int c = 0; c < 6; c++) {
			vecs[c]->ToUnifiedFormat(scan_count, fmt[c]);
		}
		duckdb::UnifiedVectorFormat date_fmt;
		if (cols.shipdate) {
			cols.shipdate->ToUnifiedFormat(scan_count, date_fmt);
		}
		for (duckdb::idx_t i = 0; i < n; i++) {
			const duckdb::idx_t r = rsel ? rsel[i] : i;
			if (cols.shipdate) {
				const auto dix = date_fmt.sel->get_index(r);
				if (!date_fmt.validity.RowIsValid(dix) ||
				    reinterpret_cast<const int32_t *>(date_fmt.data)[dix] > cols.cutoff) {
					continue;
				}
			}
			duckdb::idx_t ix[6];
			for (int c = 0; c < 6; c++) {
				ix[c] = fmt[c].sel->get_index(r);
			}
			bool skip = false;
			for (int c = 0; c < 4; c++) {
				skip = skip || !fmt[c].validity.RowIsValid(ix[c]);
			}
			if (skip) {
				continue;
			}
			GroupKey key;
			if (fmt[4].validity.RowIsValid(ix[4])) {
				key.rf = reinterpret_cast<const duckdb::string_t *>(fmt[4].data)[ix[4]].GetString();
			} else {
				key.rf_null = true;
			}
			if (fmt[5].validity.RowIsValid(ix[5])) {
				key.ls = reinterpret_cast<const duckdb::string_t *>(fmt[5].data)[ix[5]].GetString();
			} else {
				key.ls_null = true;
			}
			const int64_t q = reinterpret_cast<const int64_t *>(fmt[0].data)[ix[0]];
			const int64_t p = reinterpret_cast<const int64_t *>(fmt[1].data)[ix[1]];
			const int64_t d = reinterpret_cast<const int64_t *>(fmt[2].data)[ix[2]];
			const int64_t t = reinterpret_cast<const int64_t *>(fmt[3].data)[ix[3]];
			auto &grp = generic_[std::move(key)];
			WideAdd(grp.sum_qty, q);
			WideAdd(grp.sum_base_price, p);
			WideAdd(grp.sum_discount, d);
			WideAddWide(grp.sum_disc_price, WideProduct3(p, 100 - d, 1));
			WideAddWide(grp.sum_charge, WideProduct3(p, 100 - d, 100 + t));
			grp.count++;
		}
	}

	std::vector<uint32_t> idx_buf_;
	std::vector<int64_t> num_buf_;
	std::vector<uint8_t> code_buf_;
	int8_t rf_map_[256];
	int8_t ls_map_[256];
	uint8_t rf_chars_[kMaxDistinct] = {};
	uint8_t ls_chars_[kMaxDistinct] = {};
	int n_rf_ = 0;
	int n_ls_ = 0;
	uint64_t seen_ = 0;
	std::array<Group, 64> fast_ {};
	GroupMap generic_;
};

// ------------------------------------------------------------------- output
struct OutputRow {
	GroupKey key;
	duckdb::hugeint_t sum_qty;
	duckdb::hugeint_t sum_base_price;
	duckdb::hugeint_t sum_disc_price;
	duckdb::hugeint_t sum_charge;
	double avg_qty;
	double avg_price;
	double avg_disc;
	int64_t count_order;
};

inline double ScaleToDouble(const Wide &value, uint8_t scale, uint64_t count) {
	double scaled = duckdb::Hugeint::Cast<double>(ToHugeint(value));
	while (scale > 0) {
		scaled /= 10.0;
		scale--;
	}
	return scaled / static_cast<double>(count);
}

//! Ordered output rows (map order == ORDER BY l_returnflag, l_linestatus).
inline std::vector<OutputRow> BuildRows(const GroupMap &groups) {
	std::vector<OutputRow> rows;
	rows.reserve(groups.size());
	for (const auto &entry : groups) {
		const auto &g = entry.second;
		if (g.count == 0) {
			continue;
		}
		rows.push_back(OutputRow {entry.first, ToHugeint(g.sum_qty), ToHugeint(g.sum_base_price),
		                          ToHugeint(g.sum_disc_price), ToHugeint(g.sum_charge),
		                          ScaleToDouble(g.sum_qty, 2, g.count), ScaleToDouble(g.sum_base_price, 2, g.count),
		                          ScaleToDouble(g.sum_discount, 2, g.count), static_cast<int64_t>(g.count)});
	}
	return rows;
}

//! Emit up to one vector of rows starting at offset; returns false when exhausted.
inline bool WriteRows(const std::vector<OutputRow> &rows, duckdb::idx_t &offset, duckdb::DataChunk &output) {
	if (offset >= rows.size()) {
		return false;
	}
	const auto count = std::min<duckdb::idx_t>(rows.size() - offset, STANDARD_VECTOR_SIZE);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto &r = rows[offset + row];
		output.SetValue(0, row, r.key.rf_null ? duckdb::Value(duckdb::LogicalType::VARCHAR) : duckdb::Value(r.key.rf));
		output.SetValue(1, row, r.key.ls_null ? duckdb::Value(duckdb::LogicalType::VARCHAR) : duckdb::Value(r.key.ls));
		output.SetValue(2, row, duckdb::Value::DECIMAL(r.sum_qty, 38, 2));
		output.SetValue(3, row, duckdb::Value::DECIMAL(r.sum_base_price, 38, 2));
		output.SetValue(4, row, duckdb::Value::DECIMAL(r.sum_disc_price, 38, 4));
		output.SetValue(5, row, duckdb::Value::DECIMAL(r.sum_charge, 38, 6));
		output.SetValue(6, row, duckdb::Value::DOUBLE(r.avg_qty));
		output.SetValue(7, row, duckdb::Value::DOUBLE(r.avg_price));
		output.SetValue(8, row, duckdb::Value::DOUBLE(r.avg_disc));
		output.SetValue(9, row, duckdb::Value::BIGINT(r.count_order));
	}
	output.SetCardinality(count);
	offset += count;
	return true;
}

} // namespace q1
} // namespace accelerator_codegen

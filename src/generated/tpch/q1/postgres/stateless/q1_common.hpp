// TPC-H Q1 (stateless PostgreSQL): parameters, scan spec and the grouped
// aggregation shared by the single-threaded and parallel kernels.
#pragma once

#include "accelerator_codegen/runtime/postgres_stateless/pg_stateless.hpp"
#include "accelerator_codegen/runtime/simd.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace accelerator_codegen::pg_stateless::q1 {

inline constexpr const char *kSqlArgs = "integer";
inline constexpr const char *kSqlReturns =
    "TABLE (l_returnflag text, l_linestatus text, sum_qty numeric, sum_base_price numeric, "
    "sum_disc_price numeric, sum_charge numeric, avg_qty double precision, avg_price double precision, "
    "avg_disc double precision, count_order bigint)";

struct Params {
	int32_t cutoff; // l_shipdate <= cutoff
};

inline Params Bind(const Args &args) {
	Params p;
	p.cutoff = DaysFromCivil(1998, 12, 1) - static_cast<int32_t>(args.Int(0));
	return p;
}

enum : int { kShipdate = 0, kReturnflag, kLinestatus, kQuantity, kPrice, kDiscount, kTax };

inline ScanSpec Spec(const Params &p) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_shipdate", ColType::Date},
	             {"l_returnflag", ColType::Text},
	             {"l_linestatus", ColType::Text},
	             {"l_quantity", ColType::Decimal, 2},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2},
	             {"l_tax", ColType::Decimal, 2}};
	s.filters = {{kShipdate, FilterOp::Between, std::numeric_limits<int64_t>::min(), p.cutoff}};
	return s;
}

struct Group {
	__int128 qty = 0, price = 0, disc_price = 0, charge = 0, disc = 0;
	uint64_t count = 0;
	void Merge(const Group &o) {
		qty += o.qty;
		price += o.price;
		disc_price += o.disc_price;
		charge += o.charge;
		disc += o.disc;
		count += o.count;
	}
};

using Key = std::pair<std::string, std::string>; // (returnflag, linestatus), bytewise order == collation "C"
using GroupMap = std::map<Key, Group>;

struct Aggregator {
	static constexpr int kMaxSlots = 8;
	GroupMap groups;

	// Exact scalar path for any batch (unexpected group values, wide numbers).
	void Generic(const Batch &b) {
		const StringRef *rf = b.Str(kReturnflag);
		const StringRef *ls = b.Str(kLinestatus);
		const int64_t *q = b.I64(kQuantity), *pr = b.I64(kPrice), *d = b.I64(kDiscount), *t = b.I64(kTax);
		for (std::size_t i = 0; i < b.count; i++) {
			Group &g = groups[Key(std::string(rf[i].View()), std::string(ls[i].View()))];
			const __int128 dp = static_cast<__int128>(pr[i]) * (100 - d[i]);
			g.qty += q[i];
			g.price += pr[i];
			g.disc_price += dp;
			g.charge += dp * (100 + t[i]);
			g.disc += d[i];
			g.count++;
		}
	}

	void Consume(const Batch &b) {
		const std::size_t n = b.count;
		if (n == 0) {
			return;
		}
		const StringRef *ACCEL_RESTRICT rf = b.Str(kReturnflag);
		const StringRef *ACCEL_RESTRICT ls = b.Str(kLinestatus);
		const int64_t *ACCEL_RESTRICT q = b.I64(kQuantity);
		const int64_t *ACCEL_RESTRICT pr = b.I64(kPrice);
		const int64_t *ACCEL_RESTRICT d = b.I64(kDiscount);
		const int64_t *ACCEL_RESTRICT t = b.I64(kTax);
		// Range checks: with |price| < 2^31, |q| < 2^31, |disc|,|tax| <= 1000 every int64 lane sum of
		// 2048 rows stays below 2^63 (charge <= 2^31 * 1100^2 * 2048 < 2^63).
		if (n > 2048) {
			Generic(b);
			return;
		}
		bool ok = true;
		uint32_t codes[2048];
		uint32_t bad_len = 0;
		for (std::size_t i = 0; i < n; i++) {
			bad_len |= (rf[i].len ^ 1u) | (ls[i].len ^ 1u);
		}
		if (bad_len) {
			Generic(b);
			return;
		}
		for (std::size_t i = 0; i < n; i++) {
			ok &= (pr[i] < (int64_t(1) << 31)) & (pr[i] > -(int64_t(1) << 31)) & (q[i] < (int64_t(1) << 31)) &
			      (q[i] > -(int64_t(1) << 31)) & (d[i] <= 1000) & (d[i] >= -1000) & (t[i] <= 1000) & (t[i] >= -1000);
			codes[i] = (static_cast<uint32_t>(static_cast<uint8_t>(rf[i].ptr[0])) << 8) |
			           static_cast<uint8_t>(ls[i].ptr[0]);
		}
		uint32_t distinct[kMaxSlots];
		int nd = 0;
		for (std::size_t i = 0; i < n && ok; i++) {
			const uint32_t c = codes[i];
			bool found = false;
			for (int k = 0; k < nd; k++) {
				found |= distinct[k] == c;
			}
			if (!found) {
				if (nd == kMaxSlots) {
					ok = false;
				} else {
					distinct[nd++] = c;
				}
			}
		}
		if (!ok) {
			Generic(b);
			return;
		}
		for (int k = 0; k < nd; k++) {
			const uint32_t c = distinct[k];
			int64_t s_q = 0, s_p = 0, s_dp = 0, s_ch = 0, s_d = 0, cnt = 0;
			std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512) && defined(__AVX512DQ__)
			__m512i aq = _mm512_setzero_si512(), ap = aq, adp = aq, ach = aq, ad = aq;
			const __m512i v100 = _mm512_set1_epi64(100);
			uint64_t cn = 0;
			for (; i + 8 <= n; i += 8) {
				const __m256i cv = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(codes + i));
				const __mmask8 m = _mm256_cmpeq_epi32_mask(cv, _mm256_set1_epi32(static_cast<int>(c)));
				cn += static_cast<uint64_t>(__builtin_popcount(m));
				const __m512i vq = _mm512_loadu_si512(q + i);
				const __m512i vp = _mm512_loadu_si512(pr + i);
				const __m512i vd = _mm512_loadu_si512(d + i);
				const __m512i vt = _mm512_loadu_si512(t + i);
				const __m512i dp = _mm512_mullo_epi64(vp, _mm512_sub_epi64(v100, vd));
				const __m512i ch = _mm512_mullo_epi64(dp, _mm512_add_epi64(v100, vt));
				aq = _mm512_mask_add_epi64(aq, m, aq, vq);
				ap = _mm512_mask_add_epi64(ap, m, ap, vp);
				adp = _mm512_mask_add_epi64(adp, m, adp, dp);
				ach = _mm512_mask_add_epi64(ach, m, ach, ch);
				ad = _mm512_mask_add_epi64(ad, m, ad, vd);
			}
			s_q = _mm512_reduce_add_epi64(aq);
			s_p = _mm512_reduce_add_epi64(ap);
			s_dp = _mm512_reduce_add_epi64(adp);
			s_ch = _mm512_reduce_add_epi64(ach);
			s_d = _mm512_reduce_add_epi64(ad);
			cnt = static_cast<int64_t>(cn);
#endif
			for (; i < n; i++) {
				const int64_t m = codes[i] == c;
				const int64_t dp = pr[i] * (100 - d[i]);
				s_q += m * q[i];
				s_p += m * pr[i];
				s_dp += m * dp;
				s_ch += m * dp * (100 + t[i]);
				s_d += m * d[i];
				cnt += m;
			}
			Group &g = groups[Key(std::string(1, static_cast<char>(c >> 8)), std::string(1, static_cast<char>(c & 255)))];
			g.qty += s_q;
			g.price += s_p;
			g.disc_price += s_dp;
			g.charge += s_ch;
			g.disc += s_d;
			g.count += static_cast<uint64_t>(cnt);
		}
	}

	void Merge(const GroupMap &o) {
		for (const auto &kv : o) {
			groups[kv.first].Merge(kv.second);
		}
	}
};

inline std::string Serialize(const GroupMap &groups) {
	ByteWriter w;
	w.Put<uint64_t>(groups.size());
	for (const auto &kv : groups) {
		w.PutString(kv.first.first);
		w.PutString(kv.first.second);
		w.Put(kv.second);
	}
	return w.Take();
}

inline void Deserialize(std::string_view data, GroupMap &into) {
	ByteReader r(data);
	const auto n = r.Get<uint64_t>();
	for (uint64_t i = 0; i < n; i++) {
		std::string a = r.GetString();
		std::string b = r.GetString();
		into[Key(std::move(a), std::move(b))].Merge(r.Get<Group>());
	}
}

inline void Emit(const GroupMap &groups, ResultWriter &out) {
	for (const auto &kv : groups) {
		const Group &g = kv.second;
		if (g.count == 0) {
			continue;
		}
		const double cnt = static_cast<double>(g.count);
		out.Text(kv.first.first);
		out.Text(kv.first.second);
		out.Decimal(g.qty, 2);
		out.Decimal(g.price, 2);
		out.Decimal(g.disc_price, 4);
		out.Decimal(g.charge, 6);
		out.Double(static_cast<double>(g.qty) / (100.0 * cnt));
		out.Double(static_cast<double>(g.price) / (100.0 * cnt));
		out.Double(static_cast<double>(g.disc) / (100.0 * cnt));
		out.Int(static_cast<int64_t>(g.count));
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q1

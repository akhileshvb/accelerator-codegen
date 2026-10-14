// TPC-H Q12 (stateless PostgreSQL): parameters, scan specs and kernels shared
// by the single-threaded and parallel versions.
//
// Plan: lineitem is scanned first with the receiptdate range and shipmode IN
// pushed into the scan; the two cross-column date predicates are evaluated
// with AVX-512 compares.  The few surviving (orderkey, shipmode) pairs are
// indexed by orderkey (open addressing, per-shipmode multiplicities); orders
// is then scanned once and probed, adding the multiplicities to the high/low
// priority counters of the shipmode.
#pragma once

#include "backends/postgres/api/stateless/pg_stateless.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace accelerator_codegen::pg_stateless::q12 {

inline constexpr const char *kSqlArgs = "text, text, date";
inline constexpr const char *kSqlReturns = "TABLE (l_shipmode text, high_line_count bigint, low_line_count bigint)";

struct Params {
	std::string mode[2]; // l_shipmode IN (mode0, mode1)
	int32_t start;       // receiptdate >= start
	int32_t end_excl;    // receiptdate < start + 1 year
	int modes;           // 1 if both arguments are equal, else 2
};

inline Params Bind(const Args &args) {
	Params p;
	p.mode[0] = args.Text(0);
	p.mode[1] = args.Text(1);
	p.modes = p.mode[0] == p.mode[1] ? 1 : 2;
	p.start = args.Date(2);
	int y;
	unsigned m, d;
	CivilFromDays(p.start, y, m, d);
	p.end_excl = DaysFromCivil(y + 1, m, (m == 2 && d == 29) ? 28 : d);
	return p;
}

enum : int { kLReceipt = 0, kLShipmode = 1, kLCommit = 2, kLShip = 3, kLOrderkey = 4 };
enum : int { kOOrderkey = 0, kOPriority = 1 };

inline ScanSpec LineitemSpec(const Params &p) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_receiptdate", ColType::Date},
	             {"l_shipmode", ColType::Text},
	             {"l_commitdate", ColType::Date},
	             {"l_shipdate", ColType::Date},
	             {"l_orderkey", ColType::Int64}};
	std::vector<std::string> in = {p.mode[0]};
	if (p.modes == 2) {
		in.push_back(p.mode[1]);
	}
	s.filters = {{kLReceipt, FilterOp::Between, p.start, int64_t(p.end_excl) - 1},
	             {kLShipmode, FilterOp::TextIn, 0, 0, in}};
	return s;
}

inline ScanSpec OrdersSpec() {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_orderkey", ColType::Int64}, {"o_orderpriority", ColType::Text}};
	return s;
}

//! Surviving lineitem rows: orderkey and shipmode id (0/1 = argument index).
struct Hits {
	std::vector<int64_t> keys;
	std::vector<uint8_t> mode;
};

inline void CollectLineitem(const Batch &b, const Params &p, Hits &hits) {
	const int32_t *ACCEL_RESTRICT receipt = b.I32(kLReceipt);
	const int32_t *ACCEL_RESTRICT commit = b.I32(kLCommit);
	const int32_t *ACCEL_RESTRICT ship = b.I32(kLShip);
	const int64_t *ACCEL_RESTRICT keys = b.I64(kLOrderkey);
	const StringRef *modes = b.Str(kLShipmode);
	const std::size_t n = b.count;
	auto take = [&](std::size_t i) {
		const std::string_view v = modes[i].View();
		const uint8_t id = v == p.mode[0] ? 0 : 1;
		hits.keys.push_back(keys[i]);
		hits.mode.push_back(id);
	};
	std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	for (; i + 16 <= n; i += 16) {
		const __m512i c = _mm512_loadu_si512(commit + i);
		const __m512i r = _mm512_loadu_si512(receipt + i);
		const __m512i s = _mm512_loadu_si512(ship + i);
		unsigned m = _mm512_cmplt_epi32_mask(c, r) & _mm512_cmplt_epi32_mask(s, c);
		while (m) {
			take(i + static_cast<std::size_t>(__builtin_ctz(m)));
			m &= m - 1;
		}
	}
#endif
	for (; i < n; i++) {
		if ((commit[i] < receipt[i]) & (ship[i] < commit[i])) {
			take(i);
		}
	}
}

//! orderkey -> multiplicity per shipmode.
class OrderProbe {
public:
	void Build(const Hits &hits) {
		std::size_t cap = 16;
		while (cap < hits.keys.size() * 2) {
			cap <<= 1;
		}
		slots_.assign(cap, Slot {0, {0, 0}, 0});
		shift_ = 64 - __builtin_ctzll(cap);
		mask_ = cap - 1;
		for (std::size_t i = 0; i < hits.keys.size(); i++) {
			Slot &s = Locate(hits.keys[i]);
			s.key = hits.keys[i];
			s.used = 1;
			s.cnt[hits.mode[i]]++;
		}
	}
	std::size_t Home(int64_t key) const {
		return (static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ull) >> shift_;
	}
	//! counts for key or nullptr
	const uint32_t *Find(int64_t key) const {
		std::size_t s = Home(key);
		for (;;) {
			const Slot &sl = slots_[s];
			if (!sl.used) {
				return nullptr;
			}
			if (sl.key == key) {
				return sl.cnt;
			}
			s = (s + 1) & mask_;
		}
	}
	const void *Addr(int64_t key) const {
		return &slots_[Home(key)];
	}

private:
	struct Slot {
		int64_t key;
		uint32_t cnt[2];
		uint32_t used;
	};
	Slot &Locate(int64_t key) {
		std::size_t s = Home(key);
		while (slots_[s].used && slots_[s].key != key) {
			s = (s + 1) & mask_;
		}
		return slots_[s];
	}
	std::vector<Slot> slots_;
	int shift_ = 60;
	std::size_t mask_ = 15;
};

//! totals[mode][0] = high_line_count, totals[mode][1] = low_line_count
using Totals = std::array<std::array<int64_t, 2>, 2>;

inline void ProbeOrders(const Batch &b, const OrderProbe &probe, Totals &t) {
	const int64_t *ACCEL_RESTRICT keys = b.I64(kOOrderkey);
	const StringRef *prio = b.Str(kOPriority);
	const std::size_t n = b.count;
	constexpr std::size_t kDist = 8;
	for (std::size_t i = 0; i < n; i++) {
		if (i + kDist < n) {
			ACCEL_PREFETCH(probe.Addr(keys[i + kDist]));
		}
		const uint32_t *c = probe.Find(keys[i]);
		if (c) {
			const std::string_view v = prio[i].View();
			const int low = !(v == "1-URGENT" || v == "2-HIGH");
			t[0][low] += c[0];
			t[1][low] += c[1];
		}
	}
}

inline void Emit(const Params &p, const Totals &t, ResultWriter &out) {
	int order[2] = {0, 1};
	if (p.modes == 2 && p.mode[1] < p.mode[0]) {
		order[0] = 1;
		order[1] = 0;
	}
	for (int k = 0; k < p.modes; k++) {
		const int m = order[k];
		if (t[m][0] + t[m][1] == 0) {
			continue;
		}
		out.Text(p.mode[m]);
		out.Int(t[m][0]);
		out.Int(t[m][1]);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q12

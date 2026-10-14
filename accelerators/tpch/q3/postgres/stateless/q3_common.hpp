// TPC-H Q3 (stateless PostgreSQL): parameters, scan specs and the join /
// aggregation building blocks shared by the single-threaded and parallel kernels.
//
// Plan: customer (segment filter pushed into the scan) -> custkey set; orders
// (orderdate filter pushed into the scan) probed against it -> qualifying
// orders indexed by orderkey (open addressing, dense index); lineitem
// (shipdate filter pushed into the scan) probed against the orders, with
// prefetching and a same-key shortcut (lineitems of an order are adjacent), adds
// extendedprice*(100-discount) (exact int64, scale 4) into a per-order slot.
// Final top-10 by revenue DESC, orderdate, orderkey.
#pragma once

#include "backends/postgres/api/stateless/pg_stateless.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace accelerator_codegen::pg_stateless::q3 {

inline constexpr const char *kSqlArgs = "text, date";
inline constexpr const char *kSqlReturns =
    "TABLE (l_orderkey bigint, revenue numeric, o_orderdate date, o_shippriority bigint)";

struct Params {
	std::string segment;
	int32_t cutoff;
};

inline Params Bind(const Args &args) {
	return Params {args.Text(0), args.Date(1)};
}

enum : int { kCMktsegment = 0, kCCustkey = 1 };
enum : int { kODate = 0, kOOrderkey = 1, kOCustkey = 2, kOShippriority = 3 };
enum : int { kLShipdate = 0, kLOrderkey = 1, kLPrice = 2, kLDiscount = 3 };

inline constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
inline constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

inline ScanSpec CustomerSpec(const Params &p) {
	ScanSpec s;
	s.table = "customer";
	s.columns = {{"c_mktsegment", ColType::Text}, {"c_custkey", ColType::Int64}};
	s.filters = {{kCMktsegment, FilterOp::TextEq, 0, 0, {p.segment}}};
	return s;
}

inline ScanSpec OrdersSpec(const Params &p) {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_orderdate", ColType::Date},
	             {"o_orderkey", ColType::Int64},
	             {"o_custkey", ColType::Int64},
	             {"o_shippriority", ColType::Int32}};
	s.filters = {{kODate, FilterOp::Between, kMin, int64_t(p.cutoff) - 1}};
	return s;
}

inline ScanSpec LineitemSpec(const Params &p) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_shipdate", ColType::Date},
	             {"l_orderkey", ColType::Int64},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2}};
	s.filters = {{kLShipdate, FilterOp::Between, int64_t(p.cutoff) + 1, kMax}};
	return s;
}

//! key -> dense index (insertion order), open addressing with linear probing.
class KeyIndex {
public:
	static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
	void Reserve(std::size_t n) {
		std::size_t cap = 16;
		while (cap < n * 2) {
			cap <<= 1;
		}
		slots_.assign(cap, Slot {0, kEmpty, 0});
		shift_ = 64 - __builtin_ctzll(cap);
		mask_ = cap - 1;
		count_ = 0;
	}
	uint32_t Insert(int64_t key) {
		std::size_t s = Home(key);
		while (slots_[s].idx != kEmpty) {
			s = (s + 1) & mask_;
		}
		slots_[s] = Slot {key, count_, 0};
		return count_++;
	}
	std::size_t Home(int64_t key) const {
		return (static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ull) >> shift_;
	}
	const void *Addr(int64_t key) const {
		return &slots_[Home(key)];
	}
	uint32_t Find(int64_t key) const {
		std::size_t s = Home(key);
		for (;;) {
			const Slot &sl = slots_[s];
			if (sl.idx == kEmpty) {
				return kEmpty;
			}
			if (sl.key == key) {
				return sl.idx;
			}
			s = (s + 1) & mask_;
		}
	}
	uint32_t Size() const {
		return count_;
	}

private:
	struct Slot {
		int64_t key;
		uint32_t idx;
		uint32_t pad;
	};
	std::vector<Slot> slots_;
	int shift_ = 60;
	std::size_t mask_ = 15;
	uint32_t count_ = 0;
};

//! Qualifying orders in build order.
struct OrderList {
	std::vector<int64_t> keys;
	std::vector<int32_t> dates;
	std::vector<int32_t> prio;
};

inline void CollectCustomers(const Batch &b, std::vector<int64_t> &custkeys) {
	const int64_t *k = b.I64(kCCustkey);
	custkeys.insert(custkeys.end(), k, k + b.count);
}

inline KeyIndex BuildCustomerSet(const std::vector<int64_t> &custkeys) {
	KeyIndex set;
	set.Reserve(custkeys.size());
	for (int64_t k : custkeys) {
		if (set.Find(k) == KeyIndex::kEmpty) {
			set.Insert(k);
		}
	}
	return set;
}

inline void CollectOrders(const Batch &b, const KeyIndex &customers, OrderList &out) {
	const int32_t *ACCEL_RESTRICT date = b.I32(kODate);
	const int64_t *ACCEL_RESTRICT okey = b.I64(kOOrderkey);
	const int64_t *ACCEL_RESTRICT ckey = b.I64(kOCustkey);
	const int32_t *ACCEL_RESTRICT prio = b.I32(kOShippriority);
	for (std::size_t i = 0; i < b.count; i++) {
		if (customers.Find(ckey[i]) != KeyIndex::kEmpty) {
			out.keys.push_back(okey[i]);
			out.dates.push_back(date[i]);
			out.prio.push_back(prio[i]);
		}
	}
}

//! Per-order revenue accumulators indexed like the order list.
struct Revenue {
	std::vector<int64_t> sum;
	std::vector<uint8_t> hit;
	void Init(std::size_t n) {
		sum.assign(n, 0);
		hit.assign(n, 0);
	}
};

inline void ProbeLineitem(const Batch &b, const KeyIndex &orders, Revenue &rev) {
	const int64_t *ACCEL_RESTRICT key = b.I64(kLOrderkey);
	const int64_t *ACCEL_RESTRICT price = b.I64(kLPrice);
	const int64_t *ACCEL_RESTRICT disc = b.I64(kLDiscount);
	const std::size_t n = b.count;
	if (n == 0) {
		return;
	}
	constexpr std::size_t kDist = 16;
	for (std::size_t i = 0; i < std::min(n, kDist); i++) {
		ACCEL_PREFETCH(orders.Addr(key[i]));
	}
	int64_t last_key = key[0] - 1;
	uint32_t last_idx = KeyIndex::kEmpty;
	for (std::size_t i = 0; i < n; i++) {
		if (i + kDist < n) {
			ACCEL_PREFETCH(orders.Addr(key[i + kDist]));
		}
		if (key[i] != last_key) {
			last_key = key[i];
			last_idx = orders.Find(last_key);
		}
		if (last_idx != KeyIndex::kEmpty) {
			rev.sum[last_idx] += price[i] * (100 - disc[i]);
			rev.hit[last_idx] = 1;
		}
	}
}

inline void Emit(const OrderList &orders, const Revenue &rev, ResultWriter &out) {
	std::vector<uint32_t> cand;
	for (std::size_t i = 0; i < orders.keys.size(); i++) {
		if (rev.hit[i]) {
			cand.push_back(static_cast<uint32_t>(i));
		}
	}
	auto less = [&](uint32_t a, uint32_t b) {
		if (rev.sum[a] != rev.sum[b]) {
			return rev.sum[a] > rev.sum[b];
		}
		if (orders.dates[a] != orders.dates[b]) {
			return orders.dates[a] < orders.dates[b];
		}
		return orders.keys[a] < orders.keys[b];
	};
	const std::size_t top = std::min<std::size_t>(10, cand.size());
	std::partial_sort(cand.begin(), cand.begin() + static_cast<std::ptrdiff_t>(top), cand.end(), less);
	for (std::size_t i = 0; i < top; i++) {
		const uint32_t o = cand[i];
		out.Int(orders.keys[o]);
		out.Decimal(rev.sum[o], 4);
		out.Date(orders.dates[o]);
		out.Int(orders.prio[o]);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q3

#include "accelerator_codegen/tpch_q3_finalize.hpp"

#include <algorithm>
#include <queue>

#include "duckdb/common/exception.hpp"

namespace accelerator_codegen {
namespace {

#if defined(__SIZEOF_INT128__)
duckdb::hugeint_t FromAccumulator(Q3AccT value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}
#else
duckdb::hugeint_t FromAccumulator(const Q3AccT &value) {
	return value;
}
#endif

struct TopKLess {
	bool operator()(const Q3OutputRow &lhs, const Q3OutputRow &rhs) const {
		return BetterQ3Row(lhs, rhs);
	}
};

} // namespace

Q3AccT DiscountedRevenueRaw(int64_t extendedprice_raw, int64_t discount_raw) {
	return static_cast<Q3AccT>(extendedprice_raw) * static_cast<Q3AccT>(100 - discount_raw);
}

void AddBucketRevenue(Q3AccT &bucket, int64_t extendedprice_raw, int64_t discount_raw, int8_t sign) {
#if defined(__SIZEOF_INT128__)
	bucket += DiscountedRevenueRaw(extendedprice_raw, discount_raw) * static_cast<Q3AccT>(sign);
#else
	Q3AccT delta = DiscountedRevenueRaw(extendedprice_raw, discount_raw);
	if (sign < 0) {
		if (!duckdb::Hugeint::TrySubtractInPlace(bucket, delta)) {
			throw duckdb::OutOfRangeException("Overflow while updating Q3 bucket revenue");
		}
	} else if (!duckdb::Hugeint::TryAddInPlace(bucket, delta)) {
		throw duckdb::OutOfRangeException("Overflow while updating Q3 bucket revenue");
	}
#endif
}

void AddAccumulatedRevenue(Q3AccT &target, const Q3AccT &amount, int8_t sign) {
#if defined(__SIZEOF_INT128__)
	target += amount * static_cast<Q3AccT>(sign);
#else
	if (sign < 0) {
		if (!duckdb::Hugeint::TrySubtractInPlace(target, amount)) {
			throw duckdb::OutOfRangeException("Overflow while accumulating Q3 revenue");
		}
	} else if (!duckdb::Hugeint::TryAddInPlace(target, amount)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q3 revenue");
	}
#endif
}

duckdb::hugeint_t Q3RevenueFromAccumulator(Q3AccT raw) {
	return FromAccumulator(raw);
}

Q3AccT SumBucketsAboveCutoff(const Q3OrderState &order, duckdb::date_t cutoff_date) {
	Q3AccT total = 0;
	for (const auto &entry : order.shipdate_buckets) {
		if (entry.first > cutoff_date.days) {
			AddAccumulatedRevenue(total, entry.second, 1);
		}
	}
	return total;
}

bool BetterQ3Row(const Q3OutputRow &lhs, const Q3OutputRow &rhs) {
	if (lhs.revenue != rhs.revenue) {
		return duckdb::Hugeint::GreaterThan(lhs.revenue, rhs.revenue);
	}
	if (lhs.orderdate != rhs.orderdate) {
		return lhs.orderdate < rhs.orderdate;
	}
	if (lhs.orderkey != rhs.orderkey) {
		return lhs.orderkey < rhs.orderkey;
	}
	return lhs.shippriority < rhs.shippriority;
}

std::vector<Q3OutputRow> ComposeQ3TopK(const Q3SegmentPartition &partition, duckdb::date_t cutoff_date) {
	std::priority_queue<Q3OutputRow, std::vector<Q3OutputRow>, TopKLess> heap;
	for (const auto &entry : partition.orders) {
		const auto &order = entry.second;
		if (order.o_orderdate >= cutoff_date) {
			continue;
		}
		const auto revenue_raw = SumBucketsAboveCutoff(order, cutoff_date);
#if defined(__SIZEOF_INT128__)
		if (revenue_raw == 0) {
			continue;
		}
#else
		if (duckdb::Hugeint::Equals(revenue_raw, duckdb::hugeint_t(0, 0))) {
			continue;
		}
#endif
		Q3OutputRow row;
		row.orderkey = entry.first;
		row.revenue = Q3RevenueFromAccumulator(revenue_raw);
		row.orderdate = order.o_orderdate;
		row.shippriority = order.o_shippriority;
		if (heap.size() < kQ3TopK) {
			heap.push(row);
			continue;
		}
		if (BetterQ3Row(row, heap.top())) {
			heap.pop();
			heap.push(row);
		}
	}
	std::vector<Q3OutputRow> ordered;
	ordered.reserve(heap.size());
	while (!heap.empty()) {
		ordered.push_back(heap.top());
		heap.pop();
	}
	std::sort(ordered.begin(), ordered.end(), BetterQ3Row);
	return ordered;
}

} // namespace accelerator_codegen

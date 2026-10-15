// TPC-H Q11 (base PostgreSQL): important stock identification.
// value(part) = sum(ps_supplycost * ps_availqty) over suppliers of the nation (exact int128);
// the HAVING compares value * 10^kFracScale > total * fraction_scaled exactly.
#pragma once

#include "tpch/q2/postgres/base/q2_common.hpp"

namespace accelerator_codegen::pg_base::q11 {

using namespace pe_util;

inline constexpr const char *kSqlArgs = "text, numeric";
inline constexpr const char *kSqlReturns = "TABLE (ps_partkey bigint, value numeric)";
inline constexpr int kFracScale = 12;

struct Params {
	std::string nation;
	int64_t frac; // fraction * 10^kFracScale
};

inline Params Bind(const Args &args) {
	Params p;
	p.nation = args.Text(0);
	p.frac = args.Decimal(1, kFracScale);
	return p;
}

//! Suppliers of the nation (serial scans of the two small tables).
inline Flags LoadSuppliers(const std::string &nation, std::vector<int64_t> &keys) {
	std::vector<int32_t> nk;
	{
		ScanSpec sp;
		sp.table = "nation";
		sp.columns = {{"n_nationkey", ColType::Int32}, {"n_name", ColType::Text}};
		sp.filters = {{1, FilterOp::TextEq, 0, 0, {nation}}};
		ScanTable(sp, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				nk.push_back(b.I32(0)[i]);
			}
		});
	}
	for (int32_t n : nk) {
		ScanSpec sp;
		sp.table = "supplier";
		sp.columns = {{"s_suppkey", ColType::Int64}, {"s_nationkey", ColType::Int32}};
		sp.filters = {{1, FilterOp::Between, n, n}};
		ScanTable(sp, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				keys.push_back(b.I64(0)[i]);
			}
		});
	}
	return MakeFlags(keys);
}

inline ScanSpec PartsuppSpec(const std::vector<int64_t> &suppkeys) {
	ScanSpec s;
	s.table = "partsupp";
	s.columns = {{"ps_suppkey", ColType::Int64},
	             {"ps_partkey", ColType::Int64},
	             {"ps_availqty", ColType::Int32},
	             {"ps_supplycost", ColType::Decimal, 2}};
	// Probe partsupp through idx_partsupp_suppkey for the nation's suppliers.
	FilterSpec f{0, FilterOp::IntIn};
	f.ints = suppkeys;
	s.filters.push_back(std::move(f));
	return s;
}

//! Open-addressing partkey -> int128 sum.
class AggMap {
public:
	AggMap() {
		Alloc(1024);
	}
	void Add(int64_t key, __int128 v) {
		if ((size_ + 1) * 2 > cap_) {
			Grow();
		}
		std::size_t h = Hash(key) & (cap_ - 1);
		while (used_[h]) {
			if (keys_[h] == key) {
				sums_[h] += v;
				return;
			}
			h = (h + 1) & (cap_ - 1);
		}
		used_[h] = 1;
		keys_[h] = key;
		sums_[h] = v;
		size_++;
	}
	template <class F>
	void ForEach(F &&f) const {
		for (std::size_t i = 0; i < cap_; i++) {
			if (used_[i]) {
				f(keys_[i], sums_[i]);
			}
		}
	}

private:
	static std::size_t Hash(int64_t k) {
		return static_cast<std::size_t>((static_cast<uint64_t>(k) * 0x9E3779B97F4A7C15ull) >> 20);
	}
	void Alloc(std::size_t cap) {
		cap_ = cap;
		size_ = 0;
		used_.assign(cap, 0);
		keys_.assign(cap, 0);
		sums_.assign(cap, 0);
	}
	void Grow() {
		AggMap bigger_holder(0);
		bigger_holder.Alloc(cap_ * 2);
		ForEach([&](int64_t k, __int128 v) { bigger_holder.Add(k, v); });
		*this = std::move(bigger_holder);
	}
	explicit AggMap(int) {
	}
	std::size_t cap_ = 0, size_ = 0;
	std::vector<uint8_t> used_;
	std::vector<int64_t> keys_;
	std::vector<__int128> sums_;
};

struct State {
	AggMap map;
	std::vector<uint32_t> sel;
	State() : sel(kBatchRows + simd::kSelSlack) {
	}
	void Consume(const Batch &b, const Flags &sf) {
		const int64_t *pk = b.I64(1);
		const int32_t *qty = b.I32(2);
		const int64_t *cost = b.I64(3);
		const std::size_t n = SelectFlagged(sf, b.I64(0), b.count, sel.data());
		for (std::size_t j = 0; j < n; j++) {
			const std::size_t i = sel[j];
			map.Add(pk[i], static_cast<__int128>(cost[i]) * qty[i]);
		}
	}
};

inline void Finish(const AggMap &map, const Params &p, ResultWriter &out) {
	__int128 total = 0;
	std::vector<std::pair<int64_t, __int128>> all;
	map.ForEach([&](int64_t k, __int128 v) {
		total += v;
		all.emplace_back(k, v);
	});
	if (all.empty()) {
		return; // total is NULL: HAVING is never true
	}
	__int128 scale = 1;
	for (int i = 0; i < kFracScale; i++) {
		scale *= 10;
	}
	const __int128 thr = total * p.frac;
	std::vector<std::pair<int64_t, __int128>> rows;
	for (const auto &kv : all) {
		if (kv.second * scale > thr) {
			rows.push_back(kv);
		}
	}
	std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
		return a.second != b.second ? a.second > b.second : a.first < b.first;
	});
	for (const auto &r : rows) {
		out.Int(r.first);
		out.Decimal(r.second, 2);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_base::q11

// Shared Q15 lineitem aggregation: revenue per supplier in a dense array keyed by l_suppkey.
//
// Re-applies the shipdate range predicate itself (SIMD selection vector on flat chunks), so it is exact whether
// or not the scan pushed the filter down.  Per-call state only (owned by a kernel instance / worker).
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "prims/simd.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tpch/q15/duckdb/base/query.hpp"

namespace accelerator_codegen {
namespace q15 {

#if defined(__SIZEOF_INT128__)
using Acc = __int128_t;
inline duckdb::hugeint_t ToHugeint(const Acc &value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}
inline void AddRevenue(Acc &acc, int64_t price, int64_t discount) {
	acc += static_cast<Acc>(price) * static_cast<Acc>(100 - discount);
}
inline void AddAcc(Acc &acc, const Acc &other) {
	acc += other;
}
#else
using Acc = duckdb::hugeint_t;
inline duckdb::hugeint_t ToHugeint(const Acc &value) {
	return value;
}
inline void AddRevenue(Acc &acc, int64_t price, int64_t discount) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(price, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(acc, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q15 revenue");
	}
}
inline void AddAcc(Acc &acc, const Acc &other) {
	if (!duckdb::Hugeint::TryAddInPlace(acc, other)) {
		throw duckdb::OutOfRangeException("Overflow while merging Q15 revenue");
	}
}
#endif

constexpr int64_t kDefaultSupplierCapacity = 10001;

//! Dense per-supplier revenue (suppkey -> sum(price * (100 - discount)), scale 4).
class RevenueStore {
public:
	RevenueStore() {
		revenue_.assign(static_cast<size_t>(kDefaultSupplierCapacity), Acc {});
		has_.assign(static_cast<size_t>(kDefaultSupplierCapacity), 0);
		active_.reserve(4096);
	}

	void Add(int64_t suppkey, int64_t price, int64_t discount) {
		if (suppkey < 0) {
			return;
		}
		if (static_cast<uint64_t>(suppkey) >= revenue_.size()) {
			Grow(suppkey);
		}
		const auto index = static_cast<size_t>(suppkey);
		if (!has_[index]) {
			has_[index] = 1;
			active_.push_back(suppkey);
		}
		AddRevenue(revenue_[index], price, discount);
	}

	void MergeFrom(const RevenueStore &other) {
		for (const auto suppkey : other.active_) {
			const auto index = static_cast<size_t>(suppkey);
			if (static_cast<uint64_t>(suppkey) >= revenue_.size()) {
				Grow(suppkey);
			}
			if (!has_[index]) {
				has_[index] = 1;
				active_.push_back(suppkey);
			}
			AddAcc(revenue_[index], other.revenue_[index]);
		}
	}

	bool Has(int64_t suppkey) const {
		return suppkey >= 0 && static_cast<uint64_t>(suppkey) < has_.size() && has_[static_cast<size_t>(suppkey)];
	}

	const Acc &Revenue(int64_t suppkey) const {
		return revenue_[static_cast<size_t>(suppkey)];
	}

	const std::vector<int64_t> &Active() const {
		return active_;
	}

private:
	void Grow(int64_t suppkey) {
		size_t target = static_cast<size_t>(suppkey) + 1;
		target = std::max(target, revenue_.size() * 2);
		revenue_.resize(target, Acc {});
		has_.resize(target, 0);
	}

	std::vector<Acc> revenue_;
	std::vector<uint8_t> has_;
	std::vector<int64_t> active_;
};

class ChunkAggregator {
public:
	ChunkAggregator() : sel_(STANDARD_VECTOR_SIZE + simd::kSelSlack) {
	}

	RevenueStore &Store() {
		return store_;
	}

	const RevenueStore &Store() const {
		return store_;
	}

	void Consume(duckdb::DataChunk &chunk, const Q15Parameters &p) {
		const duckdb::idx_t n = chunk.size();
		if (n == 0) {
			return;
		}
		auto &date_vec = chunk.data[Q15LineitemChunkLayout::SHIPDATE_INDEX];
		auto &supp_vec = chunk.data[Q15LineitemChunkLayout::SUPPKEY_INDEX];
		auto &price_vec = chunk.data[Q15LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &disc_vec = chunk.data[Q15LineitemChunkLayout::DISCOUNT_INDEX];
		if (IsFlatAllValid(date_vec) && IsFlatAllValid(supp_vec) && IsFlatAllValid(price_vec) &&
		    IsFlatAllValid(disc_vec) && n <= STANDARD_VECTOR_SIZE) {
			if (p.end_date.days == std::numeric_limits<int32_t>::min()) {
				return;
			}
			const auto *dates = reinterpret_cast<const int32_t *>(duckdb::FlatVector::GetData(date_vec));
			const auto *supps = duckdb::FlatVector::GetData<int64_t>(supp_vec);
			const auto *prices = duckdb::FlatVector::GetData<int64_t>(price_vec);
			const auto *discs = duckdb::FlatVector::GetData<int64_t>(disc_vec);
			const size_t m = simd::SelectBetweenI32(dates, n, p.start_date.days, p.end_date.days - 1, sel_.data());
			for (size_t k = 0; k < m; k++) {
				const uint32_t r = sel_[k];
				store_.Add(supps[r], prices[r], discs[r]);
			}
			return;
		}
		ConsumeGeneric(chunk, p);
	}

private:
	static bool IsFlatAllValid(duckdb::Vector &v) {
		return v.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(v).AllValid();
	}

	void ConsumeGeneric(duckdb::DataChunk &chunk, const Q15Parameters &p) {
		const duckdb::idx_t n = chunk.size();
		duckdb::UnifiedVectorFormat date_f, supp_f, price_f, disc_f;
		chunk.data[Q15LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(n, date_f);
		chunk.data[Q15LineitemChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(n, supp_f);
		chunk.data[Q15LineitemChunkLayout::EXTENDEDPRICE_INDEX].ToUnifiedFormat(n, price_f);
		chunk.data[Q15LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(n, disc_f);
		const auto *dates = reinterpret_cast<const int32_t *>(date_f.data);
		const auto *supps = reinterpret_cast<const int64_t *>(supp_f.data);
		const auto *prices = reinterpret_cast<const int64_t *>(price_f.data);
		const auto *discs = reinterpret_cast<const int64_t *>(disc_f.data);
		for (duckdb::idx_t row = 0; row < n; row++) {
			const auto di = date_f.sel->get_index(row);
			const auto si = supp_f.sel->get_index(row);
			const auto pi = price_f.sel->get_index(row);
			const auto ci = disc_f.sel->get_index(row);
			if (!date_f.validity.RowIsValid(di) || !supp_f.validity.RowIsValid(si) ||
			    !price_f.validity.RowIsValid(pi) || !disc_f.validity.RowIsValid(ci)) {
				continue;
			}
			if (dates[di] < p.start_date.days || dates[di] >= p.end_date.days) {
				continue;
			}
			store_.Add(supps[si], prices[pi], discs[ci]);
		}
	}

	std::vector<uint32_t> sel_;
	RevenueStore store_;
};

} // namespace q15
} // namespace accelerator_codegen

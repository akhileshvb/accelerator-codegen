// Shared Q6 filter+sum core (single-threaded kernel and parallel workers).
//
// Re-applies all five Q6 predicates itself with SIMD selection vectors, so it is exact whether or not the scan
// pushed some/all of them down (already-qualified rows simply pass again).  Flat all-valid chunks take the
// vector path; anything else (dictionary/sliced vectors, NULLs) takes an exact scalar path.
#pragma once

#include <cstdint>
#include <vector>

#include "prims/simd.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tpch/q6/duckdb/base/query.hpp"

namespace accelerator_codegen {
namespace q6 {

class Accumulator {
public:
	Accumulator() : sel_(STANDARD_VECTOR_SIZE + simd::kSelSlack) {
	}

	void Consume(duckdb::DataChunk &chunk, const Q6Parameters &p) {
		const duckdb::idx_t n = chunk.size();
		if (n == 0) {
			return;
		}
		auto &date_vec = chunk.data[Q6LineitemChunkLayout::SHIPDATE_INDEX];
		auto &disc_vec = chunk.data[Q6LineitemChunkLayout::DISCOUNT_INDEX];
		auto &qty_vec = chunk.data[Q6LineitemChunkLayout::QUANTITY_INDEX];
		auto &price_vec = chunk.data[Q6LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		if (IsFlatAllValid(date_vec) && IsFlatAllValid(disc_vec) && IsFlatAllValid(qty_vec) &&
		    IsFlatAllValid(price_vec) && n <= STANDARD_VECTOR_SIZE) {
			const auto *dates = reinterpret_cast<const int32_t *>(duckdb::FlatVector::GetData(date_vec));
			const auto *discs = duckdb::FlatVector::GetData<int64_t>(disc_vec);
			const auto *qtys = duckdb::FlatVector::GetData<int64_t>(qty_vec);
			const auto *prices = duckdb::FlatVector::GetData<int64_t>(price_vec);
			// shipdate in [start, end)
			if (p.end_date.days == INT32_MIN || p.discount_upper_raw < p.discount_lower_raw ||
			    p.quantity_upper_raw == INT64_MIN) {
				return;
			}
			size_t m = simd::SelectBetweenI32(dates, n, p.start_date.days, p.end_date.days - 1, sel_.data());
			if (m) {
				m = simd::RefineBetweenI64(discs, sel_.data(), m, p.discount_lower_raw, p.discount_upper_raw,
				                           sel_.data());
			}
			if (m) {
				m = simd::RefineBetweenI64(qtys, sel_.data(), m, INT64_MIN, p.quantity_upper_raw - 1, sel_.data());
			}
			for (size_t k = 0; k < m; k++) {
				const uint32_t r = sel_[k];
				Add(prices[r], discs[r]);
			}
			has_value_ = has_value_ || m != 0;
			return;
		}
		ConsumeGeneric(chunk, p);
	}

	bool HasValue() const {
		return has_value_;
	}

	duckdb::hugeint_t Value() const {
#if defined(__SIZEOF_INT128__)
		return duckdb::hugeint_t(static_cast<int64_t>(sum_ >> 64), static_cast<uint64_t>(sum_));
#else
		return sum_;
#endif
	}

	void Merge(const Accumulator &o) {
		if (!o.has_value_) {
			return;
		}
#if defined(__SIZEOF_INT128__)
		sum_ += o.sum_;
#else
		if (!duckdb::Hugeint::TryAddInPlace(sum_, o.sum_)) {
			throw duckdb::OutOfRangeException("Overflow while merging Q6 revenue");
		}
#endif
		has_value_ = true;
	}

private:
	static bool IsFlatAllValid(duckdb::Vector &v) {
		return v.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(v).AllValid();
	}

	void Add(int64_t price, int64_t discount) {
#if defined(__SIZEOF_INT128__)
		sum_ += static_cast<__int128_t>(price) * static_cast<__int128_t>(discount);
#else
		duckdb::hugeint_t pv, dv, prod;
		if (!duckdb::Hugeint::TryConvert(price, pv) || !duckdb::Hugeint::TryConvert(discount, dv) ||
		    !duckdb::Hugeint::TryMultiply(pv, dv, prod) || !duckdb::Hugeint::TryAddInPlace(sum_, prod)) {
			throw duckdb::OutOfRangeException("Overflow while accumulating Q6 revenue");
		}
#endif
	}

	void ConsumeGeneric(duckdb::DataChunk &chunk, const Q6Parameters &p) {
		const duckdb::idx_t n = chunk.size();
		duckdb::UnifiedVectorFormat date_f, disc_f, qty_f, price_f;
		chunk.data[Q6LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(n, date_f);
		chunk.data[Q6LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(n, disc_f);
		chunk.data[Q6LineitemChunkLayout::QUANTITY_INDEX].ToUnifiedFormat(n, qty_f);
		chunk.data[Q6LineitemChunkLayout::EXTENDEDPRICE_INDEX].ToUnifiedFormat(n, price_f);
		const auto *dates = reinterpret_cast<const int32_t *>(date_f.data);
		const auto *discs = reinterpret_cast<const int64_t *>(disc_f.data);
		const auto *qtys = reinterpret_cast<const int64_t *>(qty_f.data);
		const auto *prices = reinterpret_cast<const int64_t *>(price_f.data);
		for (duckdb::idx_t row = 0; row < n; row++) {
			const auto di = date_f.sel->get_index(row);
			const auto ci = disc_f.sel->get_index(row);
			const auto qi = qty_f.sel->get_index(row);
			const auto pi = price_f.sel->get_index(row);
			if (!date_f.validity.RowIsValid(di) || !disc_f.validity.RowIsValid(ci) ||
			    !qty_f.validity.RowIsValid(qi) || !price_f.validity.RowIsValid(pi)) {
				continue;
			}
			if (dates[di] < p.start_date.days || dates[di] >= p.end_date.days || discs[ci] < p.discount_lower_raw ||
			    discs[ci] > p.discount_upper_raw || qtys[qi] >= p.quantity_upper_raw) {
				continue;
			}
			Add(prices[pi], discs[ci]);
			has_value_ = true;
		}
	}

	std::vector<uint32_t> sel_;
#if defined(__SIZEOF_INT128__)
	__int128_t sum_ = 0;
#else
	duckdb::hugeint_t sum_ = duckdb::Hugeint::Convert(0);
#endif
	bool has_value_ = false;
};

} // namespace q6
} // namespace accelerator_codegen

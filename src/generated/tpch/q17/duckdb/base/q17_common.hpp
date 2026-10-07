#pragma once

// Query-local building blocks shared by the single-threaded and parallel Q17 kernels.
//
// Q17 plan used here (stateless, one pass over lineitem):
//   1. scan part, collect the qualifying p_partkey set (brand + container match);
//   2. scan lineitem ONCE; a SIMD bitmap probe on l_partkey keeps only rows of qualifying
//      parts (~0.1% of lineitem) and buffers (partkey, quantity, extendedprice) for them;
//   3. finalize: per-part sum/count of quantity from the buffered rows (they contain every
//      lineitem row of every qualifying part), threshold = 0.2 * avg, then sum
//      extendedprice of buffered rows with quantity < threshold (exact 128-bit sum).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "accelerator_codegen/runtime/simd.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tpch/q17/duckdb/base/query.hpp"

namespace accelerator_codegen {
namespace q17 {

inline bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

inline bool StringEquals(const duckdb::string_t &lhs, const std::string &rhs) {
	return lhs.GetSize() == rhs.size() && std::memcmp(lhs.GetData(), rhs.data(), rhs.size()) == 0;
}

//! Append the partkeys of part rows matching brand/container to `keys`.
inline void CollectQualifyingParts(duckdb::DataChunk &chunk, const Q17Parameters &params,
                                   std::vector<int64_t> &keys) {
	auto &partkey_vector = chunk.data[Q17PartChunkLayout::PARTKEY_INDEX];
	auto &brand_vector = chunk.data[Q17PartChunkLayout::BRAND_INDEX];
	auto &container_vector = chunk.data[Q17PartChunkLayout::CONTAINER_INDEX];
	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(brand_vector) && IsFlatAllValid(container_vector)) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(brand_vector);
		auto containers = duckdb::FlatVector::GetData<duckdb::string_t>(container_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (StringEquals(brands[row], params.brand) && StringEquals(containers[row], params.container)) {
				keys.push_back(partkeys[row]);
			}
		}
		return;
	}
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat brand_format;
	duckdb::UnifiedVectorFormat container_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	brand_vector.ToUnifiedFormat(chunk.size(), brand_format);
	container_vector.ToUnifiedFormat(chunk.size(), container_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto brands = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(brand_format);
	auto containers = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(container_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto brand_index = brand_format.sel->get_index(row);
		const auto container_index = container_format.sel->get_index(row);
		if (!partkey_format.validity.RowIsValid(partkey_index) || !brand_format.validity.RowIsValid(brand_index) ||
		    !container_format.validity.RowIsValid(container_index)) {
			continue;
		}
		if (StringEquals(brands[brand_index], params.brand) && StringEquals(containers[container_index], params.container)) {
			keys.push_back(partkeys[partkey_index]);
		}
	}
}

//! Set of qualifying partkeys: bit-vector for dense non-negative keys, hash set otherwise.
class QualSet {
public:
	void Build(const std::vector<int64_t> &keys) {
		empty_ = keys.empty();
		if (empty_) {
			return;
		}
		int64_t max_key = 0;
		bool dense = true;
		for (const auto key : keys) {
			if (key < 0 || key >= (int64_t(1) << 28)) {
				dense = false;
				break;
			}
			max_key = key > max_key ? key : max_key;
		}
		if (dense) {
			nbits_ = static_cast<uint64_t>(max_key) + 1;
			words_.assign((nbits_ + 31) / 32 + 1, 0);
			for (const auto key : keys) {
				words_[static_cast<size_t>(key) >> 5] |= 1u << (key & 31);
			}
		} else {
			nbits_ = 0;
			for (const auto key : keys) {
				overflow_.insert(key);
			}
		}
	}

	bool Empty() const {
		return empty_;
	}

	inline bool Contains(int64_t key) const {
		if (nbits_ != 0) {
			return static_cast<uint64_t>(key) < nbits_ && ((words_[static_cast<size_t>(key) >> 5] >> (key & 31)) & 1u);
		}
		return overflow_.find(key) != overflow_.end();
	}

	//! sel <- indexes i in [0,n) with Contains(keys[i]); requires n + kSelSlack capacity.
	size_t Filter(const int64_t *ACCEL_RESTRICT keys, size_t n, uint32_t *ACCEL_RESTRICT sel) const {
		size_t count = 0;
		size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
		if (nbits_ != 0) {
			const __m512i nb = _mm512_set1_epi64(static_cast<long long>(nbits_));
			const __m256i low5 = _mm256_set1_epi32(31);
			const __m256i one = _mm256_set1_epi32(1);
			const void *base = words_.data();
			for (; i + 8 <= n; i += 8) {
				const __m512i key = _mm512_loadu_si512(keys + i);
				const __mmask8 in_range = _mm512_cmplt_epu64_mask(key, nb);
				const __m512i widx = _mm512_srli_epi64(key, 5);
				const __m256i w = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), in_range, widx, base, 4);
				const __m256i sh = _mm256_and_si256(_mm512_cvtepi64_epi32(key), low5);
				const __m256i bits = _mm256_srlv_epi32(w, sh);
				unsigned m = static_cast<unsigned>(_mm256_test_epi32_mask(bits, one)) & in_range;
				while (m) {
					sel[count++] = static_cast<uint32_t>(i) + static_cast<uint32_t>(__builtin_ctz(m));
					m &= m - 1;
				}
			}
		}
#endif
		for (; i < n; i++) {
			if (Contains(keys[i])) {
				sel[count++] = static_cast<uint32_t>(i);
			}
		}
		return count;
	}

private:
	bool empty_ = true;
	uint64_t nbits_ = 0;
	std::vector<uint32_t> words_;
	std::unordered_set<int64_t> overflow_;
};

//! Buffered lineitem rows of qualifying parts.
struct RowBuffer {
	std::vector<int64_t> partkey;
	std::vector<int64_t> quantity;
	std::vector<int64_t> price;
	std::vector<uint32_t> sel; // scratch
};

inline void ConsumeLineitemChunk(duckdb::DataChunk &chunk, const QualSet &qualifying, RowBuffer &buffer) {
	if (qualifying.Empty() || chunk.size() == 0) {
		return;
	}
	auto &partkey_vector = chunk.data[Q17LineitemChunkLayout::PARTKEY_INDEX];
	auto &quantity_vector = chunk.data[Q17LineitemChunkLayout::QUANTITY_INDEX];
	auto &price_vector = chunk.data[Q17LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	const size_t n = chunk.size();
	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(quantity_vector) && IsFlatAllValid(price_vector)) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
		auto prices = duckdb::FlatVector::GetData<int64_t>(price_vector);
		if (buffer.sel.size() < n + simd::kSelSlack) {
			buffer.sel.resize(n + simd::kSelSlack);
		}
		const size_t count = qualifying.Filter(partkeys, n, buffer.sel.data());
		for (size_t j = 0; j < count; j++) {
			const size_t row = buffer.sel[j];
			buffer.partkey.push_back(partkeys[row]);
			buffer.quantity.push_back(quantities[row]);
			buffer.price.push_back(prices[row]);
		}
		return;
	}
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat quantity_format;
	duckdb::UnifiedVectorFormat price_format;
	partkey_vector.ToUnifiedFormat(n, partkey_format);
	quantity_vector.ToUnifiedFormat(n, quantity_format);
	price_vector.ToUnifiedFormat(n, price_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
	auto prices = duckdb::UnifiedVectorFormat::GetData<int64_t>(price_format);
	for (size_t row = 0; row < n; row++) {
		const auto pi = partkey_format.sel->get_index(row);
		const auto qi = quantity_format.sel->get_index(row);
		const auto ei = price_format.sel->get_index(row);
		if (!partkey_format.validity.RowIsValid(pi) || !quantity_format.validity.RowIsValid(qi) ||
		    !price_format.validity.RowIsValid(ei)) {
			continue;
		}
		if (qualifying.Contains(partkeys[pi])) {
			buffer.partkey.push_back(partkeys[pi]);
			buffer.quantity.push_back(quantities[qi]);
			buffer.price.push_back(prices[ei]);
		}
	}
}

struct PartAggSlot {
	int64_t sum_raw = 0;
	int64_t count = 0;
};

//! sum(l_extendedprice) over buffered rows with quantity < 0.2 * avg(quantity of the same part).
//! Returns false when no row qualifies (SQL NULL). Result is the exact scaled-by-100 sum.
inline bool ReduceBuffers(const std::vector<const RowBuffer *> &buffers, duckdb::hugeint_t &result) {
	std::unordered_map<int64_t, PartAggSlot> agg;
	for (const auto *buffer : buffers) {
		for (size_t i = 0; i < buffer->partkey.size(); i++) {
			auto &slot = agg[buffer->partkey[i]];
			slot.sum_raw += buffer->quantity[i];
			slot.count += 1;
		}
	}
	std::unordered_map<int64_t, double> thresholds;
	thresholds.reserve(agg.size());
	for (const auto &entry : agg) {
		const double avg_quantity = (static_cast<double>(entry.second.sum_raw) / 100.0) / static_cast<double>(entry.second.count);
		thresholds[entry.first] = 0.2 * avg_quantity;
	}
	bool has = false;
#if defined(__SIZEOF_INT128__)
	__int128_t total = 0;
#else
	duckdb::hugeint_t total = duckdb::Hugeint::Convert(0);
#endif
	for (const auto *buffer : buffers) {
		for (size_t i = 0; i < buffer->partkey.size(); i++) {
			const double quantity = static_cast<double>(buffer->quantity[i]) / 100.0;
			if (!(quantity < thresholds[buffer->partkey[i]])) {
				continue;
			}
			has = true;
#if defined(__SIZEOF_INT128__)
			total += static_cast<__int128_t>(buffer->price[i]);
#else
			duckdb::hugeint_t addend;
			if (!duckdb::Hugeint::TryConvert(buffer->price[i], addend) ||
			    !duckdb::Hugeint::TryAddInPlace(total, addend)) {
				throw duckdb::OutOfRangeException("Overflow while accumulating Q17 extendedprice sum");
			}
#endif
		}
	}
#if defined(__SIZEOF_INT128__)
	result = duckdb::hugeint_t(static_cast<int64_t>(total >> 64), static_cast<uint64_t>(total));
#else
	result = total;
#endif
	return has;
}

} // namespace q17
} // namespace accelerator_codegen

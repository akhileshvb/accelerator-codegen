#pragma once

// Query-local building blocks shared by the single-threaded and parallel Q21 kernels.
//
// Plan (stateless, one pass over lineitem, no row buffering / sorting):
//   * orders(F) keys -> bitmap + per-word prefix popcounts, so every finished order has a dense rank;
//   * lineitem rows of finished orders are folded (in registers, per run of equal l_orderkey) into
//     a 24-byte per-order summary indexed by rank:
//        first supplier, "has a second supplier", first late supplier + #late lines, "has a second late supplier";
//   * an order contributes `late_count` to the wait count of its single late supplier iff it has >= 2 suppliers,
//     exactly one late supplier and that supplier is in the target nation (this is exactly
//     EXISTS(other supplier) AND NOT EXISTS(other late supplier), counted per late l1 line).
// The summary merge is commutative/associative, so runs split across chunks/workers merge exactly.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>

#include "accelerator_codegen/runtime/simd.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tpch/q21/duckdb/base/query.hpp"

namespace accelerator_codegen {
namespace q21 {

inline bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

enum StateFlag : uint8_t {
	kFirst = 1,
	kMulti = 2,
	kLate = 4,
	kLateMulti = 8,
	kLock = 128,
};

struct OrderState {
	int64_t first = 0;
	int64_t late = 0;
	uint32_t late_count = 0;
	uint8_t flags = 0;
};

//! Set of finished-order keys with a dense rank.
class FinishedOrders {
public:
	void Build(std::vector<int64_t> &keys) {
		int64_t max_key = -1;
		for (const auto key : keys) {
			max_key = key > max_key ? key : max_key;
		}
		if (max_key < 0) {
			size_ = 0;
			return;
		}
		if (max_key < (int64_t(1) << 31)) {
			dense_ = true;
			nbits_ = static_cast<uint64_t>(max_key) + 1;
			words_.assign(nbits_ / 64 + 2, 0);
			for (const auto key : keys) {
				if (key >= 0) {
					words_[static_cast<size_t>(key) >> 6] |= uint64_t(1) << (key & 63);
				}
			}
			prefix_.resize(words_.size());
			uint32_t running = 0;
			for (size_t w = 0; w < words_.size(); w++) {
				prefix_[w] = running;
				running += static_cast<uint32_t>(__builtin_popcountll(words_[w]));
			}
			size_ = running;
			return;
		}
		dense_ = false;
		sorted_.reserve(keys.size());
		for (const auto key : keys) {
			if (key >= 0) {
				sorted_.push_back(key);
			}
		}
		std::sort(sorted_.begin(), sorted_.end());
		sorted_.erase(std::unique(sorted_.begin(), sorted_.end()), sorted_.end());
		size_ = sorted_.size();
	}

	size_t Size() const {
		return size_;
	}

	//! Rank of `key` among finished orders, or -1 when absent.
	inline int64_t Rank(int64_t key) const {
		if (key < 0) {
			return -1;
		}
		if (dense_) {
			if (static_cast<uint64_t>(key) >= nbits_) {
				return -1;
			}
			const size_t w = static_cast<size_t>(key) >> 6;
			const unsigned bit = static_cast<unsigned>(key & 63);
			const uint64_t word = words_[w];
			if (!((word >> bit) & 1)) {
				return -1;
			}
			return static_cast<int64_t>(prefix_[w]) + __builtin_popcountll(word & ((uint64_t(1) << bit) - 1));
		}
		const auto it = std::lower_bound(sorted_.begin(), sorted_.end(), key);
		if (it == sorted_.end() || *it != key) {
			return -1;
		}
		return it - sorted_.begin();
	}

	//! sel <- indexes i with keys[i] finished (needs n + kSelSlack capacity).
	size_t Filter(const int64_t *ACCEL_RESTRICT keys, size_t n, uint32_t *ACCEL_RESTRICT sel) const {
		size_t count = 0;
		size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
		if (dense_) {
			const __m512i nb = _mm512_set1_epi64(static_cast<long long>(nbits_));
			const __m512i low6 = _mm512_set1_epi64(63);
			const __m512i one = _mm512_set1_epi64(1);
			const void *base = words_.data();
			for (; i + 8 <= n; i += 8) {
				const __m512i key = _mm512_loadu_si512(keys + i);
				const __mmask8 in_range = _mm512_cmplt_epu64_mask(key, nb);
				const __m512i w = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), in_range,
				                                              _mm512_srli_epi64(key, 6), base, 8);
				const __m512i bits = _mm512_srlv_epi64(w, _mm512_and_si512(key, low6));
				unsigned m = static_cast<unsigned>(_mm512_test_epi64_mask(bits, one)) & in_range;
				while (m) {
					sel[count++] = static_cast<uint32_t>(i) + static_cast<uint32_t>(__builtin_ctz(m));
					m &= m - 1;
				}
			}
		}
#endif
		for (; i < n; i++) {
			if (Rank(keys[i]) >= 0) {
				sel[count++] = static_cast<uint32_t>(i);
			}
		}
		return count;
	}

private:
	bool dense_ = true;
	uint64_t nbits_ = 0;
	size_t size_ = 0;
	std::vector<uint64_t> words_;
	std::vector<uint32_t> prefix_;
	std::vector<int64_t> sorted_;
};

struct FreeDeleter {
	void operator()(void *p) const {
		std::free(p);
	}
};

//! Zero-initialised (lazily mapped) per-order summaries indexed by rank.
class OrderStates {
public:
	void Allocate(size_t count) {
		count_ = count;
		data_.reset(static_cast<OrderState *>(std::calloc(count ? count : 1, sizeof(OrderState))));
		if (!data_) {
			throw std::bad_alloc();
		}
	}
	OrderState *data() const {
		return data_.get();
	}
	size_t size() const {
		return count_;
	}

private:
	size_t count_ = 0;
	std::unique_ptr<OrderState, FreeDeleter> data_;
};

//! Folds consecutive rows of the same order key into a register-resident run summary,
//! then merges the run into the per-order state (atomically w.r.t. a lock bit when kShared).
template <bool kShared>
class RunAccumulator {
public:
	RunAccumulator(const FinishedOrders &finished, OrderState *states) : finished_(finished), states_(states) {
	}

	inline void Add(int64_t orderkey, int64_t suppkey, bool is_late) {
		if (!active_ || orderkey != key_) {
			Flush();
			key_ = orderkey;
			active_ = true;
			first_ = suppkey;
			flags_ = kFirst;
			late_count_ = 0;
		} else if (suppkey != first_) {
			flags_ |= kMulti;
		}
		if (is_late) {
			if (!(flags_ & kLate)) {
				late_ = suppkey;
				late_count_ = 1;
				flags_ |= kLate;
			} else if (suppkey == late_) {
				late_count_++;
			} else {
				flags_ |= kLateMulti;
			}
		}
	}

	void Flush() {
		if (!active_) {
			return;
		}
		active_ = false;
		const int64_t rank = finished_.Rank(key_);
		if (rank < 0) {
			return;
		}
		OrderState &s = states_[rank];
		uint8_t f;
		if (kShared) {
			for (;;) {
				f = __atomic_load_n(&s.flags, __ATOMIC_RELAXED);
				if (!(f & kLock) && __atomic_compare_exchange_n(&s.flags, &f, static_cast<uint8_t>(f | kLock), true,
				                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
					break;
				}
#if defined(__x86_64__)
				__builtin_ia32_pause();
#endif
			}
		} else {
			f = s.flags;
		}
		uint8_t nf = static_cast<uint8_t>(f & ~kLock);
		if (!(nf & kFirst)) {
			s.first = first_;
			nf |= static_cast<uint8_t>(flags_ & (kFirst | kMulti));
		} else {
			nf |= static_cast<uint8_t>(flags_ & kMulti);
			if (s.first != first_) {
				nf |= kMulti;
			}
		}
		if (flags_ & kLate) {
			if (!(nf & kLate)) {
				s.late = late_;
				s.late_count = late_count_;
				nf |= kLate;
			} else if (s.late == late_) {
				s.late_count += late_count_;
			} else {
				nf |= kLateMulti;
			}
		}
		nf |= static_cast<uint8_t>(flags_ & kLateMulti);
		if (kShared) {
			__atomic_store_n(&s.flags, nf, __ATOMIC_RELEASE);
		} else {
			s.flags = nf;
		}
	}

private:
	const FinishedOrders &finished_;
	OrderState *states_;
	bool active_ = false;
	int64_t key_ = 0;
	int64_t first_ = 0;
	int64_t late_ = 0;
	uint32_t late_count_ = 0;
	uint8_t flags_ = 0;
};

//! Qualifying suppliers (target nation): name by first occurrence, compact ids in insertion order.
class SupplierIndex {
public:
	void Insert(int64_t suppkey, std::string name) {
		if (map_.emplace(suppkey, static_cast<int32_t>(keys_.size())).second) {
			keys_.push_back(suppkey);
			names_.push_back(std::move(name));
		}
	}

	void Freeze() {
		int64_t max_key = -1;
		bool dense = true;
		for (const auto key : keys_) {
			if (key < 0 || key >= (int64_t(1) << 22)) {
				dense = false;
				break;
			}
			max_key = key > max_key ? key : max_key;
		}
		dense_.clear();
		if (dense && max_key >= 0) {
			dense_.assign(static_cast<size_t>(max_key) + 1, -1);
			for (size_t i = 0; i < keys_.size(); i++) {
				dense_[static_cast<size_t>(keys_[i])] = static_cast<int32_t>(i);
			}
			use_dense_ = true;
		} else {
			use_dense_ = false;
		}
	}

	inline int32_t Compact(int64_t suppkey) const {
		if (use_dense_) {
			return static_cast<uint64_t>(suppkey) < dense_.size() ? dense_[static_cast<size_t>(suppkey)] : -1;
		}
		const auto it = map_.find(suppkey);
		return it == map_.end() ? -1 : it->second;
	}

	size_t Count() const {
		return keys_.size();
	}
	const std::string &Name(size_t compact) const {
		return names_[compact];
	}

private:
	bool use_dense_ = false;
	std::vector<int32_t> dense_;
	std::unordered_map<int64_t, int32_t> map_;
	std::vector<int64_t> keys_;
	std::vector<std::string> names_;
};

inline void CollectSuppliers(duckdb::DataChunk &chunk, SupplierIndex &index) {
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat name_format;
	chunk.data[Q21SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(chunk.size(), suppkey_format);
	chunk.data[Q21SupplierChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto si = suppkey_format.sel->get_index(row);
		const auto ni = name_format.sel->get_index(row);
		if (!suppkey_format.validity.RowIsValid(si) || !name_format.validity.RowIsValid(ni)) {
			continue;
		}
		index.Insert(suppkeys[si], std::string(names[ni].GetData(), names[ni].GetSize()));
	}
}

inline void CollectOrderKeys(duckdb::DataChunk &chunk, std::vector<int64_t> &keys) {
	auto &orderkey_vector = chunk.data[Q21OrdersChunkLayout::ORDERKEY_INDEX];
	if (IsFlatAllValid(orderkey_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		keys.insert(keys.end(), orderkeys, orderkeys + chunk.size());
		return;
	}
	duckdb::UnifiedVectorFormat format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto index = format.sel->get_index(row);
		if (format.validity.RowIsValid(index)) {
			keys.push_back(orderkeys[index]);
		}
	}
}

template <bool kShared>
inline void ConsumeLineitemChunk(duckdb::DataChunk &chunk, const FinishedOrders &finished,
                                 RunAccumulator<kShared> &acc, std::vector<uint32_t> &sel) {
	auto &orderkey_vector = chunk.data[Q21LineitemChunkLayout::ORDERKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q21LineitemChunkLayout::SUPPKEY_INDEX];
	auto &commitdate_vector = chunk.data[Q21LineitemChunkLayout::COMMITDATE_INDEX];
	auto &receiptdate_vector = chunk.data[Q21LineitemChunkLayout::RECEIPTDATE_INDEX];
	const size_t n = chunk.size();
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(commitdate_vector) &&
	    IsFlatAllValid(receiptdate_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto commitdates = duckdb::FlatVector::GetData<int32_t>(commitdate_vector);
		auto receiptdates = duckdb::FlatVector::GetData<int32_t>(receiptdate_vector);
		if (sel.size() < n + simd::kSelSlack) {
			sel.resize(n + simd::kSelSlack);
		}
		const size_t count = finished.Filter(orderkeys, n, sel.data());
		for (size_t j = 0; j < count; j++) {
			const size_t row = sel[j];
			acc.Add(orderkeys[row], suppkeys[row], receiptdates[row] > commitdates[row]);
		}
		return;
	}
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat commitdate_format;
	duckdb::UnifiedVectorFormat receiptdate_format;
	orderkey_vector.ToUnifiedFormat(n, orderkey_format);
	suppkey_vector.ToUnifiedFormat(n, suppkey_format);
	commitdate_vector.ToUnifiedFormat(n, commitdate_format);
	receiptdate_vector.ToUnifiedFormat(n, receiptdate_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto commitdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(commitdate_format);
	auto receiptdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(receiptdate_format);
	for (size_t row = 0; row < n; row++) {
		const auto oi = orderkey_format.sel->get_index(row);
		const auto si = suppkey_format.sel->get_index(row);
		const auto ci = commitdate_format.sel->get_index(row);
		const auto ri = receiptdate_format.sel->get_index(row);
		if (!orderkey_format.validity.RowIsValid(oi) || !suppkey_format.validity.RowIsValid(si) ||
		    !commitdate_format.validity.RowIsValid(ci) || !receiptdate_format.validity.RowIsValid(ri)) {
			continue;
		}
		if (finished.Rank(orderkeys[oi]) < 0) {
			continue;
		}
		acc.Add(orderkeys[oi], suppkeys[si], receiptdates[ri] > commitdates[ci]);
	}
}

//! wait[compact supplier] += late_count for every contributing order in [begin, end).
inline void ReduceStates(const OrderState *states, size_t begin, size_t end, const SupplierIndex &suppliers,
                         std::vector<int64_t> &wait) {
	constexpr uint8_t kMask = kMulti | kLate | kLateMulti;
	for (size_t r = begin; r < end; r++) {
		const OrderState &s = states[r];
		if ((s.flags & kMask) != (kMulti | kLate)) {
			continue;
		}
		const int32_t compact = suppliers.Compact(s.late);
		if (compact >= 0) {
			wait[static_cast<size_t>(compact)] += s.late_count;
		}
	}
}

struct OutputRow {
	std::string name;
	int64_t numwait = 0;
};

inline bool BetterOutputRow(const OutputRow &lhs, const OutputRow &rhs) {
	if (lhs.numwait != rhs.numwait) {
		return lhs.numwait > rhs.numwait;
	}
	return lhs.name < rhs.name;
}

inline std::vector<OutputRow> BuildOutput(const SupplierIndex &suppliers, const std::vector<int64_t> &wait) {
	std::vector<OutputRow> rows;
	for (size_t compact = 0; compact < suppliers.Count(); compact++) {
		if (wait[compact] == 0) {
			continue;
		}
		OutputRow row;
		row.name = suppliers.Name(compact);
		row.numwait = wait[compact];
		rows.push_back(std::move(row));
	}
	std::sort(rows.begin(), rows.end(), BetterOutputRow);
	return rows;
}

} // namespace q21
} // namespace accelerator_codegen

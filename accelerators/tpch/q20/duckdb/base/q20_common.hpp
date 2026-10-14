// Query-local helpers shared by the Q20 single-threaded and parallel kernels (per-call state only).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

#include "prims/simd.hpp"

namespace accelerator_codegen {
namespace q20 {

//! Dense byte flags keyed by partkey (TPC-H keys are dense); odd keys go to an overflow set.
class PartFlags {
public:
	PartFlags() {
		Grow(200001);
	}

	void Insert(int64_t partkey) {
		any_ = true;
		if (partkey < 0 || partkey > 1'000'000'000) {
			overflow_.insert(partkey);
			return;
		}
		if (static_cast<size_t>(partkey) >= flags_.size()) {
			Grow(std::max(static_cast<size_t>(partkey) + 1, flags_.size() * 2));
		}
		flags_[static_cast<size_t>(partkey)] = 1;
	}

	void MergeFrom(const PartFlags &other) {
		if (other.flags_.size() > flags_.size()) {
			Grow(other.flags_.size());
		}
		for (size_t i = 0; i < other.flags_.size(); i++) {
			flags_[i] |= other.flags_[i];
		}
		overflow_.insert(other.overflow_.begin(), other.overflow_.end());
		any_ = any_ || other.any_;
	}

	bool Contains(int64_t partkey) const {
		if (static_cast<uint64_t>(partkey) < flags_.size()) {
			return flags_[static_cast<size_t>(partkey)] != 0;
		}
		return !overflow_.empty() && overflow_.find(partkey) != overflow_.end();
	}

	bool Empty() const {
		return !any_;
	}
	bool SimdUsable() const {
		return overflow_.empty();
	}
	const uint8_t *Data() const {
		return flags_.data();
	}
	size_t Size() const {
		return flags_.size();
	}

private:
	void Grow(size_t n) {
		flags_.resize(n, 0);
		flags_.reserve(n + 8); // vector gathers over-read up to 3 bytes
	}

	std::vector<uint8_t> flags_;
	std::unordered_set<int64_t> overflow_;
	bool any_ = false;
};

//! sel <- rows i with flags[keys[i]] != 0 (keys outside [0, cap) are rejected).
//! flags needs >= 3 readable bytes past cap.  sel needs n + kSelSlack entries.
inline size_t SelectFlagged(const uint8_t *flags, size_t cap, const int64_t *ACCEL_RESTRICT keys, size_t n,
                            uint32_t *ACCEL_RESTRICT sel) {
	size_t count = 0;
	size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vcap = _mm512_set1_epi64(static_cast<long long>(cap));
	const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const __m256i ff = _mm256_set1_epi32(0xFF);
	for (; i + 8 <= n; i += 8) {
		const __m512i k = _mm512_loadu_si512(keys + i);
		const __mmask8 in = _mm512_cmplt_epu64_mask(k, vcap);
		const __m256i g = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), in, k, flags, 1);
		const __mmask8 hit = _mm256_mask_test_epi32_mask(in, g, ff);
		const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
		_mm256_mask_compressstoreu_epi32(sel + count, hit, idx);
		count += static_cast<size_t>(__builtin_popcount(hit));
	}
#endif
	for (; i < n; i++) {
		const uint64_t k = static_cast<uint64_t>(keys[i]);
		sel[count] = static_cast<uint32_t>(i);
		count += (k < cap) & (flags[k < cap ? k : 0] != 0);
	}
	return count;
}

//! Open-addressing (partkey, suppkey) -> sum(quantity) table.
class AggTable {
public:
	AggTable() {
		Reset(1024);
	}

	void Add(int64_t partkey, int64_t suppkey, int64_t qty) {
		if ((count_ + 1) * 2 > cap_) {
			Reset(cap_ * 2, true);
		}
		size_t pos = Slot(partkey, suppkey);
		while (part_[pos] != kEmpty) {
			if (part_[pos] == partkey && supp_[pos] == suppkey) {
				sum_[pos] += qty;
				return;
			}
			pos = (pos + 1) & (cap_ - 1);
		}
		part_[pos] = partkey;
		supp_[pos] = suppkey;
		sum_[pos] = qty;
		count_++;
	}

	const int64_t *Find(int64_t partkey, int64_t suppkey) const {
		size_t pos = Slot(partkey, suppkey);
		while (part_[pos] != kEmpty) {
			if (part_[pos] == partkey && supp_[pos] == suppkey) {
				return &sum_[pos];
			}
			pos = (pos + 1) & (cap_ - 1);
		}
		return nullptr;
	}

	void MergeFrom(const AggTable &other) {
		for (size_t i = 0; i < other.cap_; i++) {
			if (other.part_[i] != kEmpty) {
				Add(other.part_[i], other.supp_[i], other.sum_[i]);
			}
		}
	}

	size_t Size() const {
		return count_;
	}

private:
	// kEmpty is a partkey sentinel; a real partkey equal to it is not representable and cannot occur.
	static constexpr int64_t kEmpty = std::numeric_limits<int64_t>::min();

	size_t Slot(int64_t partkey, int64_t suppkey) const {
		uint64_t h = (static_cast<uint64_t>(partkey) * 0x9E3779B97F4A7C15ULL) ^
		             (static_cast<uint64_t>(suppkey) * 0xC2B2AE3D27D4EB4FULL);
		h ^= h >> 29;
		h *= 0xBF58476D1CE4E5B9ULL;
		h ^= h >> 32;
		return static_cast<size_t>(h) & (cap_ - 1);
	}

	void Reset(size_t cap, bool rehash = false) {
		std::vector<int64_t> op, os, osum;
		if (rehash) {
			op.swap(part_);
			os.swap(supp_);
			osum.swap(sum_);
		}
		cap_ = cap;
		part_.assign(cap_, kEmpty);
		supp_.assign(cap_, 0);
		sum_.assign(cap_, 0);
		count_ = 0;
		for (size_t i = 0; i < op.size(); i++) {
			if (op[i] != kEmpty) {
				Add(op[i], os[i], osum[i]);
			}
		}
	}

	size_t cap_ = 0;
	size_t count_ = 0;
	std::vector<int64_t> part_;
	std::vector<int64_t> supp_;
	std::vector<int64_t> sum_;
};

} // namespace q20
} // namespace accelerator_codegen
